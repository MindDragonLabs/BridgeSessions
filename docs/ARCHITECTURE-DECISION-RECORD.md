# bridgesessions — Architecture Decision Record (SPEC)

**Replace:** `mosh → ssh → zellij → hermes --tui`
**With:** `bs-client ⚡ bs-server → hermes --tui`

One protocol. One binary. One mesh. No SSH. No mosh. No zellij.

**Language:** C++23 (gcc 14+ / clang 18+)
**Transport:** TLS 1.2+ over TCP (prefer 1.3)
**Compression:** zstd per-frame
**Auth:** ed25519 mutual TLS + explicit pins/authorized keys
**Build:** CMake 3.25+

---

## Shipping reality (canonical SoT) — v2.0.14-alpha6

The canonical implementation is the **`bs-protocol.h` + `main.cpp` + `bs-session.h`**
tree (post-R1/R3/R5 refactor, 2026-07-23): `bs-protocol.h` (~12.6k LOC) is the header-only
protocol/daemon SoT including the wire `MessageType` enum, `main.cpp` (~0.9k LOC) is the
CLI/daemon entrypoint, and `bs-session.h` (~0.5k LOC) holds `RingBuffer`/`Session`.
`VERSION` in the repo root is the version single source of truth (`BS_VERSION` is defined
from it at build time). The pre-refactor `bridgesessions.cpp` monolith survives only as a
7-line test stub. `MeshController::run` owns
established connections and PTYs on one select loop; TLS/initial-Hello handshakes
are incremental, nonblocking, deadline-bounded, and limited to 16 pending sockets.
Long file-transfer operations borrow exactly one established TLS transport through
a bounded two-thread joinable worker pool while the loop continues serving other
peers (the `exec_busy` exclusive-borrow pattern at :7079/:7424 — **streaming
media must NOT use this borrow**; see §15.3). Local loopback IPC is authenticated
with a fresh owner-only token under the selected app home. mDNS is disabled by
default and may update addresses only for already trusted keys.

**First-class platforms (v2.0.7-alpha2+):** the daemon + client are a **single
binary** that runs on **Linux, macOS, and Windows** (ConPTY path under
`#ifdef _WIN32`; portable static builds shipped for all three). The §1 diagram
below is macOS-centric for illustration only — every node in the mesh may be any
OS, and **any** peer may drive computer use or read a conversation on **any**
other peer (CUA dispatch is v2.0.8-alpha3 design + Windows helper proof; see §15.5).

> **Wire SoT rule:** the `bs-protocol.h` `MessageType` enum is authoritative
> (0x01–0x2B). The `bs-protocol/` library enum is **frozen, test-only** (stops at 0x14) —
> decision recorded in TODO.md (2026-07-22, held 2026-07-24). New wire types land in
> `bs-protocol.h` only; regenerate the library from it if it is ever needed as a real codec.

Remote edit/vfolder sync and large image-on-wire claims are intentionally disabled
until they have dedicated nonblocking transports. This section overrides older
aspirational v1/v2 text below where the two conflict.

---

## 1. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│  macOS — BridgeSpace.app (terminal emulator & pane manager)      │
│                                                                   │
│   ┌─ Pane A ───────────────────────────┐  ┌─ Pane B ───────────┐│
│   │ bs-client --server=dev --name=hms  │  │ bs-client --srv=dev ││
│   │ stdin/stdout relay                │  │ --name=work         ││
│   │ clipboard bridge ↔ pbcopy/pbpaste │  │                     ││
│   └──────────────┬─────────────────────┘  └──────────┬──────────┘│
│                  │ bs:// over TLS 1.3/TCP (port 19949)│ bs://     │
└──────────────────┼──────────────────────────────────┼───────────┘
                   │                                  │
          ┌────────┴──────────┐               ┌───────┴──────────────┐
          │    INTERNET       │               │                      │
          └────────┬──────────┘               └───────┬──────────────┘
                   │                                  │
┌──────────────────┼──────────────────────────────────┼─────────────┐
│  Linux Server     │                                  │              │
│                   ▼                                  ▼              │
│  bs-server (systemd user service, TCP port 19949)                  │
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

**bs-client is a relay, not a terminal emulator.** BridgeSpace is the emulator. bs-client sits between BridgeSpace's PTY and the network, translating stdin/stdout ↔ bs:// protocol messages.

### ADR-001: One TLS connection per bs-client process (not per pane)

v1: Each bs-client process opens its own TLS 1.3 TCP connection. Eight panes to the same server = eight TCP connections. Simple and correct.

v2 (future optimization): A single bs-client process per server muxes all sessions over one QUIC connection (msquic). Reduces handshake overhead and keepalive traffic. Not needed for v1.

**Rationale:** Connection-per-pane means each pane is independently reconnectable, independently authenticated. Sharing a connection across processes would require IPC — not worth it for v1.

### ADR-002: IPv6-first, IPv4 compatible

bs-server binds `[::]:19949 (mesh)` (dual-stack by default on Linux). Clients resolve `--server` via DNS A/AAAA. SRV record discovery is v2.

### ADR-002a: Reuse SSH aliases for discovery, never transport

The short client form is `bs <host> [session]`. If `<host>` is not already a
BridgeSessions peer, the client expands it with `ssh -G` and imports only the
resolved `HostName`, using BridgeSessions port 19949. SSH `User`, `Port`,
`ProxyJump`, authentication, and transport are intentionally ignored. A pinned
BridgeSessions public key is still required and checked against the TLS
certificate; the SSH alias replaces only duplicate address configuration.
Terminal, clipboard, command, and file payloads remain exclusively on `bs://`.

The server maps the session name to `session.<name>.command`. The PTY belongs to
BridgeSessions and survives client disconnects, so zellij is not part of the
connection path.

---

## 2. The Protocol: bs://

### 2.1 Design Principles

| Principle | Implementation |
|-----------|---------------|
| **Reliable** | TLS 1.3 over TCP — the most proven transport stack in existence |
| **Secure** | TLS 1.3, ed25519 mutual auth, forward secrecy (X25519) |
| **Compressed** | zstd frame compression per stream (2-8x on terminal output) |
| **Low-latency** | TCP_CORK disabled, TCP_NODELAY enabled — keystrokes sent immediately |
| **Firewall-friendly** | TCP port 19949 traverses all networks. No UDP blocking issues. |
| **Multiplexed** | One TCP connection, app-level stream IDs for session multiplexing |
| **No server state for encryption** | TLS session tickets are ephemeral; server restart forces full re-handshake |

