# Engine integration guide — Whisper ↔ audio stack

Goal: the daemon handles everything from the microphone to ready-to-infer
PCM. The engine only consumes 16 kHz mono s16le PCM and feeds it into
whisper.cpp / Qualcomm VoiceAI ASR (QNN), then sends results back.

## 1. Quick start (Python)

```python
import sys
sys.path.insert(0, "client")
from astk_client import AudioStackClient, MsgType

c = AudioStackClient()          # /run/audiostack/engine.sock (override with env ASTK_ENGINE_SOCKET)
c.connect()

for f in c.frames():            # auto-PONG, retries included
    if f.type == MsgType.HELLO:
        meta = f.json()         # {"format": {"rate":16000, "encoding":"s16le"}, ...}
    elif f.type == MsgType.AUDIO:
        handle_pcm(f.payload, flags=f.flags)   # 20 ms = 640 bytes
    elif f.type == MsgType.SESSION_END:
        flush_and_finalize()
```

Inside `handle_pcm`:

```python
import numpy as np

def handle_pcm(data: bytes, flags: int):
    x = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
    vad_buffer.append(x)          # e.g. a 30-second ring (as in the pipeline notes)
```

Sending results:

```python
c.send_result("Hello.", kind="partial")            # provisional subtitle
c.send_result("Hello there.", kind="final")        # confirmed → to be stored
```

## 2. Sentence-level near-real-time strategy

Hints provided by the daemon:
- `AUDIO` frames = 20 ms chunks (group 1–2 for the Silero VAD 512-sample window)
- `flags & 0x01 (silence)`: energy-gate result — first-stage skip filter
  ("only run inference when there was speech in the last 3 s")
- `t_us`: CLOCK_MONOTONIC capture time — baseline for latency measurement
- `SESSION_BEGIN/END`: recording sessions rotated by the daemon (button/web triggers)

Recommended flow (sentence-level near-real-time):

```
AUDIO(20ms) ─→ Silero VAD (ONNX, 1 thread)
   speech start → accumulate buffer
   ~1 s silence after speech end (keep 300 ms of the leading edge) → segment final
   → whisper_full(30 s padded) → send_result(final)
   during speech, every 0.6–1 s → send_result(partial)
```

Hallucination guards (repeat trimming, Korean-specific rules) and text
stitching (character-level edit distance) are the engine's responsibility —
the daemon does not touch them.

## 3. whisper.cpp (CPU path)

- `whisper_full` expects 16 kHz mono float32 → the conversion above is all it needs
- Do **not** use `examples/stream`: it opens the microphone itself via SDL2 and
  would conflict with the daemon. Feed PCM directly instead.
- Skeleton with the same transport/VAD structure: `tests/dummy_engine.py`

## 4. NPU path (Qualcomm VoiceAI ASR / QNN)

- AI Hub pre-converted Whisper (small/base) uses fixed 30 s inputs — the daemon
  streams 20 ms chunks; fill a 30 s window in the engine and call the model
  (update floor ~0.6 s per the encoder benchmark).
- QNN FastRPC concerns only CDSP access; audio input stays identical → same protocol.

## 5. Shared-memory ring (ultra-low-latency option)

With `[engine] transport = shm`, read the 30 s ring directly:

```python
from astk_client import ShmRingReader
r = ShmRingReader("/audiostack_ring")
r.seek_to_now()
while True:
    data = r.read(3200)   # 100 ms
```

Cheaper "last N seconds" access for sliding-window modes. RESULTs still go
over the socket so backend forwarding keeps working.

## 6. Latency measurement points

daemon `AUDIO.t_us` (capture) → engine inference → `send_result` → backend
`received_at`. Compute end-of-speech → result latency together with CER.
