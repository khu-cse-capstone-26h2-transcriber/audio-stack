#!/usr/bin/env bash
# ============================================================================
# setup_fedora.sh — set up the Fedora 44 (x86_64) dev environment.
# Installs only what is needed to build/run the ALSA-direct capture stack.
# ============================================================================
set -euo pipefail

if [[ ${1:-} == --help ]]; then
  echo "usage: bash scripts/setup_fedora.sh [--with-rnnoise]"
  exit 0
fi
DSP_PACKAGES=()
if [[ ${1:-} == --with-rnnoise ]]; then
  DSP_PACKAGES=(rnnoise-devel speexdsp-devel)
  shift
fi
if [[ $# -ne 0 ]]; then echo "unknown argument: $1" >&2; exit 2; fi

if ! command -v dnf >/dev/null 2>&1; then
  echo "this script is for Fedora/RHEL (dnf). On Rubik Pi 3 use scripts/setup_rubikpi.sh." >&2
  exit 1
fi

echo "[fedora] installing build/runtime dependencies…"
sudo dnf install -y \
  gcc-c++ cmake make pkgconf-pkg-config \
  alsa-lib-devel alsa-utils alsa-sof-firmware \
  systemd "${DSP_PACKAGES[@]}"

echo
echo "[fedora] done. Build: bash scripts/build.sh"
if (( ${#DSP_PACKAGES[@]} )); then
  echo "RNNoise build: bash scripts/build.sh Release -DASTK_ENABLE_RNNOISE=ON"
fi
echo
echo "# make sure your user can access ALSA devices (audio group):"
id -nG | grep -qw audio && echo "  audio group OK" || echo "  (on desktop Fedora audio goes through PipeWire; when running the daemon directly: 'sudo usermod -aG audio $USER' then re-login)"
echo
echo "# Fedora 44 runs PipeWire as the audio server."
echo "#   If the daemon opens hw:/plughw: directly it may fail because PipeWire holds the device:"
echo "#   (1) recommended: stop PipeWire so the daemon owns the device →  systemctl --user stop pipewire pipewire-pulse"
echo "#   (2) or set the daemon device to 'default' and go through PipeWire (slightly more latency)"
