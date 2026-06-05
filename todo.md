# bridgesessions — Mesh Implementation TODO

**Target:** `bridgesessions.cpp` — single ~4,000 line file, one binary per device
**Approach:** Port from existing modules, merge, delete dead code, add mesh features
**Test strategy:** TDD — write failing test, implement, run, commit

---

## Phase 0: Clean Slate Setup

- [ ] **0.1** Delete `shadow-agent/` directory entirely (200 lines of skeleton, wrong abstraction)
- [ ] **0.2** Move old `bs-*` directories to `_archive/` for reference during porting
- [ ] **0.3** Create `bridgesessions.cpp` as empty file at repo root
- [ ] **0.4** Create `tests/` directory with `test_codec.cpp`, `test_message.cpp`, `test_tls.cpp`, `test_session.cpp`, `test_peer_config.cpp`, `test_gossip.cpp`
- [ ] **0.5** Update `CMakeLists.txt` to single target `bridgesessions` + `bridgesessions-tests`
- [ ] **0.6** Verify build: empty `main() { return 0; }` compiles on MSVC + vcpkg

---

## Phase 1: Protocol Layer (port from bs-protocol)

**Goal:** All 20 message types + 2 new mesh types, encode/decode round-trip, zstd compression, SHA-256. No network I/O — pure data layer.

- [ ] **1.1** Port message types from `bsprotocol/message.hpp` into `bridgesessions.cpp` (~200 lines)
  - Copy all 20 `MessageType` enum values
  - Copy all payload structs (KeystrokeMsg, OutputMsg, ResizeMsg, etc.)
  - Copy `Message` variant
  - Copy `Frame` struct + constants (FRAME_HEADER_SIZE, MAX_FRAME_SIZE, etc.)
  - **ADD** `Hello` (0x15) and `Gossip` (0x16) message types + their payload structs:
    ```cpp
    struct PeerInfo {
        std::string name;
        std::string addr;        // "host:port"
        std::string pubkey_hex;  // ed25519 public key
        uint64_t last_seen = 0;  // unix timestamp
    };
    struct HelloMsg {
        std::string node_name;
        std::string version;
        std::string pubkey_hex;
        std::vector<PeerInfo> known_peers;
    };
    struct GossipMsg {
        std::vector<PeerInfo> peers;
    };
    ```
  - Update `Message` variant to include HelloMsg, GossipMsg
  - Update `index_to_type[]` array to include the 2 new entries
  - Write `test_message.cpp`: verify variant holds all 22 types, `message_type()` returns correct byte for each

- [ ] **1.2** Port codec from `bsprotocol/codec.cpp` (~350 lines)
  - Copy Serializer, Decoder helpers
  - Copy all `serialize_msg()` overloads
  - Copy `encode()`, `decode()`, `message_type()`, `sha256_hex()`
  - Copy zstd_compress/zstd_decompress
  - **ADD** serialize/deserialize for HelloMsg and GossipMsg
  - Write `test_codec.cpp`: round-trip every message type, test compression threshold, test truncated frames, test malformed input, test fuzz

- [ ] **1.3** Run protocol tests: all 22 types round-trip, compression works, SHA-256 matches known vectors

---

## Phase 2: TLS + Identity Layer (port from bs-transport)

**Goal:** TLS 1.3 mTLS with ed25519. One unified context creation function works for both listen and connect. Node identity: one keypair per device.

- [ ] **2.1** Port TLS from `bstransport/tls.cpp` (~350 lines)
  - Copy OpenSSL init, RAII deleters (SslCtxDeleter, SslDeleter)
  - Copy `generate_cert_key_pair()`, `pubkey_hex_from_pem()`, `peer_public_key_hex()`
  - Copy `AuthorizedKeys` struct (load from file, contains() check)
  - **MERGE** `create_server_context()` + `create_client_context()` → `create_node_tls(config, mode)`:
    ```cpp
    enum class TlsMode { Listen, Connect };
    struct NodeTlsConfig {
        std::string cert_file;
        std::string key_file;
        std::string authorized_keys_file;  // for Listen mode: who can connect
        TofuCallback tofu_cb;              // for Connect mode: verify server fingerprint
    };
    SslCtxPtr create_node_tls(const NodeTlsConfig& cfg, TlsMode mode);
    ```
    - Listen mode: loads authorized_keys, sets SSL_VERIFY_PEER with custom verify callback
    - Connect mode: sets SSL_VERIFY_PEER with TOFU callback
    - Both: TLS 1.3 only, session cache enabled
  - **FIX** memory leak: store AuthorizedKeys* and TofuCallback* in a way that's freed with SSL_CTX (use `SSL_CTX_set_ex_data` + a free callback, or accept the tiny one-time leak)
  - Write `test_tls.cpp`: generate cert, create context in both modes, TLS handshake between two contexts, verify peer pubkey extraction

