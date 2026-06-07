# bridgesessions — Mesh Architecture Plan

**Version:** v1.1.0 (v1.0.0-mesh + mDNS, multi-attach, health, image)
**Date:** 2026-06-07
**Status:** Active

---

## 1. Vision

One binary, one config file, one keypair per device. Every node is a peer — it listens for inbound connections, connects outbound to other peers, and can both host sessions and request sessions on others. No distinction between "server" and "client." A laptop, a cloud VM, a Windows desktop, a Raspberry Pi — they all run the same binary.

**From any node, you can open a shell on any other node in the mesh.** If a brand-new device joins (call it `corp-net`), it bootstraps from one known seed peer, learns the rest of the mesh through gossip, and is immediately reachable by everyone.

---

## 2. What Dies

| Old concept | Fate |
|---|---|
| `bs-server` binary | **Gone** — merged into single `bridgesessions` binary |
| `bs-client` binary | **Gone** — merged into single `bridgesessions` binary |
| `shadow-agent` module | **Deleted** — skeleton that never shipped, wrong abstraction |
| `hosts` file (client-side aliases) | **Gone** — replaced by `peers` section in config |
| `known_servers` file (TOFU cache) | **Gone** — merged into `peers` discovery state |
| `--server` / `--connect` flags | **Gone** — config-driven, no CLI connection target |
| 5 CMakeLists.txt files | **Gone** — one file builds one binary |
| `bs-protocol` static lib | **Gone** — code moves into the single file |
| `bs-transport` static lib | **Gone** — code moves into the single file |
| `authorized_keys` as server-only concept | **Changed** — every node has one, controls who connects IN |
| `SessionManager` with `owner_id` from TLS cert | **Changed** — sessions are locally owned; peer identity is per-connection |

## 3. What Survives

| Component | Changes |
|---|---|
| 20 message types (`message.hpp`) | None — the protocol is sound |
| Codec: encode/decode/zstd/SHA-256 | None — wire format unchanged |
| TLS 1.3 + ed25519 mTLS | Merged into unified `create_node_tls()` — same context handles both listen and connect |
| Frame I/O (`read_frame`/`write_frame`) | None |
| Ring buffer (1MB scrollback) | None |
| OSC 52 clipboard scanner | None |
| ConPTY / POSIX PTY session management | `Session` gains `peer_id` field (which remote node is attached) |
| Clipboard bridge (Win32 / macOS / Linux) | None |
| Terminal raw mode | None |
| Image render (chafa on POSIX, placeholder on Windows) | None |
| JSON session persistence | None |
| spdlog structured logging | None |
| `keygen` subcommand | None |
| `authorize` subcommand | None |
| Exponential backoff for reconnection | None |
| Per-client stats JSON | Adapted: becomes per-connection stats |

---

## 4. Mesh Architecture

### 4.1 Node Identity

Every node has a single ed25519 keypair at `~/.bridgesessions/id_ed25519.pem` + `id_ed25519-cert.pem`. This one keypair is used for:

- **Outbound connections:** presented as the TLS client certificate when connecting to peers
- **Inbound connections:** verified against `authorized_keys` when peers connect to us
- **Node ID:** the hex-encoded public key is the node's canonical identity in the mesh

No distinction between "server cert" and "client cert." One identity per device.

### 4.2 Config File

Each device has `~/.bridgesessions/config` — simple `key value` format with `#` comments. Dotted keys for sections.

```ini
# ── Node identity ──
node.name shadow                      # human-readable alias (for logs, session routing)
node.listen 0.0.0.0:19948            # address:port to listen on for inbound mesh connections
# identity/cert: auto-resolved to ~/.bridgesessions/id_ed25519*.pem

# ── Mesh settings ──
mesh.max_peers 50                     # max simultaneous peer connections
mesh.gossip_interval_secs 30          # how often to exchange peer lists
mesh.reconnect_backoff_max_secs 30    # cap for exponential backoff
mesh.ping_interval_secs 5             # keepalive ping frequency
mesh.pong_timeout_secs 30             # disconnect if no pong in this window

# ── Bootstrap peers ──
seed linux-a 203.0.113.11:19948       # always try to connect to these
seed linux-b 203.0.113.12:19948

# Peers discovered via gossip/TOFU are auto-added to config by the daemon

# ── Authorized inbound peers ──
# (also checked against ~/.bridgesessions/authorized_keys as hex pubkeys, one per line)
sessions.authorized_keys_path ~/.bridgesessions/authorized_keys

# ── Session defaults ──
sessions.persistence_path ~/.bridgesessions/sessions.json
sessions.scrollback_lines 2000
sessions.idle_timeout_hours 168       # 7 days
sessions.default_shell cmd.exe        # or /bin/bash -l on POSIX
sessions.terminal xterm-256color
```

