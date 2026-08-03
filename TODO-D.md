# TODO-D.md — Phase D: Bug Fixes

Start from v2.0.6 baseline + Phase A+B changes. CTest 267/267.
Compiled binary: `build-2.0.6/bridgesessions`

## D.1 — BUG-1: exec_busy watchdog timer

**Lines:** 5124-5205 (LongOperationWorkerPool), 10141-10222 (non-interactive exec path)
**Current state:** LongOperationWorkerPool handles file transfers. Shell exec still uses older BusyGuard/daemon_shell_via_ipc. When CLI times out, exec_busy can stay true, blocking all subsequent IPC calls to that peer.

**Fix:** Add 90-second watchdog timer to the daemon's main event loop. If any `exec_busy` flag has been true for >90 seconds without being cleared, reset it. The watchdog must NOT steal SSL ownership — only clear the flag so subsequent IPC calls can proceed.

**Test:** `timeout 15 ./bridgesessions shell peer --cmd "sleep 120"` then immediately `./bridgesessions health peer` returns healthy within 90s.

## D.2 — BUG-2: Handle=0 (Windows cloud-PC Roblox input)

**Root cause:** Roblox window only visible in interactive user session, not SYSTEM Session 1.
**Fix:** Schedule Playtest task in interactive session via `schtasks /IT` flag.

## D.3 — Flashing and partial PTY output

**Lines:** ~9706 (select timeout — already fixed to 100ms in v2.0.6)
**Current state:** Already applied. Sub-millisecond responsiveness confirmed. NOT A BUG.

## D.4 — Edit/vfolder IPC daemon version skew

**Current state:** Already documented. Uses daemon IPC which requires matching versions. NOT A CODE BUG.

**TODO-D.md done when:** D.1 watchdog test passes, D.2 verified on a Windows cloud PC, no CTest regressions.
