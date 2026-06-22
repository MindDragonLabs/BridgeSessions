# bridgesessions — Architecture Decision Record (SPEC)

**Replace:** `mosh → ssh → zellij → hermes --tui`
**With:** bridgemind.ai GUI ⚡ mesh peers → any shell, any AI agent, anywhere

One binary, one config, one keypair per device. Every node is a peer. No SSH, mosh, or zellij in the mesh path.

**Language:** C++23 (gcc 14+ / clang 18+ / MSVC 2026)
**Transport:** TLS 1.3 over TCP (v1), QUIC via msquic (v2)
**Compression:** zstd per-frame
**Auth:** ed25519 mutual TLS + TOFU
**Build:** Single-file `/std:c++latest` / `g++ -std=c++23` / `clang++ -std=c++2b`

---

## 1. Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│  bridgemind.ai GUI (operator console, mesh peer)                  │
│                                                                   │
│  ┌─ Pane 1 ──────────────────┐  ┌─ Pane 2 ───────────────────┐  │
│  │ hermes on linux-b           │  │ bash on macos-peer            │  │
│  │ (browser-rendered MD)     │  │ (raw terminal output)      │  │
│  └──────────┬───────────────┘  └──────────┬──────────────────┘  │
│             │ mesh link (TLS 1.3 :19949)  │ mesh link             │
│             ▼                              ▼                      │
│  ┌─ Pane 3 ──────────────────┐  ┌─ Pane 4 ───────────────────┐  │
│  │ codex on linux-a            │  │ hermes --tui on Shadow     │  │
│  └──────────┬───────────────┘  └──────────┬──────────────────┘  │
└─────────────┼─────────────────────────────┼──────────────────────┘
              │                             │
              ▼                             ▼
    ┌──────────────────┐    ┌──────────────────────┐
    │  Shadow (Windows) │◄──►│  linux-b (Linux)       │
    │  NSSM daemon      │    │  systemd daemon      │
    │  100.124.169.66   │    │  203.0.113.12        │
    └────────┬─────────┘    └──────────┬───────────┘
             │                        │
             ▼                        ▼
    ┌──────────────────┐    ┌──────────────────────┐
    │  linux-a (Linux)   │◄──►│  macos-peer (macOS)     │
    │  systemd daemon  │    │  LaunchAgent daemon  │
    │  203.0.113.11   │    │  203.0.113.16     │
    └──────────────────┘    └──────────────────────┘
