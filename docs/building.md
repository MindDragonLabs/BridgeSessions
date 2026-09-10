# Building

One script builds every platform. It resolves all dependencies itself, so a fresh clone needs only a compiler, CMake 3.25 or newer, git, and network access.

```bash
./build.sh linux --distro ubuntu:22.04    # release Linux artifact (glibc 2.35 floor)
./build.sh linux                          # native build on this host
./build.sh macos                          # macOS (must run on macOS)
./build.sh windows                        # cross-compile with mingw-w64
./build.sh all                            # every target this host can produce
./build.sh package                        # build all, stage dist/ and SHA256SUMS
./build.sh test                           # configure, build, run ctest
./build.sh deps                           # print the pinned dependency set
./build.sh --help                         # every option
```

The version string lives in the `VERSION` file at the repo root. CMake reads that file. Bump `VERSION` before you rebuild a release. Do not hardcode a version in documentation; run `bs --version`.

## Testing

```bash
./build.sh test                        # configure, build, run the full suite
./build.sh linux --distro native       # build + test on the host
./build.sh linux --distro ubuntu:22.04 # tests on the host, artifact in the container
```

The suite opens real sockets, binds real ports, and measures latency. Two consequences:

- **ctest runs on the host, not in the container.** A stripped `ubuntu:22.04` container is a cross-compile environment, not a test environment: there the suite flakes with a different failing set each run, while the same commit passes natively. `build.sh` therefore tests on the host when the host is Linux, then builds the portable artifact in the container with tests off. Force the old behaviour with `BS_CONTAINER_TESTS=yes`.
- **Parallelism is capped at 8 and failures are retried** (`--repeat until-pass:3`). At 64-way the socket and timing tests contend. Retries remove that noise without hiding a real defect: a deterministic failure still fails all three attempts. If a test only passes on retry, treat it as suspect and run it in isolation.

If you suspect a flake, isolate it:

```bash
cd build/<target-dir>
ctest --output-on-failure -R "<test name>"      # run it alone, a few times
```

## Dependencies

