#!/usr/bin/env bash
#
# build.sh — the one builder for BridgeSessions.
#
# Builds Linux, macOS, and Windows from a single entry point, with pinned
# dependencies resolved by cmake/Dependencies.cmake (nothing hand-installed,
# no machine-local edits).
#
#   ./build.sh linux                      build for this host
#   ./build.sh linux --distro ubuntu:22.04   release build in a container
#   ./build.sh macos                      build on macOS
#   ./build.sh windows                    cross-compile with mingw-w64
#   ./build.sh all                        every target this host can produce
#   ./build.sh test                       configure, build, run ctest
#   ./build.sh package                    stage dist/ + SHA256SUMS
#   ./build.sh clean                      remove build directories
#
# Run `./build.sh --help` for the full option list.
#
set -euo pipefail

BS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BS_ROOT
readonly BS_BUILD_ROOT="${BS_ROOT}/build"
readonly BS_DIST="${BS_ROOT}/dist"

# Pinned build toolchain for container builds. Ubuntu 22.04 ships CMake 3.22;
# this project needs 3.25 or newer.
readonly BS_CMAKE_VERSION="3.28.3"
readonly BS_CMAKE_SHA256="804d231460ab3c8b556a42d2660af4ac7a0e21c98a7f8ee3318a74b4a9a187a6"

# Container images used for release Linux builds. The oldest glibc floor wins:
# a binary built against glibc 2.35 runs on every newer distro (Debian 12,
# Ubuntu 24.04, Arch), so 22.04 is the release target for Linux.
readonly BS_DEFAULT_DISTRO="ubuntu:22.04"

# mingw-w64 is a Linux-hosted cross-compiler: it runs here and emits a Windows
# PE. So "building for Windows" always needs a Linux toolchain, and whose
# toolchain decides whether C++23 works:
#   Ubuntu 22.04 -> mingw GCC 10.3  (rejects -std=c++23)
#   Ubuntu 24.04 -> mingw GCC 13.2  (accepts it)
#   Arch (rolling) -> mingw GCC 16.x (accepts it)
# Prefer a native mingw when it is new enough: it is faster, needs no container,
# and is the toolchain that produced the shipped Windows binary. Fall back to
# the 24.04 container only when the host's mingw is too old or missing.
readonly BS_WIN_DISTRO="ubuntu:24.04"
readonly BS_MINGW_MIN_GCC=13

# True when $1 (a mingw g++) is at least BS_MINGW_MIN_GCC. 0 = usable, 1 = not.
mingw_is_cxx23_capable() {
    local cc="${1:-x86_64-w64-mingw32-g++}"
    have "${cc}" || return 1
    local major
    major="$("${cc}" -dumpversion 2>/dev/null | cut -d. -f1)"
    [[ "${major}" =~ ^[0-9]+$ ]] || return 1
    [[ "${major}" -ge "${BS_MINGW_MIN_GCC}" ]]
}

# Extra packages the Windows cross build needs inside the container. The
# posix-thread mingw variant is required: the win32 variant fails at link on
# <thread>. Alternatives are pinned too, or the wrong variant wins.
readonly MINGW_BOOTSTRAP='
# The Linux bootstrap exports CC/CXX=gcc-12 for the C++23 floor. The mingw
# cross build must NOT inherit them, or OpenSSL resolves the compiler as
# x86_64-w64-mingw32-gcc-12 and fails.
unset CC CXX
apt-get install -y -qq --no-install-recommends \
    g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 >/dev/null
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
x86_64-w64-mingw32-g++ --version | head -1
'

# ── Output helpers ──────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
else
    C_RESET=""; C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""
fi
log()  { printf '%s==>%s %s\n' "${C_BOLD}" "${C_RESET}" "$*"; }
note() { printf '%s    %s%s\n' "${C_DIM}" "$*" "${C_RESET}"; }
warn() { printf '%s[!] %s%s\n' "${C_YELLOW}" "$*" "${C_RESET}" >&2; }
die()  { printf '%s[x] %s%s\n' "${C_RED}" "$*" "${C_RESET}" >&2; exit 1; }
ok()   { printf '%s[ok] %s%s\n' "${C_GREEN}" "$*" "${C_RESET}"; }

