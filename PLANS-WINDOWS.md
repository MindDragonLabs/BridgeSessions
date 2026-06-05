# bridgesessions — Windows Native Port Plan

**Branch:** v0.5.1 → v0.6.0-windows
**Target:** Windows 11 x64, MSVC 2022, Win32 APIs
**Constraint:** No MinGW, WSL, POSIX emulation, or cross-compilation
**Language:** C++23 (`/std:c++latest`)
**Build:** CMake 4.x + Ninja + vcpkg

---

## Architecture

```
bs-protocol/   → libbsprotocol.lib    (20 message types, zstd codec)     ← portable C++23
bs-transport/  → libbstransport.lib   (TLS 1.3, frame I/O)              ← OpenSSL only
bs-client/     → bs-client.exe        (Win32 Console relay agent)        ← PORT NEEDED
bs-server/     → bs-server.exe        (ConPTY session daemon)            ← PORT NEEDED
```

Two binaries, three static libs. `bs-protocol` and `bs-transport` are portable. `bs-client` and `bs-server` need POSIX→Win32 API replacement throughout.

---

## Phases

### W0: Toolchain & Dependencies (1 hour)

**Install MSVC Build Tools 2026:**
```cmd
winget install Microsoft.VisualStudio.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26200 --includeRecommended"
```

**Install CMake:**
```cmd
winget install Kitware.CMake
```

**Install vcpkg + all deps:**
```cmd
git clone https://github.com/microsoft/vcpkg C:\vcpkg
cd C:\vcpkg && .\bootstrap-vcpkg.bat
.\vcpkg install openssl:x64-windows zstd:x64-windows nlohmann-json:x64-windows cli11:x64-windows spdlog:x64-windows catch2:x64-windows
```

**Verify:**
```cmd
where cl.exe && cmake --version && ninja --version && vcpkg list
```

### W1: CMake Config + bs-protocol (1 hour)

- Add MSVC preset to CMakePresets.json
- Add `/std:c++latest` compile flag for MSVC
- Compile bs-protocol + tests
- Run tests — expected: 18/18 pass

### W2: bs-transport (1–2 hours)

- Add Winsock2 includes where sockets created (main.cpp only — frame_io.cpp uses pure OpenSSL)
- Link `ws2_32.lib`
- Compile bs-transport + tests
- Verify TLS 1.3 handshake with self-signed certs works on Windows

### W3: bs-client (3–5 days)

Core relay port — the main effort.

**Files to port/replace:**

| File | POSIX | Win32 |
|------|-------|-------|
| `terminal_raw.cpp` | `termios`, `ioctl(TIOCGWINSZ)` | `SetConsoleMode`, `GetConsoleScreenBufferInfo` |
| `clipboard_linux.cpp` | `xclip`/`wl-paste` via `popen` | `OpenClipboard`/`GetClipboardData` (new `clipboard_windows.cpp`) |
| `image_render.cpp` | `fork`/`execl`/`waitpid`/`mkstemp` | `CreateProcess` or stub |
| `main.cpp` | `poll()`, `signal()`, `sys/socket.h` | Two-thread relay + Winsock2 + `SetConsoleCtrlHandler` |

**Key design: Two-thread relay**

On POSIX, `poll()` handles stdin + socket in one call because both are fds. On Windows, console stdin is NOT a socket — can't poll it. Solution: split into two threads.

```
Thread 1 (network → stdout):
  WSAPoll(socket) → SSL_read_ex → write_frame → WriteConsole

Thread 2 (stdin → network):
  ReadConsole → write_frame(ssl)  [mutex-guarded]

Main thread:
  Clipboard poll timer, keepalive, CtrlHandler, SIGWINCH monitor
```

### W4: bs-server (3–5 days, OPTIONAL)

Only needed if server runs on Windows. Otherwise run bs-server on Linux.

| File | POSIX | Win32 |
|------|-------|-------|
| `pty.cpp` | `posix_openpt`, `fork`, `execvp` | `CreatePseudoConsole` (ConPTY), `CreateProcess` |
| `session.cpp` | `kill`, `waitpid`, `usleep` | `TerminateProcess`, `WaitForSingleObject`, `Sleep` |
| `session_manager.cpp` | `waitpid(WNOHANG)` | `WaitForSingleObject(pi.hProcess, 0)` |
| `persistence.hpp` | `::unlink`, `::rename` | `std::filesystem::remove`/`rename` |
| `keygen.cpp` | `mkdir`, `chmod`, `$HOME` | `_mkdir`, `_chmod`, `$USERPROFILE` |
| `main.cpp` | `poll()`, `signal()`, BSD sockets | `WSAPoll`, `SetConsoleCtrlHandler`, Winsock2 |

### W5: Integration Testing (1–2 days)

- Windows bs-client ↔ Linux bs-server over TCP 9948
- Keystroke relay, terminal output, resize
- Reconnection with backoff
- Clipboard (text only for W5)
- Pipe mode (`--pipe` flag)

---

## Tooling Quickstart

```cmd
REM One-shot: install everything
winget install Microsoft.VisualStudio.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26200 --includeRecommended"
winget install Kitware.CMake
git clone https://github.com/microsoft/vcpkg C:\vcpkg
cd C:\vcpkg & .\bootstrap-vcpkg.bat & .\vcpkg install openssl:x64-windows zstd:x64-windows nlohmann-json:x64-windows cli11:x64-windows spdlog:x64-windows catch2:x64-windows

REM Build
cd C:\SFTP\agent\bridgesessions
cmake --preset windows-msvc-debug
cmake --build build/windows-msvc-debug
ctest --test-dir build/windows-msvc-debug --output-on-failure
```

