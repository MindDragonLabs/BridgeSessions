# Building

BridgeSessions requires C++23 and CMake 3.25+.

Dependencies: OpenSSL 3, zstd, spdlog/fmt, CLI11, nlohmann-json, and Catch2 3 for tests.

```bash
# macOS
brew install cmake ninja openssl@3 zstd fmt spdlog cli11 nlohmann-json catch2

cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Source-only release build:

```bash
cmake -S . -B build/release -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --target bridgesessions --parallel
```

Release builds enable stack protection, fortified libc, PIE/RELRO/NX on Linux and ASLR/NX/Control Flow Guard where supported on Windows.

## Sanitizers

```bash
cmake -S . -B build/asan -GNinja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/asan --parallel 2
ctest --test-dir build/asan --output-on-failure
```

## Release platforms

- Linux x86_64: `scripts/Dockerfile.static-linux` (OpenSSL 3.5 LTS, supported dependencies)
- Windows x86_64: `scripts/build-windows-mingw.sh` or native MSVC/vcpkg
- macOS arm64: native static dependency build, Developer ID sign, notarize

`dist/` is ignored local staging. Generated artifacts belong in GitHub Releases, not commits. See [Release provenance](RELEASE-PROVENANCE.md).
