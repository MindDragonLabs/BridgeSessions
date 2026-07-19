# Building BridgeSessions

BridgeSessions is C++23 and builds with either CMake or the single-file
`build.sh`. Prebuilt binaries for Linux (x86_64), Windows (x86_64), and macOS
(arm64) are published with each release tag.

## Prerequisites

| Platform | Toolchain | Notes |
|---|---|---|
| Linux | `g++` ≥ 14 **or** `clang` ≥ 18, `cmake` ≥ 3.25 | Also needs OpenSSL, zstd, fmt, spdlog, CLI11, and nlohmann-json. Catch2 is required for tests. The direct `build.sh` link line uses `libutil` where the platform provides it. |
| Windows | MinGW-w64 `x86_64-w64-mingw32-g++` | Cross-compile from Linux, or build natively in MSVC. |
| macOS | Apple `clang` (Xcode command line tools) | arm64 and x86_64. |

### Debian / Ubuntu dependencies

```bash
sudo apt install -y g++ cmake libssl-dev libzstd-dev libfmt-dev libspdlog-dev \
  libcli11-dev nlohmann-json3-dev catch2
```

The test suite requires Catch2 **3.x**. On distributions whose `catch2` package
is still 2.x, install Catch2 3 from its upstream release before configuring
CMake.

## Build (the easy way)

The repository root has a POSIX one-command build that produces the
`bridgesessions` binary:

```bash
./build.sh
# → builds ./bridgesessions
./bridgesessions --version   # → 2.0.6
```

The script runs:

```bash
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -DBS_VERSION=\"2.0.6\" \
    -o bridgesessions bridgesessions.cpp \
    -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog
```

## Build with CMake (shipping root)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bridgesessions --version   # → 2.0.6
```

`VERSION` is the single source of truth for CLI, CMake, mesh Hello, BridgePanel,
and release scripts.

## Cross-compiling Windows from Linux

Requires MinGW OpenSSL and zstd static libraries plus MinGW-visible CLI11,
nlohmann-json, fmt, and spdlog headers (example cache layout used on the release
host under `~/.cache/bridgesessions-cross/`):

```bash
x86_64-w64-mingw32-g++ -std=c++23 -O2 -DFMT_HEADER_ONLY \
  -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 \
  -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
  -DBS_VERSION=\"2.0.6\" \
  -I"$MINGW_OPENSSL/include" -I"$ZSTD_SRC/lib" \
  -o dist/bridgesessions-windows-x86_64.exe bridgesessions.cpp \
  -L"$MINGW_OPENSSL/lib64" -L"$ZSTD_SRC/lib" \
  -lssl -lcrypto -lws2_32 -lzstd -lgdi32 -luser32 -lcrypt32 \
  -static -static-libgcc -static-libstdc++
```

## Building on macOS (arm64)

```bash
export PATH=/opt/homebrew/bin:$PATH
SSL=$(brew --prefix openssl@3)
# also: zstd fmt spdlog nlohmann-json cli11
clang++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
  -DBS_VERSION=\"2.0.6\" \
  -I"$SSL/include" -I"$(brew --prefix zstd)/include" \
  -I"$(brew --prefix fmt)/include" -I"$(brew --prefix spdlog)/include" \
  -I"$(brew --prefix nlohmann-json)/include" -I"$(brew --prefix cli11)/include" \
  -o bridgesessions-macos-arm64 bridgesessions.cpp \
  -L"$SSL/lib" -L"$(brew --prefix zstd)/lib" \
  -L"$(brew --prefix fmt)/lib" -L"$(brew --prefix spdlog)/lib" \
  -lssl -lcrypto -lzstd -lfmt -lspdlog -pthread
./bridgesessions-macos-arm64 --version   # → 2.0.6
```

Clipboard integration is Windows-only in 2.0.6.

## Tests

Unit tests live under `tests/` and build against Catch2-style helpers:

```bash
cmake -S . -B build/test -DCMAKE_BUILD_TYPE=Debug
cmake --build build/test -j
ctest --test-dir build/test --output-on-failure
```

## Troubleshooting

- **`std::expected` / `std::span` not found** — you built with `-std=c++20` or
  lower. BridgeSessions requires **C++23**.
- **Link errors for `ssl`/`zstd`/`fmt`** — install the `-dev` packages listed
  under Prerequisites.
- **`fmt` / `spdlog` ABI mismatch** — ensure the system `libfmt` and `libspdlog`
  are from the same distro snapshot; mixing sources causes link failures.
