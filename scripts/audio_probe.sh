#!/usr/bin/env bash
# Diagnose the RUBIK Pi 3 3.5 mm headset input on Ubuntu for Qualcomm.
# Usage: bash scripts/audio_probe.sh [--device plughw:0,2]
# Omitting --device lists devices without starting a recording or changing routing.
set -euo pipefail
cd "$(dirname "$0")/.."

DEVICE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      [[ $# -ge 2 && -n $2 ]] || { echo "--device needs an ALSA PCM" >&2; exit 2; }
      DEVICE="$2"; shift 2 ;;
    --help) echo "usage: $0 [--device plughw:0,2]"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

echo "=== Sound cards and capture PCMs ==="
if [[ -r /proc/asound/cards ]]; then cat /proc/asound/cards; fi
if [[ -r /proc/asound/pcm ]]; then cat /proc/asound/pcm; fi
if command -v arecord >/dev/null; then arecord -l || true; fi
if [[ -x build/astk-list-devices ]]; then build/astk-list-devices || true; fi

echo "=== Audio server occupancy ==="
if pgrep -x pipewire >/dev/null || pgrep -x pulseaudio >/dev/null; then
  echo "A sound server is running. Direct ALSA may be busy; inspect fuser /dev/snd/*."
  echo "See docs/UBUNTU-INPUT.md before stopping a server or choosing a bridge PCM."
else
  echo "No PulseAudio/PipeWire process detected; device availability still needs testing."
fi

if [[ -z "$DEVICE" ]]; then
  echo "Connect a microphone/headset to the 3.5 mm jack and configure its mixer route."
  echo "Then run: $0 --device plughw:0,2 (adjust the card and PCM to your image)."
  echo "Without a microphone: use tools/feed_wav.py; see README.md."
  exit 0
fi
command -v arecord >/dev/null || { echo "install alsa-utils first" >&2; exit 1; }

PROBE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/astk-probe.XXXXXX")
echo "=== Capture $DEVICE: mono S16_LE, 3 seconds ==="
# The board profile prefers 48 kHz, then resamples in the daemon.
CAPTURED=0
for RATE in 48000 16000; do
  if arecord -D "$DEVICE" -f S16_LE -r "$RATE" -c 1 -d 3 -t wav \
      "$PROBE_DIR/input.wav" 2>"$PROBE_DIR/capture.err"; then
    echo "Capture succeeded at requested rate $RATE: $PROBE_DIR/input.wav"
    echo "plughw may convert the rate; this does not establish native hardware support."
    CAPTURED=1
    break
  fi
done
if [[ $CAPTURED -eq 0 ]]; then
  cat "$PROBE_DIR/capture.err" >&2
  echo "Check 3.5 mm microphone compatibility, mixer routing, permissions and device ownership." >&2
  exit 1
fi
if [[ -x build/astk-capture-wav ]]; then
  build/astk-capture-wav --device "$DEVICE" --seconds 3 --out "$PROBE_DIR/pipeline.wav"
  echo "Pipeline output (16 kHz mono, high-pass and resampling): $PROBE_DIR/pipeline.wav"
fi