```

**Every node runs the same binary.** The bridgemind.ai GUI is itself a mesh peer (it has the same keypair, TLS stack, and protocol). Each pane in the GUI is a separate mesh session — not an SSH connection, not a terminal emulator pipe. It attaches directly to the remote node's daemon over the mesh link. No tunnel, no gateway, no central server.

### ADR-001: One TLS connection per pane-session attachment

Each GUI pane opens its own TLS connection to its target node. Eight panes to different nodes = eight connections to different destinations. Simple, independently reconnectable, independently authenticated.

v2 optimization: QUIC connection-per-server muxes all pane streams to one node over one connection. Not needed for v1.

### ADR-002: IPv4 with Tailscale overlay

Nodes address each other by Tailscale IPv4 (`100.x.x.x`). IPv6 dual-stack is the default on Linux hosts but mesh binds `0.0.0.0:19949`. All current nodes are on Tailscale — the mesh relies on Tailscale's WireGuard overlay for NAT traversal, not mesh-native NAT punching.

### ADR-003: Peer mesh, not client-server

Every node is simultaneously a server (accepts connections on `:19949`) and a client (dials seed peers and discovered peers). There is no "bs-server" vs "bs-client" binary — one binary, one role: **peer.** The operator console is a peer; the remote Linux servers are peers. Any node can attach a session to any other node.

This enables:
- The GUI opens panes to multiple nodes simultaneously
- Nodes exchange sessions with each other without GUI involvement
- File transfer between any two nodes without a middleman
- Swarm operations: dispatch sub-agents from any node to any other

---

## 2. The Protocol: bs:// (mesh wire)

### 2.1 Design Principles

| Principle | Implementation |
|-----------|---------------|
| **Reliable** | TLS 1.3 over TCP — most proven transport stack |
| **Secure** | TLS 1.3, ed25519 mutual auth, forward secrecy (X25519) |
| **Compressed** | zstd frame compression per stream (2-8x on terminal output) |
| **Low-latency** | TCP_NODELAY enabled — keystrokes sent immediately |
| **Firewall-friendly** | TCP port 19949 traverses all networks |
| **Multiplexed** | App-level stream IDs for session routing per connection |
| **Peer-to-peer** | Single binary, every node talks to any other node directly |

### 2.2 Message Types (22 total)

| Direction | Type | Hex | Description |
|-----------|------|-----|-------------|
| client → server | Keystroke | 0x01 | Raw key bytes or bracketed paste |
| server → client | Output | 0x02 | PTY stdout |
| client → server | Resize | 0x03 | Terminal dimension change |
| server → client | ClipboardGet | 0x04 | OSC 52 clipboard capture |
| client → server | ClipboardPut | 0x05 | User paste |
| client → server | Attach | 0x06 | Open/navigate to session |
| client → server | Detach | 0x07 | Leave session running |
| server → client | SessionList | 0x08 | Session inventory |
| server → client | ServerInfo | 0x09 | Host metadata |
| both | Ping | 0x0A | Keepalive |
| both | Pong | 0x0B | Keepalive response |
| server → client | Scrollback | 0x0C | History replay chunk |
| client → server | Signal | 0x0D | ^C, ^Z, ^\\, Restart |
| server → client | ProcExited | 0x0E | Foreground process exit |
| client → server | ScrollbackAck | 0x0F | Ready for next chunk |
| server → client | SessionDied | 0x10 | PTY crash notification |
| server → client | ClipboardEcho | 0x11 | Hash confirmation |
| both | ImageData | 0x12 | Static image (PNG/JPEG) |
| both | ImageFrame | 0x13 | Animated frame (GIF) |
| both | ImageAck | 0x14 | Frame consumed |
| both | Hello | 0x15 | Mesh node introduction |
| both | Gossip | 0x16 | Peer list exchange |
| both | SessionSearch | 0x17 | Multi-hop routing query |

### 2.3 Application-Level Stream Multiplexing

**ADR-004: Stream = session attachment, not session lifetime.**

One TLS connection carries multiple independent streams via app-level stream IDs.

```
Frame: [stream_id: u16] [type: u8] [flags: u8] [length: u16] [data]
flags: bit 0 = compressed (zstd), bit 1 = control frame
```

- **Stream ID 0 (CONTROL_STREAM_ID = 0xFFFF):** Control channel (Attach, Detach, Hello, Gossip, Ping, Pong, SessionSearch, SessionList)
- **Stream ID 1–65534:** Session channels (Keystroke, Output, Clipboard, Signal, Image)

| Event | Stream Action |
|-------|--------------|
| Client sends `Attach{session_name}` | Server allocates a stream ID for this attachment |
| Client sends `Detach` | Server sends remaining buffered output, then closes the stream |
| Client disconnects (TCP/TLS lost) | All streams implicitly closed. Session state survives on the peer node |
| Client reconnects + reattaches | New TLS connection, new stream ID for the attachment |

Sessions live 7 days by default (configurable `idle_timeout_hours`). Stream IDs are scoped to one TLS connection (65535 max).

### 2.4 Clipboard — First-class Mesh Primitive

Clipboard is a protocol message, not an escape-sequence passthrough. Works across any mesh links, not just GUI→server.

```
GUI → PEER (paste into remote session):
  1. User copies text on GUI
  2. GUI sends ClipboardPut{text, hash, timestamp} over the mesh link
  3. Peer extracts text, injects into target session's PTY via bracketed-paste
  4. Peer sends ClipboardEcho{hash} to confirm receipt
  5. GUI records last-acked hash — won't re-send same content

