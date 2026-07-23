# BridgeSessions — Quick Reference (LLM & Human)

**One binary. Mesh terminal relay.** SSH, MOSH, SCP, tmux, WinRM — all one `bridgesessions`.

---

## Install

```bash
# Linux / macOS
curl -fsSL https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.9-alpha5/scripts/install.sh | bash

# Windows PowerShell
irm https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.9-alpha5/scripts/install.ps1 | iex
```

---

## Join a mesh

**Host** generates an invite:
```
$ bridgesessions invite
Invite (valid 2h):  e7a1b2c3d4...
One-liner:
  bridgesessions join <host-ip>:19949 e7a1b2c3d4...
Or with curl install:
  curl .../install.sh | bash -s -- join <host-ip>:19949 e7a1b2c3d4...
```

**Joiner** pastes the one-liner. That's it. Identity generated, config written, host authority established, daemon started.

```
$ bridgesessions join <host-ip>:19949 e7a1b2c3d4 --start
Identity: c8efdf34adf16b9e...
Joined. Node: my-laptop  Config: ~/.bridgesessions/config
→ Daemon started.
```

---

## Shell

### One-shot command
```bash
$ bs shell test-pc2 -x 'whoami && hostname && uname -r'
agent
test-pc2
7.1.4-arch1-1
```

Exit code propagates. Stdout/stderr streamed live.

### Named persistent session (tmux built in)
```bash
$ bs shell test-pc2 -n hermes -x 'bash -lc "echo START; sleep 30; echo DONE"'
START
# ... disconnect, reconnect ...
$ bs shell test-pc2 -n hermes    # reattach — EXACT SAME PTY, child still running
DONE
```

The session survives disconnection. The PTY keeps running. Any peer can reattach.

### Named profile (daemon config)
In `~/.bridgesessions/config` on the **server**:
```ini
session.hermes.command bash -lc 'export TERM=xterm-256color; hermes --tui --yolo'
```

Then from anywhere:
```bash
$ bs shell test-pc2 -n hermes
# Launches hermes --tui --yolo. Reattach anytime.
```

### Interactive TUI (full terminal passthrough)
```bash
$ bs shell test-pc5 -n zsh
# Interactive zsh session. Ctrl-D to detach. Resize propagated.
```

Terminal size auto-detected on connect and propagated on resize (like SSH).

---

## File transfer

```bash
$ bs file send test-pc2 ./large-file.bin --wait
PROGRESS phase=send chunks=147/147 bytes=7191040 pct=100.0
OK sent large-file.bin 7191040 bytes sha256:226a95e...

# Receive lands at ~/.bridgesessions/received/
$ bs shell test-pc2 -x 'sha256sum ~/.bridgesessions/received/large-file.bin'
226a95e67859eeaba2d63aaf4d1f8694...  /home/user/.bridgesessions/received/large-file.bin
```

Hash-verified end-to-end. Works Linux↔macOS↔Windows.

---

## Mesh management

```bash
$ bs peers list          # all known peers (seeds + discovered)
$ bs health test-pc2         # data-plane health check (not just IPC)
$ bs stats                # connections, sessions, bytes
$ bs sessions             # live sessions on this node
```

---

## BridgePanel (web UI)

```
https://test-pc1.host.tail0000.ts.net/<token>/
```

- **Machines column**: all mesh peers, health dots, session counts
- **Sessions column**: click a session → Output/Comms/Docs/Connect tabs
- **Connect tab**: pick a harness (Hermes/Claude/Codex/Kimi/…), checkbox for --yolo, copy the command
- **Create modal**: `+` button on any machine → new session from the panel
- **File pane**: read/edit/save Markdown in-browser

### API
```
GET  /api/machines          → mesh tree (peers + sessions)
GET  /api/tree              → filesystem session tree
GET  /api/output?session=X  → live scrollback (incremental)
GET  /api/session/connect?session=hermes&machine=test-pc2  → {"cmd": "bs shell test-pc2 -n hermes"}
POST /api/save              → write file to session
```

---

## Deep test results (v2.0.9-alpha5)

| Test | Result |
|------|--------|
| CTest full suite | **329/329 pass** (5.1s, 329 tests) |
| Panel tests | **25/25 pass** |
| `invite` generates token | ✅ 32-char hex, 2h expiry |
| Invite prints TS IP | ✅ `<host-ip>:19949` (not 0.0.0.0) |
| `shell test-pc2 -x 'cmd'` | ✅ exit code, stdout, live streaming |
| `shell test-pc5 -x 'cmd'` | ✅ cross-OS (Linux→macOS) |
| Named session `-n stress-sess` | ✅ 10-step sequential output, correct ordering |
| Named session reattach | ✅ `-n hermes` → disconnect → `-n hermes` reattaches |
| File send test-pc2 (7MB) | ✅ 27 MiB/s, sha256 match |
| File send test-pc5 (585KB) | ✅ 0.3 MiB/s, sha256 match |
| `health test-pc2` | ✅ healthy (data-plane ok) |
| `health test-pc5` | ✅ healthy (data-plane ok) |
| BridgePanel healthz | ✅ `{"ok":true,"version":"2.0.9-alpha5"}` |
| BridgePanel /api/machines | ✅ 4 peers, test-pc5 showing 2 sessions |
| BridgePanel /api/tree | ✅ 3 sessions (default, hermes, health-bs) |
| BridgePanel /api/session/connect | ✅ `{"cmd":"bs shell test-pc1 -n hermes"}` |
| Fleet health | ✅ test-pc1/test-pc2/test-pc5 all v2.0.9-alpha5, 4 healthy peers |

### Known limitations
- **Shadow PCs** on Windows need binary deployment (SSH key + scp works for PC1; PC2 needs key fix)
- **macOS native build** requires cmake (cross-compilation from Linux not yet packaged; native build verified working)
- **BridgePanel create session** is a stub (UI functional, backend returns "not yet implemented" — IPC CREATE verb pending)
