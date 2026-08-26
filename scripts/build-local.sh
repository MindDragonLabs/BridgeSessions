#!/usr/bin/env bash
# build-local.sh — one-call local dev build for BridgeSessions.
#
# Usage:
#   scripts/build-local.sh                 # Release build into ./build-local
#   scripts/build-local.sh --debug         # Debug build
#   scripts/build-local.sh --tests         # Debug + BUILD_TESTING=ON, runs ctest
#   scripts/build-local.sh --clean         # wipe the build dir first
#   scripts/build-local.sh --dir <path>    # custom build dir (default: build-local)
#
# Produces: <build-dir>/bridgesessions  (dynamic link; for release artifacts
# use scripts/build-linux-static.sh / scripts/build-windows-mingw.sh /
# the macmini static pipeline — see docs/RELEASE-PROVENANCE.md).
set -euo pipefail

BUILD_TYPE=Release
BUILD_DIR=build-local
WITH_TESTS=0
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)  BUILD_TYPE=Debug; shift ;;
        --tests)  WITH_TESTS=1; BUILD_TYPE=Debug; shift ;;
        --clean)  CLEAN=1; shift ;;
        --dir)    BUILD_DIR="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")/.."

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR=(-G Ninja)
fi

EXTRA=()
# macOS: Homebrew tools/libs are not on the default (non-login ssh) PATH.
if [[ "${OSTYPE:-}" == darwin* ]]; then
    [[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
    [[ -d /opt/homebrew/opt/openssl@3 ]] && EXTRA+=(-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3)
fi

if [[ $CLEAN -eq 1 ]]; then
    echo "→ rm -rf $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "→ configure ($BUILD_TYPE, tests=$WITH_TESTS) → $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" ${GENERATOR[@]+"${GENERATOR[@]}"} \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTING=$([[ $WITH_TESTS -eq 1 ]] && echo ON || echo OFF) \
    -DBRIDGESESSIONS_PYTHON=OFF \
    ${EXTRA[@]+"${EXTRA[@]}"}

echo "→ build"
cmake --build "$BUILD_DIR" --parallel

BIN="$BUILD_DIR/bridgesessions"
[[ -x "$BIN" ]] || { echo "✗ build produced no $BIN" >&2; exit 1; }
echo "→ built: $BIN  ($("$BIN" --version 2>/dev/null || echo 'version check failed'))"

if [[ $WITH_TESTS -eq 1 ]]; then
    echo "→ ctest"
    (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
