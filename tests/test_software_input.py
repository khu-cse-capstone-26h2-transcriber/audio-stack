"""End-to-end WAV -> ASTK -> dummy engine -> HTTP session lifecycle, no sound card."""
import array
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock
import wave

ROOT = Path(__file__).resolve().parents[1]


class SoftwareInputTest(unittest.TestCase):
    def test_dummy_engine_flushes_trailing_speech(self):
        spec = importlib.util.spec_from_file_location('dummy_engine_under_test', ROOT / 'tests/dummy_engine.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        sent = []
        pcm = array.array('h', [1000] * 1600).tobytes()

        class Client:
            def __init__(self, path):
                pass

            def connect(self, **kwargs):
                pass

            def frames(self):
                return iter([
                    SimpleNamespace(type=module.MsgType.AUDIO, payload=pcm),
                    SimpleNamespace(type=module.MsgType.SESSION_END, json=lambda: {}),
                    SimpleNamespace(type=module.MsgType.GOODBYE),
                ])

            def send_result(self, text, **kwargs):
                sent.append((text, kwargs))

            def close(self):
                pass

        with mock.patch.object(module, 'AudioStackClient', Client), \
             mock.patch.object(sys, 'argv', ['dummy_engine']), \
             contextlib.redirect_stdout(io.StringIO()):
            module.main()
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0][1]['kind'], 'final')
        self.assertEqual(sent[0][1]['t_end_ms'], 100)

    def test_frame_stream_and_http_lifecycle(self):
        # socketpair tests the byte stream even on Windows Python without AF_UNIX.
        # The separate subprocess test below exercises the real Unix socket on Linux.
        spec = importlib.util.spec_from_file_location('feed_wav_under_test', ROOT / 'tools/feed_wav.py')
        feeder_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(feeder_module)
        from astk_client import AudioStackClient, MsgType

        events, audio, errors = [], bytearray(), []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                events.append((self.path, json.loads(self.rfile.read(int(self.headers['Content-Length'])))))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{}')

            def log_message(self, *args):
                pass

        backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        backend_thread = threading.Thread(target=backend.serve_forever, daemon=True)
        backend_thread.start()
        server_stream, engine_stream = socket.socketpair()
        server_stream.settimeout(5)
        engine_stream.settimeout(5)

        class Listener:
            def bind(self, path):
                Path(path).touch()

            def listen(self, backlog):
                pass

            def accept(self):
                return server_stream, None

            def close(self):
                server_stream.close()

        class StreamFactory:
            AF_UNIX = object()
            SOCK_STREAM = socket.SOCK_STREAM

            @staticmethod
            def socket(*args):
                return Listener()

        def run_engine():
            c = AudioStackClient()
            c._sock = engine_stream
            try:
                for frame in c.frames():
                    if frame.type == MsgType.HELLO:
                        self.assertEqual(frame.json()['format']['rate'], 16000)
                    elif frame.type == MsgType.AUDIO:
                        self.assertLessEqual(len(frame.payload), 640)
                        audio.extend(frame.payload)
                    elif frame.type == MsgType.SESSION_END:
                        c.send_result('마지막 문장', kind='final')
                    elif frame.type == MsgType.GOODBYE:
                        return
            except BaseException as exc:
                errors.append(exc)
            finally:
                c.close()

        engine_thread = threading.Thread(target=run_engine, daemon=True)
        engine_thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='astk-stream-') as td:
                wav = Path(td) / 'input.wav'
                samples = array.array('h', [1000] * 4800)
                if sys.byteorder != 'little':
                    samples.byteswap()
                with wave.open(str(wav), 'wb') as w:
                    w.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
                    w.writeframes(samples.tobytes())
                argv = ['feed_wav', str(wav), '--socket', str(Path(td) / 'engine.sock'),
                        '--no-realtime', '--finalize-seconds', '5', '--backend-url',
                        f'http://127.0.0.1:{backend.server_port}']
                with mock.patch.object(feeder_module, 'socket', StreamFactory), \
                     mock.patch.object(sys, 'argv', argv), \
                     contextlib.redirect_stdout(io.StringIO()):
                    feeder_module.main()
            engine_thread.join(timeout=5)
            self.assertFalse(engine_thread.is_alive())
            self.assertFalse(errors, errors)
            self.assertEqual(len(audio), 3200)  # 100 ms converted from 48 kHz to 16 kHz
            self.assertEqual(len(events), 3, events)
            session = events[0][1]['session_id']
            self.assertEqual([path for path, _ in events], [
                '/api/sessions', f'/api/sessions/{session}/transcripts',
                f'/api/sessions/{session}/end',
            ])
            self.assertEqual(events[1][1]['transcript']['text'], '마지막 문장')
            self.assertTrue(events[2][1]['ended_at'])
        finally:
            server_stream.close()
            engine_stream.close()
            engine_thread.join(timeout=5)
            backend.shutdown()
            backend.server_close()
            backend_thread.join()

    @unittest.skipUnless(hasattr(socket, 'AF_UNIX'), 'real AF_UNIX socket requires Linux/Unix Python')
    def test_trailing_utterance_and_session_lifecycle(self):
        events = []

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                obj = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                events.append((self.path, obj))
                self.send_response(201 if self.path == '/api/sessions' else 200)
                self.end_headers()
                self.wfile.write(b'{}')

            def log_message(self, *args):
                pass

        backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=backend.serve_forever, daemon=True)
        worker.start()
        processes = []
        try:
            with tempfile.TemporaryDirectory(prefix='astk-') as td:
                wav = Path(td) / 'speech.wav'
                # No trailing silence: only SESSION_END can finalize this utterance.
                samples = array.array('h', (int(10000 * math.sin(i * 2 * math.pi * 440 / 16000))
                                           for i in range(4000)))
                if sys.byteorder != 'little':
                    samples.byteswap()
                with wave.open(str(wav), 'wb') as w:
                    w.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
                    w.writeframes(samples.tobytes())
                path = str(Path(td) / 'engine.sock')
                env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1')
                feeder = subprocess.Popen([
                    sys.executable, str(ROOT / 'tools/feed_wav.py'), str(wav),
                    '--socket', path, '--no-realtime', '--finalize-seconds', '5',
                    '--backend-url', f'http://127.0.0.1:{backend.server_port}',
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                processes.append(feeder)
                deadline = time.monotonic() + 5
                while not Path(path).exists() and time.monotonic() < deadline and feeder.poll() is None:
                    time.sleep(0.02)
                self.assertIsNone(feeder.poll(), 'WAV server failed before the engine connected')
                engine = subprocess.Popen([
                    sys.executable, str(ROOT / 'tests/dummy_engine.py'), '--socket', path,
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                processes.append(engine)
                out, err = feeder.communicate(timeout=15)
                eout, eerr = engine.communicate(timeout=5)
                self.assertEqual(feeder.returncode, 0, (out + err).decode('utf-8', 'replace'))
                self.assertEqual(engine.returncode, 0, (eout + eerr).decode('utf-8', 'replace'))
                self.assertEqual(len(events), 3, events)
                session = events[0][1]['session_id']
                self.assertEqual([p for p, _ in events], [
                    '/api/sessions', f'/api/sessions/{session}/transcripts',
                    f'/api/sessions/{session}/end',
                ])
                self.assertTrue(events[0][1]['device'].startswith('file:'))
                transcript = events[1][1]
                self.assertEqual(transcript['session_id'], session)
                self.assertEqual(transcript['seq'], 1)
                self.assertEqual(transcript['transcript']['type'], 'final')
                self.assertIn('dummy transcript', transcript['transcript']['text'])
                self.assertTrue(events[2][1]['ended_at'])
        finally:
            for proc in processes:
                if proc.poll() is None:
                    proc.kill()
                proc.communicate()
            backend.shutdown()
            backend.server_close()
            worker.join()

    def test_invalid_input_options_fail(self):
        for args in (['--chunk-ms', '0'], ['--speed', '0'], ['--finalize-seconds', '-1']):
            with self.subTest(args=args):
                proc = subprocess.run([
                    sys.executable, str(ROOT / 'tools/feed_wav.py'), 'unused.wav', *args,
                ], capture_output=True, timeout=5)
                self.assertEqual(proc.returncode, 2)

    def test_missing_wav_fails(self):
        proc = subprocess.run([
            sys.executable, str(ROOT / 'tools/feed_wav.py'), 'missing-input.wav',
        ], capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 1)


if __name__ == '__main__':
    unittest.main()