# ── Defaults ────────────────────────────────────────────────────────────────
TARGET=""
DISTRO=""
ARCH="$(uname -m)"
BUILD_TYPE="Release"
JOBS=""
OUT_DIR="${BS_DIST}"
DO_TESTS=""
DO_STRIP="yes"
DEPS_MODE="fetch"
OPENSSL_MODE="system"
OPENSSL_EXPLICIT="no"
NO_CACHE="no"
IN_CONTAINER="no"
KEEP_CONTAINER="no"
VERBOSE="no"
PRINT_ONLY="no"
EXTRA_CMAKE_ARGS=()

usage() {
    cat <<EOF
${C_BOLD}build.sh${C_RESET} — build BridgeSessions for Linux, macOS, and Windows.

${C_BOLD}USAGE${C_RESET}
  ./build.sh <target> [options]

${C_BOLD}TARGETS${C_RESET}
  linux            Build the Linux binary. Add --distro to build inside a
                   container so the artifact carries a low glibc floor.
  macos            Build the macOS binary (must run on macOS).
  windows          Cross-compile the Windows PE with mingw-w64.
  all              Build every target this host supports.
  test             Configure, build, and run the full ctest suite.
  package          Build all supported targets, then stage dist/ + SHA256SUMS.
  clean            Remove build/ and generated build directories.
  deps             Print the resolved dependency pins.

${C_BOLD}OPTIONS${C_RESET}
  --distro IMAGE   Linux only. Build inside this container image.
                   Recommended: ${BS_DEFAULT_DISTRO} (glibc 2.35 floor).
                   Also useful: ubuntu:24.04, debian:12, archlinux:latest.
                   Use 'native' to force a build on the host.
  --arch ARCH      x86_64 | arm64. Default: host architecture.
  --build-type T   Release | RelWithDebInfo | Debug. Default: Release.
  --out DIR        Where to write artifacts. Default: ./dist
  --jobs N         Parallel build jobs. Default: all cores.
  --tests          Run ctest as part of the build. (default for native builds)
  --no-tests       Skip ctest.
  --deps MODE      fetch | system | auto. Default: fetch (pinned, portable).
  --openssl MODE   system | fetch. Default: system (fast, ABI-stable).
  --no-strip       Keep debug symbols in the staged artifact.
  --no-cache       Ignore the container dependency cache.
  --in-container   Internal: set when build.sh re-invokes itself inside a
                   container. Do not use directly.
  --keep-container Leave the container running for debugging.
  --extra ARG      Extra -D argument passed to CMake. Repeatable.
  -v, --verbose    Verbose build output.
  -n, --dry-run    Print the commands without running them.
  -h, --help       This help.

${C_BOLD}EXAMPLES${C_RESET}
  ./build.sh linux --distro ubuntu:22.04 --tests
  ./build.sh windows
  ./build.sh package --distro ubuntu:22.04
  ./build.sh linux --deps system --no-tests          # fast local iteration
EOF
}

# ── Argument parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        linux|macos|windows|all|test|package|clean|deps) TARGET="$1"; shift ;;
        --distro)        DISTRO="${2:?--distro needs a value}"; shift 2 ;;
        --arch)          ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --build-type)    BUILD_TYPE="${2:?--build-type needs a value}"; shift 2 ;;
        --out)           OUT_DIR="${2:?--out needs a value}"; shift 2 ;;
        --jobs)          JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        --deps)          DEPS_MODE="${2:?--deps needs a value}"; shift 2 ;;
        --openssl)       OPENSSL_MODE="${2:?--openssl needs a value}"; OPENSSL_EXPLICIT="yes"; shift 2 ;;
        --tests)         DO_TESTS="yes"; shift ;;
        --no-tests)      DO_TESTS="no"; shift ;;
        --no-strip)      DO_STRIP="no"; shift ;;
        --no-cache)      NO_CACHE="yes"; shift ;;
        --in-container)  IN_CONTAINER="yes"; shift ;;
        --keep-container) KEEP_CONTAINER="yes"; shift ;;
        --extra)         EXTRA_CMAKE_ARGS+=("${2:?--extra needs a value}"); shift 2 ;;
        -v|--verbose)    VERBOSE="yes"; shift ;;
        -n|--dry-run)    PRINT_ONLY="yes"; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

