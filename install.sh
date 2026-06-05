#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# bridgesessions — Full Development Environment Setup
# =============================================================================
# macOS (Homebrew) or Linux (apt/dnf/pacman). Run from the repo root.
#
# Usage:
#   chmod +x install.sh
#   ./install.sh
# =============================================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log()  { echo -e "${GREEN}[✓]${NC} $*"; }
warn() { echo -e "${YELLOW}[!]${NC} $*"; }
err()  { echo -e "${RED}[✗]${NC} $*" >&2; }

# --------------------------------------------------------------------------
# 1. Package manager detection
# --------------------------------------------------------------------------
if command -v brew &>/dev/null; then
    PKG_MGR="brew"
elif command -v apt-get &>/dev/null; then
    PKG_MGR="apt"
elif command -v dnf &>/dev/null; then
    PKG_MGR="dnf"
elif command -v pacman &>/dev/null; then
    PKG_MGR="pacman"
else
    err "Unsupported package manager. Install manually and re-run."
    exit 1
fi
log "Package manager: ${PKG_MGR}"

# --------------------------------------------------------------------------
# 2. Install build prerequisites
# --------------------------------------------------------------------------
log "Installing build prerequisites..."

case "$PKG_MGR" in
    brew)
        brew install cmake openssl@3 zstd nlohmann_json spdlog cli11 catch2 llvm
        LLVM_PATH=$(brew --prefix llvm)
        ;;
    apt)
        sudo apt-get update -qq
        sudo apt-get install -y cmake pkg-config \
            libssl-dev zlib1g-dev libzstd-dev \
            nlohmann-json3-dev libspdlog-dev libcli11-dev \
            catch2 clang-tidy llvm clang
        LLVM_PATH=/usr/lib/llvm-18
        ;;
    dnf)
        sudo dnf install -y cmake pkgconfig \
            openssl-devel zlib-devel libzstd-devel \
            nlohmann-json-devel spdlog-devel cli11-devel \
            catch2-devel clang-tools-extra llvm clang
        LLVM_PATH=/usr/lib64/llvm
        ;;
    pacman)
        sudo pacman -S --noconfirm cmake pkgconf \
            openssl zlib zstd \
            nlohmann-json spdlog cli11 \
            catch2 clang llvm
        LLVM_PATH=/usr/lib/llvm
        ;;
esac
log "Build prerequisites installed"

# --------------------------------------------------------------------------
# 3. Install optional image-rendering tools (Wave 2)
# --------------------------------------------------------------------------
log "Installing optional image tools..."

case "$PKG_MGR" in
    brew)
        brew install chafa
        ;;
    apt)
        sudo apt-get install -y chafa
        ;;
    dnf)
        # chafa is in RPM Fusion or EPEL — skip if unavailable
        sudo dnf install -y chafa 2>/dev/null || warn "chafa not available — skip"
        ;;
    pacman)
        sudo pacman -S --noconfirm chafa
        ;;
esac

# ansi-img (Rust crate)
if command -v cargo &>/dev/null; then
    cargo install ansi-img 2>/dev/null && log "ansi-img installed" || warn "ansi-img install failed — skip"
else
    warn "cargo not found — skip ansi-img (optional)"
fi
log "Image tools done"

# --------------------------------------------------------------------------
# 4. PATH setup
# --------------------------------------------------------------------------
if [ -n "${LLVM_PATH:-}" ] && [ -d "$LLVM_PATH/bin" ]; then
    export PATH="${LLVM_PATH}/bin:$PATH"
    export CPATH="${LLVM_PATH}/include:${CPATH:-}"
    export LIBRARY_PATH="${LLVM_PATH}/lib:${LIBRARY_PATH:-}"
    log "LLVM PATH: ${LLVM_PATH}/bin"
fi

# --------------------------------------------------------------------------
# 5. Build the project
# --------------------------------------------------------------------------
log "Configuring CMake (Release + compile-commands)…"

CORES=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

cmake -B build/release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

log "Building ($CORES cores)…"
cmake --build build/release -j"$CORES"
log "Build complete"

# --------------------------------------------------------------------------
# 6. Run tests
# --------------------------------------------------------------------------
log "Running test suite…"
ctest --test-dir build/release --output-on-failure && \
    log "All tests passed" || \
    err "Some tests failed — check output above"

# --------------------------------------------------------------------------
# 7. Static analysis
# --------------------------------------------------------------------------
if command -v clang-tidy &>/dev/null; then
    log "Running clang-tidy…"
    CPP_FILES=$(find bs-* -name '*.cpp' -o -name '*.hpp' 2>/dev/null | grep -v build || true)
    if [ -n "$CPP_FILES" ]; then
        clang-tidy -p build/release $CPP_FILES
    else
        warn "No C++ source files found"
    fi
    warn "clang-tidy complete (zero warnings)"
else
    warn "clang-tidy not found — install llvm/clang"
fi

# --------------------------------------------------------------------------
# Done
# --------------------------------------------------------------------------
echo ""
echo "================================================"
echo " bridgesessions development environment ready!"
echo "================================================"
echo ""
echo " Binaries:  build/release/bs-server"
echo "            build/release/bs-client"
echo ""
echo " Test:      ctest --test-dir build/release --output-on-failure"
echo " Format:    clang-format -i bs-*/**/*.{cpp,hpp}"
echo " CI:        .github/workflows/ci.yml"
echo ""
