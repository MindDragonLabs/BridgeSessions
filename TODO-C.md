# TODO-C.md — Phase C: Deferred Features

## C.1 — Send detach signal to child process

**Task:** When `--signal-on-detach` is set and all peers disconnect from a session, send the specified signal to the child.

Lines to modify:
- `detach_connection_session()` (~line 5720)
- After calling `sessions_.detach()`, check if `session->peer_ids.empty()` (no remaining attached peers)
- If so and `--signal-on-detach` is set: call `kill(session->child_pid, sig)` on POSIX, `GenerateConsoleCtrlEvent`/`TerminateProcess` on Windows
- Test: attach, detach, verify child received SIGHUP

## C.2 — vfolder pull / push / watch

**Existing code:** `vfolder` CLI11 subcommand exists at line 11489 but is a stub. Edit uses download/edit/upload pattern.

**Tasks:**
- C.2a: `vfolder pull <peer> <remote-dir> [local-dir]` — download directory tree via daemon IPC
- C.2b: `vfolder push <peer> <local-dir> [remote-dir]` — upload directory tree
- C.2c: `vfolder watch <peer> <dir>` — one-way continuous sync at 5s interval
- C.2d: SHA-256 dedup to skip unchanged files

## C.3 — Complete vfolder subcommand in CLI11

**Lines:** ~11489-11502
**Current state:** Subcommand definitions exist with flag variables but no callback. `vfolder_name`, `vfolder_src`, `vfolder_dest`, `vfolder_watch`, `vfolder_cmd` are declared but dead.

## C.4 — Multi-hop routing

**Lines:** 410-415 (AttachMsg.routing field), dispatch_message (~line 7730)
**Current state:** `routing` field is in AttachMsg but daemon ignores it.
**Task:** When daemon receives AttachMsg with routing != empty, forward via mesh TCP to the named peer. Max 3 hops.

## C.5 — Public packaging

- Homebrew formula
- AUR package
- Windows MSI installer

**TODO-C.md done when:** All C.x items pass tests, no CTest regressions.