PEER → GUI (copy from remote session):
  1. Process inside session emits OSC 52 sequence
  2. Peer daemon captures OSC 52 from PTY output
  3. Peer strips it from terminal stream (never rendered)
  4. Peer sends ClipboardGet{text, hash}
  5. GUI lands it in local clipboard
  6. GUI hashes it — won't re-send back to that peer
```

**ADR-005: Clipboard race prevention via hash echo.**
**ADR-006: Clipboard compression enabled (zstd level 3) for payloads >256 bytes.**

### 2.5 Wire Format

```
┌──────────────────────────────────────────────────────────┐
│ Frame: [stream_id: u16] [type: u8] [flags: u8] [len: u16]│
│        [data: length bytes]                               │
│                                                           │
│ flags: bit 0 = compressed (zstd)                          │
│        bit 1 = control frame                              │
└──────────────────────────────────────────────────────────┘
```

- Binary framing (not newline-delimited JSON)
- zstd compression on frames >256 bytes (Output, Clipboard, Scrollback)
- Never compressed: Keystroke, Ping/Pong (too small to benefit)
- Max frame size: 65,535 bytes (u16). Image/Clipboard frames >64KB use chunking
- Max image payload: 50 MB (separate cap for ImageData/ImageFrame)

---

## 3. Session & PTY Lifecycle

### 3.1 Session States

```
CREATED ──► RUNNING ──► DETACHED (client gone, PTY alive)
    │            │              │
    │            │              ├── ATTACHED (client reconnected)
    │            │              │
    │            ▼              ▼
    │         DIED           KILLED (explicit, or idle timeout)
    │       (PTY crash)         │
    │            │              │
    │     auto_restart?         │
    │       ├─ Yes → RUNNING    │
    │       └─ No  → EXITED     │
    │                           │
    └───────────────────────────┘