- [ ] **2.2** Port frame I/O from `bstransport/frame_io.cpp` (~100 lines)
  - Copy `read_frame()`, `write_frame()`, `ssl_check()` helper
  - Write test: frame round-trip over a TLS connection

- [ ] **2.3** Identity bootstrap
  - On startup, if `~/.bridgesessions/id_ed25519.pem` doesn't exist, auto-generate
  - Write pubkey hex to `~/.bridgesessions/id_ed25519.pub`
  - Migrate from legacy `_bs_autocert.pem` + `_bs_autokey.pem` if present and `id_ed25519*` missing
  - **FIX** `is_gif_magic()` dead-code bug (remove the function, only use `is_gif_magic_alt()`)

---

## Phase 3: Session + PTY Layer (port from bs-server)

**Goal:** Local PTY session management. Sessions are locally owned; peer_id tracks which remote node is attached. ConPTY on Windows, fork+exec on POSIX.

- [ ] **3.1** Port ring buffer from `ring_buffer.hpp` (~250 lines)
  - Copy entire template class
  - No changes needed

- [ ] **3.2** Port OSC 52 scanner from `osc52_capture.hpp` (~130 lines)
  - Copy `scan_osc52()`, base64 decoder, detail helpers
  - No changes needed

- [ ] **3.3** Port persistence from `persistence.hpp` (~200 lines)
  - Copy `SessionMeta` struct, `save_sessions()`, `load_sessions()`
  - No changes needed

- [ ] **3.4** Port logging from `logging.hpp` (~60 lines)
  - Copy `get_logger()`, `log_event()`
  - Update log path to `bs-mesh.log`

- [ ] **3.5** Port session + PTY from `session.cpp` + `pty.cpp` (~400 lines merged)
  - Copy `Session` struct, **ADD** `peer_id` field:
    ```cpp
    struct Session {
        std::string name;
        std::string peer_id;     // pubkey hex of attached remote peer (empty if detached)
        std::string command;
        #ifdef _WIN32
        HANDLE master_fd, child_pid, write_handle, hpcon;
        #else
        int master_fd, child_pid;
        #endif
        SessionState state;
        RingBuffer<1'048'576> scrollback;
        // timestamps, auto_restart fields...
    };
    ```
  - Copy Session destructor, move constructor/assignment
  - Copy `create_session()` — ConPTY on Windows, fork+exec on POSIX
  - Copy `resize_pty()`
  - Copy `SessionState` enum, `session_state_str()`
  - Write `test_session.cpp`: create session, write keystrokes, read output, verify scrollback, resize, kill

- [ ] **3.6** Port session registry (simplified from session_manager.cpp, ~300 lines)
  - **SIMPLIFY**: no more `owner_id` in keys. Session names are locally unique. `peer_id` just tracks who's attached.
  - Single `std::unordered_map<std::string, std::unique_ptr<Session>> sessions_`
  - Methods: `attach()`, `detach()`, `list()`, `get()`, `kill()`, `reap_dead()`, `prune_idle()`, `resurrect()`, `load_persisted_sessions()`, `save_persisted_sessions()`
  - Thread-safe: `std::shared_mutex`
  - Auto-restart with circuit breaker (3 failures in 60s window)
  - Write test: create, attach, detach, kill, list, resurrect

---

## Phase 4: Config Layer (NEW)

**Goal:** YAML config per device. Read on startup, write updates (peer discovery). Hand-rolled or minimal YAML subset.

