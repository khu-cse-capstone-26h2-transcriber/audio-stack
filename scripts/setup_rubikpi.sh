#!/usr/bin/env bash
# Set up Ubuntu for Qualcomm on RUBIK Pi 3 (QCS6490, arm64).
# Installs build/capture dependencies only; does not flash an image or install QNN.
set -euo pipefail

if [[ ${1:-} == --help ]]; then
  echo "usage: bash scripts/setup_rubikpi.sh [--with-rnnoise]"
  echo "Run on Ubuntu for Qualcomm (arm64). For Fedora use setup_fedora.sh."
  exit 0
fi
WITH_RNNOISE=0
if [[ ${1:-} == --with-rnnoise ]]; then WITH_RNNOISE=1; shift; fi
if [[ $# -ne 0 ]]; then echo "unknown argument: $1" >&2; exit 2; fi

[[ -r /etc/os-release ]] || { echo "cannot identify OS: /etc/os-release missing" >&2; exit 1; }
. /etc/os-release
if [[ ${ID:-} != ubuntu ]]; then
  echo "[rubikpi] expected Ubuntu for Qualcomm, found ${PRETTY_NAME:-unknown OS}." >&2
  echo "For a Fedora host use bash scripts/setup_fedora.sh." >&2
  exit 1
fi
if [[ $(uname -m) != aarch64 ]]; then
  echo "[rubikpi] expected the arm64 board image; run this script on RUBIK Pi 3." >&2
  exit 1
fi
command -v apt-get >/dev/null || { echo "apt-get is required" >&2; exit 1; }

echo "[rubikpi] ${PRETTY_NAME}: installing build and audio tools"
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libasound2-dev alsa-utils \
  python3 python3-venv usbutils
if (( WITH_RNNOISE )); then
  sudo apt-get install -y curl ca-certificates libspeexdsp-dev
  bash "$(dirname "$0")/build_rnnoise.sh"
  echo "[rubikpi] RNNoise build: bash scripts/build.sh Release -DASTK_ENABLE_RNNOISE=ON"
fi

cat <<'EOF'
[rubikpi] Build: bash scripts/build.sh

XVF3800 USB input: see docs/XVF3800.md for channel profiles and first recording.

Physical input: a compatible microphone/headset connected to the 3.5 mm jack.
Check the installed image's card and PCM numbering before using this example:
  cat /proc/asound/cards
  arecord -l
  amixer -c 0 cset "name=MultiMedia3 Mixer PRIMARY_MI2S_TX" 1
  bash scripts/audio_probe.sh --device plughw:0,2

Without a microphone, use software input (separate terminals):
  python3 tools/feed_wav.py samples/jfk.wav --socket /tmp/astk-file/engine.sock
  python3 tests/dummy_engine.py --socket /tmp/astk-file/engine.sock

See docs/UBUNTU-INPUT.md for audio permissions, sound-server ownership,
ALSA loopback tests and service installation. No mixer settings were changed.
EOF