```

**ADR-008: PTY death handling.** When PTY exits unexpectedly: send `SessionDied{exit_code, signal}` to attached client(s). If `auto_restart: true`: respawn after `restart_delay_secs`. 3 failures in 60s → EXITED (circuit breaker).

**Lifecycle rules:**
- Sessions survive client disconnects indefinitely (DETACHED state)
- Idle timeout: 7 days default, resets on any PTY output
- Output buffer: zstd-compressed circular buffer, last 16K lines (~8 MB default)
- On reattach: replay last 2K lines in 500-line chunks, each chunk ACK'd

### 3.2 Multi-Attach

`Session::peer_ids` is a `vector<string>` — multiple peers can attach to the same session simultaneously. PTY output fanned out to all attached peers. Keystrokes echoed as `OutputMsg` to other peers. `detach()` is per-peer; session only transitions to DETACHED when all peers leave.

### 3.3 Multi-Hop Routing

`AttachMsg` has a `routing` field. When a node receives an AttachMsg where `routing != its own node_name`, it forwards to the target peer if directly connected, or broadcasts a `SessionSearchMsg` (0x17) to all peers. The target node converts `SessionSearchMsg` to a local `AttachMsg` and handles it normally. This enables shell access to peers not directly connected.

### 3.4 Quick Process Restart (v1.5)

`SignalMsg` (0x0D) extended with `Restart` variant. A peer sends `Signal{Restart, process="hermes"}` over the session stream. The daemon:
1. Kills the target process
2. Spawns a fresh one (same cmd, same PTY)
3. Continues the session — terminal reattaches seamlessly

No SSH, no systemctl, no hoop-jumping. Supported: `bash`, `hermes`, `codex`, `claude`, or any arbitrary command.

### 3.5 Signal Handling

| Client sends | Server does |
|-------------|------------|
| Signal{CtrlC} | Send SIGINT/SIGTERM to foreground process group |
| Signal{CtrlZ} | Send SIGTSTP to foreground process group |
| Signal{CtrlBackslash} | Send SIGQUIT |
| Signal{Restart, process} | Kill + respawn process in same PTY |
| Client disconnect | Session → DETACHED (NOT killed) |
| Daemon shutdown (SIGTERM) | Graceful: wait 5s, SIGKILL all sessions |

### 3.6 Markdown Render Hint (v1.5)

OutputMsg gains a `render_hint` flag (single bit in frame flags). When set, the bridgemind.ai GUI knows this output is markdown and renders it as HTML (headings, code blocks, tables, images) via its browser engine. When clear, output displays as raw terminal text. This makes `hermes agent` output (heavily markdown) beautiful inline.

---

## 4. Peer Discovery & Mesh Formation

### 4.1 Seed Peers

Static config entries in `~/.bridgesessions/config`:
```
seed linux-b 203.0.113.12:19949 pubkey=358e0bb8b4e3bc24...
```

On startup, daemon dials all seeds with TLS 1.3 + Hello exchange. Backoff: 100ms initial, doubles to 30s max, scheduled per-addr to avoid event-loop starvation. One failed dial per loop iteration.

### 4.2 mDNS LAN Discovery

Custom multicast on `224.0.0.252:19949` (NOT standard mDNS `224.0.0.251:5353`). JSON presence announcements every 30s. Auto-adds discovered peers to runtime `config_.discovered` list. **Not persisted** to config file (prevents config_reload churn loop).

### 4.3 Gossip Protocol

Every `gossip_interval_secs` (default 30s), each node sends a `GossipMsg` (0x16) containing its known peers (seeds + discovered) with pubkeys. Recipients merge into their own `discovered` list. Only peers with non-empty pubkeys are gossiped (prevents Hello incompatibility from 8-bit pubkey field overflow).

### 4.4 Duplicate Connection Resolution

When both A and B dial each other simultaneously, two TCP connections form for the same peer pair. Deterministic tie-break:

- Smaller pubkey node → keeps OUTBOUND connection (it initiated)
- Larger pubkey node → keeps INBOUND connection (peer initiated)

Both endpoints independently apply the same rule from the same pubkey pair, converging on the same surviving physical connection. The loser is closed gracefully after the winner is established.

---

## 5. Authentication & Key Management

### 5.1 Key Model

```
┌────────────────────────────────────────────────────┐
│  Keypair: ed25519 (OpenSSL EVP_PKEY_ED25519)        │
│  Stored in: ~/.bridgesessions/                     │
│    id_ed25519.pem        — private key              │
│    id_ed25519-cert.pem   — self-signed X.509 cert   │
│    id_ed25519.pub        — hex pubkey (64 chars)    │
│                                                      │
│  authorized_keys (one hex pubkey per line):          │
│    Controls who can connect to *this* node           │
│    TOFU: first keypair auto-generates on daemon      │
│          first start, no prompt needed               │
└────────────────────────────────────────────────────┘
```

### 5.2 Bootstrap (First Run)

1. Daemon starts, `bootstrap_identity("~/.bridgesessions")` checks for existing keypair
2. If none: generate ed25519 keypair, write `.pem` + `.cert.pem` + `.pub`
3. Legacy migration: if `_bs_autocert.pem` + `_bs_autokey.pem` exist (old format), copy → new names, derive `.pub` hex
4. `create_node_tls()` loads cert+key, configures TLS 1.2+ (not 1.3-only — cross-platform edge compatibility)
5. Accept loop verifies peer certs against `authorized_keys`; unknown → `tls_rejected`

### 5.3 Authorization

```
bridgesessions authorize <hex-pubkey>   # append to authorized_keys
```

One hex pubkey per line, 64 hex chars. No whitespace, no comments. Reloaded from disk on SIGHUP / config mtime change.

### 5.4 mTLS Handshake

1. Client dial, TCP established
2. TLS 1.2+ handshake with client + server certificates (ed25519)
3. Custom verify callback checks peer pubkey against `authorized_keys`
4. On success: exchange Hello (node name, pubkey, version, known peers)
5. On failure: close TLS, log `tls_connect_failed` / `tls_accept_failed` with pubkey snippet for debugging

**Bounded handshake:** Both `SSL_connect` and `SSL_accept` are wrapped with `ssl_handshake_blocking()` — uses `select()` + deadline instead of spinning on `SSL_ERROR_WANT_READ/WRITE`. Inbound timeout 2s, outbound timeout 3s.

---

## 6. Mesh Controller (Daemon Architecture)

### 6.1 Event Loop

Single-threaded `select()`-based event loop in `MeshController::run()`:

```
while (running_):
  1. Rebuild fd_set: listen_fd + cli_ipc_listen_fd + all conn sockets
  2. select() (with ping_interval_secs timeout)
  3. [SIGHUP check on POSIX]: reload_seeds_from_disk()
  4. [IPC readable]: cli_ipc_accept_one() — event-driven, not polled
  5. [listen_fd readable]: accept new peer, begin TLS + Hello
  6. [conn socket readable]: check_conn_read → drain buffered frames
  7. [conn socket writable]: flush write buffer
  8. try_dial_seeds() — one bounded dial per loop per addr
  9. Ping check: send PingMsg to each conn
  10. Pong timeout: close conns where now - last_pong > pong_timeout_secs
  11. build_gossip() every gossip_interval_secs
  12. mDNS announcement every 30s