### 4.3 Directory Layout (per device)

```
~/.bridgesessions/
├── config                    ← per-device config (simple key=value format)
├── id_ed25519.pem            ← node identity private key (0600)
├── id_ed25519.pub            ← hex-encoded public key
├── id_ed25519-cert.pem       ← self-signed X.509 cert
├── authorized_keys           ← who can connect to me (hex pubkeys, one per line)
├── sessions.json             ← persisted session metadata
├── bs-mesh.log               ← structured JSON log
├── clients/                  ← per-connection stats JSON (one file per remote peer)
│   └── linux-a-abc123.json
```

---

## 5. Mesh Discovery & Gossip

### 5.1 Bootstrap

1. On startup, read `config.yaml` → `peers.seeds`
2. For each seed, attempt TLS connection with exponential backoff
3. On successful connect, exchange `Hello` messages (node name, pubkey, known peers)

### 5.2 Gossip Protocol

Every `gossip_interval_secs` (default 30s), each node sends its known peer list to all connected peers:

```
GossipMsg {
    vector<PeerInfo> peers;  // all peers I know about
}

PeerInfo {
    string name;             // "shadow", "linux-a", etc.
    string addr;             // "203.0.113.11:9948"
    string pubkey_hex;       // ed25519 public key
    uint64_t last_seen;      // unix timestamp
}
```