### 2.2 Message Types

```
┌──────────────────┬──────────────────┬──────────────────────────────────────┐
│ Direction        │ Type             │ Semantics                            │
├──────────────────┼──────────────────┼──────────────────────────────────────┤
│ client → server  │ Keystroke        │ Raw key bytes (or bracketed-paste)    │
│ server → client  │ Output           │ PTY stdout (already rendered)         │
│ client → server  │ Resize           │ Terminal size changed {cols, rows}    │
│ server → client  │ ClipboardGet     │ OSC 52 captured → client clipboard    │
│ client → server  │ ClipboardPut     │ User pasted → inject into session     │
│ client → server  │ Attach           │ {session_name, cols, rows, term}      │
│ client → server  │ Detach           │ Preserve session, close stream        │
│ server → client  │ SessionList      │ [{name, state, uptime}]               │
│ server → client  │ ServerInfo       │ {hostname, version, load}             │
│ both             │ Ping/Pong        │ Keepalive (every 5s)                  │
│ server → client  │ Scrollback       │ Last N lines on attach (chunked)      │
│ client → server  │ Signal           │ ^C, ^Z, ^\ → forward to foreground    │
│ server → client  │ ExitCode         │ Foreground process exited {code}      │
│ server → client  │ ScrollbackAck    │ Client ready for next scrollback chunk│
│ server → client  │ SessionDied      │ PTY exited unexpectedly (crash, OOM)  │
│ server → client  │ ClipboardEcho    │ Server already has this clipboard hash│
└──────────────────┴──────────────────┴──────────────────────────────────────┘
```

> **Correction (v2.0.8-alpha3):** the table above stops at `ClipboardEcho`
> (0x11). The shipped daemon (`bs-protocol.h`) defines **0x12–0x2B** as well.
> The `bs-protocol/` library enum is **frozen test-only** (stops at 0x14).
> `bs-protocol.h` is wire SoT. Extended set:

| Type | Dir | Code | Semantics |
|------|-----|------|-----------|
| ImageData | both | 0x12 | Static image payload (PNG/JPEG) |
| ImageFrame | both | 0x13 | Animated image payload (GIF frame/blob) |
| ImageAck | both | 0x14 | Image/frame consumed acknowledgement |
| Hello | both | 0x15 | Mesh node introduction |
| Gossip | both | 0x16 | Mesh peer-list exchange |
| SessionSearch | both | 0x17 | Search for a session across the mesh |
| SdpOffer | both | 0x18 | WebRTC SDP offer (over TCP gossip; **stub**, no product path — NAT/WebRTC compiled out via `-DBS_NO_WEBRTC`) |
| SdpAnswer | both | 0x19 | WebRTC SDP answer (stub) |
| DhtFindNode | both | 0x1A | Kademlia find-node (stub; `-DBS_NO_DHT`) |
| DhtFindValue | both | 0x1B | Kademlia find-value (stub; `-DBS_NO_DHT`) |
| FileMeta | both | 0x1C | File metadata (name, size, checksum, total_chunks) |
| FileChunk | both | 0x1D | File data chunk (chunk_index, data) |
| FileAck | both | 0x1E | File chunk ack / next-chunk request |
| FileRequest | c→s | 0x1F | Request file transfer (path) |
| *(next free)* | — | **0x20+** | Reserved for v2.0.8-alpha3 additions below |

#### 2.2a v2.0.8-alpha3 planned message types (0x20+)