```

**Key property:** The event loop is the entire daemon. No thread pool, no async. Every I/O path (accept, connect, read, write, IPC, mDNS) goes through `select()` on sockets. This guarantees no concurrency bugs in session state.

### 6.2 CLI IPC Port

Loopback `127.0.0.1:19980` (kMeshCliPort). Plain-text protocol:

```
>> HEALTH linux-b\n
<< linux-b healthy\n
```

This is the **daemon's own operational API** — not a mesh link. CLI subcommands (`health`, `stats`) query IPC first, then fall back to direct dial only if no daemon answers. IPC response uses `conns_` `last_pong` freshness, NOT a synchronous ping (which the event loop would consume).

### 6.3 Single-Instance Guard

On `run()` start: probe `127.0.0.1:19980` with 1.5s timeout. If a daemon answers, log `mesh_already_running` and refuse to start. Without this guard, `SO_REUSEADDR` lets a second daemon co-bind both ports and split-brain the mesh.

### 6.4 Steady-State Recv Timeout

Established peer sockets get a 10s `SO_RCVTIMEO` (`kPeerRecvTimeoutMs`). This bounds the `check_conn_read` drain loop: `SSL_pending()` only guarantees ≥1 buffered byte (not a full frame), so a frame split across TLS records could block `read_frame` indefinitely. The timeout degrades that to `close_conn` + reconnect via backoff.

---

## 7. Deployment & Operations

### 7.1 Cluster (current)

| Node | OS | Daemon | Binary | Config path |
|------|----|--------|--------|-------------|
| Shadow | Windows 11 | NSSM service | bridgesessions.exe | `C:\Users\Shadow\.bridgesessions\config` |
| linux-b | Linux | systemd bsmesh.service | bsmesh | `~/.bridgesessions/config` |
| linux-a | Linux | systemd bsmesh.service | bsmesh | `~/.bridgesessions/config` |
| macos-peer | macOS | LaunchAgent | bridgesessions | `~/.bridgesessions/config` |

Mesh port: **19949** (TCP, all nodes). CLI IPC port: **19980** (loopback, all nodes). mDNS port: **19949** (UDP, same port).

### 7.2 Firewall

```
Windows: configure-firewall.ps1 — 19949/TCP, 19980/TCP, 19949/UDP
Linux:   ufw allow 19949/tcp (if enabled)
macOS:   no config needed (launchd)
```

### 7.3 Health Matrix

```bash
bash matrix.sh   # → 12-direction health check across all 4 nodes
```

Each node runs `bridgesessions health <peer>` — queries local daemon IPC for live conn `last_pong` freshness. Cross-node checks from Linux nodes use `timeout 12 ./bsmesh health <peer>`.

### 7.4 Logs

| Node | Path | Format |
|------|------|--------|
| Shadow | `C:\Users\Shadow\.bridgesessions\bs-mesh.log` | JSON, spdlog rotating file sink |
| linux-b/linux-a | `~/.bridgesessions/bs-mesh.log` | JSON |
| macos-peer | `~/.bridgesessions/bs-mesh.log` | JSON |

Always truncate logs before diagnosing fresh behavior (`truncate -s0` on POSIX, `Clear-Content` on Windows). Append-only monotonic clock resets per process restart, so tail across cycles mixes state from different binaries.

---

## 8. Compression & Encryption Stack

```
Application:   bs:// protocol messages
                       │
