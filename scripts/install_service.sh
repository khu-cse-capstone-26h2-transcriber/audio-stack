#!/usr/bin/env bash
# install_service.sh — install audiostackd as a systemd service
# targets: Qualcomm Linux (Debian) / Fedora (systemd common)
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $EUID -ne 0 ]]; then echo "run as root: sudo ./scripts/install_service.sh"; exit 1; fi

command -v audiostackd >/dev/null || cmake --install build

mkdir -p /etc/audiostack
[[ -f /etc/audiostack/audiostack.conf ]] || cp config/audiostack.conf /etc/audiostack/audiostack.conf

# Service account (default: root — the usual choice on single-user embedded boards).
# For hardening: useradd -rG audio -s /usr/sbin/nologin audiostack and change
# User=root to audiostack in the unit file.
cp systemd/audiostack.service /etc/systemd/system/audiostack.service
systemctl daemon-reload
systemctl enable audiostack.service
systemctl restart audiostack.service

sleep 1
systemctl --no-pager status audiostack.service || true
echo
echo "logs: journalctl -u audiostack -f"
echo "control: astk-ctl status | astk-ctl 'session rotate'"
echo "after editing config: systemctl restart audiostack"
