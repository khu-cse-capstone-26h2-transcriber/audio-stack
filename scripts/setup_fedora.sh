#!/usr/bin/env bash
# ============================================================================
# setup_fedora.sh — set up the Fedora 44 (x86_64) dev environment.
# Installs only what is needed to build/run the ALSA-direct capture stack.
# ============================================================================
set -euo pipefail

if ! command -v dnf >/dev/null 2>&1; then
  echo "this script is for Fedora/RHEL (dnf). On Rubik Pi 3 use scripts/setup_rubikpi.sh." >&2
  exit 1
fi

echo "[fedora] installing build/runtime dependencies…"
sudo dnf install -y \
  gcc-c++ cmake make pkgconf-pkg-config \
  alsa-lib-devel alsa-utils alsa-sof-firmware \
  systemd

echo
echo "[fedora] done. Build: ./scripts/build.sh"
echo
echo "# make sure your user can access ALSA devices (audio group):"
id -nG | grep -qw audio && echo "  audio group OK" || echo "  (on desktop Fedora audio goes through PipeWire; when running the daemon directly: 'sudo usermod -aG audio $USER' then re-login)"
echo
echo "# Fedora 44 runs PipeWire as the audio server."
echo "#   If the daemon opens hw:/plughw: directly it may fail because PipeWire holds the device:"
echo "#   (1) recommended: stop PipeWire so the daemon owns the device →  systemctl --user stop pipewire pipewire-pulse"
echo "#   (2) or set the daemon device to 'default' and go through PipeWire (slightly more latency)"