- [ ] **4.1** Define `MeshConfig` struct with all fields from plan.md §4.2
  ```cpp
  struct PeerEntry {
      std::string name;
      std::string addr;       // "host:port"
      std::string pubkey_hex; // empty until discovered via Hello
      uint64_t last_seen = 0;
  };
  struct MeshConfig {
      std::string node_name = "unnamed";
      std::string listen_addr = "0.0.0.0";
      uint16_t listen_port = 19948;
      size_t max_peers = 50;
      int gossip_interval_secs = 30;
      int reconnect_backoff_max_secs = 30;
      int ping_interval_secs = 5;
      int pong_timeout_secs = 30;
      std::vector<PeerEntry> seeds;
      std::vector<PeerEntry> discovered;
      std::string authorized_keys_path;
      std::string persistence_path;
      int scrollback_lines = 2000;
      int idle_timeout_hours = 168;
      std::string default_shell;
      std::string terminal = "xterm-256color";
  };
  ```

- [ ] **4.2** Implement YAML config parser/loader (~150 lines)
  - Use nlohmann/json (read YAML? No — nlohmann is JSON-only.)
  - **Decision:** Config is YAML-like but actually JSON for v1:
    ```json
    // ~/.bridgesessions/config.json
    {
      "node": { "name": "shadow", "listen": ":19948" },
      "mesh": { "max_peers": 50, "gossip_interval_secs": 30 },
      "peers": {
        "seeds": [
          {"name": "linux-a", "addr": "203.0.113.11:9948"}
        ],
        "discovered": []
      },
      "sessions": { "persistence_path": "...", "default_shell": "cmd.exe" }
    }
    ```
  - Actually: write a simple YAML subset parser that handles exactly the format we need (~100 lines of string splitting, no library needed). Avoids forcing users to write JSON.
  - **Alternative (simpler):** Use key=value format like the existing `hosts` file, extended:
    ```
    # ~/.bridgesessions/config
    node.name shadow
    node.listen :19948
    mesh.max_peers 50
    mesh.gossip_interval_secs 30
    seed linux-a 203.0.113.11:9948
    seed linux-b 203.0.113.12:19948
    ```
    This is trivial to parse. Zero dependencies. Same format as the existing `hosts` and `authorized_keys` files. Users already understand it. Go with this.
  - Parse function: `MeshConfig load_config(const std::string& path)`
  - Save function: `void save_config(const std::string& path, const MeshConfig& cfg)` (updates discovered peers)

- [ ] **4.3** Write `test_peer_config.cpp`: load config, save config, round-trip, defaults for missing fields

---

## Phase 5: Mesh Connection Manager (NEW)

**Goal:** Single event loop that accepts inbound connections, initiates outbound connections to seeds/discovered peers, handles gossip, resolves duplicate connections. The heart of the mesh.

- [ ] **5.1** Connection struct
  ```cpp
  struct MeshConn {
      std::string peer_name;       // "linux-a"
      std::string peer_pubkey;     // hex, learned via Hello
      std::string peer_addr;       // "203.0.113.11:9948"
      SslPtr ssl;
      SOCKET sock_fd;
      bool is_outbound;            // did we initiate this?
      std::chrono::steady_clock::time_point last_pong;
      Session* attached_session;   // which local session the remote peer is in (nullptr if none)
      std::string remote_session;  // which remote session we're attached to (empty if none)
  };
  ```

- [ ] **5.2** Accept loop (port from bs-server main.cpp, simplified)
  - Create listen socket (dual-stack IPv4/IPv6, SO_REUSEADDR)
  - On accept: TLS handshake, verify peer against authorized_keys
  - Extract peer pubkey
  - Check for duplicate connection (same pubkey already connected?)
  - If duplicate and our pubkey < their pubkey: drop new connection, keep existing
  - If duplicate and our pubkey > their pubkey: drop existing, accept new
  - Exchange `Hello` messages (send ours, receive theirs)
  - Store peer info from Hello (name, pubkey, known peers)
  - Add to `peers.discovered` in config
  - Save config

- [ ] **5.3** Outbound connect (port from bs-client main.cpp tls_connect + connect_and_relay)
  - For each seed in config: resolve address, TLS connect, present our cert
  - TOFU: on first connect to a peer, trust their cert fingerprint
  - On mismatch: log warning, reject (user must update config or re-authorize)
  - Exchange `Hello` messages
  - Check for duplicate: if peer already connected via inbound, drop this outbound
  - Add to `peers.discovered`

