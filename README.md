# audio-stack

Linux audio capture stack for the on-device Speech Transcriber project.
The board target is **RUBIK Pi 3 (QCS6490), running Ubuntu for Qualcomm**.
Fedora x86_64 remains a host development environment.

The stack has USB input profiles for the **ReSpeaker XVF3800 4-Mic Array**.
The default config still uses a compatible microphone/headset on the board's
3.5 mm jack. Until the hardware is available, use the software input below.

The stack converts capture audio to **16 kHz / mono / S16_LE PCM**, sends it to
the STT engine, and forwards transcripts to the web backend.

```text
XVF3800 or 3.5 mm mic -> ALSA -> audiostackd -> mono PCM -> STT engine
                                       ^                  |
                                       +--- RESULT JSON --+
                                       |
                                       +-> HTTP backend -> SSE -> web app

WAV file -> tools/feed_wav.py -> same engine protocol -> HTTP backend
WAV file -> ALSA snd-aloop -> audiostackd -> same engine protocol
```

## Build on Ubuntu for Qualcomm

Install the board's Ubuntu for Qualcomm image before running these commands.
The setup script installs packages; it does not flash the board or install QNN.

```bash
bash scripts/setup_rubikpi.sh
bash scripts/build.sh
```

For a Fedora development host, use `bash scripts/setup_fedora.sh` instead.

For XVF3800 bring-up and optional RNNoise, follow the
[XVF3800 setup and comparison guide](docs/XVF3800.md). It covers the USB
firmware profiles, selecting a processed beam, and recording all raw routes.

## Software input without a microphone

This is the first integration path while microphone hardware is unavailable.
It needs Python 3, an AF_UNIX-capable OS and Node.js for the optional backend.
No ALSA device, sound card or kernel loopback module is needed by the WAV feeder.

Run each command in a separate terminal, from the repository root:

```bash
# Terminal 1: stand-in web backend and SSE demo
node tests/dummy_backend.js 3000

# Terminal 2: serve WAV audio; waits for the engine to connect
python3 tools/feed_wav.py samples/jfk.wav \
  --socket /tmp/astk-file/engine.sock --backend-url http://127.0.0.1:3000

# Terminal 3: receive audio and send fake transcripts
python3 tests/dummy_engine.py --socket /tmp/astk-file/engine.sock
```

Open `http://localhost:3000/` before starting terminal 3 to see live events.
`GET /api/sessions` contains the session, transcripts and end time after the run.
The fake transcript confirms transport, not speech recognition accuracy.
Use a separate socket for the feeder and daemon; only one server may own a path.

To use the actual PyTorch Whisper example instead of the dummy engine, install
its dependencies in a virtual environment (`python3 -m venv .venv`, activate it,
then `python -m pip install openai-whisper`) and run:

```bash
python3 client/whisper_engine.py --socket /tmp/astk-file/engine.sock \
  --model small --language en
# Direct file transcription, without the ASTK transport:
python3 client/whisper_engine.py --wav samples/jfk.wav --model small --language en
```

To include the **real C++ daemon and ALSA capture path**, use a Linux kernel with
`snd-aloop` available. Start the backend and engine in other terminals first:

```bash
python3 tests/dummy_engine.py --socket /tmp/astk-loopback/engine.sock
bash scripts/feed_wav_loopback.sh samples/jfk.wav -- \
  --engine-socket /tmp/astk-loopback/engine.sock
```

If the board kernel does not provide `snd-aloop`, use the Python WAV feeder.
See [Ubuntu and input setup](docs/UBUNTU-INPUT.md) for the test boundaries.

## 3.5 mm microphone input on the board

The supplied board configuration uses `plughw:0,2`, 48 kHz, one channel. This is
the official audio guide's example with the onboard card numbered 0. Verify
the actual card, capture PCM and mixer control on the installed Ubuntu image.

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
arecord -l
./build/astk-list-devices

# Example when the onboard card is card 0:
amixer -c 0 cset "name=MultiMedia3 Mixer PRIMARY_MI2S_TX" 1
bash scripts/audio_probe.sh --device plughw:0,2

./build/audiostackd --config config/audiostack.conf \
  --engine-socket /tmp/astk-headset/engine.sock
# Another terminal:
python3 tests/dummy_engine.py --socket /tmp/astk-headset/engine.sock
```

The daemon resamples 48 kHz input to 16 kHz. `plughw` may also convert formats;
a successful 16 kHz `arecord` call does not prove native hardware support.
For a different card number, change `[capture] device` or pass `--device`.
On a desktop host, select its actual capture PCM or explicitly use
`--device default`. If PulseAudio or PipeWire owns the device, see the input
guide before using direct ALSA capture.

## Team integration and deployment

- **Engine**: `client/astk_client.py` receives AUDIO and sends
  `send_result(text, kind="partial"|"final")`.
  See [engine integration](docs/INTEGRATION-ENGINE.md) and [protocol](docs/PROTOCOL.md).
- **Web backend**: receive session start, transcript and end POSTs, then relay
  results over SSE. See [web integration](docs/INTEGRATION-WEB.md).
  These routes are the audio stack's proposed ingest contract; align them with
  the application backend's session manager and STT adapter before integration.
- **Board service**: verify the selected capture profile in the foreground first, update
  `/etc/audiostack/audiostack.conf`, then run `sudo bash scripts/install_service.sh`.
  Logs: `journalctl -u audiostack -f`. State: `./build/astk-ctl status`.
  The service does not configure mixer routing or start the engine/backend.

## Layout

```text
include/audiostack/  C++ capture, format conversion, IPC, ring and HTTP headers
src/                C++ library and audiostackd
tools/              device listing, WAV capture, control and WAV feeder
client/             protocol client, WAV loader and PyTorch Whisper example
tests/              dummy engine/backend and software-input integration test
samples/            speech and synthetic test WAVs
config/             board capture configuration and loopback override
scripts/            Ubuntu/Fedora setup, build, probe, install and loopback test
systemd/            service unit
docs/               Ubuntu/input setup and engine/web/protocol contracts
dts/                experimental I2S overlay, outside the current input baseline
```

Run the hardware-free integration test with:

```bash
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

IPC defaults to `/run/audiostack/`. Use an explicit writable socket path for
unprivileged tests. The optional shared-memory ring holds about 30 seconds.
Capture XRUNs are recovered automatically; dropped audio and backend queue
counts are visible through `astk-ctl status`. Buffers are memory-only and finite.
