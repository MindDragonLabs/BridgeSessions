# bridgesessions — Design Sketch (GUIDELINE.md)

**Replace:** `mosh → ssh → zellij → hermes --tui`
**With:** `bs-client ⚡ bs-server → hermes --tui`

One protocol. Two binaries. No SSH. No mosh. No zellij.

**Language:** C++23 (gcc 14+ / clang 18+) | **Transport:** TLS 1.3 over TCP (v1), QUIC via msquic (v2)
**Compression:** zstd | **Auth:** ed25519 mutual TLS + TOFU | **Build:** CMake 3.25+

---

## What It Is

bridgesessions replaces the fragile stack of mosh, SSH, zellij, and Hermes TUI with a single reliable relay. bs-client runs on macOS inside BridgeSpace. bs-server runs on Linux. They talk over a binary protocol called bs://. The session lives on the server regardless of client state. Clipboard is a first-class protocol message — no escape sequences, no corruption, guaranteed delivery.

**bs-client is a relay, not a terminal emulator.** BridgeSpace is the emulator. bs-client sits between BridgeSpace's PTY and the network, translating stdin/stdout ↔ bs:// protocol messages.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│  macOS — BridgeSpace.app (terminal emulator & pane manager)      │
│                                                                   │
│   ┌─ Pane A ───────────────────────────┐  ┌─ Pane B ───────────┐│
│   │ bs-client --server=dev --name=hms  │  │ bs-client --srv=dev ││
│   │ stdin/stdout relay                │  │ --name=work         ││
│   │ clipboard bridge ↔ pbcopy/pbpaste │  │                     ││
│   └──────────────┬─────────────────────┘  └──────────┬──────────┘│
│                  │ bs:// over TLS 1.3/TCP (port 9948)│ bs://     │
└──────────────────┼──────────────────────────────────┼───────────┘
                   │                                  │
          ┌────────┴──────────┐               ┌───────┴──────────────┐
          │    INTERNET       │               │                      │
          └────────┬──────────┘               └───────┬──────────────┘
                   │                                  │
┌──────────────────┼──────────────────────────────────┼─────────────┐
│  Linux Server     │                                  │              │
│                   ▼                                  ▼              │
│  bs-server (systemd user service, TCP port 9948)                  │
│    ┌──────────────────────────────────────────────────────┐        │
│    │  Session Multiplexer (replaces zellij)               │        │
│    │  ┌─ session: hms ──────────────────────────────────┐ │        │
│    │  │  PTY → hermes --tui                             │ │        │
│    │  │  Output buffer (zstd-compressed, last 16K lines)│ │        │
│    │  ├─ session: work ─────────────────────────────────┤ │        │
│    │  │  PTY → bash -l                                  │ │        │
│    │  ├─ session: logs ─────────────────────────────────┤ │        │
│    │  │  PTY → journalctl -f                            │ │        │
│    │  └─────────────────────────────────────────────────┘ │        │
│    │  Clipboard relay (OSC 52 capture → protocol message) │        │
│    └──────────────────────────────────────────────────────┘        │
└────────────────────────────────────────────────────────────────────┘
```

## The Protocol: bs://

A binary, length-prefixed frame protocol carried over TLS 1.3 TCP. App-level stream IDs multiplex multiple sessions over one connection.

```
Frame: [stream_id: u16] [type: u8] [flags: u8] [length: u16] [data]
flags: bit 0 = compressed (zstd), bit 1 = control frame
```

**17 message types (v1.0):** Keystroke, Output, Resize, ClipboardGet, ClipboardPut, Attach, Detach, SessionList, ServerInfo, Ping, Pong, Scrollback, Signal, ExitCode, ScrollbackAck, SessionDied, ClipboardEcho. **Wave 2 adds:** ImageData, ImageFrame, ImageAck.

**Stream ID 0** is the control channel (Attach, Detach, Ping, SessionList). Stream IDs 1–65535 carry session data.

**Compression:** zstd on frames >256 bytes (Output, Clipboard, Scrollback). Never compress Keystroke or Ping/Pong.

## Clipboard — The Killer Feature

Clipboard is a first-class protocol message, not an escape sequence passthrough that breaks when mosh/zellij corrupt it. Two-way, guaranteed delivery:

```
MACOS → SERVER (paste):
  bs-client polls NSPasteboard → ClipboardPut → server → PTY via bracketed-paste
  Server confirms → ClipboardEcho{hash}
  Client won't re-send same hash

SERVER → MACOS (copy):
  hermes emits OSC 52 → bs-server captures + strips from output
  → ClipboardGet → bs-client → pbcopy
  Client marks hash as last-acked
```

Hash-based race prevention: `ClipboardEcho` confirms the server has a clipboard payload. Client skips sending content whose hash matches the last-acked hash. No paste loops on high-latency links.

## Session Lifecycle

```
CREATED → RUNNING → DETACHED (client gone, PTY alive)
      ↘ DIED → (auto_restart?) → RUNNING / EXITED
      ↘ KILLED (explicit or idle timeout)

Sessions live 7 days. Circular output buffer (zstd-compressed, 16K lines).
Reconnect replays last 2K lines in 500-line chunks with ACK pacing.
auto_restart: respawn PTY on crash. 3 failures in 60s → EXITED.
```

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Language | C++23 | Zero-GC, `std::expected`, `std::print`, `std::flat_map`, msquic path to v2 |
| Transport v1 | TLS 1.3 over TCP | Firewall-friendly, no UDP blocking |
| Transport v2 | QUIC via msquic | Library swap, same protocol layer |
| Compression | zstd | 2-8x on terminal output, BSD license |
| Auth | ed25519 mutual TLS | OpenSSL EVP_PKEY_ED25519, TOFU |
| Concurrency | Thread-per-connection | Adequate to 10K, zero-GC guarantee |
| Wire format | Binary, hand-rolled | 200 lines of C++, debuggable with xxd |
| Clipboard bridge | ObjC++ NSPasteboard | Native macOS clipboard, no pbpaste exec |

## v1 Feature Cut

**Must have:** TLS 1.3 + ed25519, session multiplexer, two-way clipboard with hash echo, scrollback replay, reconnection with exponential backoff, signal forwarding, keygen + authorize, BridgeSpace minimal integration, health check.

**Won't do (v2):** QUIC via msquic, predictive echo, port forwarding, fan-out, NAT traversal, session recording, SRV discovery.

**Wave 2 (current):** Image & animation transfer — see TODO.md.

**v1 is "a reliable, encrypted, compressed, two-way-clipboard session relay that replaces mosh+ssh+zellij."**

---

*Full architectural decisions with ADR numbers and deep research findings: see [ARCHITECTURE.md](./ARCHITECTURE.md).*
*Implementation plan with phased tasks and timeline: see [PLANS.md](./PLANS.md).*
*Active task checklist: see [TODO.md](./TODO.md).*
*Agent dispatch rules: see [AUTONOMOUS.md](./AUTONOMOUS.md).*
