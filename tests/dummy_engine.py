#!/usr/bin/env python3
"""
dummy_engine — end-to-end validation of the whole audio stack before the
real Whisper engine is ready.

- connects to engine.sock and receives AUDIO frames
- finds utterances with a simple energy gate (VAD stand-in)
- sends a "fake transcript" RESULT frame back to the daemon when an
  utterance ends (which then flows to the Node backend POST -> SSE)

When replacing this with the real engine, use client/astk_client.py and
follow docs/INTEGRATION-ENGINE.md instead.

usage: python3 tests/dummy_engine.py [--socket /run/audiostack/engine.sock]
"""
import argparse
import math
import os
import array
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client"))
from astk_client import AudioStackClient, MsgType  # noqa: E402


def rms_db(payload: bytes) -> float:
    a = array.array("h")
    a.frombytes(payload)
    if not a:
        return -140.0
    acc = sum((v / 32768.0) ** 2 for v in a)
    r = (acc / len(a)) ** 0.5
    return -140.0 if r < 1e-7 else 20.0 * math.log10(r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", default=None)
    ap.add_argument("--threshold-db", type=float, default=-40.0)
    ap.add_argument("--silence-ms", type=int, default=900,
                    help="utterance ends after this much silence")
    args = ap.parse_args()

    c = AudioStackClient(args.socket)
    print("[dummy-engine] waiting for engine.sock…", flush=True)
    c.connect(wait=True)
    print("[dummy-engine] connected. receiving audio.", flush=True)

    speech_bytes = 0
    silence_ms = 0
    utter = 0
    for f in c.frames():
        if f.type == MsgType.HELLO:
            print("[dummy-engine] HELLO:", f.json(), flush=True)
            c.send_status(model="dummy-vad", loaded=True)
        elif f.type == MsgType.SESSION_BEGIN:
            print("[dummy-engine] SESSION_BEGIN:", f.json(), flush=True)
        elif f.type == MsgType.AUDIO:
            dur_ms = len(f.payload) // 32  # 16k * s16 = 32 bytes/ms
            db = rms_db(f.payload)
            if db > args.threshold_db:
                speech_bytes += len(f.payload)
                silence_ms = 0
            else:
                silence_ms += dur_ms
            if speech_bytes and silence_ms >= args.silence_ms:
                utter += 1
                secs = speech_bytes / 2 / 16000
                text = (f"[dummy transcript {utter}] utterance {secs:.1f}s detected "
                        f"— real Whisper text replaces this")
                c.send_result(text, kind="final",
                              t_start_ms=0, t_end_ms=int(secs * 1000), conf=0.0)
                print(f"[dummy-engine] RESULT #{utter} ({secs:.1f}s) -> backend", flush=True)
                speech_bytes = 0
        elif f.type == MsgType.HEARTBEAT:
            pass
        elif f.type in (MsgType.SESSION_END, MsgType.GOODBYE):
            print("[dummy-engine] session ended:", f.json(), flush=True)
            speech_bytes = 0


if __name__ == "__main__":
    main()
