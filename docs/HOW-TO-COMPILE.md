# HOW-TO-COMPILE.md — Portable static BridgeSessions (all 3 platforms)

Authoritative recipe for producing **portable static** BridgeSessions binaries that
run on fleet hosts without missing-library errors. The naive `cmake`/`build.sh` build
links spdlog/zstd/OpenSSL/fmt **dynamically** and targets the builder's glibc — it
breaks on older hosts (e.g. test-pc4: `GLIBC_2.38 not found`, `libspdlog missing`). These
recipes eliminate that by statically linking every app dependency.

**Version stamp**: edit `VERSION` in the repo root (single source of truth). All recipes
below embed it via `-DBS_VERSION=...` (CMake does this automatically; the direct `g++`
lines must pass it).

## Dependency truth table

| Dep | Static build | Notes |
|-----|--------------|-------|
| OpenSSL | static `libssl.a`/`libcrypto.a` | Linux→`/opt/ossl`; macOS→`~/local`; Win→`~/bs-win/lib64` |
| zstd | static `libzstd.a` | `ZSTD_BUILD_SHARED=OFF` |
| fmt | static `libfmt.a` | `BUILD_SHARED_LIBS=OFF` |
| spdlog | static `libspdlog.a` | `BUILD_SHARED_LIBS=OFF`; needs fmt in prefix |
| nlohmann_json | **header-only** | no build; just include path (fetch single header) |
| CLI11 | **header-only** | no build; just include path (fetch single header) |
| Catch2 | static (v3) | only needed if configuring via CMake; tests optional |
| C++ std lib | Linux/Win: `-static-libstdc++`/`-static`; macOS: system `libc++` (always present) |

> glibc on Linux MUST stay dynamic (required for DNS/nsswitch). Only the app deps are
> static. Windows PE with `-static` pulls in libgcc/winpthread/stdc++ — fully self-contained
> except OS DLLs (KERNEL32/USER32/WS2_32/ADVAPI32/CRYPT32 + UCRT, which ship with Win10+).

---

## Linux x86_64 — `ubuntu:22.04` container (glibc 2.35 floor)

Build inside a container so the binary targets glibc **2.35** (runs on test-pc4/Ubuntu 22.04
through current Arch). The Dockerfile is **versioned at `scripts/Dockerfile.static-linux`**
(it previously lived at `/tmp/bs-static/Dockerfile` on test-pc1 and was lost once — do not
move it back out of git). Build from the repo root:

```bash
docker build -f scripts/Dockerfile.static-linux -t bs-static-builder .
docker create --name x bs-static-builder
docker cp x:/src/build/bridgesessions dist/bridgesessions-linux-x86_64
docker rm x
```

To rebuild against **current source without re-baking the image**, mount the repo over
`/src` and build in a side dir (keeps your host `build/` untouched):

```bash
docker run --rm --user $(id -u):$(id -g) -v "$PWD":/src -w /src bs-static-builder bash -c \
  'cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_PREFIX_PATH="/opt/ossl;/opt/zstd;/opt/fmt;/opt/spdlog;/opt/catch2" \
     -DOPENSSL_ROOT_DIR=/opt/ossl -DZSTD_ROOT=/opt/zstd -DSPDLOG_ROOT=/opt/spdlog \
     -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
     -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
     -B build-static . && cmake --build build-static -j"$(nproc)" --target bridgesessions'
cp build-static/bridgesessions dist/bridgesessions-linux-x86_64
```

Container essentials (traps baked in):
- ubuntu:22.04 ships CMake 3.22 but `CMakeLists.txt` needs ≥ 3.25 → install CMake 3.28 from
  the Kitware tarball.
- Default `g++` is 11 (no `<expected>`); install **gcc-13** via the toolchain PPA. The PPA
  GPG key must be fetched over HTTPS (`keyserver.ubuntu.com` import fails in a sandbox) and
  apt resolves the cross-deps — do **not** install gcc-13 from bare `.deb` files (missing
  `libstdc++6` upgrade breaks it).