[[ -n "${TARGET}" ]] || { usage; exit 1; }

case "${DEPS_MODE}" in fetch|system|auto) ;; *) die "--deps must be fetch, system, or auto" ;; esac
case "${OPENSSL_MODE}" in system|fetch) ;; *) die "--openssl must be system or fetch" ;; esac
case "${BUILD_TYPE}" in Release|RelWithDebInfo|Debug) ;; *) die "--build-type must be Release, RelWithDebInfo, or Debug" ;; esac

if [[ -z "${JOBS}" ]]; then
    if command -v nproc >/dev/null 2>&1; then JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else JOBS=4; fi
fi

# Native builds exercise the code, so they test by default. Container release
# builds do too: the artifact is the release gate.
if [[ -z "${DO_TESTS}" ]]; then
    DO_TESTS="yes"
fi

run() {
    if [[ "${PRINT_ONLY}" == "yes" ]]; then
        printf '    %s\n' "$*"
    else
        "$@"
    fi
}

# ── Host capability detection ───────────────────────────────────────────────
HOST_OS="$(uname -s)"
case "${HOST_OS}" in
    Linux)  HOST_KIND="linux" ;;
    Darwin) HOST_KIND="macos" ;;
    *)      HOST_KIND="unknown" ;;
esac

have() { command -v "$1" >/dev/null 2>&1; }

host_can() {
    case "$1" in
        linux)
            [[ "${HOST_KIND}" == "linux" ]] \
                || { have docker && docker info >/dev/null 2>&1; } ;;
        macos)   [[ "${HOST_KIND}" == "macos" ]] ;;
        windows) have x86_64-w64-mingw32-g++ ;;
        *)       return 1 ;;
    esac
}

# ── Build directory layout ──────────────────────────────────────────────────
# Every target gets its own tree so a container build can never collide with a
# native one. BS_BUILD_SUBDIR overrides the name and is how the container
# invocation keeps its output separate from the host's.
BS_BUILD_SUBDIR="${BS_BUILD_SUBDIR:-}"

# Resolve the build directory for a logical target name.
build_dir_for() {
    printf '%s/%s' "${BS_BUILD_ROOT}" "${BS_BUILD_SUBDIR:-$1}"
}

# ── CMake invocation ────────────────────────────────────────────────────────
cmake_configure_build() {
    local src="$1" build_dir="$2"
    local -a args=(
        -S "${src}" -B "${build_dir}"
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DBS_DEPS_MODE="${DEPS_MODE}"
        -DBS_OPENSSL="${OPENSSL_MODE}"
        -DBUILD_TESTING="$([[ "${DO_TESTS}" == "yes" || "${TARGET}" == "test" ]] && echo ON || echo OFF)"
    )
    if [[ "${VERBOSE}" == "yes" ]]; then args+=(-DCMAKE_VERBOSE_MAKEFILE=ON); fi
    local a
    for a in "${EXTRA_CMAKE_ARGS[@]:-}"; do
        [[ -n "${a}" ]] && args+=("${a}")
    done

    log "configure (${BUILD_TYPE}, deps=${DEPS_MODE}, openssl=${OPENSSL_MODE})"
    run cmake "${args[@]}"
    log "build (${JOBS} jobs)"
    local -a bargs=(--build "${build_dir}" --parallel "${JOBS}")
    [[ "${VERBOSE}" == "yes" ]] && bargs+=(--verbose)
    run cmake "${bargs[@]}"
}

run_ctest() {
    local build_dir="$1"
    [[ "${DO_TESTS}" == "yes" || "${TARGET}" == "test" ]] || return 0
    log "ctest"
    # Cap parallelism. This suite opens real sockets and binds real ports; at
    # 64-way parallelism the socket/timing tests contend and flake, and the
    # failing set is different every run. A modest cap keeps the signal clean.
    local jobs="${JOBS}"
    if [[ "${jobs}" =~ ^[0-9]+$ ]] && [[ "${jobs}" -gt 8 ]]; then jobs=8; fi
    run ctest --test-dir "${build_dir}" --output-on-failure --parallel "${jobs}"
}

