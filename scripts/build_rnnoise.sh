#!/usr/bin/env bash
# Build the pinned Xiph release locally (Ubuntu 24.04 lacks librnnoise-dev).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="$ROOT/.cache/rnnoise-0.2"
URL=https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37

if [[ -f "$PREFIX/lib/librnnoise.a" && -f "$PREFIX/lib/pkgconfig/rnnoise.pc" ]]; then
  echo "RNNoise 0.2 already built: $PREFIX"
  exit 0
fi

TMP_BUILD=$(mktemp -d)
trap 'rm -rf -- "$TMP_BUILD"' EXIT
curl --fail --location --retry 2 "$URL" -o "$TMP_BUILD/rnnoise-0.2.tar.gz"
cd "$TMP_BUILD"
printf '%s  %s\n' "$SHA256" rnnoise-0.2.tar.gz | sha256sum -c -
tar -xzf rnnoise-0.2.tar.gz
cd rnnoise-0.2
./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" \
  --disable-shared --enable-static --disable-examples
make -j"$(nproc)"
make install
echo "RNNoise 0.2 installed in $PREFIX"