On receiving a `GossipMsg`:
1. For each peer in the message that I don't know about: add to `peers.discovered` with TOFU
2. For peers I already know: update `last_seen` timestamp
3. If I learn about a new peer, try to connect to it (if under `max_peers`)
4. Forward the gossip to my other peers (but don't echo back to sender)

### 5.3 LAN Discovery (mDNS) — v1.1

On startup, the daemon joins a custom multicast group (`224.0.0.252:19949` — chosen to avoid conflicting with real mDNS). Every 30s it broadcasts a compact JSON presence announcement:

```json
{"name":"shadow","port":19948,"pubkey":"abc123..."}
```

Other nodes on the same LAN (including the Tailscale flat network) receive these announcements and auto-add discovered peers to `config_.discovered`, then attempt connections. No seed configuration needed for LAN peers.

### 5.4 Duplicate Connection Resolution

If node A connects to node B and node B simultaneously connects to node A, both sides detect the duplicate by comparing pubkeys:

- The node with the **lexicographically smaller pubkey hex** keeps its outbound connection
- The other node drops its outbound and accepts the inbound
- Result: exactly one TLS connection per pair of nodes

### 5.5 Mesh Scaling

| Mesh size | Strategy |
|---|---|
| 2–10 nodes | Full mesh — every node connects to every other node |
| 10–50 nodes | Partial mesh — connect to seeds + gossip-discovered peers up to `max_peers`. Sessions route through intermediate nodes if needed (future: Phase 7) |
| 50+ nodes | Same as above + consider DHT for discovery (future) |

For the initial implementation: **full mesh up to `max_peers`**. Session routing through intermediates (multi-hop) is a future feature.

---

## 6. Session Model

### 6.1 Bidirectional Sessions

The `AttachMsg` no longer implies "client asks server for a PTY." It means: **"I want a session on you."** Whoever receives the message spawns the PTY and owns the session locally.

```
linux-a → shadow:  AttachMsg{session_name="build", cols=120, rows=40}
                 → shadow spawns ConPTY running cmd.exe
                 → shadow streams OutputMsg back to linux-a
                 → linux-a sends KeystrokeMsg to shadow
```

If shadow wants a shell on linux-a:
```
shadow → linux-a:  AttachMsg{session_name="logs", cols=80, rows=24}
                 → linux-a spawns PTY running /bin/bash
                 → linux-a streams OutputMsg back to shadow
```

Same message type, same wire format, same relay loop — just evaluated on whichever node receives it.

### 6.2 Session Identity

Sessions are **locally owned** by the node that spawned the PTY. The `Session` struct gains a `peer_id` field:

```cpp
struct Session {
    std::string name;           // session name
    std::string peer_id;        // pubkey hex of the remote peer currently attached (empty if detached)
    std::string command;        // shell command
    // ... PTY handles, ring buffer, state, timestamps
};
```

Session names are local to each node. `linux-a` can have a session called "build" that's totally independent from `shadow`'s session called "build."

### 6.3 Multi-Attach — v1.1

Multiple peers can attach to the same session simultaneously. Each peer's keystrokes are multiplexed to the same PTY, and output is fanned out to all attached peers. `Session::peer_id` (single string) has been upgraded to `Session::peer_ids` (vector). The `SessionRegistry::attach()` method accepts an optional `peer_pubkey` parameter to track distinct peers.

```cpp
struct Session {
    std::string name;
    std::vector<std::string> peer_ids; // all peers currently attached
    // ... PTY handles etc.
};
```

When all peers detach, the session transitions to `Detached` state and can be reattached later (with scrollback replay). A session only dies when its child process exits.

### 6.4 Session Routing — v2

Multi-hop session routing (forwarding `AttachMsg` through intermediate peers when the target is not directly connected) is deferred to v2. In v1, all nodes in the mesh are directly connected (full mesh up to `max_peers`), so the `AttachMsg` is always received by the intended target directly.

### 6.4 Remote Session Targeting

The CLI for requesting a session on a remote node:

```bash
# Open a shell on linux-a
bridgesessions shell linux-a

# Open a specific named session on linux-a
bridgesessions shell linux-a:build

# Open a session with a custom command
bridgesessions shell linux-a:build --cmd "htop"

# List sessions on a remote node
bridgesessions sessions linux-a

# List sessions on all known peers  
bridgesessions sessions --all
```

---

## 7. Single Binary Structure

`bridgesessions.cpp` — approximately 3,500–4,000 lines in one file.

```
bridgesessions.cpp
│
├── [~300 lines]  Includes, constants, platform abstractions
│                 (NOMINMAX, ssize_t typedefs, SOCKET_CLOSE macros, etc.)
│
├── [~150 lines]  YAML config parsing (hand-rolled or nlohmann/json)
│                 Config struct, load/save, defaults
│
├── [~200 lines]  Message types (ported from bsprotocol/message.hpp)
│                 20 message types, Message variant, Frame struct
│
├── [~350 lines]  Codec (ported from bsprotocol/codec.cpp)
│                 encode(), decode(), zstd_compress/decompress, sha256_hex()
│
├── [~350 lines]  TLS + identity (merged from bstransport/tls.cpp)
│                 create_node_tls(), generate_cert_key_pair(),
│                 server_cert_verify(), client_cert_verify() → unified verify_peer(),
│                 pubkey_hex_from_pem(), peer_public_key_hex()
│
├── [~100 lines]  Frame I/O (ported from bstransport/frame_io.cpp)
│                 read_frame(), write_frame()
│
├── [~250 lines]  Ring buffer (ported from ring_buffer.hpp)
│                 Thread-safe circular buffer, read_last_lines(), snapshot()
│
├── [~130 lines]  OSC 52 scanner (ported from osc52_capture.hpp)
│                 scan_osc52(), base64 decode
│
├── [~200 lines]  Persistence (ported from persistence.hpp)
│                 save_sessions(), load_sessions()
│
├── [~60 lines]   Logging (ported from logging.hpp)
│                 get_logger(), log_event()
│
├── [~400 lines]  Session + PTY (merged from session.cpp + pty.cpp)
│                 Session struct, create_session(), resize_pty(),
│                 Session destructor, reap_dead(), auto_restart logic
│
├── [~300 lines]  Session registry (simplified from session_manager.cpp)
│                 std::unordered_map<name, Session*>, attach/detach/list/get/kill,
│                 resurrect, prune_idle. No more owner_id keys — sessions are
│                 locally owned, peer_id just tracks who's attached.
│
├── [~500 lines]  Mesh connection manager
│                 - Accept inbound connections (like current bs-server main loop)
│                 - Initiate outbound connections (like current bs-client tls_connect)
│                 - Single event loop: select()/WSAPoll() across listener + all peer sockets
│                 - Gossip: PeerHello/GossipMsg exchange
│                 - Duplicate connection resolution
│                 - Reconnection with backoff
│                 - mDNS LAN discovery
│
├── [~500 lines]  Unified relay (merged from both run_relay() implementations)
│                 - One relay loop per connection, direction-agnostic
│                 - Handles inbound sessions (spawn PTY on AttachMsg)
│                 - Handles outbound sessions (send AttachMsg, relay keystrokes)
│                 - All 20 message types handled once
│
├── [~150 lines]  Clipboard bridge (ported from clipboard_windows.cpp + clipboard_bridge.hpp)
│
├── [~100 lines]  Terminal raw mode (ported from terminal_raw.cpp)
│
├── [~150 lines]  Image render (ported from image_render.cpp)
│
├── [~100 lines]  Peer/host management (simplified from host_config.cpp)
│                 load_peers(), save_peers(), upsert_peer(), remove_peer()
│
├── [~60 lines]   keygen + authorize subcommands (ported from keygen.cpp)
│
└── [~300 lines]  main() — CLI11 argument parsing, mode dispatch
                  Subcommands: keygen, authorize, shell, sessions, peers, health, stats
```

---

## 8. Message Type Changes

The existing 20 message types are preserved. Two new types are added for mesh:

| Type | Direction | Purpose |
|---|---|---|
| `Hello` (0x15) | bidirectional | Sent on first connect: node name, pubkey, version, known peer list |
| `Gossip` (0x16) | bidirectional | Periodic peer list exchange |

The `Hello` message replaces the implicit "server banner" — both sides send it after TLS handshake.

---

## 9. CLI

```bash
# Daemon mode (default — no subcommand)
bridgesessions                           # start mesh node, listen + connect

# Key management
bridgesessions keygen                    # generate id_ed25519 keypair
bridgesessions authorize <pubkey>        # add peer to authorized_keys

# Session management
bridgesessions shell <peer>              # open shell on a remote peer
bridgesessions shell <peer> -n <name>    # named session on remote peer
bridgesessions shell <peer> -x <cmd>     # run specific command
bridgesessions sessions                  # list local sessions
bridgesessions sessions <peer>           # list sessions on remote peer
bridgesessions sessions --all            # list sessions on all peers

# Peer management
bridgesessions peers list                # list all known peers + connection status
bridgesessions peers add <name> <addr>   # add a seed peer
bridgesessions peers remove <name>       # remove a peer

# Diagnostics
bridgesessions --version                 # print version
```

**Future (v2):** `stats` — reserved for diagnostics.

**Done in v1.1:** `health <peer>`, `image <file>`, `stats` — ping/pong health check, terminal image preview, connection stats.

---

## 10. Config Per Device

Each device has its own `~/.bridgesessions/config`. The minimum required to join the mesh:

**Brand-new device (corp-net):**
```ini
node.name corp-net
node.listen 0.0.0.0:19948
seed shadow 100.124.169.66:19948
```

That's it. On first run, bridgesessions:
1. Auto-generates `id_ed25519.pem` + cert if missing
2. Connects to shadow
3. Exchanges `Hello` → shadow learns corp-net's pubkey via TOFU
4. Shadow adds corp-net to its `peers.discovered`
5. Shadow gossips corp-net's existence to linux-a, linux-b
6. linux-a and linux-b connect to corp-net
7. corp-net is now a full mesh member

**To authorize corp-net to connect inbound to shadow**, shadow's operator runs:
```bash
bridgesessions authorize <corp-net-pubkey>
```
Or manually adds the pubkey to `authorized_keys`.

---

## 11. Build System

```cmake
# CMakeLists.txt — single file, single target
cmake_minimum_required(VERSION 3.25)
project(bridgesessions VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(zstd REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog REQUIRED)
find_package(Catch2 REQUIRED)  # tests only

add_executable(bridgesessions bridgesessions.cpp)
target_link_libraries(bridgesessions
    OpenSSL::SSL OpenSSL::Crypto
    zstd::libzstd
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    spdlog::spdlog
)
if(WIN32)
    target_link_libraries(bridgesessions ws2_32.lib)
endif()

# Tests (separate target, links same source sans main)
add_executable(bridgesessions-tests
    bridgesessions.cpp
    tests/test_codec.cpp
    tests/test_message.cpp
    tests/test_tls.cpp
    tests/test_session.cpp
    tests/test_peer_config.cpp
    tests/test_gossip.cpp
)
target_compile_definitions(bridgesessions-tests PRIVATE BS_TESTING)
target_link_libraries(bridgesessions-tests
    bridgesessions-libs Catch2::Catch2WithMain
)
```

Or, for a zero-build-system approach:
```bash
# Windows
cl /std:c++latest /EHsc /Fe:bridgesessions.exe bridgesessions.cpp \
   /I C:/vcpkg/installed/x64-windows/include \
   /link C:/vcpkg/installed/x64-windows/lib/openssl.lib \
         C:/vcpkg/installed/x64-windows/lib/zstd.lib \
         ws2_32.lib

# POSIX
g++ -std=c++23 -O2 -o bridgesessions bridgesessions.cpp \
    -lssl -lcrypto -lzstd -pthread
```

---

## 12. Migration Path

1. Build `bridgesessions` single binary
2. Run it alongside existing `bs-server` on a different port for testing
3. Verify mesh connectivity between two nodes
4. Cut over: stop `bs-server`, switch `bridgesessions` to port 19948
5. Existing clients (`bs-client`) can still connect — the wire protocol is unchanged
6. Deprecate `bs-client` — use `bridgesessions shell <peer>` instead

---

## 13. Open Questions / Future

### Done (v1.1)

| Feature | Notes |
|---|---|
| mDNS LAN discovery | Custom multicast on 224.0.0.252:19949, 30s interval |
| Multi-attach (multi-viewer) | `peer_ids` vector, output fan-out |
| `health <peer>` CLI | Ping/pong with 3s timeout |
| `image <file>` CLI | Text placeholder on Windows, chafa on POSIX |
| `stats` CLI | Connection + session statistics |
| Ctrl+D/EOF on Windows | 0x1A (Ctrl+Z) treated as EOF in shell_peer loop |
| Multi-attach keystroke echo | Keystrokes fanned out to all other attached peers |
| `connect_and_hello()` helper | Deduplicates ~35-line connect+TLS+Hello pattern (3 call sites) |
| `find_peer_addr()` helper | Centralizes seed+discovered lookup (3 call sites) |

### Deferred to v2 / Future

| Feature | Priority | Notes |
|---|---|---|
| `stats` CLI subcommand | Medium | Per-connection stats: bytes in/out, uptime, latency |
| `anim <file>` CLI subcommand | Medium | Animated GIF preview via sequential chafa frames |
| `peers list` live status | Medium | Show connection state, latency, uptime — currently config-only |
| Multi-attach: keystroke echo | Low | Keystrokes from peer A should echo to peer B in same session |
| Multi-hop session routing | High | Forward AttachMsg through intermediate peers |
| Session recording / replay | Low | Save session history to file, play back |
| Encryption at rest for sessions | Medium | Encrypt sessions.json |
| Mesh-wide session search | Low | "Find session X on any node" |
| WebRTC transport for browser peers | Low | Browser-based mesh member |
| DHT for >100 node meshes | Low | Gossip scales fine to ~50 |
| NAT traversal without Tailscale | Low | Tailscale provides flat IP space |
| `Ctrl+D` detection on Windows shell | Medium | 0x1A currently treated as key data, not EOF |
| Daemon `--daemon` / `--service` flag | Medium | Install as Windows service or systemd unit |
| TLS close_notify before socket close | Low | Proper SSL_shutdown() for truncation detection |
| Test suite: live mesh integration | High | test_two_node_mesh.ps1, test_three_node_mesh.ps1 — need live nodes |
| Test suite: old client backward compat | Medium | Verify old bs-client works against new daemon |
| Test suite: cross-platform matrix | Medium | Windows↔Linux, ConPTY↔PTY |
| Build: CMakeLists.txt unification | Low | One CMakeLists.txt for both bridgesessions + tests |
| Config: `--config-dir` flag for non-~/.bridgesessions paths | Low | Support portable config directories |
| Docs: man page / `--help` coverage for all subcommands | Low | `shell`, `sessions` subcommands need full help text |