| Type | Dir | Code | Payload / semantics |
|------|-----|------|---------------------|
| AttachAck | s→c | 0x21 | `{attach_id, session_name, cols, rows}` — server assigns canonical attach id; echoes effective PTY geometry (min-wins policy) |
| OutputGap | s→c | 0x22 | `{dropped_bytes u64}` — emitted when a per-connection output queue overruns; makes "identical ordered bytes" (TODO #5d) testable instead of silently violated (today `catch(...){}` at :8401 drops bytes) |
| ConversationAppend | both | 0x23 | `{conv_id, seq u64, ts u64, agent_id(pubkey hex), role u8, body utf8}` — seq assigned by store; mesh-relayable (any peer appends/reads) |
| ConversationQuery | c→s | 0x24 | `{conv_id, since_seq}` |
| ConversationBatch | s→c | 0x25 | Ordered message run (response to 0x24) |
| CuaRequest | c→s | 0x26 | `{request_id u32, action u8 (screen_info/key/text/mouse_move/mouse_button/wheel/capture), x i16, y i16, button u8, hid_key u32, modifiers u8, text}` — **keys on wire are USB HID usage IDs, never platform keycodes** |
| CuaResponse | s→c | 0x27 | `{request_id u32, status u8, error, screen_w, screen_h, format}` — capture results reference a following `ImageData` (0x12) frame, not inline pixels |

`AttachMsg` (0x06) is **extended** (not replaced) with an optional trailing
`client_instance_id` field using the existing backward-compat pattern
(serialize ~:714, tolerant deserialize ~:1129). `CuaRequest/Response` are
**FULL in alpha3** (all-3-OS backends + 6-pair matrix + WebRTC); the Windows
`cua-helper` is the sole risk gate (see §15.1).


**ADR-003: Three additional message types**

`ScrollbackAck` — Client ACKs each scrollback chunk to pace replay. Default chunk: 500 lines.
`SessionDied` — PTY death notification with exit_code and signal.
`ClipboardEcho` — Hash-based clipboard race prevention (see ADR-005).

### 2.3 Application-Level Stream Multiplexing

**ADR-004: Stream = session attachment, not session lifetime**

> **Correction (v2.0.8-alpha3):** the frame schema below is the *designed* envelope,
> but the **shipped monolith does NOT currently use per-session stream IDs** — every
> frame (control and session output) is written on **stream 0** (`write_frame(target.ssl.get(),
> message, 0)` at `:8401`). The `stream_id` field exists in the wire frame but is
> effectively unused today; sessions are distinguished by `attached_session` on the
> connection, not by a stream ID. Per-session stream multiplexing is a **future**
> enhancement, not current behavior. Treat the table below as the target design, not
> the deployed reality.

One TCP connection carries multiple independent streams via app-level stream IDs
*(target design — see correction above)*.

```
Frame: [stream_id: u16] [type: u8] [flags: u8] [length: u16] [data]
flags: bit 0 = compressed (zstd), bit 1 = control frame
```

- **Stream ID 0:** Control channel + all session output (current monolith reality)
- **Stream ID 1–65535:** *(target)* Session channels (Keystroke, Output, Clipboard, Signal)

| Event | Stream Action |
|-------|--------------|
| Client sends `Attach{session_name}` | *(target)* Server allocates a stream ID for this attachment |
| Client sends `Detach` | Server sends remaining buffered output, then closes the stream |
| Client disconnects (TCP connection lost) | All streams implicitly closed. Session state preserved on server. |
| Client reconnects + re-attaches | New TCP connection, *(target)* new stream ID allocated for the attachment. |

Sessions live 7 days. *(Target)* stream IDs scoped to a single TCP connection (65535 max).

### 2.4 Clipboard — Two-Way, Guaranteed

```
MACOS → SERVER (paste into session):
  1. User presses Cmd+V in BridgeSpace
  2. macOS delivers paste text via PTY (bracketed-paste)
  3. bs-client polls NSPasteboard every 500ms, hashes content
  4. If hash differs from last sent: send ClipboardPut{text, hash, timestamp}
  5. Server injects text into session PTY (bracketed-paste)
  6. Server sends ClipboardEcho{hash} to confirm receipt
  7. Client records last-acked hash — won't re-send this content

SERVER → MACOS (copy from session):
  1. hermes --tui emits OSC 52 sequence
  2. bs-server captures OSC 52 from PTY output
  3. bs-server strips it from terminal stream (never rendered)
  4. bs-server sends ClipboardGet{text, hash} as protocol message
  5. bs-client calls pbcopy — text lands in macOS clipboard
  6. bs-client records this hash as last-acked (won't re-send back)
```

**ADR-005: Clipboard race prevention via hash echo**
**ADR-006: Clipboard compression enabled** (zstd level 3 for payloads >256 bytes)

### 2.5 Wire Format

```
┌──────────────────────────────────────────────────────────┐
│ Frame: [stream_id: u16] [type: u8] [flags: u8] [len: u16]│
│        [data: length bytes]                               │
│                                                           │
│ flags: bit 0 = compressed (zstd)                          │
│        bit 1 = control frame (stream 0 only)              │
└──────────────────────────────────────────────────────────┘
```

- Binary framing (not newline-delimited JSON)
- zstd compression on frames >256 bytes (Output, Clipboard, Scrollback)
- Never compressed: Keystroke, Ping/Pong (too small)
- Max frame size: 65535 bytes (u16). Larger payloads use chunking (Scrollback chunks, v2 ClipboardChunk)

---

## 3. Mesh node daemon (Linux / macOS / Windows)

> **Correction (v2.0.8-alpha3):** this section was titled "Remote Linux Daemon".
> Since v2.0.7-alpha2 the daemon is a **single binary** running on **all three**
> OSes (Linux systemd user service, macOS launchd, Windows service/ConPTY). The
> §1 diagram shows a macOS client + Linux server for illustration; the mesh is
> OS-agnostic — any node may be any OS.

### 3.1 Startup & Auto-Spawn

```
User connects:       bs-client --server=dev.example.com --name=hms
                       │
                       ▼
bs-server receives:  Attach{session_name: "hms"}
                       │
                       ▼
Session "hms" exists? ──No──► Auto-spawn: hermes --tui
                       │
                      Yes
                       │
                       ▼
                    Attach client to existing PTY
                    Send scrollback buffer (chunked, client-ACK'd)
                    Send ClipboardGet for latest clipboard
```

**ADR-007: Command resolution order**
1. Client `--cmd` flag (always wins)
2. Server `config.yaml` per-session `command`
3. Server `config.yaml` `default_command`
4. Hardcoded default: `hermes --tui`

If no `--name` provided: attach to `default_session` from server config. If no default configured, list sessions and wait for client choice.

### 3.2 Session Lifecycle

```
CREATED ──► RUNNING ──► DETACHED (client gone, PTY alive)
    │            │              │
    │            │              ├── ATTACHED (client reconnected)
    │            │              │
    │            ▼              ▼
    │         DIED          KILLED (explicit, or idle timeout)
    │       (PTY crash)         │
    │            │              │
    │     auto_restart?         │
    │       ├─ Yes → RUNNING    │
    │       └─ No  → EXITED     │
    │                           │
    └───────────────────────────┘
```

**ADR-008: PTY death handling**

When PTY exits unexpectedly: send `SessionDied{exit_code, signal}` to attached client. If `auto_restart: true`: respawn after `restart_delay_secs`. 3 failures in 60s → EXITED.

**Other rules:**
- Sessions survive client disconnects indefinitely
- Idle timeout: 7 days default, resets on any PTY output
- Output buffer: zstd-compressed **raw-byte** circular buffer, `kDefaultRingBufferSize = 1 MiB` (`:2238`); default `scrollback_lines = 2000` (`:2873`, configurable). The buffer stores raw ANSI bytes — there is **no terminal emulator**, so cross-geometry reflow is a P2 **stretch** (best-effort, not a release gate).
- On reattach: replay last 2K lines in 500-line chunks, each chunk ACK'd

### 3.3 Multi-Client Per Session

> **Correction (v2.0.8-alpha3):** The owner-scoped namespace described below
> is **true only of the `bs-server/` reference library** (`session_manager.cpp`
> keys by `owner_id + "\x1f" + name`). The **shipped monolith**
> (`bridgesessions.cpp`) keys `sessions_` by **session name only**
> (`sessions_.find(name)`, `sessions_[name]` — verified at :4061/:4131/:4170).
> Any authorized key can therefore attach to any named session on a daemon;
> `peer_ids` is a per-session list of *all* attached pubkeys, not a namespace
> key. Treat the monolith as wire SoT. See §3.3a for the v2.0.8 attachment model.

**Monolith reality (v2.0.7+):** a session name is a **global per-daemon**
label. Multiple distinct authorized keys may attach to the same session and all
receive fanned-out `Output` (fanout verified at `pty_output_poller` :8386-8442).
What is **NOT** first-class today is *same-key multi-attach* bookkeeping:

- `peer_ids` is **deduped by pubkey** on attach (`bridgesessions.cpp:4068-4072`,
  `4086-4090`): N connections from the same key collapse to **one** `peer_ids`
  entry. There is no per-connection attach id, no per-connection scrollback
  cursor, no attach count.
- Detach erases `peer_ids` **by value** (`sessions_.detach(name, pubkey)`,
  :4168-4179) — `std::remove` drops *all* entries for that pubkey.
- A reconnect-race guard `has_replacement_transport` (:5771-5782) keeps the
  session alive across same-key detach by accident of pubkey matching, but the
  identity model is pubkey-granular, not connection-granular.

#### 3.3a v2.0.8-alpha3 attachment model (target)

`Session` gains `attachments: map<attach_id, {owner_pubkey, conn_idx, cols,
rows, attached_at}>`. `attach_id` is server-assigned (echoed in a new
`AttachAck` message) or client-chosen trailing field on `AttachMsg` (backward-
compatible, like `command`/`signal_on_detach`). `peer_ids` becomes a **derived**
unique-pubkey view for persistence compatibility.

- Detach is keyed by `attach_id`, not pubkey → fixes same-key N-attach collapse.
- `--signal-on-detach` fires only when `attachments.empty()` AND state was
  Attached/Running.
- **Geometry policy (resize war):** server PTY size = **MIN(cols), MIN(rows)**
  across live attachments (tmux model), recomputed on attach/detach/Resize.
  `AttachAck` reports the effective size. Last-writer-wins is the alternative;
  min-wins is the v2.0.8 default and must be documented.
- Spectator role (read-only, no Keystroke/ComputerUse) is **in alpha3** (attachments flag).

### 3.4 Signal Handling

| Client sends | Server does |
|-------------|------------|
| Signal{CtrlC} | Send SIGINT to foreground process group |
| Signal{CtrlZ} | Send SIGTSTP to foreground process group |
| Signal{CtrlBackslash} | Send SIGQUIT to foreground process group |
| Client disconnects | Session → DETACHED (NOT killed) |
| bs-server shutdown | Graceful: SIGTERM → wait 5s → SIGKILL all sessions |

---

## 4. bs-client (mesh relay agent — Linux / macOS / Windows)

### 4.1 Startup Flow

```
BridgeSpace opens a new pane
         │
         ▼
bs-client --server=dev.example.com --name=hms
         │
         ├── 1. Load keypair from ~/.bridgesessions/id_ed25519.pem
         ├── 2. Resolve --server via DNS A/AAAA
         ├── 3. TCP dial to <resolved>:19949 (mesh)
         ├── 4. TLS 1.3 handshake (mutual auth via ed25519)
         ├── 5. Send Attach{session_name: "hms", cols, rows, term: "xterm-kitty"}
         ├── 6. Receive scrollback buffer in chunks → write each chunk to stdout
         │      After each chunk: send ScrollbackAck
         └── 7. Enter relay loop:
                stdin  → Keystroke → server
                server → Output    → stdout
                server → ClipboardGet → pbcopy
                server → ClipboardEcho → update last_acked_hash
                server → SessionDied → notify user
                NSPasteboard change → ClipboardPut → server
                SIGWINCH → Resize{cols, rows} → server
```

### 4.2 Clipboard Bridge

C++ implementation via Objective-C++ bridge:

```cpp
// clipboard_bridge.mm
#import <AppKit/NSPasteboard.h>
#include <string>
#include <string_view>
#include <atomic>

class ClipboardBridge {
    NSPasteboard* pb_ = [NSPasteboard generalPasteboard];
    std::atomic<std::string> last_acked_hash_;
public:
    std::string read();   // [pb_ stringForType:NSPasteboardTypeString]
    void write(std::string_view text);  // [pb_ setString:forType:]
    void ack(std::string_view hash) { last_acked_hash_ = hash; }
    bool should_send(std::string_view hash) const {
        return last_acked_hash_.load() != hash;
    }
};
```

**ADR-009: Clipboard ownership is in bs-client, not BridgeSpace**

### 4.3 Reconnection

```
Network blip
    │
    ▼
TCP connection lost (all streams implicitly closed)
    │
    ▼
bs-client retries (exponential backoff: 100ms → 200ms → 400ms → ... max 5s)
    │
    ▼
Reconnected within 30s?
    ├── Yes: Attach{session_name} → resume where you left off
    │         (server has buffered output during disconnection)
    └── No:  Exit with message:
              "Session 'hms' survived — reattach with:
               bs-client --server=dev --name=hms"
```

**ADR-010: Server restart = full TLS handshake.** TLS session tickets are ephemeral. Server restart → full handshake (same as SSH reconnect).

---

## 5. Authentication & Key Management

### 5.1 Key Model

```
┌─────────────────────────────────────────────────────┐
│  Keypair: ed25519 (OpenSSL EVP_PKEY_ED25519)         │
│  Stored in: ~/.bridgesessions/id_ed25519.pem        │
│  Public key registered on server:                    │
│    ~/.bridgesessions/authorized_keys                 │
│    (one ed25519 public key per line, hex-encoded)    │
│                                                      │
│  bs-client loads keypair at startup:                 │
│    1. --key flag (explicit path)                     │
│    2. ~/.bridgesessions/id_ed25519.pem               │
└─────────────────────────────────────────────────────┘
```

### 5.2 Server Registration (One-Time)

```bash
# On macOS — generate keypair
bs-client keygen
# → ed25519 keypair written to ~/.bridgesessions/id_ed25519.pem

# On server — register the key
bs-server authorize <hex-pubkey>
# → appends to ~/.bridgesessions/authorized_keys

# Or, automated pairing:
bs-client pair --server=dev.example.com --bootstrap-token=<one-time-code>
```

### 5.3 Connection

```bash
bs-client --server=dev.example.com                    # connect
bs-client --server=dev.example.com --name=hms          # specific session
bs-client --server=dev.example.com --name=logs \
          --cmd="journalctl -f"                        # custom command
bs-client --server=dev.example.com --list              # list sessions
bs-client --server=dev.example.com --name=hms --kill   # kill session
```

### 5.4 Health Check

```bash
bs-server health          # Exit 0 if healthy
bs-server status --json   # Structured output
```

---

## 6. BridgeSpace Integration

### 6.1 Pane ↔ Session Mapping

```
BridgeSpace window
├── Pane: "dev:hms"     ← bs-client --server=dev --name=hms
├── Pane: "dev:work"    ← bs-client --server=dev --name=work
├── Pane: "prod:hms"    ← bs-client --server=prod --name=hms
└── Pane: "dev:logs"    ← bs-client --server=dev --name=logs --cmd="journalctl -f"
```

**ADR-011: v1 BridgeSpace integration is minimal**
1. BridgeSpace spawns bs-client as child process with correct args
2. Splitting a pane copies the server context
3. Closing a pane sends SIGTERM to bs-client (which sends Detach gracefully)

Everything else (servers.json, session list UI, key management) is v2.

---

## 7. Deployment & Operations

### 7.1 Server Install

```bash
# Install binary (CMake-built, static-linked)
scp bs-server user@host:~/.local/bin/

# Bootstrap
mkdir -p ~/.bridgesessions
echo "<hex-pubkey>" >> ~/.bridgesessions/authorized_keys

# systemd user service
bs-server install --user
systemctl --user enable --now bs-server
```

### 7.2 Client Install

```bash
# macOS
cp bs-client /usr/local/bin/
# Or build from source: cmake --build build/release
```

### 7.3 Firewall

```
Server: TCP 19949 (TLS 1.3)
Client: Ephemeral port
```

### 7.4 Monitoring

```bash
bs-server status --json
# { "hostname": "dev", "version": "1.0.0", "uptime_seconds": 86400,
#   "sessions": [...], "bytes_in": 1234567, "bytes_out": 9876543,
#   "compression_ratio": 3.2, "connections": 2 }
```

---

## 8. Compression & Encryption Stack

```
Application:   bs:// protocol messages
                       │
Transport:     TLS 1.3 over TCP
               ├── Forward secrecy (X25519 key exchange)
               ├── TLS session tickets (ephemeral, for session resumption)
               └── TCP congestion control (OS-managed, battle-tested)
                       │
Compression:   zstd per-frame
               ├── Level 3 (default: balance speed/ratio)
               ├── 2-5x compression on terminal output
               ├── Disabled for Keystroke messages (< 16 bytes)
               ├── Enabled for Clipboard messages
               └── Enabled for Scrollback replay chunks
                       │
Identity:      ed25519 keypair
               ├── Client: proves identity to server
               ├── Server: pinned on first connect, verified thereafter
               └── No CA infrastructure needed
```

**Why TLS over TCP for v1?**
- Zero firewall issues — TCP 443/19949 traverses everything
- OpenSSL is the most audited crypto library on the planet
- TCP congestion control is OS-managed and bulletproof
- Application-level multiplexing is ~200 lines of C++
- v2 swaps TCP for QUIC via msquic — same protocol codecs, different transport backend

**Why zstd?**
- 2-8x compression on ANSI-heavy terminal output
- Faster than gzip at equivalent ratios
- Single .so/.dylib link dependency (BSD-licensed)
- Dictionary training possible for common ANSI patterns (v2)

**ADR-012: Server restart invalidates TLS session tickets.** Clients fall back to full handshake — same as SSH.

---

## 9. Language: C++23

| C++23 Strength | Why It Matters Here |
|---------------|-------------------|
| Zero-GC latency | No GC pauses in a relay loop that runs for years |
| `std::expected<T,E>` | Fallible ops return `expected` — no `tl::expected` dependency (ADR C02) |
| `std::print` | Type-safe formatted output, no iostream or fmt overhead |
| `std::flat_map` | Cache-friendly sorted session map; better locality than `unordered_map` |
| `std::mdspan` | Zero-overhead multi-dimensional view for ring buffer spans |
| `deducing this` | Cleaner CRTP for codec and ring buffer templates |
| `[[assume(expr)]]` | Optimization hints for hot relay loop paths |
| OpenSSL native bindings | No CGo overhead, direct EVP_PKEY_ED25519 |
| Single binary deploy | Static link OpenSSL + zstd → single ~5MB binary |
| `posix_openpt` / `forkpty` | PTY allocation via libc, zero dependency |
| Cross-compile | `cmake --toolchain linux-arm64.cmake` from macOS |
| Sanitizers | ASan + UBSan + TSan from day one. Fuzzer on decode. |
| msquic path to v2 | When ready: swap `bs-transport/tcp_tls.cpp` for `bs-transport/quic.cpp`. Same protocol layer. |
| Deterministic destruction | RAII for fds, TLS contexts, child processes. No finalizer races. |

**Why not Go?** GC pauses are sub-millisecond in modern Go, but a relay that handles terminal I/O at tens of sessions is trivially within C++'s zero-GC profile. The real win: C++ makes the v2 QUIC migration a library swap under the same protocol layer, rather than a full rewrite. C++23's `std::expected` and `std::print` make the code feel modern without pulling in Boost or fmt.

---

## 10. v1 Feature Cut (Shippable MVP)

### Must Have

- [ ] bs-server: single-binary daemon, systemd user service
- [ ] bs-client: stdin/stdout relay binary
- [ ] TLS 1.3 over TCP with ed25519 mutual auth
- [ ] zstd compression on Output, Clipboard, and Scrollback frames
- [ ] App-level stream multiplexing (stream IDs for session routing)
- [ ] Session lifecycle: CREATED → RUNNING → DETACHED → ATTACHED → DIED → EXITED
- [ ] PTY death handling: SessionDied message, auto_restart config
- [ ] Two-way clipboard (ClipboardGet + ClipboardPut + ClipboardEcho)
- [ ] Clipboard race prevention via hash echo
- [ ] Reconnection with exponential backoff
- [ ] Scrollback replay in ACK'd chunks
- [ ] Signal forwarding (^C, ^Z, ^\\)
- [ ] Keygen + authorize workflow
- [ ] BridgeSpace: minimal v1 integration
- [ ] Output buffer: zstd-compressed circular, 16K lines, ~8 MB per session
- [ ] Health check endpoint

### Won't Do (v2+) — remaining research surface

> **Correction (v2.0.8-alpha3):** several items below were already shipped and are
> moved out of "won't do": **file transfer** (`FileMeta`/`FileChunk`/`FileAck`/`FileRequest`
> 0x1C–0x1F) and **multi-client fan-out** (live `Output` fan-out to all attached
> clients, `pty_output_poller` :8386-8442) both exist in v2.0.7-alpha2. QUIC/msquic
> and WebRTC/NAT remain research-only (compiled out via `-DBS_NO_WEBRTC`/no-msquic).

- [ ] Native QUIC via msquic (connection migration, 0-RTT, stream multiplexing)
- [ ] Multi-window/multi-pane layout server-side
- [ ] UDP hole punching / NAT traversal
- [ ] Web gateway
- [ ] Mosh-style predictive echo
- [ ] Session recording / audit
- [ ] SRV record discovery
- [ ] BridgeSpace servers.json UI integration
- [ ] Single bs-client process muxing panes over one connection

**v1/v2 statement of record:** *"a reliable, encrypted, compressed, two-way-clipboard
session relay that replaces mosh+ssh+zellij"* — superseded by the multi-OS mesh daemon
shipped in v2.0.7-alpha2.

---

## 11. Monorepo Layout

```
bridgesessions/
├── CMakeLists.txt              # Top-level project
├── CMakePresets.json           # debug, release, macOS, linux targets
├── cmake/
│   ├── FindZstd.cmake
│   └── toolchain-linux-amd64.cmake
│   └── toolchain-linux-arm64.cmake
├── bs-protocol/                # Shared protocol types (static lib)
│   ├── CMakeLists.txt
│   ├── include/bsprotocol/
│   │   ├── message.hpp         # Message type enum, variant structs
│   │   └── codec.hpp           # encode/decode + zstd
│   ├── src/
│   │   └── codec.cpp
│   └── tests/
│       ├── message_test.cpp
│       └── codec_test.cpp
├── bs-transport/               # TLS + frame I/O (static lib)
│   ├── CMakeLists.txt
│   ├── include/bstransport/
│   │   ├── tls.hpp             # ServerContext, ClientContext, TOFU
│   │   └── frame_io.hpp        # read_frame, write_frame
│   ├── src/
│   │   ├── tls.cpp
│   │   └── frame_io.cpp
│   └── tests/
│       └── tls_test.cpp
├── bs-server/                  # Mesh daemon library/reference (all 3 OSes; executable)
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── session.hpp
│   │   ├── session_manager.hpp
│   │   ├── ring_buffer.hpp
│   │   ├── pty_linux.cpp
│   │   ├── osc52_capture.hpp
│   │   └── persistence.hpp
│   └── tests/
├── bs-client/                  # Client relay library/reference (all 3 OSes; executable)
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── clipboard_bridge.mm # Objective-C++ for NSPasteboard (macOS)
│   │   └── terminal_raw.cpp    # tcsetattr cfmakeraw
│   └── tests/
├── bs-protocol.h               # CANONICAL wire + daemon SoT (header-only; ~12.6k LOC)
├── main.cpp                    # CLI + daemon entrypoint (~0.9k LOC)
├── bs-session.h                # RingBuffer + Session types (~0.5k LOC)
├── bridgesessions.cpp          # 7-line pre-refactor test stub (not the product)
├── tools/
│   ├── bridgepanel/            # BridgePanel HTTP server (publish/note/message)
│   └── windows-cua/            # Windows CUA PowerShell scripts (local-only)
├── scripts/                    # build/CI/helper scripts
├── tests/                      # integration + multi-attach + protocol tests
├── docs/
│   ├── ARCHITECTURE.md         # This file — the spec
│   ├── ROADMAP.md              # Implementation phases + timeline
│   ├── protocol.md             # Wire format spec
│   ├── RELEASE-2.0.8-alpha3.md # v2.0.8-alpha3 phased plan
│   └── deployment.md           # Server setup, firewalling
├── .clang-format
├── .clang-tidy
├── .gitignore
└── README.md
```

> **Correction (v2.0.14-alpha6):** the `bs-client`/`bs-server` directories are
> **library/reference code that builds on all three OSes**, not OS-specific. The
> shipped product is the `bs-protocol.h` + `main.cpp` + `bs-session.h` tree (one
> binary, three portable static builds). The `bs-protocol/` library enum is frozen
> test-only (see §15.6).

---

## 12. Architecture Decision Records — Index

| ADR | Decision | Section |
|-----|----------|---------|
| 001 | One TLS connection per bs-client process (not per pane) | §1 |
| 002 | IPv6-first, IPv4 compatible | §1 |
| 003 | New message types: ScrollbackAck, SessionDied, ClipboardEcho | §2.2 |
| 004 | App-level stream ID = session attachment, not session lifetime | §2.3 |
| 005 | Clipboard race prevention via hash echo | §2.4 |
| 006 | Clipboard compression enabled (zstd) | §2.5 |
| 007 | Command resolution order (--cmd > config > default) | §3.1 |
| 008 | PTY death handling with auto_restart | §3.2 |
| 009 | Clipboard ownership in bs-client, not BridgeSpace | §4.2 |
| 010 | Server restart = full TLS handshake | §4.3 |
| 011 | v1 BridgeSpace integration is minimal | §6 |
| 012 | Server restart invalidates TLS session tickets | §8 |
| 013 | TLS-over-TCP for v1; QUIC via msquic for v2 | §8 |
| 014 | TCP congestion control (OS-managed) is adequate | §8 |
| 015 | No connection migration in v1; reconnect with backoff | §4.3 |
| 016 | Multipath deferred (N/A over TCP) | — |
| 017 | TCP primary transport; no fallback needed | §7.3 |
| 018 | Hand-rolled binary framing over schema frameworks | §2.5 |
| 019 | ALPN identifies protocol; first message negotiates version | — |
| 020 | TCP flow control provides sufficient backpressure | §2.3 |
| 021 | No keystroke batching — send immediately | §2.1 |
| 022 | TOFU with explicit `rotate-key` command | §5 |
| 023 | authorized_keys + revoked_keys flat files | §5 |
| 024 | 0-RTT not applicable (TCP); disabled in v2 | — |
| 025 | Same-user model, env sanitization, ulimit | §3 |
| 026 | Thread-per-session; adequate to 10K concurrent | ROADMAP.md | **Superseded by v2.0.7 single select-loop + bounded 2-thread file-transfer pool** (see §0) |
| 027 | Raw ANSI ring buffer, zstd-compressed, 1MB default | §3.2 |
| 028 | Session metadata persistence (JSON), explicit resurrect | ROADMAP.md |
| 029 | Per-user bs-server (mosh model, no root) | §3 |
| 030 | Raw mode on stdin distinguishes Ctrl+D from PTY close | §4.1 |
| 031 | BridgeSpace owns SIGWINCH; bs-server applies TIOCSWINSZ | §4.1 |
| 032 | NSPasteboard polling at 500ms via ObjC++ bridge | §4.2 |
| 033 | Plain text clipboard v1; binary + MIME for v2 | — |
| 034 | Large clipboard payloads deferred to v2 chunking | — |
| 035 | bs-client allocates own PTY; BridgeSpace connects via Unix socket | §6 |
| 036 | Hybrid error propagation: exit codes + stderr + protocol Error frames | §6 |

### C++-Specific ADRs

| ADR | Rule |
|-----|------|
| C01 | **RAII for all resources.** unique_ptr for fds, custom deleters for SSL_CTX. No raw new/delete. |
| C02 | **No exceptions across thread boundaries.** `std::expected<T, Error>` for fallible ops. |
| C03 | **Header-only where possible.** Protocol types, codec, ring buffer. .cpp for non-template code. |
| C04 | **Compile-time polymorphism.** std::variant + std::visit for message types. Templates for ring buffer. |
| C05 | **Sanitizers in CI from day one.** ASan + UBSan + TSan on every PR. LibFuzzer on decode(). |

---

## 13. Deep Research Findings

All 26 research questions investigated May 2026 via DeerFlow v2 (TinyFish + Exa + corpus search across 12,619 indexed sources). Key findings that shaped the architecture:

| Research Area | Finding | Impact |
|--------------|---------|--------|
| UDP blocking rates | 5-15% of enterprise networks block UDP. Palo Alto/Ubiquiti admins actively recommend blocking QUIC. | Confirmed TLS-over-TCP for v1. No UDP dependency. |
| Go QUIC library options | Production-tested (Caddy, Syncthing) but CGo overhead and GC interaction make native C++ libraries preferable. | C++23 + msquic/lsquic — native FFI-free QUIC libraries. |
| 0-RTT replay attacks | Real — TLS 1.3 0-RTT data is inherently replayable. Attach message replay could disrupt sessions. | 0-RTT disabled. v2 requires anti-replay nonce. |
| Per-connection concurrency model | Before choosing C++, confirmed the model scales: fine to 10K concurrent with ~2KB overhead/connection. GC becomes the bottleneck at 100K+ in managed runtimes. | Thread-per-session in C++ — same architecture, zero-GC guarantee. |
| ed25519 key rotation | SSH-style authorized_keys + revoked_keys flat files are the standard. TOFU is adequate for <50 servers. | Adopted flat-file model. |
| NSPasteboard API | No push notification — macOS clipboard managers poll `changeCount`. pbpaste polling is universal. | ObjC++ bridge polls changeCount every 500ms. No Swift helper needed. |

---

## 14. Open Questions

1. **Bootstrapping on fresh servers?** v1: manual install (scp binary, echo key, systemd enable). v2: `bs-client bootstrap --ssh-user=X`.
2. **Connection brokering / NAT traversal?** v1: direct TCP to known IPs. v2: Tailscale/WireGuard overlay.
3. **Latency hiding?** v1: no predictive echo (same UX as SSH). v2: Mosh-style state synchronization.
4. **Terminal identification?** v1: client reads TERM from env, passes in Attach. Server sets on PTY. Supported: xterm-256color, xterm-kitty, tmux-256color.
5. **Session recording?** v2: `--record-sessions` with tee between PTY and ring buffer.
6. **Graceful hostname changes?** v1: DNS A/AAAA at dial. v2: SRV records.

---

## 15. v2.0.8-alpha3 Feature Architecture

This section captures the architecture decisions for the five required features
(TODO.md + PLANS.md) as endorsed by the 2026-07-20 MoA review (Grok 4.5 + Kimi K3
workers, `stack.moa.worker` judge), with the **operator override (2026-07-20) to build
all five to FULL functionality in 2.0.8-alpha3** (the MoA had recommended a thin slice +
2.0.9 deferral). Phase sequencing and per-phase verification gates live in `PLANS.md`.

### 15.1 Computer use (CUA) — FULL in alpha3 (all-3-OS backends + 6-pair matrix + WebRTC)

**Wire types (see §2.2a):** `CuaRequest` (0x26) / `CuaResponse` (0x27). Keys on the wire
are **USB HID usage IDs**, never platform keycodes; each backend translates HID→native.
Capture results reference a following `ImageData` (0x12) frame — already exists and is
mesh-transferable.

**Dispatch surface:** new `bs-cua/` static lib, one backend per OS behind a common
interface (`inject_key / inject_text / inject_pointer / screen_info / capture`):

| OS | Inject backend | Capture | alpha3 bar |
|----|---------------|---------|------------|
| Windows | `SendInput` + per-logon **cua-helper** process | GDI BitBlt / ffmpeg gdigrab | **Must work** in interactive user session |
| Linux | X11 `XTest` first; `uinput`/evdev fallback | XGetImage / grim | Inject + capture both functional |
| macOS | `CGEvent` (TCC grant) | `CGWindowListCreateImage` | Inject + capture both functional |

**`Handle=0` / Session-1 blocker (the one hard research risk):** a daemon running as
SYSTEM cannot inject into the visible user desktop session. The alpha3 fix is a
**per-user logon helper agent** (`bridgesessions cua-helper`) started at user login; the
daemon delegates inject/capture to it over an authenticated named pipe. **This is the
sole risk gate for the whole CUA feature** — Linux and macOS ship regardless of the
Windows outcome. If the Windows cloud-PC `cua-helper` PoC fails to resolve Handle=0, Windows
injection is the only blocked item (documented, not silently half-built).

**6-pair matrix (from→to, all combinations):** Linux→{Win,Mac,Linux}, Win→{Linux,Mac,Win},
Mac→{Linux,Win,Mac}. Each pair dispatched via the mesh relay (`shell`/`use` routing model)
and verified by screenshot diff / telemetry. Vision leg: capture on one OS → analyzed on
another. **WebRTC live-media path** (5b) carries CUA frames with sub-second cadence.

**Security gate (non-negotiable):** CUA is RCE-adjacent. Gated by `authorized_keys` like
shell/exec; capability advertised in `ServerInfo` (0x09) as `cua: true`; **every injected
action audit-logged**. No CUA over untrusted WAN without the Tailscale story.

### 15.2 Bridge Panel conversations — FULL in alpha3 (store + CLI + virtualized render + mesh relay)

Panel today is a 1486-line stdlib HTTP server whose only write paths are
`publish()`/`note()` copying Markdown into `sessions/<session>/<comms|documents>/`.
**Zero conversation/message model exists** (verified by grep).

**Store (alpha3):** append-only JSONL per thread —
`<data_home>/sessions/<session>/conversations/<conv_id>.jsonl`, one JSON per line
`{seq, ts, agent(pubkey hex|human), role, body}` + `<conv_id>.meta.json`. Namespaced by
originating agent pubkey; readable by other mesh peers.

**Writers:** new `bridgepanel.py message --session S --conv C --role R --text …` (mirrors
`publish()`); any mesh peer with the panel token can append. Mesh-native relay via
`ConversationAppend` (0x23) / `ConversationQuery` (0x24) / `ConversationBatch` (0x25) so a
peer on any node appends/reads — satisfies "any agent on the mesh" without the panel
itself becoming a mesh client.

**Readers:** `build_tree` gains a "Conversations" node; GET returns JSON (`?since_seq=`) +
paginated HTML; **client-side virtualized list** (no jank at 5000 messages); copy
per-message. Long documents also support incremental append (5c).

**Tests:** pytest — multi-agent interleave ordered by seq; 5000-message virtualization
scroll smooth (no layout thrash); append-without-token rejected; mesh relay A→B→C ordering
preserved. UI verified in a **real browser, 0 console errors** (operator standard).

### 15.3 Streaming — FULL in alpha3 (harden fanout + progressive transfer + panel incremental)

**Correction:** TODO #5's "no streaming path exists" is **false for shell output**.
`pty_output_poller` (:8386-8442) already fans `Output` to every attached conn each
event-loop pass. The real gaps:

1. **Silent byte loss** — fanout failure is swallowed by `catch(...){}` at :8401; a slow
   client loses bytes with no signal. Fix: per-connection bounded output queue; high-water
   → drop-oldest + emit `OutputGap` (0x22); catastrophic lag → close that conn only.
2. **No flow control / lag tracking.** `OutputGap` makes "identical ordered bytes" (5d) testable.
3. **`exec_busy` starvation** — file/media transfer borrows the transport exclusively
   (:7079/:7424). **Live streams must NOT take `exec_busy`**; they ride normal frames.
4. **5b progressive transfer / live media** — `file recv` yields partial bytes + SHA at
   completion; a live media stream (screenshots/video frames) flows from a source peer to a
   viewer peer without a full capture-then-transfer cycle.
5. **5c panel incremental** — conversation messages and long documents append live (no republish).

**Tests:** long command → 2 attached clients, identical ordered bytes, bounded lag; throttle
one → `OutputGap` observed, other unaffected; kill one mid-stream → survivor unaffected;
E2E live-media cloud-PC→viewer with sub-second cadence.

### 15.4 Cross-resolution display correctness — FULL harness + doctor (server-side reflow = stretch)

Parameterized harness drives PTYs at 80×24 / 120×40 / 160×50 / 200×100 (+ intermediate);
asserts no wrap beyond width, no dropped rows, byte-exact scrollback replay **at the same
geometry**, and CJK/emoji/box-drawing survive capture→transfer→render. `doctor` prints size
+ a glyph sample + environment facts (Wayland/X11/TCC) feeding the CUA risk gates.

**Stretch (best-effort, not a release gate):** a server-side terminal emulator for true
cross-geometry reflow. The ring buffer stores **raw ANSI bytes** today; byte-exact replay
across *different* geometries needs a TE. If the stretch slips, document the limitation
rather than block release.

### 15.5 Multi-attach, same source PC (see §3.3a) — FULL + spectator

Already partially works by accident of `has_replacement_transport`; the v2.0.8 fix is the
`attachments` map + `attach_id`-keyed detach + MIN-geometry policy + new `AttachAck` (0x21).
**Spectator role** (read-only `attachments` flag: receives `Output`, `Keystroke`/`ComputerUse`
rejected with `Error`) is included. **Tests:** 3 same-key conns attach, all receive output;
close 2 → session+child survive; close last → `--signal-on-detach` fires; reattach with same
`instance_id` = replace, new `instance_id` = additional; resize from two conns → min-wins;
spectator receives output but cannot inject.

### 15.6 bs-protocol library drift (P0 decision)

The `bs-protocol/` library enum (stops at 0x14) is **behind** `bs-protocol.h` (0x01–0x2B)
and its `AttachMsg` lacks `command`/`signal_on_detach`. **Decided 2026-07-22 (option b):
freeze the library as test-only.** `bs-protocol.h` is wire SoT; regenerate the library
from it only if a real codec is ever needed. Recorded in TODO.md.

---

**Status:** v2.0.14-alpha6 shipped (`bs-protocol.h` + `main.cpp` + `bs-session.h`; 3-platform portable static binaries: linux-x86_64 / macos-arm64 / windows-x86_64.exe). All five v2.0.8-alpha3 features are built and wired at the protocol/daemon layer; remaining gaps are UI/CLI-level (`bs use` subcommand, panel Conversations render, Win/Mac CUA injection backends, parameterized geometry tests — see `TODO.md`).
**Language:** C++23 (firm; gcc 14+ / clang 18+).
**Transport:** TLS 1.2+/1.3 over TCP :19949 (QUIC/msquic noted as v2 research only — not built).
**Auth:** ed25519 mutual TLS + `authorized_keys` flat-file (firm).
**Compression:** zstd per-frame (firm).
**Next:** v2.0.8-alpha3 — building all five features to FULL functionality: multi-attach (same-key) + spectator, cross-resolution display harness + doctor (reflow stretch), streaming fanout + progressive transfer + panel incremental, panel conversation store + virtualized render + mesh relay, cross-platform CUA (all-3-OS backends + 6-pair matrix + WebRTC). Phases in `PLANS.md`, items in `TODO.md`, architecture in §15.
