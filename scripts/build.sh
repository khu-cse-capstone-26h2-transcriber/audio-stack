#!/usr/bin/env bash
# build.sh — common build for Fedora 44 (x86_64) and Qualcomm Linux/Debian (arm64)
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_TYPE="${1:-Release}"
cmake -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j"$(nproc)"

echo
echo "build complete:"
ls -1 build/audiostackd build/astk-* 2>/dev/null || true
echo
echo "quick test (capture only, no engine/backend):"
echo "  ./build/astk-list-devices"
echo "  ./build/astk-capture-wav --device plughw:1,0 --seconds 5 --out /tmp/astk.wav"
