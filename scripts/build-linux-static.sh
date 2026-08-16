#!/bin/bash
# Linux static build via bs-static-builder docker image on fecv3.
# Self-locates the repo root (do NOT hardcode $HOME/bridgesessions — that
# clone has historically diverged). Builds static binary (glibc 2.35 floor).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
echo "=== REPO: $REPO ==="
echo "=== VERSION: $(cat VERSION) ==="

# Clean any prior build dir with sudo — docker leaves root-owned artifacts
# (runs as root by default), and a plain `rm -rf` as the operator uid fails
# with "Permission denied" on the next run.
sudo -n rm -rf build-static || rm -rf build-static

# Build fresh source into a side dir via the pre-baked static builder image.
# Run as root (container default) so /opt deps resolve; do NOT pass --user —
# the image's uid space doesn't match the host operator.
docker run --rm -v "$PWD":/src -w /src bs-static-builder bash -c \
  'rm -rf build-static && \
   cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_PREFIX_PATH="/opt/ossl;/opt/zstd;/opt/fmt;/opt/spdlog;/opt/catch2" \
     -DOPENSSL_ROOT_DIR=/opt/ossl -DZSTD_ROOT=/opt/zstd -DSPDLOG_ROOT=/opt/spdlog \
     -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
     -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
     -B build-static . && cmake --build build-static -j"$(nproc)" --target bridgesessions'

# Recover ownership so the operator can copy the artifact out.
sudo -n chown -R "$(id -u):$(id -g)" build-static 2>/dev/null || true

echo "=== build-static result ==="
ls -la build-static/bridgesessions
echo "=== ldd (expect only libc + ld-linux) ==="
ldd build-static/bridgesessions
echo "=== version ==="
./build-static/bridgesessions --version
echo "=== copy to dist ==="
mkdir -p dist
cp build-static/bridgesessions dist/bridgesessions-linux-x86_64
ls -la dist/bridgesessions-linux-x86_64
echo "LINUX BUILD OK"