- [ ] **5.4** Event loop
  - Single `select()` (Windows) or `poll()` (POSIX) across:
    - Listen socket
    - All connected peer sockets
    - Timer for gossip (every `gossip_interval_secs`)
    - Timer for ping keepalive (every `ping_interval_secs`)
    - Timer for LAN discovery broadcast (every 60s)
  - Connection lifecycle:
    - Read frames from any peer socket
    - Route frames to appropriate handler (session attach, keystroke, clipboard, gossip, etc.)
    - Check pong timeout per connection
    - Reconnect with backoff for disconnected seeds

- [ ] **5.5** Gossip implementation
  - Every `gossip_interval_secs`:
    - Build `GossipMsg` with all known peers (seeds + discovered)
    - Send to all connected peers
  - On receiving `GossipMsg`:
    - For each unknown peer: add to `peers.discovered`, save config
    - Try to connect to newly discovered peers (if under `max_peers`)
    - Forward gossip to other peers (don't echo back to sender)

- [ ] **5.6** mDNS LAN discovery
  - Windows: use `DNS-SD` via Win32 API or simple UDP multicast
  - POSIX: send UDP multicast to `224.0.0.251:5353` with JSON payload
  - Listen for other nodes' broadcasts
  - On receiving a broadcast: add to `peers.discovered` if new, try to connect
  - **Note:** This is Phase 5.6 — if too complex for v1, skip. Seed-based bootstrap + gossip covers the mesh.

- [ ] **5.7** Write `test_gossip.cpp`:
  - Start 3 nodes on different ports, connect them in a chain
  - Verify gossip propagates peer info to all nodes
  - Verify duplicate connection resolution
  - Verify reconnection after disconnect

---

## Phase 6: Unified Relay (merge from bs-client + bs-server)

**Goal:** One relay loop handles all message types for all connections. Direction-agnostic: same code dispatches whether we're hosting a session or attached to a remote session.

- [ ] **6.1** Inbound session handler (peer wants a shell on us)
  - On `AttachMsg` from peer:
    - Extract `session_name` and `routing` hint
    - If `routing` is set and not our name: forward to that peer (future)
    - Else: call session registry `attach(name, cmd, cols, rows, term)`
    - Set `conn.attached_session = session*`
    - Send scrollback if reattaching
    - Start streaming output via `OutputMsg`
  - On `KeystrokeMsg` from peer:
    - Write to `conn.attached_session->write_handle`
  - On `ResizeMsg` from peer:
    - Call `resize_pty(conn.attached_session->hpcon, cols, rows)`
  - On `DetachMsg` from peer:
    - Set session state to Detached, clear `conn.attached_session`
  - On `SignalMsg` from peer:
    - Send appropriate signal to child process
  - On `ClipboardMsg` from peer:
    - Write bracketed paste to PTY, echo hash back

- [ ] **6.2** Outbound session relay (we want a shell on a peer)
  - On user command `shell linux-a`:
    - Send `AttachMsg{session_name, cols, rows, term}` to linux-a's connection
    - Set `conn.remote_session = session_name`
  - On `OutputMsg` from peer:
    - Write to local stdout
  - On `ClipboardMsg` from peer:
    - Write to local clipboard
  - On `ExitCodeMsg` or `SessionDiedMsg` from peer:
    - Clear `conn.remote_session`, notify user

- [ ] **6.3** Common message handling (same for all connections)
  - `PingMsg` → reply with `PongMsg`
  - `PongMsg` → update `last_pong` timestamp
  - `ScrollbackMsg` → write to output, ack
  - `ImageDataMsg` / `ImageFrameMsg` → render, ack
  - `SessionListMsg` → format and display
  - `ServerInfoMsg` → display peer info
  - `HelloMsg` → process peer info, queue gossip
  - `GossipMsg` → merge discovered peers

- [ ] **6.4** PTY output polling
  - For each locally-owned session with an attached peer:
    - Read from PTY master fd
    - Write to ring buffer
    - OSC 52 scan
    - Send `OutputMsg` (+ optional `ClipboardMsg`) to the attached peer's connection
    - Check if child process exited → reap, send `SessionDiedMsg`

- [ ] **6.5** Local terminal integration (when user runs `bridgesessions shell <peer>`)
  - Enable raw mode on local terminal
  - Read from stdin → send `KeystrokeMsg` to peer
  - Detect Ctrl+C, Ctrl+Z, Ctrl+\ → send `SignalMsg`
  - Detect Ctrl+D → send `DetachMsg`
  - Handle SIGWINCH → send `ResizeMsg`
  - Write received `OutputMsg` to stdout
  - Clipboard bridge (poll local clipboard, send `ClipboardMsg`)

---

## Phase 7: CLI + main() (merge from both main.cpp files)

**Goal:** Unified CLI with subcommands for daemon mode, session management, peer management, diagnostics.

- [ ] **7.1** CLI design with CLI11
  ```cpp
  // Default (no subcommand): daemon mode
  bridgesessions [--config ~/.bridgesessions/config]
  
  // Subcommands:
  bridgesessions keygen
  bridgesessions authorize <hex-pubkey>
  bridgesessions shell <peer>[:session] [--cmd <command>] [--cols N] [--rows N]
  bridgesessions sessions [<peer>|--all]
  bridgesessions peers [list|add|remove]
  bridgesessions health <peer>
  bridgesessions stats [--json]
  bridgesessions image <file>
  bridgesessions anim <file>
  bridgesessions host add <name> <addr> [--key ...] [--cert ...]
  bridgesessions host remove <name>
  bridgesessions host list
  ```

- [ ] **7.2** Daemon mode (default)
  - Load config
  - Generate identity if missing
  - Create node TLS context
  - Start listen socket
  - Connect to seeds with backoff
  - Enter event loop (Phase 5.4)
  - Graceful shutdown on SIGINT/SIGTERM: detach all sessions, save config, close connections

- [ ] **7.3** `shell` subcommand
  - Load config, connect to specified peer, send `AttachMsg`, enter relay loop
  - This is essentially the current `bs-client` behavior but uses the mesh connection

- [ ] **7.4** `sessions` subcommand
  - Local: list sessions from registry
  - Remote: send `SessionListMsg` to peer, display response
  - `--all`: query all connected peers

- [ ] **7.5** `peers` subcommand
  - `peers list`: show all seeds + discovered, connection status, uptime
  - `peers add <name> <addr>`: add to seeds in config
  - `peers remove <name>`: remove from seeds in config

- [ ] **7.6** Port `keygen` and `authorize` from keygen.cpp (~60 lines)

---

## Phase 8: Client Support Layer (port from bs-client)

**Goal:** Terminal raw mode, clipboard bridge, image render, peer file management. These are used by the `shell` subcommand and the daemon's PTY output handling.

- [ ] **8.1** Port terminal raw mode from `terminal_raw.cpp` (~100 lines)
  - `enable_raw_mode()`, `restore_terminal()`, `get_winsize()`
  - Windows: `SetConsoleMode` + `ENABLE_VIRTUAL_TERMINAL_INPUT`
  - POSIX: `tcgetattr` + `cfmakeraw`

- [ ] **8.2** Port clipboard bridge from `clipboard_windows.cpp` + `clipboard_bridge.hpp` (~150 lines)
  - Windows: `OpenClipboard` / `GetClipboardData` / `SetClipboardData`
  - macOS: `NSPasteboard` (from `clipboard_bridge.mm`)
  - Linux: `xclip` / `wl-paste` via `popen` (from `clipboard_linux.cpp`)

- [ ] **8.3** Port image render from `image_render.cpp` (~150 lines)
  - `read_binary_file()`, `detect_image_format()`, `parse_gif_metadata()`
  - `make_image_data_message()`, `make_image_frame_message()`
  - `render_image_message()` — chafa on POSIX, text placeholder on Windows
  - **FIX**: remove broken `is_gif_magic()`, keep only `is_gif_magic_alt()`

- [ ] **8.4** Port peer/host management from `host_config.cpp` (~100 lines, simplified)
  - `load_peers()`, `save_peers()`, `upsert_peer()`, `remove_peer()`, `find_peer()`
  - Operate on `peers.seeds` in config

---

## Phase 9: Integration Testing

**Goal:** Two-node mesh, three-node mesh, session relay, gossip, reconnection, duplicate resolution.

- [ ] **9.1** Two-node mesh test
  - Start node A on port 19948, node B on port 19949
  - Configure each to seed the other
  - Verify: both connect, Hello exchanged, peers.discovered populated
  - Run `shell <other>` from each — verify shell works bidirectionally
  - Verify ping/pong keepalive

- [ ] **9.2** Three-node gossip test
  - A seeds B, B seeds C (chain)
  - Verify A discovers C via gossip from B
  - Verify A connects to C
  - Verify all three can shell into each other

- [ ] **9.3** Duplicate connection test
  - A seeds B, B seeds A
  - Start both simultaneously
  - Verify exactly one TCP connection between them
  - Verify the pubkey comparison picks a consistent winner

- [ ] **9.4** Reconnection test
  - Kill node B
  - Verify A detects timeout, marks B disconnected
  - Restart B
  - Verify A reconnects via backoff
  - Verify sessions survive reconnect (detach/reattach)

- [ ] **9.5** Session persistence test
  - Create session on A, attach from B
  - Kill A, restart A
  - Verify session appears in `sessions` list as recoverable
  - B reconnects, attaches — verify session resurrects

- [ ] **9.6** Cross-platform test matrix
  - Windows ↔ Linux (Shadow ↔ linux-a)
  - Windows ↔ Windows (two Windows nodes)
  - Verify ConPTY ↔ PTY interoperability (output encoding, resize, signals)
  - Verify clipboard bridge text round-trip

- [ ] **9.7** Existing client compatibility
  - Start `bridgesessions` on port 19948 (replacing bs-server)
  - Connect with old `bs-client` binary
  - Verify shell works, scrollback works, ping/pong works
  - Wire protocol is unchanged — this should just work

---

## Phase 10: Documentation & Cleanup

- [ ] **10.1** Update `plan.md` with any design decisions made during implementation
- [ ] **10.2** Write `README.md` for the single-file project:
  - Build instructions (Windows MSVC, POSIX g++)
  - Config file format and options
  - Quickstart: 2-node mesh in 5 minutes
  - CLI reference
- [ ] **10.3** Archive old code: move `bs-protocol/`, `bs-transport/`, `bs-server/`, `bs-client/` to `_archive/`
- [ ] **10.4** Delete `shadow-agent/`
- [ ] **10.5** Verify single-file build with no subdirectories:
  ```bash
  cl /std:c++latest /EHsc /Fe:bridgesessions.exe bridgesessions.cpp \
     /I C:/vcpkg/installed/x64-windows/include \
     /link openssl.lib zstd.lib ws2_32.lib
  ```

---

## Implementation Order (Dependency Graph)

```
Phase 0 (setup)
  └─> Phase 1 (protocol) ──> Phase 2 (TLS) ──> Phase 3 (session)
                                                    │
                          Phase 4 (config) ◄────────┘
                                │
                          Phase 5 (mesh conn mgr)
                                │
                          Phase 6 (unified relay)
                                │
                    ┌───────────┴───────────┐
              Phase 7 (CLI)          Phase 8 (client support)
                    │                      │
                    └──────────┬───────────┘
                          Phase 9 (integration tests)
                                │
                          Phase 10 (docs/cleanup)
```

Phases 0-4 are ports from existing code (low risk). Phase 5 is the new mesh logic (highest risk, most novel). Phase 6 merges the relay loops (moderate risk). Phases 7-8 are ports + CLI wiring. Phase 9 validates everything together.

---

## Estimated Line Counts

| Component | Lines |
|---|---|
| Platform abstractions + includes | ~300 |
| Config parsing | ~150 |
| Message types (22) + Frame | ~250 |
| Codec (encode/decode/zstd/SHA-256) | ~350 |
| TLS + identity | ~350 |
| Frame I/O | ~100 |
| Ring buffer | ~250 |
| OSC 52 scanner | ~130 |
| Persistence | ~200 |
| Logging | ~60 |
| Session + PTY | ~400 |
| Session registry | ~300 |
| **Mesh connection manager** | **~500** |
| **Unified relay** | **~500** |
| Clipboard bridge | ~150 |
| Terminal raw mode | ~100 |
| Image render | ~150 |
| Peer/host management | ~100 |
| keygen + authorize | ~60 |
| main() + CLI | ~300 |
| **TOTAL** | **~4,300** |
