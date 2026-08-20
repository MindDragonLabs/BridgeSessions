# Design

BridgeSessions is one executable with three roles: mesh daemon, client CLI, and persistent session multiplexer.

```text
CLI ── mutual TLS ── peer daemon ── PTY / ConPTY
                            ├── files
                            ├── desktop helper
                            └── peer/session gossip
```

## Core decisions

- **Pinned identity:** each node owns an Ed25519 key. Trust is the raw pinned key, not a certificate subject or public CA.
- **Persistent sessions:** the server owns the terminal and child; attachments are temporary. Detach does not kill the session.
- **Single event loop:** established sockets are non-blocking and outgoing frames use bounded queues.
- **Exclusive TLS ownership:** long operations borrow one transport via `exec_busy`; no two threads touch the same `SSL*`.
- **Verified transfers:** metadata is bounded, paths canonicalized, writes use `.part`, SHA-256 precedes atomic publish, and valid partials survive reconnect.
- **Backpressure:** wire lengths, receive work per tick, TLS TX, PTY input, worker IPC, and logical output queues are bounded.
- **Desktop isolation:** Windows/macOS CUA uses a token-authenticated loopback helper in the interactive user session.
- **Artifact separation:** git contains source/automation; binaries and SBOMs are GitHub Release assets.

## Source map

| File | Responsibility |
|---|---|
| `main.cpp` | CLI and self-upgrade |
| `bs-protocol.h` | codec, TLS, mesh, transfers, IPC |
| `bs-session.h` | session lifetime and scrollback |
| `bs-session-worker.h` | optional per-session process |
| `bs-cua-helper.h` | desktop helper |
| `macos-capture.mm` | ScreenCaptureKit |

## Current limits

- `select()` inherits the platform FD ceiling; this targets small trusted fleets.
- Authorization is host-level, not capability-scoped.
- Cross-platform compatibility currently caps TLS at 1.2.
- Windows ConPTY and macOS permissions require platform E2E testing.
