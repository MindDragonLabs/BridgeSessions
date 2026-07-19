# Architecture

> **Forward-looking design archive.** The shipping 2.0.6-alpha2 implementation is the
> single [`bridgesessions.cpp`](../bridgesessions.cpp) monolith. See
> [`LEGACY_CODE.md`](../LEGACY_CODE.md) for the boundary between shipping and
> retained experimental code. Modular `bs-client` / `bs-server`, dual-stack
> IPv6, and the command fallbacks below are not current operational claims.

**Replace:** `mosh → ssh → zellij → hermes --tui`
**With:** `bs-client ⚡ bs-server → hermes --tui`

One protocol. One binary. One mesh. No SSH. No mosh. No zellij.

| Property | Choice |
|---|---|
| Language | C++23 (gcc 14+ / clang 18+) |
| Transport | TLS 1.2+ prefer 1.3 over TCP (v1), QUIC via msquic (v2) |
| Compression | zstd per-frame |
| Auth | ed25519 mutual TLS + TOFU |
| Build | CMake 3.25+ (or single-file `build.sh`) |

## Overview

```
 macOS — BridgeSpace / terminal
   ┌─ Pane A ───────────────┐  ┌─ Pane B ───────────┐
   │ bs-client --server=dev  │  │ bs-client --srv=dev │
   │ stdin/stdout relay      │  │                    │
   └───────────┬─────────────┘  └─────────┬──────────┘
               │  bs:// over TLS 1.2+/TCP (port 19949) │
               └──────────────────┬───────────────────┘
                          INTERNET
               ┌──────────────────┼───────────────────┐
               ▼                                      ▼
        bs-server (systemd user service, TCP 19949)
          ┌──────────────────────────────────────────┐
          │  Session Multiplexer (replaces zellij)    │
          │  ┌─ session: hms ──────────────────────┐  │
          │  │  PTY → hermes --tui                 │  │
          │  │  Output buffer (zstd, last 16K)     │  │
          │  ├─ session: work ─────────────────────┤  │
          │  │  PTY → bash -l                      │  │
          │  └─────────────────────────────────────┘  │
          │  Clipboard relay (OSC 52 → protocol)      │
          └──────────────────────────────────────────┘
```

`bs-client` is a **relay, not a terminal emulator**. It sits between your
terminal and the network, translating stdin/stdout ↔ `bs://` protocol messages.
The server owns the PTY and the session, so a disconnected client never kills
your work.

## Architecture Decision Records

- **ADR-001 — One TLS connection per `bs-client` process.** Eight panes to the
  same server = eight independent TCP connections. Each is independently
  reconnectable and authenticated. (v2 may mux over one QUIC connection.)
- **ADR-002 — IPv6-first, IPv4-compatible.** `bs-server` binds `[::]:19949`
  (dual-stack on Linux). SRV discovery is v2.
- **ADR-002a — Reuse SSH aliases for *discovery*, never transport.** The short
  form `bs <host> [session]` expands `<host>` with `ssh -G` to import only the
  resolved `HostName` (port 19949). `User`, `Port`, `ProxyJump`, and
  authentication are ignored — a pinned BridgeSessions public key is still
  required and checked against the TLS certificate.
- **ADR-004 — Stream = session attachment, not session lifetime.** One TCP
  connection carries multiple streams via app-level stream IDs (see
  [Protocol](protocol.md)).
- **ADR-007 — Command resolution order:** (1) client `--cmd`, (2)
  `session.<name>.command`, (3) `sessions.default_shell`.
- **ADR-008 — PTY death handling.** On unexpected PTY exit, the server sends
  `SessionDied{exit_code, signal}`. With `auto_restart: true` it respawns after
  `restart_delay_secs`; 3 failures in 60s → `EXITED`.
- **ADR-009 — Clipboard ownership lives in `bs-client`.** The client tracks the
  last-acked clipboard hash to prevent echo loops.
- **ADR-010 — Server restart = full TLS handshake.** Session tickets are
  ephemeral.

## Components

### bs-server (remote daemon)

- Owns PTYs and the session multiplexer.
- Auto-spawns the configured command when a client attaches to a new session.
- Sessions survive client disconnects indefinitely; idle timeout 7 days
  (resets on output).
- Output buffer: zstd-compressed circular buffer, last 16K lines.
- On reattach: replays last 2K lines in 500-line chunks, each ACK'd.

### bs-client (relay agent)

- Loads its ed25519 keypair, resolves the server, dials TCP 19949, performs
  mutual TLS 1.2+ (prefer 1.3), then attaches to a session.
- Reconnects with exponential backoff (100 ms → 5 s); reattaches transparently
  if the server buffered output during the gap.
- On macOS, bridges the pasteboard (pbcopy/pbpaste) for two-way clipboard.

## Authentication & keys

- Keypair: ed25519, stored at `~/.bridgesessions/id_ed25519.pem`.
- Server `authorized_keys` (one hex pubkey per line) gates who may attach.
- Sessions are namespaced by the **authenticated client pubkey**, so two
  different keys can each own a session called `agent` without collision.

See [Usage](usage.md) and [Configuration](configuration.md) for the commands.
