# CUA Signal-Handling Scenarios (BridgeSessions v2.1)

How Ctrl-C and session-detach signals behave in interactive and non-interactive
modes, plus the `--signal-forward` / `--signal-on-detach` flags.

## 1. Interactive session — Ctrl-C forwards to the remote child

```
bs shell <peer> <session>
# long-running build running in the remote PTY
make -j8
# user presses Ctrl-C
```

* Default behavior (and with `--signal-forward on`): the local terminal is in
  raw mode with `ISIG` stripped (POSIX) / `ENABLE_PROCESSED_INPUT` removed
  (Windows), so the `0x03` byte is forwarded as a **keystroke** to the PTY.
* The remote child receives **SIGINT** (the PTY translates `0x03` to SIGINT).
  The build stops, the shell prompt returns, and **the BridgeSessions session
  stays alive and attached**.
* With `--signal-forward off`: the local terminal keeps `ISIG`
  (POSIX) / `ENABLE_PROCESSED_INPUT` (Windows), so Ctrl-C raises a **local**
  console control event caught by the `bs` CLI itself. The remote child is
  **not** signaled; the local CLI handles the interrupt.

Verified by: `tests/test_cua_signal.cpp` —
`Interactive Ctrl-C delivers SIGINT to child, session survives`.

## 2. Non-interactive `--cmd` — Ctrl-C kills the local CLI

```
bs shell <peer> <session> --cmd "make"
# in another terminal: kill -INT <bs-pid>
```

* `--cmd` runs the command without entering interactive raw mode. A local
  Ctrl-C terminates the `bs` CLI process (exit code **130** = 128 + SIGINT),
  and the daemon terminates the remote one-shot child.
* This is correct-by-design: a non-interactive command has no live terminal to
  forward keystrokes to, so the local process is the signal target.

## 3. Session detach — `--signal-on-detach`

```
bs shell <peer> <session> --signal-on-detach HUP
# ... work ...
# last peer disconnects (Ctrl-D, close window, network drop)
```

* When the **last** peer detaches, the server sends the requested signal to the
  session's child process:
  * POSIX: `kill(child_pid, sig)` for `HUP` / `TERM` / `INT` / `QUIT` / `KILL`.
  * Windows: `GenerateConsoleCtrlEvent(CTRL_C_EVENT, ...)` for `INT`/`KILL`/
    `HUP`/`QUIT`, `TerminateProcess` for `TERM`.
* The session stays **Detached** — the signal is delivered to the child so
  daemons / long jobs learn the terminal left, but the session itself is not
  dropped.
* Unknown signal names are ignored (logged, no crash, no kill).
* Wire format: the request rides in `AttachMsg.signal_on_detach`
  (str_prefixed_u16, optional). v1.6/v1.7 clients that don't send it are still
  decoded correctly (guarded by `d.ok(2)`).

Verified by: `tests/test_cua_signal.cpp` —
`Detach signal is delivered to child on last-peer detach` and
`Detach with unknown signal name is a no-op, no crash`.

## Signal name reference

| Name  | POSIX | Windows action                        |
|-------|-------|---------------------------------------|
| HUP   | SIGHUP| Ctrl-C event (`GenerateConsoleCtrlEvent`) |
| TERM  | SIGTERM| `TerminateProcess`                  |
| INT   | SIGINT| Ctrl-C event                          |
| QUIT  | SIGQUIT| Ctrl-C event                         |
| KILL  | SIGKILL| Ctrl-C event (uncatchable on POSIX)  |

> Note: the direct child spawned by the server is `/bin/sh -c <command>`
> (POSIX). Whether a forwarded signal reaches a *grandchild* job depends on the
> shell's job-control handling; for long-running daemons that install their own
> handlers (or are started directly), the signal is delivered as configured.
