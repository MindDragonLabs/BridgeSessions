#!/bin/bash
# ci-win-deps.sh — build the static mingw-w64 dependency prefix consumed by
# scripts/build-windows-mingw.sh (prefix layout probe: $BS_WIN_PREFIX, else
# /opt/bs-win, else ~/bs-win).
#
# Runs on any linux host with the mingw-w64 toolchain:
#   CI:    ubuntu-24.04 runner, installed inline (see release-builds.yml)
#   local: docker run --rm -v "$PWD":/src ubuntu:24.04 bash /src/scripts/ci-win-deps.sh
#
# Produces in $1 (prefix dir):
#   include/CLI11.hpp           (CLI11 2.7.2, header-only)
#   include/fmt/, lib/libfmt.a  (fmt 12.2.0, static)
#   include/spdlog/, lib/libspdlog.a (spdlog 1.17.0, static, external fmt)
#   include/openssl..., lib/{libssl.a,libcrypto.a} + lib64 symlink (OpenSSL 3.5.7, static)
#   include/zstd.h..., lib/libzstd.a (zstd 1.5.7, static)
#
# Versions are pinned to match scripts/Dockerfile.static-linux (the proven
# linux release env) so all three release binaries track the same dep set.
set -euo pipefail

PREFIX="${1:?usage: ci-win-deps.sh <prefix-dir>}"
DEPS_DL="${TMPDIR:-/tmp}/bs-win-deps-dl"
JOBS="${JOBS:-4}"
# Virgin-prefix safe: wget -O / cmake -S do not create parent dirs.
mkdir -p "$PREFIX/include" "$PREFIX/lib"
mkdir -p "$PREFIX" "$DEPS_DL"
cd "$DEPS_DL"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
  echo "=== installing mingw-w64 (posix threads variant) ==="
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build perl wget ca-certificates \
    g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
fi
# Pin the posix variant: std::thread on windows needs winpthreads (win32
# variant fails at link on <thread>).
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

TOOLCHAIN="$DEPS_DL/mingw-toolchain.cmake"
# NOTE: $PREFIX must be in CMAKE_FIND_ROOT_PATH — with MODE_* ONLY, package
# search paths are re-rooted against the root list, and spdlog's
# find_package(fmt) would otherwise never see the deps prefix
# (observed: "Could not find a package configuration file provided by fmt").
cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 "$PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# ---- CLI11 2.7.2 (header-only) -------------------------------------------
# Use the release amalgamated single header. The repo's include/CLI/CLI.hpp is
# a multi-file wrapper (#include "Version.hpp" etc.) and cannot be lifted out
# of its directory (observed: fatal error: Version.hpp: No such file).
if [[ ! -f "$PREFIX/include/CLI11.hpp" ]]; then
  echo "=== CLI11 2.7.2 ==="
  wget -q https://github.com/CLIUtils/CLI11/releases/download/v2.7.2/CLI11.hpp \
    -O "$PREFIX/include/CLI11.hpp"
fi

# ---- nlohmann/json 3.12.0 (header-only) ------------------------------------
# The on-host Arch build leaked /usr/include/nlohmann-json; CI must be
# hermetic, so ship it in the prefix (pinned to match the linux Dockerfile).
if [[ ! -f "$PREFIX/include/nlohmann/json.hpp" ]]; then
  echo "=== nlohmann/json 3.12.0 ==="
  wget -q https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz -O json.tar.gz
  tar xf json.tar.gz
  mkdir -p "$PREFIX/include"
  cp -r json-3.12.0/single_include/nlohmann "$PREFIX/include/"
fi

# ---- zstd 1.5.7 (static) ---------------------------------------------------
if [[ ! -f "$PREFIX/lib/libzstd.a" ]]; then
  echo "=== zstd 1.5.7 ==="
  wget -q https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz -O zstd.tar.gz
  tar xf zstd.tar.gz
  cmake -GNinja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
    -DZSTD_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -B zstd-build zstd-1.5.7/build/cmake
  cmake --build zstd-build -j"$JOBS"
  cmake --install zstd-build
fi

# ---- OpenSSL 3.5.7 (static, mingw64 target) --------------------------------
if [[ ! -f "$PREFIX/lib/libssl.a" ]]; then
  echo "=== OpenSSL 3.5.7 ==="
  wget -q https://www.openssl.org/source/openssl-3.5.7.tar.gz
  tar xf openssl-3.5.7.tar.gz
  ( cd openssl-3.5.7 && \
    ./Configure mingw64 no-shared no-tests no-asm \
      --cross-compile-prefix=x86_64-w64-mingw32- \
      --prefix="$PREFIX" --libdir=lib && \
    make -j"$JOBS" && make install_sw )
fi
# build-windows-mingw.sh links ssl/crypto from $PREFIX/lib64 — match the
# local prefix layout regardless of where openssl installed them.
if [[ -f "$PREFIX/lib/libssl.a" && ! -e "$PREFIX/lib64" ]]; then
  ln -s "$PREFIX/lib" "$PREFIX/lib64"
fi

# ---- fmt 12.2.0 (static) ----------------------------------------------------
if [[ ! -f "$PREFIX/lib/libfmt.a" ]]; then
  echo "=== fmt 12.2.0 ==="
  wget -q https://github.com/fmtlib/fmt/archive/refs/tags/12.2.0.tar.gz -O fmt.tar.gz
  tar xf fmt.tar.gz
  cmake -GNinja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DFMT_TEST=OFF -DFMT_DOC=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -B fmt-build fmt-12.2.0
  cmake --build fmt-build -j"$JOBS"
  cmake --install fmt-build
fi

# ---- spdlog 1.17.0 (static, external fmt — matches Dockerfile.static-linux) -
if [[ ! -f "$PREFIX/lib/libspdlog.a" ]]; then
  echo "=== spdlog 1.17.0 ==="
  wget -q https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz -O spdlog.tar.gz
  tar xf spdlog.tar.gz
  cmake -GNinja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DSPDLOG_FMT_EXTERNAL=ON \
    -DCMAKE_PREFIX_PATH="$PREFIX" -DSPDLOG_BUILD_EXAMPLE=OFF \
    -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_BUILD_BENCH=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -B spdlog-build spdlog-1.17.0
  cmake --build spdlog-build -j"$JOBS"
  cmake --install spdlog-build
fi

echo "=== prefix content ==="
ls -la "$PREFIX/include" "$PREFIX/lib"
[[ -f "$PREFIX/include/CLI11.hpp" && -f "$PREFIX/lib/libzstd.a" \
   && -f "$PREFIX/lib/libssl.a" && -f "$PREFIX/lib/libcrypto.a" \
   && -f "$PREFIX/lib/libfmt.a" && -f "$PREFIX/lib/libspdlog.a" ]] \
  || { echo "FATAL: prefix incomplete" >&2; exit 1; }
echo "WIN DEPS OK: $PREFIX"
