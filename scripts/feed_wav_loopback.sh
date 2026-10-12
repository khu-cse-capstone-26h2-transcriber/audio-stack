#!/usr/bin/env bash
# ============================================================================
# feed_wav_loopback.sh — test the REAL audiostackd without a microphone using
# the snd-aloop virtual loopback sound card (any Linux; shown here for Fedora).
#
#   aplay(file.wav) → plughw:CARD,0 ──loopback── plughw:CARD,1 → audiostackd
#
# usage: ./scripts/feed_wav_loopback.sh [wavfile] [--daemon-args "<extra>"]
# then connect the engine as usual: python3 client/whisper_engine.py
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

WAV="${1:-samples/jfk.wav}"
shift || true
DAEMON_ARGS=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --daemon-args) DAEMON_ARGS="$2"; shift 2 ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

[[ -x build/audiostackd ]] || { echo "build first: ./scripts/build.sh"; exit 1; }

if ! grep -qi loopback /proc/asound/cards; then
  echo "[loopback] loading snd-aloop module…"
  sudo modprobe snd-aloop
fi
CARD=$(grep -i -m1 loopback /proc/asound/cards | awk '{print $1}')
[[ -n "$CARD" ]] || { echo "[loopback] loopback card not found"; exit 1; }

echo "[loopback] card=$CARD  play=plughw:$CARD,0  capture=plughw:$CARD,1"
# shellcheck disable=SC2086
./build/audiostackd --device "plughw:$CARD,1" $DAEMON_ARGS &
DP=$!
trap 'kill $DP 2>/dev/null || true' EXIT
sleep 1

aplay -D "plughw:$CARD,0" "$WAV"
sleep 1
echo "[loopback] done — frames went through the real daemon pipeline."
echo "           engine sees them on engine.sock; daemon stats: ./build/astk-ctl status"
