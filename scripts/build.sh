#!/usr/bin/env bash
# build.sh - common build for Ubuntu for Qualcomm (arm64) and Fedora (x86_64)
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_TYPE="${1:-Release}"
if [[ $# -gt 0 ]]; then shift; fi
LOCAL_RNNOISE="$PWD/.cache/rnnoise-0.2/lib/pkgconfig"
if [[ -f "$LOCAL_RNNOISE/rnnoise.pc" ]]; then
  export PKG_CONFIG_PATH="$LOCAL_RNNOISE${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
cmake --build build -j"$(nproc)"

echo
echo "build complete:"
ls -1 build/audiostackd build/astk-* 2>/dev/null || true
echo
echo "quick test (capture only, no engine/backend):"
echo "  ./build/astk-list-devices"
echo "  ./build/astk-capture-wav --device plughw:0,2 --seconds 5 --out /tmp/astk.wav"
echo "without a microphone (separate terminals):"
echo "  python3 tools/feed_wav.py samples/jfk.wav --socket /tmp/astk-file/engine.sock"
echo "  python3 tests/dummy_engine.py --socket /tmp/astk-file/engine.sock"