They are pinned in [`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake) and built from source by default:

| Dependency | Pin | How |
|---|---|---|
| spdlog | `v1.15.3` | Source, built with its **bundled** fmt, static |
| nlohmann/json | `v3.11.3` | Source, header-only |
| CLI11 | `v2.4.2` | Source, header-only |
| zstd | `v1.5.6` | Source, static |
| Catch2 | `v3.8.0` | Source, tests only |
| OpenSSL | `openssl-3.0.16` | System by default; `--openssl fetch` to build static |

spdlog uses its bundled fmt on purpose. Using a distribution's spdlog drags in an external `libfmt` whose soname differs per distribution — that is how a release binary ended up requiring `libspdlog.so.1` and `libfmt.so.8` and could not load on Arch.

Only `libssl.so.3` and `libcrypto.so.3` are taken from the system. Those sonames are stable across every supported platform.

| Option | Effect |
|---|---|
| `--deps fetch` | Default. Build every dependency from the pins. Reproducible. |
| `--deps system` | Use packages installed on the build host. Fast for development, not reproducible. |
| `--deps auto` | System packages where available, pins otherwise. |
| `--openssl system` | Default. `find_package(OpenSSL)`. |
| `--openssl fetch` | Build OpenSSL from source and link it statically. Slow. |

## Developer build

```bash
./build.sh test                              # configure, build, run the full suite
./build.sh linux --deps system --no-tests    # fastest iteration
```

Or drive CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Distro packages needed for a `--deps system` build:

```bash
# Debian / Ubuntu
apt-get install build-essential cmake ninja-build libssl-dev zstd libzstd-dev \
                libspdlog-dev libfmt-dev nlohmann-json3-dev libcli11-dev catch2

# Arch
pacman -S base-devel cmake ninja openssl zstd spdlog fmt nlohmann-json cli11 catch2

# macOS
brew install cmake ninja openssl@3 zstd fmt spdlog cli11 nlohmann-json catch2
```

## Release platforms

| Platform | How | Runtime dependencies |
|---|---|---|
| Linux x86_64 | `./build.sh linux --distro ubuntu:22.04` | glibc 2.35+, `libssl.so.3`, `libcrypto.so.3` |
| macOS arm64 | `./build.sh macos`, then sign | System frameworks only |
| Windows x86_64 | `./build.sh windows` (mingw-w64) | OS DLLs only |

Why the container: the glibc floor comes from the image, not from the build host. A binary built on Ubuntu 24.04 or Arch will not start on a 22.04 host. Building in `ubuntu:22.04` produces one artifact that runs on every supported distribution.

Release builds enable stack protection, fortified libc, PIE/RELRO/NX on Linux, and ASLR/NX/Control Flow Guard where the Windows toolchain supports them.

## Verify an artifact before publishing

```bash
# Linux: no unresolved libraries, and it runs
ldd dist/bridgesessions-linux-x86_64 | grep 'not found' && echo FAIL
dist/bridgesessions-linux-x86_64 --version

# macOS: system libraries only
otool -L dist/bridgesessions-macos-arm64

# Windows: OS DLLs only
x86_64-w64-mingw32-objdump -p dist/bridgesessions-windows-x86_64.exe | grep 'DLL Name'
```

CI runs exactly these checks on every push and pull request, so a non-portable artifact fails before it reaches a release.

`dist/` is local staging. Git ignores it. Publish artifacts through GitHub Releases. See [Release provenance](RELEASE-PROVENANCE.md).

## macOS signing

`scripts/sign-macos.sh` signs a Mach-O with the first **Developer ID Application** identity in the keychain. It refuses ad-hoc signing.

```bash
./scripts/sign-macos.sh build/macos-arm64/bridgesessions dist/bridgesessions-macos-arm64
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

Windows is the one target that cannot be built natively on a Windows machine in this setup. `mingw-w64` is a **Linux-hosted cross-compiler**: it runs on Linux and emits a Windows PE. So "building for Windows" always means "run a Linux toolchain that targets Windows", and *which* Linux toolchain decides whether C++23 compiles at all:

| Linux host | mingw-w64 GCC | `-std=c++23` |
|---|---|---|
| Ubuntu 22.04 | 10.3 | rejected |
| Ubuntu 24.04 | 13.2 | accepted |
| Debian 12 | 12.2 | accepted |
| Arch (rolling) | 16.x | accepted |

This project requires C++23, so a Ubuntu 22.04 container cannot build the Windows binary — that is the entire reason the two targets use different images. The Linux glibc floor is irrelevant for a Windows PE.

`./build.sh windows` picks the toolchain for you:

1. If the host has `x86_64-w64-mingw32-g++` with GCC 13 or newer, build natively. This is the fast path and the toolchain that produced the shipped binary.
2. Otherwise, run the build inside `ubuntu:24.04`.

Force one or the other:

```bash
./build.sh windows                        # auto (native if capable)
./build.sh windows --distro native        # host toolchain, fails loudly if too old
./build.sh windows --distro ubuntu:24.04  # force the container
```

OpenSSL is the one dependency that cannot come from the build host — a Linux `libssl` would produce a Linux binary. `scripts/ci-win-deps.sh` builds a static mingw prefix containing OpenSSL (plus fmt, spdlog, zstd, CLI11); `build.sh` looks for it in `BS_WIN_PREFIX`, then `/opt/bs-win`, then `~/bs-win`, and builds it if missing. It works with apt or pacman.

```bash
bash scripts/ci-win-deps.sh "$PWD/build/mingw-prefix"   # optional; build.sh does this
BS_WIN_PREFIX="$PWD/build/mingw-prefix" ./build.sh windows
```

The prefix is toolchain-pinned: rebuild the dependencies and link the executable with the same mingw distribution. Mixing distributions' mingw builds fails on `__imp__` CRT symbols. Use the **posix** thread variant; the win32 variant fails at link on `<thread>`. Arch ships only the posix variant.

The remaining dependencies cross-compile from source, which keeps spdlog on its bundled fmt. The build pins `WINVER`/`_WIN32_WINNT`/`NTDDI_VERSION` to `0x0A000006`; older mingw 11 header sets gate `HPCON` behind that NTDDI level.

## Release builds on CI

`.github/workflows/ci.yml` builds and tests Linux (22.04 and 24.04), macOS arm64, and Windows mingw on every push and pull request, and scans for secrets. `.github/workflows/release.yml` builds all three, generates source archives and `SHA256SUMS`, and publishes the GitHub release when you push a tag.

## Sanitizers

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
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

## Regenerating the command reference

`docs/cli.md` is generated from the binary, never hand-written:

```bash
python3 scripts/gen-cli-docs.py > docs/cli.md
python3 scripts/gen-cli-docs.py --check     # fails when the file is stale
```