# Stage one binary into OUT_DIR with a tidy, release-ready name.
stage_artifact() {
    local bin="$1" name="$2"
    if [[ "${PRINT_ONLY}" == "yes" ]]; then
        note "would stage ${bin} -> ${OUT_DIR}/${name}"
        return 0
    fi
    [[ -f "${bin}" ]] || die "expected binary not found: ${bin}"
    mkdir -p "${OUT_DIR}"
    cp -f "${bin}" "${OUT_DIR}/${name}"
    if [[ "${DO_STRIP}" == "yes" && "${BUILD_TYPE}" != "Debug" ]]; then
        case "${name}" in
            *.exe) have x86_64-w64-mingw32-strip && x86_64-w64-mingw32-strip \
                       "${OUT_DIR}/${name}" 2>/dev/null || true ;;
            *)     strip "${OUT_DIR}/${name}" 2>/dev/null || true ;;
        esac
    fi
    chmod +x "${OUT_DIR}/${name}" 2>/dev/null || true
    local sum size
    if have sha256sum; then
        sum="$(sha256sum "${OUT_DIR}/${name}" | cut -c1-16)"
    else
        sum="$(shasum -a 256 "${OUT_DIR}/${name}" | cut -c1-16)"
    fi
    if have du; then size="$(du -h "${OUT_DIR}/${name}" | cut -f1 | tr -d ' ')"; else size="?"; fi
    printf '    %s  %s  %s\n' "${sum}…" "${size}" "${name}"
}

suffix_for_arch() {
    case "$1" in
        arm64|aarch64) echo "arm64" ;;
        *)             echo "x86_64" ;;
    esac
}