- Catch2 v3 tarball: `https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.0.tar.gz`
  (the `releases/download/.../Catch2-3.8.0.tar.gz` path **404s**).
- OpenSSL `./Configure linux-x86_64 no-shared no-tests`; zstd/fmt/spdlog cmake
  `BUILD_SHARED_LIBS=OFF` (spdlog gets `-DCMAKE_PREFIX_PATH=/opt/fmt`).
- **Link**: `-DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"`. WITHOUT this,
  test-pc4 dies with `GLIBCXX_3.4.32 not found`.
- Add a `.dockerignore` excluding `build/` (a stale host `CMakeCache.txt` poisons configure).
- Result check: `ldd dist/bridgesessions-linux-x86_64` → only `libc.so.6` + `ld-linux`.

---

## macOS arm64 — native on test-pc5 (no brew, no sudo)

test-pc5 has Apple clang 17 + git + network, but **no cmake, no brew, no sudo**. Drop a
portable CMake, build static deps into `~/local`, compile.

```bash
# 1) portable CMake (no sudo)
curl -fsSL -o cmake.tar.gz https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3-macos-universal.tar.gz
tar xf cmake.tar.gz && mv cmake-3.28.3-macos-universal ~/cmake
export PATH="$HOME/cmake/CMake.app/Contents/bin:$PATH"
export CC=clang CXX=clang++

# 2) header-only deps (nlohmann_json, CLI11) — fetch single headers
mkdir -p ~/local/include
curl -fsSL -o ~/local/include/nlohmann_json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
curl -fsSL -o ~/local/include/CLI11.hpp https://github.com/CLIUtils/CLI11/raw/v2.4.2/single-include/CLI11.hpp

# 3) static libs (OpenSSL/zstd/fmt/spdlog) — each: configure + make install to ~/local
#    OpenSSL: ./Configure darwin64-arm64-cc no-shared no-tests --prefix=$HOME/local
#    zstd:    cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -DZSTD_BUILD_SHARED=OFF -DCMAKE_INSTALL_PREFIX=$HOME/local
#    fmt:     cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=$HOME/local
#    spdlog:  cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH=$HOME/local -DCMAKE_INSTALL_PREFIX=$HOME/local

# 4) configure + build
cd ~/bridgesessions && rm -rf build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_PREFIX_PATH="$HOME/local" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
      -DOPENSSL_ROOT_DIR=$HOME/local -DZSTD_ROOT=$HOME/local -DSPDLOG_ROOT=$HOME/local \
      -B build .
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Traps:
- **Do NOT pass `-GNinja`** — test-pc5 has no Ninja; default Unix Makefiles only. A stale
  `CMakeCache.txt` from a failed Ninja run will keep erroring — `rm -rf build` first.
- Result: `file build/bridgesessions` → `Mach-O 64-bit arm64`; `otool -L` → only
  `/usr/lib/libc++.1.dylib` + `/usr/lib/libSystem.B.dylib` (both always present).

---

## Windows x86_64 PE — MinGW cross-compile on test-pc1 (no sshd on target)

test-pc1 has `x86_64-w64-mingw32-g++`. Build static deps into `~/bs-win`, then compile
**`main.cpp` directly** with `g++` (avoids CMake `find_package` IMPORTED_IMPLIB
pain on Windows cross-builds). Post-refactor the product TU is `main.cpp` (which includes
`bs-protocol.h` → `bs-session.h`); the pre-refactor `bridgesessions.cpp` monolith is a
7-line stub — do NOT feed it to the compiler.

Include shim: the source wants `<CLI/CLI.hpp>` and `<nlohmann/json.hpp>`, but `~/bs-win/include`
has a flat `CLI11.hpp` plus an `nlohmann/` tree. One-time:

```bash
mkdir -p /tmp/bs-win-shim/CLI && ln -sf ~/bs-win/include/CLI11.hpp /tmp/bs-win-shim/CLI/CLI.hpp
```

```bash
TRIPLE=x86_64-w64-mingw32
PREFIX=$HOME/bs-win
# static deps (each cmake with -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C/CXX_COMPILER=$TRIPLE-{gcc,g++}):
#   zstd:   -DZSTD_BUILD_SHARED=OFF
#   fmt:    -DBUILD_SHARED_LIBS=OFF
#   spdlog: -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH=$PREFIX
#   OpenSSL: ./Configure mingw64 no-shared no-tests --cross-compile-prefix=$TRIPLE-
#            → installs to $PREFIX/lib64 (NOT lib/)
# header-only nlohmann_json/CLI11: drop json.hpp / CLI11.hpp into $PREFIX/include

