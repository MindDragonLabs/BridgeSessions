# TODO-B.md — Phase B: Ctrl‑C safety audit + hardening

Start from v2.0.6 baseline + Phase A fix applied. CTest 267/267.

## B.1 — Write automated test for Ctrl‑C in interactive session  ✅ DONE

**Done:** `tests/test_cua_signal.cpp` — `Interactive Ctrl-C delivers SIGINT to child, session survives`
- Attaches `bash -lc 'trap "touch <sentinel>" INT; sleep 30'`, sends keystroke `\x03` via `write_pty_input`.
- Asserts sentinel file appears (child got SIGINT) AND session stays alive (state != Died, child still running).
- POSIX-only (Windows ConPTY signal forwarding covered by Phase A.3 E2E on hardware).

## B.2 — Verify non‑interactive --cmd Ctrl‑C exit code  ⏳ TODO (behavior spec, no code change)

**Task:** `./bridgesessions shell localhost --cmd "sleep 60" &` → `kill -INT` the CLI → verify exit 130.
The non-interactive path doesn't enter raw mode, so a local Ctrl-C kills the CLI (exit 130) and the
remote child is terminated by the daemon. Confirm this holds and document it in B.5.
**Note:** This is correct-by-design; needs a manual/CI verification step, not a C++ change.

## B.3 — Add --signal-forward flag  ✅ DONE (code) / ⏳ doc

**Done:** `--signal-forward` already plumbed end-to-end (flag → `shell_peer(signal_forward)` →
`InteractiveTerminalGuard(forward_ctrl_c)` → POSIX `cfmakeraw` ISIG handling at lines 4798/4810).
**Hardening added:** Windows `enable_raw_mode(forward_ctrl_c)` now honors the param — when OFF,
`ENABLE_PROCESSED_INPUT` is kept so Ctrl-C raises a local console control event (matches POSIX ISIG).
POSIX already correct.

## B.4 — Add --signal-on-detach flag  ✅ DONE (code + test)

**Done:** Real mechanism implemented (NOT a no-op stub):
- `AttachMsg` gained `signal_on_detach` field; serialized/deserialized (str_prefixed_u16, wire-compatible
  with v1.6/1.7 clients via `d.ok(2)` guard).
- `Session::detach_signal` stores the request; carried through move/copy ctors.
- On attach, `s->detach_signal` is set from `AttachMsg.signal_on_detach`.
- `SessionRegistry::detach` applies it when the last peer detaches: POSIX `::kill(child, sig)`
  (HUP/TERM/INT/QUIT/KILL), Windows `GenerateConsoleCtrlEvent` (INT-style) / `TerminateProcess` (TERM).
  Session stays Detached; signal delivered to the child. Unknown names are ignored (logged, no crash).
- Test `Detach signal is delivered to child on last-peer detach` (TERM trap sentinel) + unknown-signal no-op test.

## B.5 — End‑to‑end scenario doc  ⏳ TODO

**Task:** Write `docs/cua-signal-scenarios.md` covering interactive SIGINT, non-interactive --cmd exit 130,
and --signal-on-detach HUP/TERM semantics. Wire format + signal-name table.

## Status
- Code complete: B.1 (test), B.3 (Windows hardening + already-wired POSIX), B.4 (wire + server signal + tests).
- Wait on: B.2 verification, B.5 doc.
- All CTest must remain green (267/267 + 3 new = target 270/270).