---

## Per-File Porting Map

### Portable (no changes)

| File | Reason |
|------|--------|
| `bs-protocol/include/bsprotocol/message.hpp` | Pure C++23 types, no OS deps |
| `bs-protocol/include/bsprotocol/codec.hpp` | Pure C++23 interface |
| `bs-protocol/src/codec.cpp` | OpenSSL + zstd only (both portable) |
| `bs-protocol/tests/message_test.cpp` | Catch2 only |
| `bs-protocol/tests/codec_test.cpp` | Catch2 only |
| `bs-transport/include/bstransport/tls.hpp` | OpenSSL types only |
| `bs-transport/include/bstransport/frame_io.hpp` | OpenSSL + protocol types |
| `bs-transport/src/frame_io.cpp` | SSL_read_ex/SSL_write_ex only — no sockets |
| `bs-transport/src/tls.cpp` | OpenSSL + X.509 — platform-independent crypto |
| `bs-server/src/ring_buffer.hpp` | Pure C++ template |
| `bs-server/src/osc52_capture.hpp` | Pure string scanning, no OS deps |
| `bs-server/src/logging.hpp` | spdlog (portable) + `getenv` |
| `bs-client/src/host_config.hpp` | Pure C++ filesystem |
| `bs-client/src/host_config.cpp` | Pure C++ filesystem + `getenv` |

### Needs porting

| File | POSIX surface | Win32 replacement |
|------|--------------|-------------------|
| `bs-client/src/terminal_raw.cpp` | `termios.h`, `sys/ioctl.h`, `unistd.h` | `windows.h` — `SetConsoleMode`, `GetConsoleScreenBufferInfo` |
| `bs-client/src/terminal_raw.hpp` | `termios.h` | New `SavedConsole` struct |
| `bs-client/src/clipboard_linux.cpp` | `sys/wait.h`, `popen` | New `clipboard_windows.cpp` — `OpenClipboard`, `GetClipboardData` |
| `bs-client/src/clipboard_bridge.hpp` | Platform detection macros | Add `#elif defined(_WIN32)` path |
| `bs-client/src/image_render.cpp` | `sys/wait.h`, `unistd.h` — `fork`, `execl`, `waitpid`, `mkstemp`, `write` | `CreateProcess` + pipes or stub |
| `bs-client/src/main.cpp` | `poll.h`, `signal.h`, `sys/socket.h`, `netdb.h`, `arpa/inet.h`, `netinet/in.h`, `sys/un.h`, `unistd.h`, `sys/ioctl.h`, `sys/wait.h`, `termios.h` | `winsock2.h`, `ws2tcpip.h`, `afunix.h`, `windows.h` + two-thread relay |
| `bs-client/src/keygen.cpp` | `sys/stat.h`, `getenv("HOME")` | `_mkdir`, `_chmod`, `getenv("USERPROFILE")` |
| `bs-server/src/pty.cpp` | `sys/ioctl.h`, `termios.h`, `unistd.h`, `signal.h`, `sys/wait.h`, `fcntl.h` | `CreatePseudoConsole`, `CreateProcess`, `ResizePseudoConsole` |
| `bs-server/src/pty.hpp` | `session.hpp` only | No OS deps — keep as-is after Session port |
| `bs-server/src/session.cpp` | `unistd.h`, `sys/wait.h`, `signal.h` — `kill`, `waitpid`, `usleep` | `TerminateProcess`, `WaitForSingleObject`, `Sleep` |
| `bs-server/src/session.hpp` | `ring_buffer.hpp` only | No OS deps — keep as-is |
| `bs-server/src/session_manager.cpp` | `sys/wait.h` — `waitpid` | `WaitForSingleObject` |
| `bs-server/src/session_manager.hpp` | Pure C++ | No OS deps — keep as-is |
| `bs-server/src/persistence.hpp` | `sys/stat.h`, `unistd.h` — `::unlink`, `::rename` | `std::filesystem::remove`, `std::filesystem::rename` |
| `bs-server/src/main.cpp` | `poll.h`, `signal.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `unistd.h`, `fcntl.h` | `winsock2.h`, `ws2tcpip.h`, `windows.h` + `WSAPoll` |

---

## Dependency Budget

| Tool | Cost | Install |
|------|------|---------|
| VS Build Tools 2022 | Free | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| CMake 4.x | Free | `winget install Kitware.CMake` |
| Ninja | Free | Already in Hermes venv |
| OpenSSL 3.x | Free | `vcpkg install openssl:x64-windows` |
| zstd | Free | `vcpkg install zstd:x64-windows` |
| nlohmann/json | Free | `vcpkg install nlohmann-json:x64-windows` |
| CLI11 | Free | `vcpkg install cli11:x64-windows` |
| spdlog | Free | `vcpkg install spdlog:x64-windows` |
| Catch2 | Free | `vcpkg install catch2:x64-windows` |

**$0 total.** All free/open-source.

---

## v2 After Windows Port

Once bs-client compiles and relays on Windows, layer in these v2 features (all platform-agnostic):

1. **v2-2** Bootstrapping — auto key deploy
2. **v2-6** Session recording — tee output to file
3. **v2-8** Rich clipboard — MIME type support
4. **v2-9** Clipboard chunking — >64KB payloads
5. **v2-1** QUIC — swap transport backend under same protocol layer

Skip Linux-only features (cgroups, io_uring). Skip Windows-irrelevant ones (NAT traversal).
