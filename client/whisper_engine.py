#!/usr/bin/env python3
"""
whisper_engine — a working STT engine example on top of the audio stack.

Framework: PyTorch Whisper (openai-whisper).
    pip install -U openai-whisper        # pulls torch/numpy

Two modes:

  1) OFFLINE — no daemon, no socket, no mic. Just check that Whisper itself
     works on a WAV file:
         python3 client/whisper_engine.py --wav samples/jfk.wav --model small

  2) ONLINE — full-stack: consume ASTK AUDIO frames from engine.sock
     (real audiostackd, or tools/feed_wav.py as the mic stand-in), run an
     energy VAD, transcribe each utterance, and send partial/final results:
         python3 client/whisper_engine.py --model small --language ko

The transport stays identical when you later swap the VAD for Silero or the
recognizer for the QNN/NPU runtime.
"""
import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from astk_client import AudioStackClient, MsgType, Flags  # noqa: E402

TARGET_RATE = 16000


def load_model(name):
    try:
        import whisper  # openai-whisper (PyTorch)
    except ImportError:
        sys.exit("openai-whisper is not installed:  pip install -U openai-whisper")
    print(f"[whisper-engine] loading model '{name}' …", flush=True)
    return whisper.load_model(name)


def transcribe(model, float_mono, language):
    res = model.transcribe(float_mono, language=language or None, fp16=False)
    text = res["text"].strip()
    return text, res


# ---------------------------------------------------------------- offline ---
def offline(args):
    import numpy as np
    from wav_io import read_wav_mono16
    data = read_wav_mono16(args.wav)
    audio = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
    model = load_model(args.model)
    t0 = time.perf_counter()
    text, res = transcribe(model, audio, args.language)
    dt = time.perf_counter() - t0
    print(f"\nfull text: {text}")
    print("\nsegments:")
    for seg in res.get("segments", []):
        a, b = seg["timestamp"]
        print(f"  [{a:6.2f} -> {b:6.2f}] {seg['text'].strip()}")
    print(f"\n({len(audio)/TARGET_RATE:.2f} s of audio transcribed in {dt:.2f} s)")


