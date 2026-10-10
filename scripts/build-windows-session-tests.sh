#!/usr/bin/env bash
# Build the native restart fixture using the Windows daemon's existing tree.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${BS_BUILD_ROOT:-$ROOT/build}"
OUTPUT="$BUILD_ROOT/windows-session-tests"
mkdir -p "$OUTPUT"

if [[ -f "$BUILD_ROOT/container-windows-x86_64/CMakeCache.txt" ]]; then
  # Ubuntu 22.04's host MinGW lacks C++23. build.sh falls back to Ubuntu
  # 24.04 and leaves both its cross-build and static prefix in the mounted
  # tree. Reuse those paths and that compiler, rather than configuring a
  # fresh host (Linux) project under build/windows-x86_64.
  [[ "$BUILD_ROOT" == "$ROOT/build" ]] || {
    echo "container Windows tests require the daemon's standard build root" >&2
    exit 1
  }
  docker run --rm -v "$ROOT:/work" -w /work ubuntu:24.04 bash -lc '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends cmake make git ca-certificates \
      g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix >/dev/null
    update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
    cmake -S . -B build/container-windows-x86_64 -DBUILD_TESTING=ON
    cmake --build build/container-windows-x86_64 --target test_relay --parallel 2
  '
  cp "$BUILD_ROOT/container-windows-x86_64/test_relay.exe" "$OUTPUT/"
elif [[ -f "$BUILD_ROOT/windows-x86_64/CMakeCache.txt" ]]; then
  cmake -S "$ROOT" -B "$BUILD_ROOT/windows-x86_64" -DBUILD_TESTING=ON
  cmake --build "$BUILD_ROOT/windows-x86_64" --target test_relay --parallel 2
  cp "$BUILD_ROOT/windows-x86_64/test_relay.exe" "$OUTPUT/"
else
  echo "build the Windows daemon with build.sh before its session tests" >&2
  exit 1
fi

file "$OUTPUT/test_relay.exe" | grep -E 'PE32\+.*x86-64' >/dev/null