# ── Linux: native ───────────────────────────────────────────────────────────
build_linux_native() {
    [[ "${HOST_KIND}" == "linux" ]] || die "native Linux build needs a Linux host (use --distro IMAGE)"
    local build_dir; build_dir="$(build_dir_for "linux-$(suffix_for_arch "${ARCH}")")"
    cmake_configure_build "${BS_ROOT}" "${build_dir}"
    run_ctest "${build_dir}"
    stage_artifact "${build_dir}/bridgesessions" "bridgesessions-linux-$(suffix_for_arch "${ARCH}")"
}
build_in_container() {
    local image="$1" inner_target="$2" bootstrap_extra="$3"; shift 3

    local arch; arch="$(suffix_for_arch "${ARCH}")"
    local name="bs-build-$$"
    local bootstrap; bootstrap="$(container_bootstrap \
        | sed -e "s/__CMAKE_VERSION__/${BS_CMAKE_VERSION}/g" \
              -e "s/__CMAKE_SHA256__/${BS_CMAKE_SHA256}/g")"
    bootstrap="${bootstrap}
${bootstrap_extra}"

    # Pass the caller's options through to the in-container invocation.
    local inner_args="--build-type ${BUILD_TYPE} --jobs ${JOBS} --deps ${DEPS_MODE} --openssl ${OPENSSL_MODE} --out /work/dist"
    [[ "${DO_TESTS}" == "yes" ]] && inner_args+=" --tests" || inner_args+=" --no-tests"
    [[ "${DO_STRIP}" == "no" ]] && inner_args+=" --no-strip"
    [[ "${VERBOSE}" == "yes" ]] && inner_args+=" --verbose"
    local a; for a in "${EXTRA_CMAKE_ARGS[@]:-}"; do [[ -n "${a}" ]] && inner_args+=" --extra ${a}"; done

    log "container build: ${image} (${arch}) — target ${inner_target}"

    local -a docker_args=(
        run --rm --name "${name}"
        -v "${BS_ROOT}:/work"
        -w /work
        # The container runs as root (apt needs it). Hand ownership back so the
        # host user can clean or reuse the tree.
        -e "BS_HOST_UID=$(id -u)"
        -e "BS_HOST_GID=$(id -g)"
        # Keep the container's build tree separate from the host's, so a
        # container run can never overwrite (or be mistaken for) a native build.
        -e "BS_BUILD_SUBDIR=container-${arch}"
        -e "CONTAINER_INNER_TARGET=${inner_target}"
        -e "CONTAINER_INNER_ARGS=${inner_args}"
    )

    if [[ "${PRINT_ONLY}" == "yes" ]]; then
        note "would run in ${image}: bootstrap, then ./build.sh ${inner_target} ${inner_args}"
        return 0
    fi

    have docker || die "docker is required for container builds (use --distro native)"
    docker info >/dev/null 2>&1 || die "docker daemon is not reachable"

    docker_args+=("${image}" bash -lc "${bootstrap}
set -euo pipefail
trap 'chown -R \"\${BS_HOST_UID:-0}:\${BS_HOST_GID:-0}\" /work/build /work/dist 2>/dev/null || true' EXIT
./build.sh \"\${CONTAINER_INNER_TARGET}\" --in-container \${CONTAINER_INNER_ARGS}
")

    run docker "${docker_args[@]}"
}

# ── Linux: container ────────────────────────────────────────────────────────
# The container build exists for one reason: the glibc floor. Building on a
# newer host produces a binary that refuses to start on older LTS releases.
container_bootstrap() {
    cat <<'BOOTSTRAP'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq
    # Build essentials. Note: no spdlog/zstd/json/cli11 packages — those are
    # pinned and built from source by cmake/Dependencies.cmake.
    apt-get install -y -qq --no-install-recommends \
        build-essential g++ gcc git perl pkg-config ca-certificates \
        libssl-dev zlib1g-dev \
        curl wget ninja-build python3 python3-dev python3-venv xz-utils file >/dev/null
    # Ubuntu 22.04 defaults to gcc-11, which lacks complete C++23 support.
    if apt-cache show g++-12 >/dev/null 2>&1; then
        apt-get install -y -qq --no-install-recommends g++-12 gcc-12 >/dev/null
        export CC=gcc-12 CXX=g++-12
    fi
elif command -v pacman >/dev/null 2>&1; then
    pacman -Sy --noconfirm --needed base-devel cmake ninja git perl python python-pytest openssl zstd >/dev/null
fi

# CMake floor: this project requires >= 3.25. Install the pinned binary when
# the distro's own CMake is older.
if ! cmake --version 2>/dev/null | head -1 | awk '{print $3}' | \
      awk -F. '{ exit !($1 > 3 || ($1 == 3 && $2 >= 25)) }'; then
    echo "==> installing pinned CMake __CMAKE_VERSION__"
    tmp="$(mktemp -d)"
    curl -fsSL -o "$tmp/cmake.tgz" \
      "https://github.com/Kitware/CMake/releases/download/v__CMAKE_VERSION__/cmake-__CMAKE_VERSION__-linux-x86_64.tar.gz"
    echo "__CMAKE_SHA256__  $tmp/cmake.tgz" | sha256sum -c - >/dev/null
    mkdir -p /opt/cmake
    tar xzf "$tmp/cmake.tgz" -C /opt/cmake --strip-components=1
    ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake
    ln -sf /opt/cmake/bin/ctest  /usr/local/bin/ctest
    rm -rf "$tmp"
fi
cmake --version | head -1
BOOTSTRAP
}

build_linux_container() {
    local image="$1"

    # A container is a cross-compile environment, not a test environment. This
    # suite opens real sockets and binds real ports; run inside a stripped
    # container at high parallelism it flakes (a different set fails each run)
    # while the same commit passes natively. So: test on the host when the host
    # can, and use the container only to produce the portable artifact.
    if [[ "${DO_TESTS}" == "yes" && "${HOST_KIND}" == "linux" \
          && "${BS_CONTAINER_TESTS:-no}" != "yes" ]]; then
        log "tests (native host) — the container only produces the artifact"
        local tdir; tdir="$(build_dir_for "test")"
        DO_TESTS="yes" cmake_configure_build "${BS_ROOT}" "${tdir}"
        run_ctest "${tdir}"
        # The artifact build must not re-run the suite.
        local saved="${DO_TESTS}"; DO_TESTS="no"
        build_in_container "${image}" linux ""
        DO_TESTS="${saved}"
    else
        build_in_container "${image}" linux ""
    fi

    stage_artifact "${BS_BUILD_ROOT}/container-$(suffix_for_arch "${ARCH}")/bridgesessions" \
                   "bridgesessions-linux-$(suffix_for_arch "${ARCH}")"
}

# ── macOS ───────────────────────────────────────────────────────────────────
build_macos() {
    [[ "${HOST_KIND}" == "macos" ]] || die "macOS builds must run on macOS"
    local arch; arch="$(suffix_for_arch "$(uname -m)")"
    if [[ "${ARCH}" != "$(uname -m)" ]]; then arch="$(suffix_for_arch "${ARCH}")"; fi
    local build_dir; build_dir="$(build_dir_for "macos-${arch}")"

    # A Homebrew OpenSSL produces a binary that links
    # /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib, which does not exist on a
    # machine without Homebrew. Build OpenSSL statically for a portable
    # artifact unless the caller asked for the system one explicitly.
    if [[ "${OPENSSL_EXPLICIT}" != "yes" && "${OPENSSL_MODE}" == "system" ]]; then
        OPENSSL_MODE="fetch"
        note "macOS: building OpenSSL from source for a portable artifact"
    fi
    if [[ "${OPENSSL_MODE}" == "fetch" ]]; then
        EXTRA_CMAKE_ARGS+=("-DBS_STATIC_DEPS=ON")
    fi

    cmake_configure_build "${BS_ROOT}" "${build_dir}"
    run_ctest "${build_dir}"

    # Stage first, then sign. Stripping after signing invalidates the signature
    # and the kernel kills the process (SIGKILL, "Killed: 9").
    stage_artifact "${build_dir}/bridgesessions" "bridgesessions-macos-${arch}"

    if [[ "${PRINT_ONLY}" != "yes" ]]; then
        # Ad-hoc sign so the binary runs without a Gatekeeper prompt. Release
        # signing and notarization are separate (scripts/sign-macos.sh).
        codesign --force --sign - "${OUT_DIR}/bridgesessions-macos-${arch}" 2>/dev/null \
            || note "ad-hoc codesign skipped"
        codesign --verify --strict "${OUT_DIR}/bridgesessions-macos-${arch}" 2>/dev/null \
            && note "ad-hoc signature valid" \
            || note "ad-hoc signature NOT valid"
    fi
}

# ── Windows (mingw-w64 cross) ───────────────────────────────────────────────
# OpenSSL is the one dependency that cannot be resolved from the build host:
# a Linux libssl would link a Linux binary. scripts/ci-win-deps.sh builds a
# self-contained static mingw prefix (OpenSSL, fmt, spdlog, zstd, CLI11).
# Everything else cross-compiles from source via cmake/Dependencies.cmake.
mingw_prefix() {
    if [[ -n "${BS_WIN_PREFIX:-}" ]]; then echo "${BS_WIN_PREFIX}"; return; fi
    if [[ -d /opt/bs-win/include && -f /opt/bs-win/lib/libssl.a ]]; then
        echo /opt/bs-win; return
    fi
    if [[ -d "${HOME}/bs-win/include" && -f "${HOME}/bs-win/lib/libssl.a" ]]; then
        echo "${HOME}/bs-win"; return
    fi
    echo ""
}

build_windows() {
    # Prefer a native mingw when it is C++23-capable. Otherwise fall back to the
    # ubuntu:24.04 container, whose mingw is new enough. --distro native forces
    # the host toolchain; any other --distro forces the container.
    local native_ok="no"
    mingw_is_cxx23_capable && native_ok="yes"

    local use_container="no"
    if [[ "${IN_CONTAINER}" != "yes" ]]; then
        if [[ "${DISTRO}" == "native" ]]; then
            use_container="no"
        elif [[ -n "${DISTRO}" ]]; then
            use_container="yes"
        elif [[ "${native_ok}" == "no" ]]; then
            use_container="yes"
        fi
    fi

    if [[ "${use_container}" == "yes" && "${IN_CONTAINER}" != "yes" ]]; then
        build_in_container "${DISTRO:-${BS_WIN_DISTRO}}" windows "${MINGW_BOOTSTRAP}"
        stage_artifact "${BS_BUILD_ROOT}/windows-x86_64/bridgesessions.exe" \
                       "bridgesessions-windows-x86_64.exe"
        if [[ "${PRINT_ONLY}" != "yes" ]] && have x86_64-w64-mingw32-objdump; then
            note "DLL imports (expect OS DLLs only):"
            x86_64-w64-mingw32-objdump -p "${OUT_DIR}/bridgesessions-windows-x86_64.exe" 2>/dev/null \
                | grep "DLL Name" | sed 's/^/      /' || true
        fi
        return 0
    fi

    if ! mingw_is_cxx23_capable; then
        local have_v; have_v="$(x86_64-w64-mingw32-g++ -dumpversion 2>/dev/null || echo none)"
        die "mingw-w64 GCC ${have_v} cannot compile C++23 (need >= ${BS_MINGW_MIN_GCC}).
Rebuild inside the container instead:
    ./build.sh windows --distro ${BS_WIN_DISTRO}
Or install a newer mingw-w64 on this host."
    fi
    note "mingw: $(x86_64-w64-mingw32-g++ --version | head -1)"

    local prefix; prefix="$(mingw_prefix)"
    if [[ -z "${prefix}" ]]; then
        log "building the static mingw dependency prefix (first run only)"
        local target_prefix="${BS_ROOT}/build/mingw-prefix"
        run bash "${BS_ROOT}/scripts/ci-win-deps.sh" "${target_prefix}" \
            || die "mingw dependency prefix build failed"
        prefix="${target_prefix}"
    fi
    note "mingw prefix: ${prefix}"

    # Windows version resource (ProductName/FileVersion generated from VERSION).
    if [[ -f "${BS_ROOT}/scripts/gen-windows-version-rc.sh" ]]; then
        run bash -c "cd '${BS_ROOT}' && bash scripts/gen-windows-version-rc.sh" >/dev/null 2>&1 || \
            note "version resource generation skipped"
    fi

    local build_dir; build_dir="$(build_dir_for "windows-x86_64")"
    local -a args=(-S "${BS_ROOT}" -B "${build_dir}"
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
        -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
        "-DCMAKE_FIND_ROOT_PATH=${prefix}"
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
        "-DOPENSSL_ROOT_DIR=${prefix}"
        "-DCMAKE_PREFIX_PATH=${prefix}"
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        # spdlog/zstd/json/CLI11 cross-compile cleanly from source; using the
        # prefix for them would couple us to its exact spdlog/fmt pair.
        -DBS_DEPS_MODE=fetch
        -DBS_OPENSSL=system
        -DBUILD_TESTING=OFF)
    log "configure windows (mingw-w64, static)"
    run cmake "${args[@]}"
    log "build windows"
    run cmake --build "${build_dir}" --parallel "${JOBS}"
    stage_artifact "${build_dir}/bridgesessions.exe" "bridgesessions-windows-x86_64.exe"
    if [[ "${PRINT_ONLY}" != "yes" ]] && have x86_64-w64-mingw32-objdump; then
        note "DLL imports (expect OS DLLs only):"
        x86_64-w64-mingw32-objdump -p "${OUT_DIR}/bridgesessions-windows-x86_64.exe" 2>/dev/null \
            | grep "DLL Name" | sed 's/^/      /' || true
    fi
}

# ── Aggregate targets ───────────────────────────────────────────────────────
build_all() {
    local did=0
    if host_can linux; then
        if [[ -n "${DISTRO}" && "${DISTRO}" != "native" ]]; then
            build_linux_container "${DISTRO}"
        elif [[ "${HOST_KIND}" == "linux" ]]; then
            build_linux_native
        else
            build_linux_container "${BS_DEFAULT_DISTRO}"
        fi
        did=$((did+1))
    else
        warn "skipping linux: no Linux host and no usable docker"
    fi
    if host_can macos; then build_macos; did=$((did+1)); else warn "skipping macos: host is ${HOST_KIND}"; fi
    if host_can windows; then build_windows; did=$((did+1)); else warn "skipping windows: mingw-w64 not found"; fi
    [[ "${did}" -gt 0 ]] || die "this host cannot build any target"
}

do_package() {
    build_all
    [[ "${PRINT_ONLY}" == "yes" ]] && return 0
    log "staging source archives"
    mkdir -p "${OUT_DIR}"
    local ver; ver="$(tr -d '[:space:]' < "${BS_ROOT}/VERSION")"
    run git -C "${BS_ROOT}" archive --format=tar.gz \
        --prefix="bridgesessions-${ver}/" -o "${OUT_DIR}/bridgesessions-${ver}-source.tar.gz" HEAD
    run git -C "${BS_ROOT}" archive --format=zip \
        --prefix="bridgesessions-${ver}/" -o "${OUT_DIR}/bridgesessions-${ver}-source.zip" HEAD
    log "checksums"
    ( cd "${OUT_DIR}" && shasum -a 256 ./* 2>/dev/null | sort -k2 > SHA256SUMS || \
      sha256sum ./* | sort -k2 > SHA256SUMS )
    ok "packaged into ${OUT_DIR}"
    ( cd "${OUT_DIR}" && ls -1 )
}

do_clean() {
    log "clean"
    local dirs=()
    for d in "${BS_BUILD_ROOT}"/linux-* "${BS_BUILD_ROOT}"/macos-* \
             "${BS_BUILD_ROOT}"/windows-* "${BS_BUILD_ROOT}"/container-* \
             "${BS_BUILD_ROOT}"/mingw-prefix "${BS_BUILD_ROOT}"/test; do
        [[ -e "${d}" ]] && dirs+=("${d}")
    done
    if [[ ${#dirs[@]} -eq 0 ]]; then
        ok "clean — nothing to remove"
        return 0
    fi
    if [[ "${PRINT_ONLY}" == "yes" ]]; then
        note "would remove: ${dirs[*]}"
        return 0
    fi

    # Container builds run as root and can leave files the host user cannot
    # delete. Fall back to a root container for those trees instead of failing
    # with a wall of "Permission denied".
    local removed=0 failed=0
    for d in "${dirs[@]}"; do
        if rm -rf "${d}" 2>/dev/null; then
            removed=$((removed+1))
        else
            failed=$((failed+1))
        fi
    done
    if [[ "${failed}" -gt 0 ]] && have docker && docker info >/dev/null 2>&1; then
        note "${failed} tree(s) need root; removing via a container"
        local rel=()
        for d in "${dirs[@]}"; do [[ -e "${d}" ]] && rel+=("/w${d#${BS_ROOT}}"); done
        if docker run --rm -v "${BS_ROOT}:/w" alpine:3.20 rm -rf "${rel[@]}" 2>/dev/null; then
            failed=0
        fi
    fi
    for d in "${dirs[@]}"; do
        [[ -e "${d}" ]] && warn "could not remove ${d} (owned by root? try: sudo rm -rf)"
    done
    ok "clean"
}

do_deps() {
    cat <<EOF
${C_BOLD}Pinned dependencies${C_RESET} (cmake/Dependencies.cmake)

  spdlog          v1.15.3    built from source, bundled fmt, static
  nlohmann/json   v3.11.3    header-only
  CLI11           v2.4.2     header-only
  zstd            v1.5.6     static
  Catch2          v3.8.0     tests only
  OpenSSL         openssl-3.0.16   system by default (--openssl fetch for static)

  Mode:    --deps fetch|system|auto     (default: fetch)
  OpenSSL: --openssl system|fetch       (default: system)

  Container toolchain: CMake ${BS_CMAKE_VERSION} (sha256 ${BS_CMAKE_SHA256:0:16}…)
  Release Linux image: ${BS_DEFAULT_DISTRO} (glibc 2.35 floor)
EOF
}

# ── Dispatch ────────────────────────────────────────────────────────────────
case "${TARGET}" in
    linux)
        if [[ -n "${DISTRO}" && "${DISTRO}" != "native" ]]; then
            build_linux_container "${DISTRO}"
        else
            build_linux_native
        fi ;;
    macos)   build_macos ;;
    windows) build_windows ;;
    all)     build_all ;;
    test)    DO_TESTS="yes"
             if [[ "${HOST_KIND}" == "linux" ]]; then
                 local td; td="$(build_dir_for test)"
                 cmake_configure_build "${BS_ROOT}" "${td}"
                 run_ctest "${td}"
             else
                 build_macos
             fi ;;
    package) do_package ;;
    clean)   do_clean ;;
    deps)    do_deps ;;
    *)       die "unknown target: ${TARGET}" ;;
esac

if [[ "${PRINT_ONLY}" != "yes" && "${TARGET}" != "clean" && "${TARGET}" != "deps" ]]; then
    ok "done — artifacts in ${OUT_DIR}"
fi
