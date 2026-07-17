# Protocol (`bs://`)

BridgeSessions speaks a single binary-framed protocol over TLS 1.2+ (prefer 1.3).

## Design principles

| Principle | Implementation |
|---|---|
| Reliable | TLS 1.2+ (prefer 1.3) over TCP |
| Secure | ed25519 mutual auth, X25519 forward secrecy |
| Compressed | zstd per-frame (2–8× on terminal output) |
| Low-latency | `TCP_NODELAY`, keystrokes sent immediately |
| Firewall-friendly | single TCP port (19949) |
| Multiplexed | one TCP connection, app-level stream IDs |

## Wire format

```
Frame: [stream_id: u16] [type: u8] [flags: u8] [length: u16] [data]
flags: bit 0 = compressed (zstd), bit 1 = control frame (stream 0 only)
```

- Binary framing, not newline-delimited JSON.
- zstd on frames > 256 bytes (Output, Clipboard, Scrollback).
- Never compressed: Keystroke, Ping/Pong (too small).
- Max frame size 65535 bytes (u16); larger payloads are chunked.

## Stream multiplexing

One TCP connection carries many streams via app-level stream IDs.

- **Stream 0** — control channel (Attach, Detach, SessionList, Ping/Pong).
- **Stream 1–65535** — session channels (Keystroke, Output, Clipboard, Signal).

| Event | Stream action |
|---|---|
| Client `Attach{session_name}` | Server allocates a stream ID for the attachment. |
| Client `Detach` | Server flushes buffered output, then closes the stream. |
| Client TCP disconnect | All streams implicitly closed; **session state preserved**. |
| Reconnect + re-attach | New TCP connection, new stream ID. |

Sessions live 7 days. Stream IDs are scoped to a single TCP connection
(65535 max) — effectively unlimited headroom.

## Message types

| Direction | Type | Semantics |
|---|---|---|
| c→s | Keystroke | Raw key bytes (or bracketed-paste) |
| s→c | Output | PTY stdout (rendered) |
| c→s | Resize | Terminal size {cols, rows} |
| s→c | ClipboardGet | OSC 52 captured → client clipboard |
| c→s | ClipboardPut | User paste → inject into session |
| c→s | Attach | {session_name, cols, rows, term} |
| c→s | Detach | Preserve session, close stream |
| s→c | SessionList | [{name, state, uptime}] |
| s→c | ServerInfo | {hostname, version, load} |
| both | Ping/Pong | Keepalive (every 5 s) |
| s→c | Scrollback | Last N lines on attach (chunked) |
| c→s | Signal | ^C, ^Z, ^\ → foreground process |
| s→c | ExitCode | Foreground process exited {code} |
| c→s | ScrollbackAck | Client ready for next scrollback chunk |
| s→c | SessionDied | PTY exited unexpectedly |
| s→c | ClipboardEcho | Server already has this clipboard hash |

## Clipboard (two-way, guaranteed)

- **Client → server (paste):** user pastes → client hashes → sends
  `ClipboardPut{text, hash}` → server injects (bracketed-paste) → server
  echoes `ClipboardEcho{hash}` → client records last-acked hash.
- **Server → client (copy):** app emits OSC 52 → server captures, strips from
  the stream, sends `ClipboardGet{text, hash}` → client writes to local
  clipboard.

Hash echo (ADR-005) prevents the copy/paste echo loop.