Transport:     TLS 1.3 over TCP
               ├── Forward secrecy (X25519 key exchange)
               ├── TLS session tickets (ephemeral, for resumption)
               └── TCP congestion control (OS-managed)
                       │
Compression:   zstd per-frame
               ├── Level 3 (default: balance speed/ratio)
               ├── 2-5x compression on terminal output
               ├── Disabled for Keystroke messages (< 16 bytes)
               └── Enabled for Clipboard, Output, Scrollback
                       │
Identity:      ed25519 keypair
               ├── Client: proves identity to server (both sides)
               ├── Peer: pinned on first connect, verified thereafter
               └── No CA infrastructure needed
```

**Why TLS over TCP for v1:**
- Zero firewall issues — TCP 19949 traverses everything
- OpenSSL is the most audited crypto library
- TCP congestion control is OS-managed and bulletproof
- App-level multiplexing is ~200 lines of C++
- v2 swaps TCP for QUIC via msquic — same protocol, different transport

**Why zstd:**
- 2-8x compression on ANSI-heavy terminal output
- Faster than gzip at equivalent ratios
- Single link dependency (BSD-licensed)
- Dictionary training possible for common ANSI patterns (v2)

**ADR-012: Server restart invalidates TLS session tickets.** Clients fall back to full handshake — same as SSH.

---

## 9. Language: C++23

| C++23 Strength | Why It Matters Here |
|---------------|-------------------|
| Zero-GC latency | No GC pauses in relay loop that runs for years |
| `std::expected<T,E>` | Fallible ops without exception overhead |
| `std::print` | Type-safe formatted output |
| `std::flat_map` | Cache-friendly session map |
| `std::mdspan` | Zero-overhead view for ring buffer spans |
| `deducing this` | Cleaner CRTP for codec/ring buffer templates |
| `[[assume(expr)]]` | Optimization hints for hot relay loop |
| OpenSSL native bindings | Direct EVP_PKEY_ED25519, no CGo |
| Single binary deploy | Static link → ~1.1 MB binary on Windows |
| RAII everywhere | unique_ptr for fds, custom deleters for SSL_CTX |
| msquic path to v2 | Library swap, not a rewrite |

**Why not Go?** GC pauses sub-millisecond, but the real win: C++ makes v2 QUIC migration a library swap under the same protocol layer, not a full rewrite. C++23's `std::expected` and `std::print` keep the code modern without pulling in Boost or fmt.

---

## 10. v1.4 Feature Cut (Shipped)

| Area | Status |
|------|--------|
| Single-binary peer-to-peer mesh | ✅ |
| TLS 1.3 mTLS with ed25519 + TOFU | ✅ |
| 22 message types (0x00–0x17) | ✅ |
| Session lifecycle (multi-attach, detach, kill, resurrect) | ✅ |
| mDNS LAN discovery + gossip peer propagation | ✅ |
| Config with seeds, pubkeys, CRLF-safe parsing | ✅ |
| Daemon health IPC :19980 (all platforms) | ✅ |
| CLI: shell, sessions, peers, keygen, authorize, health, stats, image, anim | ✅ |
| Session persistence (v1:plain JSON, atomic write) | ✅ |
| Ring buffer + scrollback replay (chunked, ACK'd) | ✅ |
| Clipboard: OSC 52 capture + ClipboardPut + hash echo | ✅ |
| Duplicate-conn resolution (deterministic tie-break) | ✅ |
| Reconnect backoff with per-addr scheduling | ✅ |
| Single-instance guard | ✅ |
| Bounded TLS handshake (select + deadline) | ✅ |
| Steady-state recv timeout (10s) on peer sockets | ✅ |
| Cross-platform: Windows MSVC / Linux g++ / macOS clang | ✅ |
| Tests: 1009 assertions, 16 suites | ✅ |
| 4-node production validation, 12/12 health green | ✅ |

## v1.5 — In Progress

| Feature | Wire change | Status |
|---------|-------------|--------|
| `file send` / `file recv` — mesh-native peer-to-peer transfer | New FileTransfer/FileChunk types | `[ ]` |
| `restart` signal — kill+respawn process over mesh | Extend SignalMsg with Restart | `[ ]` |
| `render_hint` flag on OutputMsg — markdown vs raw terminal | Flag bit in frame header | `[ ]` |
| `edit <peer>:<path>` — open remote file, save delta patch | Reuses transfer infrastructure | `[ ]` |
| Virtual folder mapping — local↔remote live sync | New daemon filesystem-watch thread | `[ ]` |
| `stats` IPC parity — daemon conns/sessions over :19980 | Parse in cli_ipc_accept_one | `[ ]` |

## v2 — Future

| Feature | Rationale |
|---------|-----------|
| QUIC via msquic | Eliminates TCP head-of-line blocking on high-latency links |
| Nonblocking TLS handshakes in event loop | Kill last blocking call for total async |
| Session recording + replay | ImageFrame already has frame-order metadata |
| Read-only spectators (fan-out mode) | Shared session view without write access |
| SRV record peer discovery | DNS-based fleet discovery, no seed config |
| Dictionary-trained zstd | Higher compression ratio on ANSI terminal output |

---

## 11. Source Layout

```
bridgesessions/
├── bridgesessions.cpp          ← Single source file (~5,847 lines, ~232 KB)
├── CMakeLists.txt              ← CMake project (optional; `cl`/`g++` preferred)
├── _run_tests.ps1              ← Windows test harness (builds + runs 16 suites)
├── tests/                      ← 16 Catch2 test suites (include bridgesessions.cpp directly)
│   ├── test_message.cpp
│   ├── test_codec.cpp
│   ├── test_frame_io.cpp
│   ├── test_tls.cpp
│   ├── test_tls_reliability.cpp
│   ├── test_session.cpp
│   ├── test_session_registry.cpp
│   ├── test_mesh.cpp
│   ├── test_mesh_reliability.cpp
│   ├── test_config.cpp
│   ├── test_identity.cpp
│   ├── test_osc52.cpp
│   ├── test_ring_buffer.cpp
│   ├── test_relay.cpp
│   ├── test_multi_attach.cpp
│   └── test_authorized_keys_reload.cpp
├── docs/
│   ├── ARCHITECTURE.md         ← This file
│   ├── GUIDELINE.md            ← Design sketch + vision
│   ├── PLANS.md                ← Implementation plan
│   ├── TODO.md                 ← Task checklist
│   └── AUTONOMOUS.md           ← Agent dispatch rules
├── scripts/                    ← Build helpers
├── scratch/                    ← Ignored one-off debug scripts
├── matrix.sh                   ← 4-node health matrix
├── configure-firewall.ps1      ← Windows firewall rules
├── install-daemon.ps1          ← Windows NSSM setup
├── config.shadow.production.example
├── authorized_keys.all         ← Consolidated 4-node pubkey set
├── config.{linux-a,linux-b,macos-peer} ← Per-node deployment configs
├── com.bridgesessions.mesh.plist  ← macOS LaunchAgent template
├── deploy_remote.sh            ← Linux deploy helper
└── build_release.ps1           ← Windows release build
```

---

## 12. Architecture Decision Records — Index

| ADR | Decision | Section |
|-----|----------|---------|
| 001 | One TLS connection per pane-session attachment | §1 |
| 002 | IPv4 with Tailscale overlay | §1 |
| 003 | Peer mesh, not client-server | §1 |
| 004 | App-level stream ID = session attachment, not session lifetime | §2.3 |
| 005 | Clipboard race prevention via hash echo | §2.4 |
| 006 | Clipboard compression enabled (zstd) | §2.5 |
| 008 | PTY death handling with auto_restart | §3.1 |
| 012 | Daemon restart = full TLS handshake | §8 |
| 013 | TLS-over-TCP for v1; QUIC via msquic for v2 | §8 |
| 014 | TCP congestion control (OS-managed) is adequate | §8 |
| 015 | No connection migration in v1; reconnect with backoff | §9 |
| 017 | TCP primary transport; no fallback needed | §7.2 |
| 018 | Hand-rolled binary framing over schema frameworks | §2.5 |
| 019 | ALPN identifies protocol; first message negotiates version | — |
| 021 | No keystroke batching — send immediately | §2.1 |
| 022 | TOFU with explicit `keygen` command | §5.2 |
| 023 | authorized_keys flat file | §5.1 |
| 025 | Same-user model, env sanitization | — |
| 027 | Raw ANSI ring buffer, zstd-compressed, 16K lines | §3.1 |
| 028 | Session metadata persistence (JSON), explicit resurrect | §3.1 |
| 029 | Per-user daemon (mosh model, no root) | §7 |
| 030 | Raw mode on stdin distinguishes Ctrl+D from PTY close | — |
| 032 | System clipboard polling at 500ms | §2.4 |
| 033 | Plain text clipboard v1; binary + MIME for v2 | — |
| 035 | bridgemind.ai GUI is a mesh peer, not a terminal emulator | §1 |
| 037 | Single-source cross-platform build (no libs) | §11 |

### C++-Specific ADRs

| ADR | Rule |
|-----|------|
| C01 | RAII for all resources. unique_ptr for fds, custom deleters for SSL_CTX |
| C02 | No exceptions across thread boundaries. `std::expected<T, Error>` |
| C03 | Single-file — no library targets, everything in bridgesessions.cpp |
| C04 | Compile-time polymorphism: std::variant + std::visit for message types |
| C05 | Test suite before every release commit |

---

## 13. Key Data: 4-Node Cluster

| Node | OS | Tailscale IP | Pubkey (first 16) | Daemon |
|------|----|-------------|-------------------|--------|
| Shadow | Windows 11 | 100.124.169.66 | e702d6ad10e1891f | NSSM |
| linux-b | Linux | 203.0.113.12 | 358e0bb8b4e3bc24 | systemd |
| linux-a | Linux | 203.0.113.11 | 80f749207dabc121 | systemd |
| macos-peer | macOS | 203.0.113.16 | b29a006f19ac5037 | LaunchAgent |

Mesh forms within ~100s of all daemons starting. 12/12 health directions all `healthy`. `pong_timeout=0`, `config_reload=0` across all nodes (no flap, no churn loop).

See `matrix.sh` for reproduction, `install-daemon.ps1` for Shadow, `com.bridgesessions.mesh.plist` for macOS.

---

**Status:** v1.4.0 — cross-platform peer mesh validated on 4 nodes.
**Language:** C++23 (single source file).
**Transport:** TLS 1.3 over TCP v1 → QUIC via msquic v2.
**Auth:** ed25519 mutual TLS + TOFU.
**Compression:** zstd per-frame.
**Next:** v1.5 — file transfer, restart signal, markdown render hint, remote file editing, virtual folder sync.
