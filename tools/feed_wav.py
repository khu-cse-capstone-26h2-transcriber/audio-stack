#!/usr/bin/env python3
"""
feed_wav — microphone stand-in: streams a WAV file into the ASTK protocol.

It binds an engine.sock exactly like audiostackd does (HELLO / SESSION_BEGIN /
20 ms AUDIO frames / SESSION_END), so the engine side (client/whisper_engine.py
or the real Whisper pipeline) runs completely unchanged. Received RESULT frames
are printed and optionally forwarded to the backend (same JSON envelope the
C++ daemon sends), so the whole SSE demo flow works without any hardware.

usage:
  python3 tools/feed_wav.py samples/jfk.wav                     # real-time
  python3 tools/feed_wav.py samples/jfk.wav --no-realtime       # burst (fast test)
  python3 tools/feed_wav.py samples/synth_speech.wav --loop     # continuous
  python3 tools/feed_wav.py samples/jfk.wav --backend-url http://127.0.0.1:3000
"""
import argparse
import array
import json
import math
import os
import select
import socket
import sys
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client"))
from astk_client import AudioStackClient, MsgType, Flags, HEADER, MAGIC, VERSION  # noqa: E402
from wav_io import read_wav_mono16, WavError, TARGET_RATE  # noqa: E402


def rms_db(payload: bytes) -> float:
    a = array.array("h")
    a.frombytes(payload)
    if not a:
        return -140.0
    acc = sum((v / 32768.0) ** 2 for v in a)
    r = (acc / len(a)) ** 0.5
    return -140.0 if r < 1e-7 else 20.0 * math.log10(r)


def parse_results(buf):
    """Extract complete frames from the receive buffer; returns (frames, rest)."""
    frames = []
    while len(buf) >= HEADER.size:
        magic, ver, type_, flags, seq, t_us, plen = HEADER.unpack(buf[:HEADER.size])
        if magic != MAGIC or ver != VERSION or plen > 1 << 20:
            raise ValueError("engine sent a bad frame — protocol mismatch?")
        if len(buf) < HEADER.size + plen:
            break
        frames.append({"type": type_, "flags": flags, "seq": seq, "t_us": t_us,
                       "payload": buf[HEADER.size:HEADER.size + plen]})
        buf = buf[HEADER.size + plen:]
    return frames, buf


def post_to_backend(base_url, session_id, seq, transcript_obj, audio_t_us):
    body = json.dumps({
        "session_id": session_id,
        "seq": seq,
        "received_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "audio_t_us": audio_t_us,
        "transcript": transcript_obj,
    }, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(base_url.rstrip("/") + f"/api/sessions/{session_id}/transcripts",
                                 data=body, headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=3).read()
    except Exception as e:  # backend optional for local tests
        print(f"[feed-wav] backend POST failed: {e}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("wav", nargs="+", help="WAV file(s) to stream")
    ap.add_argument("--socket", default=None,
                    help="engine.sock path (default: $ASTK_ENGINE_SOCKET or /run/audiostack/engine.sock)")
    ap.add_argument("--chunk-ms", type=int, default=20)
    ap.add_argument("--speed", type=float, default=1.0, help="real-time pacing factor")
    ap.add_argument("--no-realtime", action="store_true", help="send as fast as possible")
    ap.add_argument("--loop", action="store_true", help="repeat the file(s) forever")
    ap.add_argument("--gate-db", type=float, default=-45.0, help="silence flag threshold")
    ap.add_argument("--backend-url", default="", help="optional: forward RESULTs to this base URL")
    args = ap.parse_args()

    path = args.socket or os.environ.get("ASTK_ENGINE_SOCKET", "/run/audiostack/engine.sock")

    files = []
    for p in args.wav:
        try:
            pcm = read_wav_mono16(p)
        except (WavError, OSError) as e:
            print(f"[feed-wav] {e}", file=sys.stderr)
            return 1
        dur = len(pcm) / 2 / TARGET_RATE
        files.append((p, pcm))
        print(f"[feed-wav] loaded {p}: {dur:.2f} s @16k mono s16")

    chunk_bytes = TARGET_RATE * 2 * args.chunk_ms // 1000

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    try:
        os.unlink(path)  # stale socket from a previous run
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    os.chmod(path, 0o660)
    srv.listen(1)
    print(f"[feed-wav] engine.sock ready at {path} — start the engine now")

    seq = 0
    try:
        while True:
            conn, _ = srv.accept()
            print("[feed-wav] engine connected", flush=True)
            conn.setblocking(False)
            c = AudioStackClient.__new__(AudioStackClient)
            c._sock, c._buf, c._seq = conn, b"", 0
            rbuf = b""

            def drain():
                nonlocal rbuf
                while True:
                    if not select.select([conn], [], [], 0)[0]:
                        return
                    data = conn.recv(65536)
                    if not data:
                        return
                    rbuf += data
                    frames, rbuf = parse_results(rbuf)
                    for fr in frames:
                        if fr["type"] == MsgType.RESULT:
                            obj = json.loads(fr["payload"].decode("utf-8"))
                            seq += 1
                            print(f"[feed-wav] RESULT #{seq}: {obj.get('type')} "
                                  f"\"{obj.get('text','')}\"", flush=True)
                            if args.backend_url:
                                post_to_backend(args.backend_url, session_id, seq,
                                                obj, fr["t_us"])
                        elif fr["type"] == MsgType.ENGINE_STATUS:
                            print("[feed-wav] engine status:",
                                  json.loads(fr["payload"].decode("utf-8")), flush=True)

            session_id = "sess-feed-" + time.strftime("%Y%m%d-%H%M%S")
            c.send_json(MsgType.HELLO, json.dumps({
                "proto": 1, "name": "feed_wav", "version": "0.1.0",
                "format": {"rate": TARGET_RATE, "channels": 1, "encoding": "s16le"},
                "chunk_ms": args.chunk_ms, "cache_seconds": 0,
                "capture_device": "file:" + ";".join(p for p, _ in files),
                "session_id": session_id,
            }))
            c.send_json(MsgType.SESSION_BEGIN, json.dumps({"session_id": session_id}))

            looping = True
            while looping:
                for p, pcm in files:
                    print(f"[feed-wav] streaming {p} …", flush=True)
                    deadline = time.perf_counter()
                    t_base = int(time.monotonic() * 1e6)
                    for i in range(0, len(pcm), chunk_bytes):
                        chunk = pcm[i:i + chunk_bytes]
                        flags = Flags.SILENCE if rms_db(chunk) < args.gate_db else 0
                        c.send_frame(MsgType.AUDIO, chunk, flags)
                        if not args.no_realtime:
                            deadline += args.chunk_ms / 1000.0 / args.speed
                            time.sleep(max(0.0, deadline - time.perf_counter()))
                        drain()
                    drain()
                    if not args.loop:
                        looping = False
                if not args.loop:
                    break

            c.send_json(MsgType.SESSION_END, json.dumps({"session_id": session_id}))
            c.send_frame(MsgType.GOODBYE, b'{"reason":"file done"}')
            time.sleep(0.2)  # let the engine read the last frames
            conn.close()
            if not args.loop:
                print("[feed-wav] done")
                break
    except KeyboardInterrupt:
        print("\n[feed-wav] interrupted")
    finally:
        srv.close()
        try:
            os.unlink(path)
        except OSError:
            pass


if __name__ == "__main__":
    main()
