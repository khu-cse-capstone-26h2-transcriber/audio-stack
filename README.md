# audio-stack

Linux audio capture stack for the on-device Speech Transcriber project.
Works on **Rubik Pi 3 (QCS6490, Qualcomm Linux/Debian)** and builds unchanged
on **Fedora 44 (x86_64)**.

It captures the microphone, converts to **16 kHz / mono / S16_LE PCM** (the
Whisper input format), streams it to the STT engine, and forwards the engine's
transcripts to the web backend.

```
mic (ALSA) → audiostackd → PCM frames → Whisper engine (client/astk_client.py)
                ↑                [transcript JSON] ↓
                └── HTTP POST → Node/Express backend → SSE → React
```

## Build

```bash
# Rubik Pi 3
sudo ./scripts/setup_rubikpi.sh
# Fedora 44
sudo ./scripts/setup_fedora.sh

./scripts/build.sh          # → build/audiostackd, build/astk-*
```

## Test it in 30 seconds (no engine/backend needed yet)

```bash
./scripts/audio_probe.sh --device plughw:1,0   # check mic + 16k/48k support

node tests/dummy_backend.js 3000 &             # backend stand-in (SSE demo page)
./build/audiostackd --device plughw:1,0 &      # the stack itself
python3 tests/dummy_engine.py &                # engine stand-in (fake transcripts)

# open http://localhost:3000/ and talk → fake transcripts appear live
./build/astk-ctl status                        # daemon state
```

### No mic? Use a WAV file instead

```bash
# 1) sanity-check PyTorch Whisper itself on a real speech sample (no socket):
python3 client/whisper_engine.py --wav samples/jfk.wav --model small

# 2) full flow without hardware — feed_wav.py streams the file over engine.sock
#    exactly like the daemon would (start the engine first):
python3 client/whisper_engine.py --model small --language en &
python3 tools/feed_wav.py samples/jfk.wav --backend-url http://127.0.0.1:3000

# 3) test the REAL C++ daemon without a mic (snd-aloop loopback card):
./scripts/feed_wav_loopback.sh samples/jfk.wav
```

## Choosing the capture device

| setup | how |
|---|---|
| USB mic (ReSpeaker Mic Array v2.0) | `--device plughw:1,0` — native 16 kHz, no resampling |
| Rubik Pi 3 on-board 3.5 mm mic | `amixer -c 0 cset "name=MultiMedia3 Mixer PRIMARY_MI2S_TX" 1` then `--device plughw:0,2` (48 kHz → auto-resampled to 16 kHz) |
| Fedora laptop/desktop | stop PipeWire (`systemctl --user stop pipewire pipewire-pulse`) or use `--device default` |

Run `./build/astk-list-devices` to see all cards. Config lives in
`config/audiostack.conf` (→ `/etc/audiostack/audiostack.conf` when installed).

## For team members

- **Engine (Whisper)**: connect with `client/astk_client.py`, receive 20 ms
  AUDIO frames, reply with `send_result(text, kind="partial"|"final")`.
  → `docs/INTEGRATION-ENGINE.md`, `docs/PROTOCOL.md`
- **Web backend**: the daemon POSTs to 3 routes (`/api/sessions`,
  `/api/sessions/{id}/transcripts`, `/api/sessions/{id}/end`); relay the body
  to SSE. → `docs/INTEGRATION-WEB.md`
- **Deploy on the board**: `sudo ./scripts/install_service.sh`
  (systemd, auto-restart), logs: `journalctl -u audiostack -f`

## Layout

```
include/audiostack/  C++ library headers (ALSA capture, IPC, shm ring, HTTP)
src/                 library implementation + audiostackd daemon
tools/               C++ helpers (list-devices, capture-wav, ctl) + feed_wav.py
client/              astk_client.py (protocol client), whisper_engine.py (PyTorch
                     Whisper example engine), wav_io.py
tests/               dummy_engine.py, dummy_backend.js (integration stand-ins)
samples/             jfk.wav (real speech), synth_speech.wav (VAD test)
scripts/             setup/build/probe/install + feed_wav_loopback.sh + udev rules
systemd/             audiostack.service
docs/                PROTOCOL.md, INTEGRATION-ENGINE.md, INTEGRATION-WEB.md
dts/                 INMP441 I2S overlay template (experimental)
```

## Notes

- IPC defaults: AF_UNIX sockets under `/run/audiostack/`
  (`engine.sock`, `ctrl.sock`); optional zero-copy path: `transport = shm`
  (30 s ring, see PROTOCOL.md §2).
- Backend delivery is plain HTTP (loopback/LAN). For remote TLS, put a reverse
  proxy in front of the backend and point `base_url` at it.
- XRUNs are recovered automatically (`snd_pcm_recover`); counters via `astk-ctl status`.
