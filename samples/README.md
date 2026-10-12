# Sample audio files

- `jfk.wav` — from the whisper.cpp repository (MIT license). 16 kHz / mono /
  s16, 11 s. Expected text (en):
  *"And so my fellow Americans ask not what your country can do for you, ask
  what you can do for your country."*
- `synth_speech.wav` — synthetic voiced bursts (generated with this repo's
  settings). Not real speech: for VAD/streaming/plumbing tests only.

For Korean tests, drop in any WAV and convert if needed:

```bash
ffmpeg -i input.mp3 -ar 16000 -ac 1 -c:a pcm_s16le samples/ko_test.wav
```
