# BridgeSessions 2.0.10-alpha5

**Structural refactor + Windows non-interactive shell fix.**

## Changes since 2.0.9-alpha5

### Refactor
- **R1+R3:** Extracted `bs-protocol.h` (12,193 lines) + `main.cpp` (835 lines) from the 13,490-line monolith. Protocol is header-only; CLI/daemon entry is a separate translation unit. 25 test files updated.
- **R5:** Extracted `RingBuffer` + `Session` types into `bs-session.h` (470 lines, self-contained).

### Fixes
- **Windows `-x` hang (RCA 2026-07-23):** `SessionDiedMsg` now fans out to `DirectSession` connections (non-interactive mode). Previously only `attached_session` connections received the death notice, causing `shell -x` to spin forever when the child process exited (e.g. `Start-Process` spawning a daemon on a Windows peer).

### Known issues
- macOS binary uses the single-file compile path; CMake build on macOS not tested.
- Health probe intermittently reports `data-plane probe failed: bad request` even when shell works.

## Assets

| File | Platform |
|------|----------|
| `bridgesessions-linux-x86_64` | Linux x86_64 |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 (MinGW static) |
| `bridgesessions-macos-arm64` | macOS arm64 (Apple Silicon) |
| `bridgesessions-2.0.10-alpha5-source.tar.gz` | Source archive |
| `bridgesessions-2.0.10-alpha5-source.zip` | Source archive (zip) |
| `SHA256SUMS` | Checksums |
| `SBOM-binaries.json` | Software bill of materials |

## Verification

```bash
sha256sum -c SHA256SUMS
```

Or individually:

```bash
sha256sum bridgesessions-linux-x86_64
```

## Upgrade

Replace your existing binary. No config changes needed from 2.0.9-alpha5.
