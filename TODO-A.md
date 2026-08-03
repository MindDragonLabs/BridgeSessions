# TODO-A.md — Phase A: CUA from Windows

Start with v2.0.6 baseline. CTest 267/267.

## A.1 — Wire Windows interactive raw-mode into CLI shell path

**Current state:** `enable_raw_mode()` exists for both Windows (line 4764) and POSIX (line 4798). It's used by the non-interactive exec path. The interactive shell path uses `is_windows_cli_oneshot_command()` (line 2489) to decide whether to use ConPTY or plain pipes. When the command is a one-shot, plain pipes are used. When it's an interactive shell, ConPTY is created (line 2548‑2640).

**Gap:** The client-side (origin) CLI on Windows does not enter raw terminal mode when running `bs shell peer` interactively. Only the non-interactive `--cmd` path does.

**Fix:** In `main()` / the shell subcommand handler, after connecting to the peer and before entering the PTY read/write loop, call `enable_raw_mode()` on the client side so keystrokes are forwarded without local line editing or echo.

**Verification:** On TEST-PC1, after fix: `bs shell TEST-PC2` → type a character → it should appear on the remote immediately (not wait for Enter).

## A.2 — Ensure ConPTY resize is called on ResizeMsg

**Current state:** `ResizeMsg` handler calls `resize_pty()` which on Windows calls `ResizePseudoConsole(hpcon, ...)` (line 8050‑8060). This is in `handle_inbound_session` — it handles resizes from a remote peer attaching to a local session.

**Gap:** When Windows is the *origin* and attaches to a Linux/macOS session, resizing the Windows console should send a `ResizeMsg` to the remote. The local-resize detection code needs to call `write_frame(ResizeMsg)` when the Windows console dimensions change.

**Fix:** In the interactive shell main loop on Windows, poll for console resize events (or use `ReadConsoleInput` with `WINDOW_BUFFER_SIZE_EVENT` detection) and send `ResizeMsg` to the peer.

**Verification:** `bs shell TEST-PC2` from TEST-PC1 → resize the console window → `stty size` on TEST-PC2 shows new dimensions.

## A.3 — Ensure GenerateConsoleCtrlEvent works for ConPTY sessions

**Current state:** `inbound_session_handler` at line 8034 calls `GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(child_pid))` for Windows child processes. This works when the *remote* peer sends `SignalMsg::CtrlC`.

**Gap:** Need a test that the CTRL_C_EVENT reaches a ConPTY-hosted child process and terminates only that child — not the ConPTY host itself.

**Test:** Create a child process that installs a `SetConsoleCtrlHandler` and runs in a loop. Send `SignalMsg::CtrlC` over the mesh. Verify the handler fires and the child exits gracefully. The ConPTY host session stays alive.

## A.4 — Cross-direction E2E: Windows → Linux interactive CUA

**Test scenario:**
1. On TEST-PC1: `bs shell TEST-PC2` (interactive, no `--cmd`)
2. Type `hostname` → expect `TEST-PC2`
3. Type `ls /home/user` → expect directory listing
4. Resize the window → `stty size` should reflect new dimensions
5. Start a long-running command: `bash -c 'trap "echo GOT_SIGINT" INT; sleep 30'`
6. Press Ctrl‑C → expect `GOT_SIGINT`, prompt returns, session stays alive

## A.5 — Cross-direction E2E: Windows → macOS interactive CUA

Same as A.4 but target = TEST-PC3, zsh.

**TODO-A.md done when:**
- A.1 through A.5 all pass on live hardware
- No CTest regressions (267/267 baseline)
- Output captured and logged
