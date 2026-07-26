# BridgeSessions 2.0.15-alpha6

**Warning-hygiene release. Quick-connect `join` and daemonize paths now check return
values instead of silently ignoring failures.**

## Changes since 2.0.14-alpha6

### Fixes
- **Joiner side (`main.cpp`):** `save_config()` and `ensure_private_directory()` failures
  now abort `join` with a clear error and non-zero exit (previously ignored — a failed
  config save left a "joined" node with nothing persisted). `std::system()` daemon
  auto-start reports non-zero rc instead of printing "Daemon started" unconditionally.
- **Host side (`bs-protocol.h`):** quick-connect auto-authorize now reports persistence
  failures to the joiner via `JoinReplyMsg.error` (`ok=false`) instead of replying "ok"
  while never writing `authorized_keys` — the ghost-auth pattern behind
  "join succeeded, later connections get `hello_rejected`".
- **Daemonize:** all three `freopen("/dev/null", …)` stdio detaches are checked; failure
  exits the child instead of running with half-attached stdio.
- Build is clean of `-Wunused-result` on gcc-13 (Linux) and MinGW g++ (Windows).

## Assets

Same three platform binaries as 2.0.14-alpha6; build matrix unchanged
(`scripts/Dockerfile.static-linux`, test-pc5 native arm64, MinGW cross on test-pc1).
Raw download:

```text
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.15-alpha6/dist/bridgesessions-linux-x86_64
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.15-alpha6/dist/bridgesessions-windows-x86_64.exe
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.15-alpha6/dist/bridgesessions-macos-arm64
```

## Verification

```bash
./bridgesessions --version   # → 2.0.15-alpha6
```

329/329 CTest green. No config or wire-protocol changes; drop-in upgrade from
2.0.14-alpha6.
