# Building BridgeSessions

BridgeSessions is C++23 and builds with either CMake or the single-file
`build.sh`. Prebuilt binaries for Linux (x86_64), Windows (x86_64), and macOS
(arm64) are published with each release tag.

## Prerequisites

| Platform | Toolchain | Notes |
|---|---|---|
| Linux | `g++` ≥ 14 **or** `clang` ≥ 18, `cmake` ≥ 3.25 | Also needs `libssl`, `zstd`, `fmt`, `spdlog`, `libutil`. |
| Windows | MinGW-w64 `x86_64-w64-mingw32-g++` | Cross-compile from Linux, or build natively in MSVC. |
| macOS | Apple `clang` (Xcode command line tools) | arm64 and x86_64. |

### Debian / Ubuntu dependencies

```bash
sudo apt install -y g++ cmake libssl-dev libzstd-dev libfmt-dev libspdlog-dev
```

## Build (the easy way)

The repository root has a POSIX one-command build that produces the
`bridgesessions` binary:

```bash
./build.sh
# → builds ./bridgesessions
./bridgesessions --version   # → 2.0.0
```

The script runs:

```bash
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -o bridgesessions bridgesessions.cpp \
    -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog
```

## Build with CMake (modular tree)

The `bs-client`, `bs-server`, `bs-transport`, and `bs-protocol` subprojects
build via CMake:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

This produces the per-component binaries (`bs-server`, `bs-client`, …) and the
shared libraries under `build/release`.

## Cross-compiling Windows from Linux

```bash
x86_64-w64-mingw32-g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -o bridgesessions.exe bridgesessions.cpp \
    -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog \
    -static -static-libgcc -static-libstdc++
```

## Building on macOS

macOS has no `cmake` in the base install; use `clang` directly (same flags as
`build.sh`):

```bash
clang++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -o bridgesessions bridgesessions.cpp \
    -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog
```

The clipboard bridge and pasteboard integration use Objective-C++ and are
compiled in automatically on Apple platforms.

## Tests

Unit tests live under `tests/` and build against Catch2-style helpers:

```bash
cmake -S . -B build/test -DBUILD_TESTS=ON
cmake --build build/test -j
ctest --test-dir build/test
```

## Troubleshooting

- **`std::expected` / `std::span` not found** — you built with `-std=c++20` or
  lower. BridgeSessions requires **C++23**.
- **Link errors for `ssl`/`zstd`/`fmt`** — install the `-dev` packages listed
  under Prerequisites.
- **`fmt` / `spdlog` ABI mismatch** — ensure the system `libfmt` and `libspdlog`
  are from the same distro snapshot; mixing sources causes link failures.