# ------------------------------------------------------------------- VAD ----
class EnergyVad:
    """Buffer + gate: start a segment after min_speech_ms voiced, end it after
    silence_tail_ms of silence (or max_seg_s). 300 ms of pre-roll is kept."""

    def __init__(self, thr_db=-40.0, min_speech_ms=300, silence_tail_ms=900, max_seg_s=30.0):
        self.thr_db, self.min_speech_ms = thr_db, min_speech_ms
        self.silence_tail_ms, self.max_seg_s = silence_tail_ms, max_seg_s
        self.chunks = []          # list of np.int16 arrays
        self.state = "idle"
        self.speech_ms = self.silence_ms = 0
        self.seg_start_idx = 0
        self.seg_t_start_us = None
        self.last_t_us = None

    def reset(self):
        self.chunks, self.state = [], "idle"
        self.speech_ms = self.silence_ms = 0
        self.seg_start_idx = 0
        self.seg_t_start_us = self.last_t_us = None

    @staticmethod
    def db(x):
        if len(x) == 0:
            return -140.0
        r = math.sqrt(sum((float(v) / 32768.0) ** 2 for v in x) / len(x))
        return -140.0 if r < 1e-7 else 20.0 * math.log10(r)

    def feed(self, x, t_us, flags):
        """Returns True when a segment just completed (data stays in buffers
        until pop_segment())."""
        import numpy as np
        dur_ms = len(x) * 1000 // TARGET_RATE
        voiced = not (flags & Flags.SILENCE) and self.db(x) > self.thr_db
        self.chunks.append(x)
        self.last_t_us = t_us

        if self.state == "idle":
            self.speech_ms = self.speech_ms + dur_ms if voiced else 0
            if voiced and self.speech_ms >= self.min_speech_ms:
                self.state = "speaking"
                # keep ~300 ms pre-roll
                self.seg_start_idx = max(0, len(self.chunks) - (300 // max(1, dur_ms)))
                first = self.chunks[self.seg_start_idx]
                self.seg_t_start_us = t_us - (len(self.chunks) - self.seg_start_idx) * len(first) * 1e6 / TARGET_RATE
            else:
                if len(self.chunks) > 400:  # cap idle buffer (20 ms chunks ≈ 8 s)
                    self.chunks = self.chunks[-200:]
            return False

        # speaking
        self.silence_ms = 0 if voiced else self.silence_ms + dur_ms
        seg_samples = sum(len(c) for c in self.chunks[self.seg_start_idx:])
        if self.silence_ms >= self.silence_tail_ms or seg_samples >= self.max_seg_s * TARGET_RATE:
            return True
        return False

    def speaking(self):
        return self.state == "speaking"

    def pop_segment(self):
        import numpy as np
        seg = np.concatenate(self.chunks[self.seg_start_idx:]) if self.chunks else np.zeros(0, np.int16)
        out = (seg.astype(np.float32) / 32768.0, self.seg_t_start_us, self.last_t_us)
        self.reset()
        return out


# ----------------------------------------------------------------- online ---
def online(args):
    import numpy as np
    c = AudioStackClient(args.socket)
    print(f"[whisper-engine] connecting to {c.path} … (start audiostackd or "
          f"tools/feed_wav.py first)", flush=True)
    c.connect()
    model = load_model(args.model)
    vad = EnergyVad(args.threshold_db, args.min_speech_ms, args.silence_tail_ms, args.max_seg_s)
    c.send_status(model=args.model, framework="pytorch-whisper")

    base_t_us = None
    last_partial = 0.0
    n_final = 0

    for f in c.frames():
        if f.type == MsgType.HELLO:
            print("[whisper-engine] HELLO:", f.json(), flush=True)
        elif f.type == MsgType.SESSION_BEGIN:
            vad.reset()
            base_t_us = None
            print("[whisper-engine] session begin:", f.json(), flush=True)
        elif f.type == MsgType.AUDIO:
            x = np.frombuffer(f.payload, dtype=np.int16)
            if len(x) == 0:
                continue
            if base_t_us is None:
                base_t_us = f.t_us
            done = vad.feed(x, f.t_us, f.flags)

            if args.partial_every > 0 and vad.speaking() and \
                    time.monotonic() - last_partial >= args.partial_every:
                last_partial = time.monotonic()
                import numpy as _np
                cur = _np.concatenate(vad.chunks[vad.seg_start_idx:]).astype(_np.float32) / 32768.0
                text, _ = transcribe(model, cur, args.language)
                if text:
                    c.send_result(text, kind="partial",
                                  t_start_ms=int((vad.seg_t_start_us - base_t_us) / 1000))

            if done:
                audio, t0, t1 = vad.pop_segment()
                if len(audio) >= TARGET_RATE * 0.2:  # ignore <200 ms blips
                    text, _ = transcribe(model, audio, args.language)
                    if text:
                        n_final += 1
                        c.send_result(text, kind="final",
                                      t_start_ms=int((t0 - base_t_us) / 1000),
                                      t_end_ms=int((t1 - base_t_us) / 1000))
                        print(f"[whisper-engine] FINAL #{n_final}: {text}", flush=True)
        elif f.type == MsgType.SESSION_END:
            if vad.speaking():
                audio, t0, t1 = vad.pop_segment()
                text, _ = transcribe(model, audio, args.language)
                if text:
                    c.send_result(text, kind="final",
                                  t_start_ms=int((t0 - base_t_us) / 1000),
                                  t_end_ms=int((t1 - base_t_us) / 1000))
            print("[whisper-engine] session end", flush=True)
        elif f.type == MsgType.GOODBYE:
            print("[whisper-engine] daemon said goodbye", flush=True)
            return


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--wav", help="offline mode: transcribe this file and exit")
    ap.add_argument("--socket", default=None, help="engine.sock path (online mode)")
    ap.add_argument("--model", default="small", help="tiny|base|small|medium (or path)")
    ap.add_argument("--language", default=None, help="e.g. ko / en (default: auto-detect)")
    ap.add_argument("--threshold-db", type=float, default=-40.0)
    ap.add_argument("--min-speech-ms", type=int, default=300)
    ap.add_argument("--silence-tail-ms", type=int, default=900)
    ap.add_argument("--max-seg-s", type=float, default=30.0)
    ap.add_argument("--partial-every", type=float, default=0.0,
                    help="emit partial transcripts every N seconds during speech (0=off)")
    args = ap.parse_args()

    if args.wav:
        offline(args)
    else:
        online(args)


if __name__ == "__main__":
    main()
