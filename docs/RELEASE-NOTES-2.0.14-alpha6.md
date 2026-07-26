# BridgeSessions 2.0.14-alpha6

**File-transfer double-compression fix + release housekeeping. First tag carrying
post-refactor binaries for all three platforms.**

## Changes since 2.0.10-alpha5

### Fixes
- **file-xfer double-compression (3 sites):** `file_send_wait_on_transport`,
  `daemon_edit_up`, and `daemon_vfolder_sync` each ran a manual `zstd_compress()` on
  chunk data before `write_frame()`, whose `encode()` compresses again — the receiver
  decompressed once and got zstd frames instead of payload, breaking transfers (worst on
  Windows/MinGW). All 6 send paths now pass raw bytes to `write_frame()` → single
  `encode()` compress.
- **SSL large frames:** `SSL_write` retries on `WANT_WRITE`/`WANT_READ` instead of failing
  the frame (large transfers over slow links).
- **Windows file-xfer stability:** serve transfers run synchronously and worker-thread
  transfers use blocking socket mode — kills the worker-pool SSL race behind the
  2.0.12-era hang reports.
- **file-xfer:** silent fire-and-forget send path removed — all sends use the WAIT variant
  so errors surface.
- **BridgePanel:** phantom sessions filtered, Create endpoint wired, `html.escape` fix.

### Features (2.0.12-alpha5, folded 2.0.11)
- **Remote video capture:** `CuaVideoCaptureMsg` (0x2A) / `CuaVideoCaptureResultMsg` (0x2B)
  wired through dispatch.
- **Native screen capture** on Linux/macOS/Windows.
- **BridgePanel 10-tab fleet dashboard:** Fleet tab (spoke health, harness status, event
  log) + Events, Models, Health, Settings tabs.

### Housekeeping
- `ARCHITECTURE.md` + `docs/HOW-TO-COMPILE.md` updated for the R1/R3/R5 layout
  (`bs-protocol.h` + `main.cpp` + `bs-session.h`).
- Linux static-builder Dockerfile versioned at `scripts/Dockerfile.static-linux`
  (previously `/tmp/bs-static/Dockerfile` — lost once).
- PLANS.md archived as SHIPPED; dead monolith-era root `test_config.cpp` removed.
- Hardcoded tailnet IP scrubbed from the tree (env-var placeholder).
- 329/329 CTest green.

### Known issues
- Win/macOS CUA keyboard/mouse injection backends are stubs ("P5c"); capture works,
  injection does not. Linux injection (xdotool) works end-to-end.
- BridgePanel Conversations UI not yet built (daemon store + wire types exist).
- No `bs use` CLI subcommand yet; parameterized geometry display tests exist as source
  but are not in the CTest suite.
- Quick-connect TOFU accepts all keys on first contact (`.audit/moa-20260724` P1) —
  pin your peers.
- Health probe intermittently reports `data-plane probe failed: bad request` even when
  shell works.

## Assets

| File | Platform |
|------|----------|
| `bridgesessions-linux-x86_64` | Linux x86_64 (static, glibc 2.35 floor) |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 (MinGW static) |
| `bridgesessions-macos-arm64` | macOS arm64 (Apple Silicon) |
| `SHA256SUMS` | Checksums (regenerate locally; gitignored) |
| `SBOM-binaries.json` | Software bill of materials (gitignored) |

Raw download (from tag):

```text
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.14-alpha6/dist/bridgesessions-linux-x86_64
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.14-alpha6/dist/bridgesessions-windows-x86_64.exe
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.14-alpha6/dist/bridgesessions-macos-arm64
```

## Verification

```bash
./bridgesessions --version   # → 2.0.14-alpha6
# Linux:  ldd → only libc.so.6 + ld-linux
# macOS:  otool -L → only libc++.1.dylib + libSystem.B.dylib
# Windows: objdump -p | grep "DLL Name" → OS DLLs only
```

## Upgrade

Replace your existing binary. No config changes needed from 2.0.9-alpha5 or later.
Fleet note: peers on ≤ 2.0.7 don't send `sessions_summary_json` — remote session counts
in BridgePanel populate after they upgrade.