mkdir -p build-win
$TRIPLE-g++ -static -std=c++23 -O3 -DNDEBUG \
  -DBS_VERSION=\"$(cat VERSION)\" -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
  -isystem $PREFIX/include -isystem /tmp/bs-win-shim \
  main.cpp -o build-win/bridgesessions.exe \
  $PREFIX/lib/libspdlog.a $PREFIX/lib/libfmt.a \
  $PREFIX/lib64/libssl.a $PREFIX/lib64/libcrypto.a $PREFIX/lib/libzstd.a \
  -lpthread -lws2_32 -lcrypt32 -lgdi32 -luser32
```

Traps:
- **`-static` is mandatory** → pulls in libgcc/winpthread/stdc++ (no `libstdc++-6.dll` etc).
- OpenSSL lands in **`lib64/`**, not `lib/` — link `$PREFIX/lib64/lib{ssl,crypto}.a`.
- The CMake `find_package` config paths expect `.dll.a` implibs and emit `IMPORTED_IMPLIB`
  errors for static-only installs — that's why we link the `.a` files directly instead of
  going through CMake's executable target.
- Result: `$TRIPLE-objdump -p build-win/bridgesessions.exe | grep "DLL Name"` → only
  KERNEL32/USER32/WS2_32/ADVAPI32/CRYPT32 + `api-ms-win-crt-*` (OS-provided).

### Shipping to test-pc7 (Win11, no sshd)

test-pc7 has **no SSH server** — use **WinRM** (port 5985, NTLM; credentials
recorded locally in test-pc1 `~/.ssh/config` / vault — never commit them). Host a temp HTTP server on test-pc1's
Tailscale IP, then `curl.exe` it from a WinRM PowerShell session:

```bash
# test-pc1: serve the PE (bind TS IP only)
cd ~/bridgesessions/dist && python3 -m http.server 8800 --bind <test-pc1-tailscale-ip> &
# WinRM push (run from test-pc1):
python3 - <<'PY'
import winrm
s = winrm.Session('<windows-host-ip>', auth=('shadow','<winrm-password>'), transport='ntlm')
ps = (r'$url="http://<test-pc1-tailscale-ip>:8800/bridgesessions-windows-x86_64.exe";'
      r'$d="$env:USERPROFILE\.local\bin\bridgesessions.exe";'
      r'New-Item -ItemType Directory -Force -Path (Split-Path $d) | Out-Null;'
      r'curl.exe -fL -o $d $url; & $d --version')
print(s.run_ps(ps).std_out.decode(errors='replace'))
PY
kill %1   # stop the temp server
```

---

## Verify + publish

```bash
# Linux
ldd dist/bridgesessions-linux-x86_64        # only libc.so.6 + ld-linux
# macOS
otool -L dist/bridgesessions-macos-arm64     # only libc++.1.dylib + libSystem
# Windows
x86_64-w64-mingw32-objdump -p dist/bridgesessions-windows-x86_64.exe | grep "DLL Name"
# all
dist/<bin> --version                         # → 2.0.7-alpha2
```

Binaries live in `dist/`. The named release binaries are committed to git (`.gitignore`
ignores only the dev `bridgesessions.exe` + `*.o`/`*.obj`); `SHA256SUMS`/`SBOM` are
gitignored by design (the downloader regenerates them). After changing `dist/`:

```bash
git add -f dist/bridgesessions-{linux-x86_64,macos-arm64,windows-x86_64.exe}
git commit -m "dist: rebuild portable static binaries"
git tag -f v2.0.7-alpha2 HEAD
git push --force codeberg main && git push --force codeberg v2.0.7-alpha2
```
