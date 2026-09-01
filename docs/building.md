# Building

BridgeSessions needs C++23 and CMake 3.25 or newer.

Dependencies: OpenSSL 3, zstd, spdlog/fmt, CLI11, nlohmann-json, and Catch2 3 for tests.

Current release stamp: **`26.09.01-release`**. The version string lives in the `VERSION` file at the repo root. CMake reads that file. Bump `VERSION` before you rebuild a release.

## Developer build

```bash
# macOS
brew install cmake ninja openssl@3 zstd fmt spdlog cli11 nlohmann-json catch2

cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Source-only release build

```bash
cmake -S . -B build/release -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --target bridgesessions --parallel
./build/release/bridgesessions --version
```

Release builds enable stack protection, fortified libc, PIE/RELRO/NX on Linux, and ASLR/NX/Control Flow Guard where the Windows toolchain supports them.

## Release platforms

| Platform | How |
|---|---|
| Linux x86_64 | `scripts/Dockerfile.static-linux` (OpenSSL 3 LTS, static third-party libs). The binary may still link glibc. |
| Windows x86_64 | `scripts/build-windows-mingw.sh` or native MSVC/vcpkg. Imports must be OS DLLs only. |
| macOS arm64 | Native build. Sign with Developer ID. Notarize for Gatekeeper. |

`dist/` is local staging. Git ignores it. Publish artifacts through GitHub Releases. See [Release provenance](RELEASE-PROVENANCE.md).

## macOS signing

`scripts/sign-macos.sh` signs a Mach-O with the first **Developer ID Application** identity in the keychain. It refuses ad-hoc signing.

```bash
./scripts/sign-macos.sh build/bridgesessions dist/bridgesessions-macos-arm64
codesign --verify --strict --verbose=2 dist/bridgesessions-macos-arm64
```

Set `BS_DEV_ID` if more than one identity exists.

If the build Mac has no Developer ID certificate, build there and sign on a Mac that has the certificate. Copy only the unsigned binary and the entitlements. Do not email an unprotected `.p12`.

To keep two signing Macs:

1. On the Mac that already signs, open Keychain Access.
2. Export **Developer ID Application** as a `.p12`. Use a long password.
3. Copy the `.p12` over the mesh or another encrypted path. Do not commit it.
4. On the second Mac: `security import cert.p12 -k ~/Library/Keychains/login.keychain-db -T /usr/bin/codesign`.
5. Confirm with `security find-identity -v -p codesigning`.
6. Delete the `.p12` from both disks after the import.

You also need the Apple WWDR intermediate. The private key never belongs in git.

`scripts/install-local-macos.sh` installs a local build to `~/.local/bin` and signs it. An unsigned or ad-hoc local install can die with SIGKILL (exit 137).

## Windows cross-compile

One command bootstraps everything (hermetic, no host packages touched):

```bash
bash scripts/ci-win-deps.sh    # builds the static MinGW prefix once
./scripts/build-windows-mingw.sh
file dist/bridgesessions-windows-x86_64.exe
```

`ci-win-deps.sh` compiles OpenSSL 3.5.7, zstd 1.5.7, fmt 12.2.0, spdlog 1.17.0,
CLI11 2.7.2, and nlohmann-json 3.12.0 as static libraries into a prefix, then
checks completeness. The prefix is toolchain-pinned: rebuild deps and link the
exe with the same MinGW (mixing distributions' mingw builds fails on
`__imp__` crt symbols).

The build script looks for the prefix in `BS_WIN_PREFIX`, then `/opt/bs-win`,
then `~/bs-win`. It pins `WINVER`/`_WIN32_WINNT`/`NTDDI_VERSION` to
`0x0A000006` (older mingw 11 header sets gate `HPCON` behind that NTDDI level).

## Release builds on CI

Release binaries for all three platforms are built by GitHub-hosted runners:
`.github/workflows/release-builds.yml`. It runs on `workflow_dispatch` and
`v*` tags only — fork PRs never execute it and never see signing secrets.
Artifacts carry the exact names the release pipeline stages. Local
`scripts/Dockerfile.static-linux`, `scripts/build-windows-mingw.sh`, and
native macOS builds remain the developer/testing lane.

## Sanitizers

```bash
cmake -S . -B build/asan -GNinja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/asan --parallel 2
ctest --test-dir build/asan --output-on-failure
```

## Public-tree scan

```bash
bash scripts/prepublish-scan.sh
```

The scan must pass before you publish. It blocks private addresses, key material, and operator names from a local blocklist.
