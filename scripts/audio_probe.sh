#!/usr/bin/env bash
# ============================================================================
# audio_probe.sh — check mic hardware / kernel drivers / latency basics.
# usage: ./scripts/audio_probe.sh [--device plughw:1,0]
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

DEVICE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

echo "=== 1) kernel sound cards (/proc/asound/cards) ==="
cat /proc/asound/cards
echo
echo "=== 2) PCM streams (/proc/asound/pcm) ==="
cat /proc/asound/pcm
echo
echo "=== 3) USB audio devices (lsusb) ==="
command -v lsusb >/dev/null && lsusb -v 2>/dev/null | grep -E "^Bus|idVendor|idProduct|iProduct" | head -40 || echo "lsusb not installed"
echo
echo "=== 4) audio server occupancy ==="
if pgrep -x pipewire >/dev/null; then
  echo "⚠ PipeWire is running — the daemon opening hw:/plughw: directly may conflict."
  echo "  option A (recommended, low latency): systemctl --user stop pipewire pipewire-pulse"
  echo "  option B: set [capture] device = default in audiostack.conf (via PipeWire)"
elif pgrep -x pulseaudio >/dev/null; then
  echo "⚠ PulseAudio is running — the survey decided against it; run the daemon alone:"
  echo "  systemctl --user stop pulseaudio"
else
  echo "no audio server running — direct ALSA use is fine"
fi
echo
echo "=== 5) device selection ==="
if [[ -z "$DEVICE" ]]; then
  if [[ -x build/astk-list-devices ]]; then
    build/astk-list-devices || true
  fi
  echo "re-run with an explicit device: ./scripts/audio_probe.sh --device plughw:<c>,<d>"
  exit 0
fi
echo "test capture: $DEVICE (16 kHz mono S16_LE, 3 s)"
TMP=/tmp/astk_probe_$$.wav
if arecord -D "$DEVICE" -f S16_LE -r 16000 -c 1 -d 3 -t wav "$TMP" 2>/tmp/astk_probe_err; then
  echo "  OK → $TMP"
  echo "  native 16 kHz confirmed (e.g. ReSpeaker v2.0)"
else
  echo "  16 kHz failed — retrying at 48 kHz:"
  if arecord -D "$DEVICE" -f S16_LE -r 48000 -c 1 -d 3 -t wav "$TMP" 2>>/tmp/astk_probe_err; then
    echo "  OK → $TMP (daemon will capture at 48k and resample to 16k in software)"
  else
    echo "  capture failed:"; cat /tmp/astk_probe_err
    echo "  check: audio group membership, amixer routing (on-board mic), device connection"
    exit 1
  fi
fi
echo
echo "=== 6) daemon pipeline check (if built) ==="
if [[ -x build/astk-capture-wav ]]; then
  build/astk-capture-wav --device "$DEVICE" --seconds 3 --out /tmp/astk_pipe.wav \
    && echo "  OK → /tmp/astk_pipe.wav (16k mono, 50 Hz HP, resampling included)"
fi
