#!/usr/bin/env bash
# ============================================================================
# setup_rubikpi.sh — set up Rubik Pi 3 (QCS6490) Qualcomm Linux / Debian.
#
# Per the survey:
#   - Qualcomm Linux (Debian based) / Thundercomm Debian image → apt available
#   - Yocto-based Qualcomm Linux (older firmware) → no apt: cross-build guidance
# ============================================================================
set -euo pipefail

IS_ARM64=$(uname -m)
if command -v apt-get >/dev/null 2>&1; then
  echo "[rubikpi] apt-based system detected ($IS_ARM64). installing dependencies…"
  sudo apt-get update
  sudo apt-get install -y \
    build-essential cmake pkg-config \
    libasound2-dev alsa-utils \
    systemd

  echo
  echo "[rubikpi] done. Build: ./scripts/build.sh"
  echo
  echo "# to use the on-board 3.5mm mic input (per official docs; may vary by firmware):"
  echo "#   amixer -c 0 cset \"name=MultiMedia3 Mixer PRIMARY_MI2S_TX\" 1"
  echo "#   arecord -Dplughw:0,2 -f S16_LE -r 48000 -c 1 -d 5 /opt/mic_test.wav"
  echo "# USB mics (ReSpeaker v2.0 etc.) are plughw:1,0 — native 16 kHz (matches Whisper)"
  echo
  echo "# check audio group: sudo usermod -aG audio $USER  (then re-login)"
  echo "# run as a service: sudo ./scripts/install_service.sh"
elif command -v dnf >/dev/null 2>&1; then
  echo "[rubikpi] Fedora detected — use scripts/setup_fedora.sh instead." >&2
  exit 1
else
  cat >&2 <<'EOF'
[rubikpi] no apt found — this looks like a Yocto-based Qualcomm Linux image.
Options:
  1) (recommended) Boot the Thundercomm Debian 13/12 image or the
     qualcomm-linux/qcom-deb-images trixie image: apt works and you can build
     directly on the board.
  2) Stay on Yocto: cross-compile from a host PC with the OpenEmbedded /
     Qualcomm Linux SDK toolchain (CMake toolchain file; link alsa-lib from
     the SDK staging dir).
     e.g. . <sdk>/environment-setup-aarch64-oe-linux && cmake -B build -DCMAKE_TOOLCHAIN_FILE=...
EOF
  exit 1
fi
