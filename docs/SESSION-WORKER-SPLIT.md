# BridgeSessions Session-Worker Split

## Problem

The daemon process owns every session's PTY and child process (via `forkpty()`).
When the daemon dies — intentional restart, crash, binary update — `Session::~Session()`
kills all children and closes all PTYs. Client reconnect is impossible because the
session no longer exists.

## Solution: Per-Session Worker Process

Split session ownership from the mesh controller. Each session runs as an independent
process that survives controller restarts.

### Process model

```
┌─────────────────────────────┐
│  bs-meshd (controller)      │  Restartable, thin
│  • Mesh TLS (19949)         │
│  • CLI IPC (19980)          │
│  • BridgePanel API          │
│  • Gossip, routing          │
│  • Worker lifecycle mgmt    │
└───────┬─────────────────────┘
        │ Unix domain socket
        │ /run/bs-sessions/<name>.sock
        ├──────────────────────┐
        ▼                      ▼
┌──────────────┐      ┌──────────────┐
│ bs-sessiond  │      │ bs-sessiond  │  Long-lived
│ session:foo  │      │ session:bar  │  Owns PTY + child
│ bash (pid N) │      │ zsh (pid M)  │
└──────────────┘      └──────────────┘
```

### Worker: `bridgesessions session-worker`

New CLI subcommand. Spawned by the controller per session.

```
bridgesessions session-worker \
  --socket /run/bs-sessions/foo.sock \
  --name foo \
  --command "/bin/bash -l" \
  --cols 80 --rows 24 \
  --term xterm-256color
```

Worker responsibilities:
1. `forkpty()` the shell
2. Listen on Unix domain socket
3. Event loop: `select(master_fd, listen_fd, client_fds...)`
4. PTY output → forward to all connected controller clients
5. Controller input → write to PTY master fd
6. Maintain scrollback ring buffer (1MB, same as current)
7. On child exit: notify clients with DIED message, exit worker

### Controller ↔ Worker IPC Protocol

Reuse the existing framed-message serialization (no TLS needed — local socket).
Messages are length-prefixed frames with a 1-byte type tag.

```
Controller → Worker:
  WORKER_INPUT     (type 0x01): raw bytes → PTY master write
  WORKER_RESIZE    (type 0x02): uint16 cols, uint16 rows
  WORKER_DETACH    (type 0x03): detach one client (uint32 attach_id)
  WORKER_PING      (type 0x04): liveness check

Worker → Controller:
  WORKER_OUTPUT    (type 0x81): raw bytes from PTY (with render_markdown hint)
  WORKER_SCROLLBACK(type 0x82): scrollback snapshot on new attach
  WORKER_DIED      (type 0x83): int32 exit_code, int32 signal_num
  WORKER_READY     (type 0x84): session spawned ok (string name, int32 pid)
  WORKER_PONG      (type 0x85): liveness reply
```

### Controller startup: worker discovery

On `run()`, before the main event loop:
1. Scan `/run/bs-sessions/*.sock` (or `~/.bridgesessions/sessions/`)
2. For each socket: connect, send `WORKER_PING`
3. If alive: register as a "managed worker session" in SessionRegistry
4. If dead: unlink the socket

This means: **kill the controller, start a new one, sessions are still there.**

### Controller: attach path changes

Current (`AttachMsg` handler, line 8947):
```
1. Resolve session command
2. sessions_.attach_connection() → create_session() → forkpty()
3. Set conn.attached_session = &session
4. Send scrollback
```

New:
```
1. Resolve session command
2. Check if worker socket exists for this session name
3. If not: spawn worker process, wait for WORKER_READY
4. Connect to worker socket (or reuse existing connection)
5. Send WORKER_INPUT for initial scrollback request
6. Set conn.attached_session = &session (session now proxies through worker)
7. Worker sends WORKER_SCROLLBACK → forward to client
```

### SessionRegistry changes

Session struct gains:
```cpp
// Worker process management
std::string worker_socket_path;
SOCKET worker_fd = INVALID_SOCKET;  // connection to worker (if connected)
pid_t worker_pid = -1;             // worker process PID (not shell PID)
bool is_worker_managed = false;     // true when session runs in a worker
```

When `is_worker_managed`:
- `master_fd` / `child_pid` are NOT used directly by controller
- `pty_output_poller()` reads from `worker_fd` instead of `master_fd`
- `KeystrokeMsg` handler writes to `worker_fd` instead of `write_pty_input()`
- `waitpid()` is NOT called — worker sends `WORKER_DIED` instead

### Client reconnect: UX improvements

The `shell_peer` reconnect loop already exists (line 12068). Improvements:
1. **Indefinite reconnect** — configurable max attempts (default: unlimited)
2. **Visual feedback** — show `[bs] Connection lost — reconnecting... (attempt N)` on stderr
3. **Scrollback recovery** — on reconnect, controller sends scrollback from worker
4. **Max backoff** — configurable (default 30s, was 5s)
5. **Ctrl-C during reconnect** — first Ctrl-C cancels reconnect immediately

### Backwards compatibility

- Existing sessions (pre-worker) continue to work inline
- Worker is only spawned for new sessions when `sessions.use_workers = true` (default: true)
- `sessions.use_workers = false` falls back to legacy inline forkpty
- Tests that spawn sessions directly still work (worker mode is transparent)

### Platform notes

- **POSIX**: Unix domain socket at `~/.bridgesessions/run/<name>.sock`
- **Windows**: Named pipe `\\.\pipe\bs-session-<name>` or TCP localhost
- Worker process spawned via `fork()` + `exec()` (POSIX) or `CreateProcess()` (Windows)
- Worker writes a PID file for cleanup detection

### Rollout plan

1. Implement worker process (`session-worker` subcommand + IPC protocol)
2. Modify SessionRegistry to support worker-managed sessions
3. Modify attach path to spawn/connect workers
4. Add worker discovery on controller startup
5. Enhance client reconnect UX
6. Build, test (336+ existing tests must pass), deploy to peer-linux-a
7. Test: kill daemon mid-session → session survives → restart daemon → reattach
