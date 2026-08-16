#!/bin/bash
# Windows x86_64 PE cross-compile via MinGW on fecv3 (~/bs-win static deps).
# Self-locates the repo root (do NOT hardcode $HOME/bridgesessions — that
# clone has historically diverged).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
TRIPLE=x86_64-w64-mingw32-g++
PREFIX="$HOME/bs-win"
VERSION="$(cat VERSION)"
echo "=== REPO: $REPO ==="
echo "=== VERSION: $VERSION ==="

# Ensure CLI shim exists (source wants <CLI/CLI.hpp>).
mkdir -p /tmp/bs-win-shim/CLI
ln -sf "$PREFIX/include/CLI11.hpp" /tmp/bs-win-shim/CLI/CLI.hpp

rm -rf build-win
mkdir -p build-win
"$TRIPLE" -static -std=c++23 -O3 -DNDEBUG \
  -DBS_VERSION="\"$VERSION\"" -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
  -isystem "$PREFIX/include" -isystem /tmp/bs-win-shim \
  main.cpp -o build-win/bridgesessions.exe \
  "$PREFIX/lib/libspdlog.a" "$PREFIX/lib/libfmt.a" \
  "$PREFIX/lib64/libssl.a" "$PREFIX/lib64/libcrypto.a" "$PREFIX/lib/libzstd.a" \
  -lpthread -lws2_32 -lcrypt32 -lgdi32 -luser32

echo "=== DLL imports (expect only OS DLLs) ==="
x86_64-w64-mingw32-objdump -p build-win/bridgesessions.exe | grep "DLL Name" || true

echo "=== copy to dist ==="
mkdir -p dist
cp build-win/bridgesessions.exe dist/bridgesessions-windows-x86_64.exe
ls -la dist/bridgesessions-windows-x86_64.exe
echo "WINDOWS BUILD OK"
