#!/usr/bin/env bash
# Feed a WAV through ALSA snd-aloop and the real C++ capture daemon.
# Requires a Linux kernel providing snd-aloop; otherwise use tools/feed_wav.py.
# Usage: bash scripts/feed_wav_loopback.sh [wavfile] [-- daemon-option ...]
# Start the backend and engine in other terminals before playback.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ ${1:-} == --help ]]; then
  echo "usage: $0 [wavfile] [-- daemon-option ...]"
  echo "example: $0 samples/jfk.wav -- --engine-socket /tmp/astk-loopback/engine.sock"
  exit 0
fi
WAV=samples/jfk.wav
if [[ $# -gt 0 && $1 != -- ]]; then WAV=$1; shift; fi
DAEMON_ARGS=()
if [[ $# -gt 0 ]]; then
  [[ $1 == -- ]] || { echo "use -- before daemon options" >&2; exit 2; }
  shift
  DAEMON_ARGS=("$@")
fi
[[ -f $WAV ]] || { echo "WAV not found: $WAV" >&2; exit 1; }
[[ -x build/audiostackd ]] || { echo "build first: bash scripts/build.sh" >&2; exit 1; }
command -v aplay >/dev/null || { echo "install alsa-utils first" >&2; exit 1; }

if ! grep -qi loopback /proc/asound/cards 2>/dev/null; then
  if ! sudo modprobe snd-aloop; then
    echo "snd-aloop unavailable. Use tools/feed_wav.py (README.md)." >&2
    exit 1
  fi
fi
CARD=$(awk 'tolower($0) ~ /loopback/ {print $1; exit}' /proc/asound/cards)
[[ -n $CARD ]] || { echo "loopback card not found" >&2; exit 1; }

./build/audiostackd --config config/loopback.conf \
  --device "plughw:$CARD,1" "${DAEMON_ARGS[@]}" &
DAEMON_PID=$!
cleanup() { kill "$DAEMON_PID" 2>/dev/null || true; wait "$DAEMON_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 1
kill -0 "$DAEMON_PID" 2>/dev/null || { echo "daemon failed to start" >&2; exit 1; }
echo "Ensure the engine has connected before continuing."
read -r -p "Press Enter to play $WAV through loopback: "
aplay -D "plughw:$CARD,0" "$WAV"
echo "Playback finished. Inspect transcripts and daemon stats in another terminal."
read -r -p "Press Enter to stop the daemon after the final result arrives: "
