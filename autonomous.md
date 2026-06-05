# bridgesessions Mesh — Autonomous Build Plan

**Goal:** One binary, one config file, one keypair per device. Mesh architecture — every node is a peer. Single `bridgesessions.cpp` file (~4,300 lines), built with TDD by Hermes sub-agents.

**Start:** 2026-06-05
**Target:** Fully working mesh on Shadow ↔ linux-a ↔ linux-b

---

## Autonomous Workflow

### Agent Model

**ALL work uses Hermes `delegate_task`.** No external CLI tools. Sub-agents get `deepseek/deepseek-v4-pro`, same as parent.

| Complexity | How | Max parallel | Use for |
|---|---|---|---|
| **Complex** | Single-task delegate_task | N/A (serial) | Mesh manager, unified relay, TLS merge, session registry |
| **Medium** | Batch delegate_task | 3 concurrent | Protocol porting, config parser, CLI wiring |
| **Quick** | Batch delegate_task | 3 concurrent | Ring buffer, OSC 52, frame I/O, tests |

Every sub-agent gets `toolsets: ["terminal", "file"]` (add `"web"` if they need to look up library docs).

### Dispatch format

For single tasks:
```
delegate_task(
  goal="Phase X.Y: one-line summary",
  context="<full prompt with file paths, constraints, TDD requirements>",
  toolsets=["terminal", "file"]
)
```

For parallel batches:
```
delegate_task(
  tasks=[
    {goal: "Phase 1.1: ...", context: "...", toolsets: ["terminal", "file"]},
    {goal: "Phase 3.1: ...", context: "...", toolsets: ["terminal", "file"]},
    {goal: "Phase 3.2: ...", context: "...", toolsets: ["terminal", "file"]},
  ]
)
```

### Parallelism Schedule

| Window | Tasks | Type |
|---|---|---|
| After Phase 0 | 1.1 + 3.1 + 3.2 | 3 parallel leaf agents |
| After 1.1+3.1+3.2 | 1.2 + 2.1 + 3.3 | 3 parallel leaf agents |
| After 1.2+2.1+3.3 | 2.2 + 3.4 + 4 | 3 parallel leaf agents |
| After all converge | 5 (mesh) | Serial — critical path |
| After Phase 5 | 6 (relay) | Serial — depends on 5 |
| After Phase 6 | 7 + 8 | 2 parallel leaf agents |
| After 7+8 | 9 (integration) | Sequential |

### TDD Gate

Every sub-agent MUST follow the TDD cycle:
1. Write failing test
2. Verify it fails
3. Write minimal code
4. Verify it passes
5. Commit

If a sub-agent ships code without tests, **reject the commit and re-dispatch**.

---

## Phase 0: Clean Slate Setup

### Task 0.1: Delete shadow-agent and archive old modules

**Execution:** Hermes parent direct (not a sub-agent)
**Duration:** 2 minutes

Steps:
1. Delete `C:\Users\Shadow\bridgesessions\shadow-agent\` (entire directory)
2. Create `C:\Users\Shadow\bridgesessions\_archive\`
3. Move `bs-protocol\`, `bs-transport\`, `bs-server\`, `bs-client\` into `_archive\`
4. Create empty `bridgesessions.cpp` with just: `#include <cstdio>` + `int main() { return 0; }`
5. Update CMakeLists.txt to single target `bridgesessions` (link OpenSSL, zstd, nlohmann, CLI11, spdlog)
6. Verify: `cmake --preset windows-msvc-debug && cmake --build build/windows-msvc-debug`
7. Verify: `build/windows-msvc-debug/bridgesessions.exe` exits 0
8. `git add -A && git commit -m "phase0: clean slate for mesh rewrite"`

---

## Phase 1: Protocol Layer

### Task 1.1: Port message types + add Hello/Gossip (parallel with 3.1, 3.2)

**Dispatch:**
```
delegate_task(
  goal: "Phase 1.1: Port message types into bridgesessions.cpp, add Hello and Gossip types (21 total)",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead plan.md §5-6 and todo.md §Phase 1.\n\nYour task:\n1. Open _archive/bs-protocol/include/bsprotocol/message.hpp\n2. Port all 20 MessageType enum values into bridgesessions.cpp inside namespace bs::mesh\n3. Port all payload structs (KeystrokeMsg, OutputMsg, ResizeMsg, ClipboardMsg, etc.)\n4. Port the Message variant (std::variant<19 types>)\n5. Port Frame struct + constants (FRAME_HEADER_SIZE, MAX_FRAME_SIZE, COMPRESSION_THRESHOLD, MAX_IMAGE_BYTES)\n6. ADD two new message types:\n   - Hello (0x15): node_name, version, pubkey_hex, vector<PeerInfo> known_peers\n   - Gossip (0x16): vector<PeerInfo> peers\n   Where PeerInfo = {name, addr, pubkey_hex, last_seen}\n7. Update Message variant to 21 alternatives (HelloMsg + GossipMsg added)\n8. Update index_to_type[] array to map all 21 variant indices to wire bytes\n9. Write tests/tests/test_message.cpp with Catch2:\n   - Verify variant holds all 21 types\n   - Verify message_type() returns correct byte for each\n   - Verify struct equality operators work\n10. Verify: ctest --test-dir build/windows-msvc-debug -R 'message' --output-on-failure\n11. Commit: 'phase1.1: port message types + add Hello/Gossip (21 types)'\n\nMUST follow TDD: write test first, watch it fail, then implement.\n\nBuild command: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest command: cd C:/Users/Shadow/bridgesessions && ctest --test-dir build/windows-msvc-debug -R 'message' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 1.2: Port codec (encode/decode + SHA-256 + zstd) — run after 1.1

**Dispatch:**
```
delegate_task(
  goal: "Phase 1.2: Port codec into bridgesessions.cpp — encode, decode, SHA-256, zstd for 21 types",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead todo.md tasks 1.2-1.3.\n\nYour task:\n1. Open _archive/bs-protocol/src/codec.cpp\n2. Port into bridgesessions.cpp (namespace bs::mesh):\n   - Serializer/Decoder helper structs\n   - All serialize_msg() overloads for ALL 21 types\n   - Add serialize/deserialize for HelloMsg and GossipMsg\n   - encode() function: message -> wire bytes\n   - decode() function: wire bytes -> message\n   - message_type() function\n   - sha256_hex() function (OpenSSL EVP)\n   - zstd_compress() / zstd_decompress() helpers\n   - max_encoded_size() function\n3. Write tests/tests/test_codec.cpp with Catch2:\n   - Round-trip EVERY message type (all 21)\n   - Test compression threshold (payload >256 bytes gets FLAG_COMPRESSED)\n   - Test truncated frames throw\n   - Test malformed input throws\n   - Test SHA-256 against known vectors ('abc' -> 'ba7816bf...')\n   - Test image payloads >50MB cap rejected\n4. Verify: ctest --test-dir build/windows-msvc-debug -R 'codec' --output-on-failure\n5. Commit: 'phase1.2: port codec with encode/decode/SHA256/zstd (21 types round-trip)'\n\nMUST follow TDD for EVERY message type.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest --test-dir build/windows-msvc-debug -R 'codec' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 2: TLS + Identity Layer

### Task 2.1: Port TLS with unified create_node_tls() — run after Phase 1

**Dispatch:**
```
delegate_task(
  goal: "Phase 2.1: Merge TLS server+client contexts into unified create_node_tls()",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead plan.md §2-4 and todo.md §Phase 2.\n\nYour task:\n1. Open _archive/bs-transport/src/tls.cpp\n2. Port into bridgesessions.cpp (namespace bs::mesh):\n   - RAII deleters: SslCtxDeleter, SslDeleter\n   - generate_cert_key_pair(common_name) -> {cert_pem, key_pem}\n   - pubkey_hex_from_pem(key_pem) -> hex string\n   - peer_public_key_hex(ssl*) -> hex string\n   - AuthorizedKeys struct (load from file, contains() check)\n   - extract_raw_pubkey(), pubkey_hex(), hex_decode() helpers\n3. MERGE create_server_context() + create_client_context() into ONE function:\n\n   enum class TlsMode { Listen, Connect };\n   struct NodeTlsConfig {\n       std::string cert_file;\n       std::string key_file;\n       std::string authorized_keys_file;\n       std::function<bool(const std::string&)> tofu_cb;\n   };\n   SslCtxPtr create_node_tls(const NodeTlsConfig& cfg, TlsMode mode);\n\n   - Listen: loads authorized_keys, sets SSL_VERIFY_PEER with custom callback\n   - Connect: sets SSL_VERIFY_PEER with TOFU callback\n   - Both: TLS 1.3 only, session cache enabled\n4. Write tests/tests/test_tls.cpp:\n   - Generate keypair, create Listen+Connect contexts, TLS handshake\n   - Verify peer pubkey extraction matches\n   - Verify authorized_keys: allowed key passes, wrong key fails\n   - Verify TOFU: first connect accepts, same fp accepts, different fp rejects\n5. Verify: ctest -R 'tls' --output-on-failure\n6. Commit: 'phase2.1: unified create_node_tls() with Listen/Connect modes'\n\nMUST follow TDD. Accept the one-time AuthorizedKeys*/TofuCallback* leak per context.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'tls' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 2.2: Port frame I/O — run after 2.1

**Dispatch:**
```
delegate_task(
  goal: "Phase 2.2: Port frame I/O (read_frame/write_frame) into bridgesessions.cpp",
  context: "Work directory: C:/Users/Shadow/bridgesessions\n\nYour task:\n1. Open _archive/bs-transport/src/frame_io.cpp\n2. Port read_frame() and write_frame() into bridgesessions.cpp (namespace bs::mesh)\n3. These are thin wrappers over SSL_read_ex/SSL_write_ex calling encode()/decode()\n4. Write a test that round-trips a frame through a TLS connection from create_node_tls()\n5. Verify: ctest -R 'frame_io' --output-on-failure\n6. Commit: 'phase2.2: port frame I/O (read_frame/write_frame)'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'frame_io' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 2.3: Identity bootstrap — run after 2.1

**Dispatch:**
```
delegate_task(
  goal: "Phase 2.3: Identity bootstrap — auto-generate ed25519 keypair on first run",
  context: "Work directory: C:/Users/Shadow/bridgesessions\n\nYour task:\n1. Add identity bootstrap to bridgesessions.cpp:\n   - At startup, if ~/.bridgesessions/id_ed25519.pem doesn't exist:\n     a. Generate keypair via generate_cert_key_pair('bridgesessions')\n     b. Write key to ~/.bridgesessions/id_ed25519.pem (0600 perms)\n     c. Write cert to ~/.bridgesessions/id_ed25519-cert.pem\n     d. Write pubkey hex to ~/.bridgesessions/id_ed25519.pub\n   - If legacy _bs_autocert.pem + _bs_autokey.pem exist but id_ed25519* don't: migrate them\n2. Write a test that:\n   - Uses a temp dir as 'home'\n   - Calls bootstrap\n   - Verifies all 3 files created with correct content\n3. Verify: ctest -R 'identity' --output-on-failure\n4. Commit: 'phase2.3: identity bootstrap with auto-keygen on first run'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'identity' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 3: Session + PTY Layer

### Task 3.1: Port ring buffer (parallel with 1.1, 3.2)

**Dispatch:**
```
delegate_task(
  goal: "Phase 3.1: Port RingBuffer template into bridgesessions.cpp",
  context: "Work directory: C:/Users/Shadow/bridgesessions\n\nYour task:\n1. Open _archive/bs-server/src/ring_buffer.hpp\n2. Port the entire RingBuffer<Capacity> template class into bridgesessions.cpp (namespace bs::mesh)\n3. No changes needed — it's a pure template, no OS deps\n4. Write tests/tests/test_ring_buffer.cpp with Catch2:\n   - Write data, read full snapshot\n   - Write >Capacity, verify oldest overwritten\n   - read_last_lines() correctness\n   - Thread safety: two threads writing simultaneously\n5. Verify: ctest -R 'ring_buffer' --output-on-failure\n6. Commit: 'phase3.1: port ring buffer template'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'ring_buffer' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 3.2: Port OSC 52 scanner (parallel with 1.1, 3.1)

**Dispatch:**
```
delegate_task(
  goal: "Phase 3.2: Port OSC 52 clipboard scanner into bridgesessions.cpp",
  context: "Work directory: C:/Users/Shadow/bridgesessions\n\nYour task:\n1. Open _archive/bs-server/src/osc52_capture.hpp\n2. Port scan_osc52() and all detail helpers into bridgesessions.cpp (namespace bs::mesh)\n3. No changes needed — pure string scanning, no OS deps\n4. Write tests/tests/test_osc52.cpp with Catch2:\n   - Detect OSC 52 sequence in output\n   - Extract base64-decoded clipboard text\n   - Verify cleaned text has sequences stripped\n   - Test BEL-terminated and ST-terminated sequences\n   - Test incomplete sequence (left in stream)\n5. Verify: ctest -R 'osc52' --output-on-failure\n6. Commit: 'phase3.2: port OSC 52 clipboard scanner'\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'osc52' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 3.3: Port session + PTY with peer_id field — run after 3.1, 3.2

**Dispatch:**
```
delegate_task(
  goal: "Phase 3.3: Port Session struct + PTY management with peer_id field",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead plan.md §6 and todo.md §Phase 3.\n\nYour task:\n1. Open _archive/bs-server/src/session.hpp and session.cpp\n2. Port Session struct into bridgesessions.cpp (namespace bs::mesh). ADD peer_id field:\n\n   struct Session {\n       std::string name;\n       std::string peer_id;  // pubkey hex of remote peer currently attached (empty if detached)\n       std::string command;\n       #ifdef _WIN32\n       HANDLE master_fd, child_pid, write_handle, hpcon;\n       #else\n       int master_fd, child_pid;\n       #endif\n       SessionState state;\n       RingBuffer<1048576> scrollback;  // 1 MiB\n       std::chrono::steady_clock::time_point created_at, last_output_at, last_attach_at;\n       bool auto_restart;\n       int restart_failures;\n       std::chrono::steady_clock::time_point restart_window_start;\n       // move-only, no copy\n   };\n\n3. Port SessionState enum (8 states) + session_state_str()\n4. Port Session destructor (clean up handles, terminate process)\n5. Port Session move constructor + move assignment\n6. Open _archive/bs-server/src/pty.cpp\n7. Port create_session(name, command, cols, rows, term) — ConPTY on Windows, fork/exec on POSIX\n8. Port resize_pty() — ResizePseudoConsole on Windows, TIOCSWINSZ on POSIX\n9. Port spawn_child() / open_pty() helpers\n10. Write tests/tests/test_session.cpp:\n    - Create session (cmd.exe /c echo hello on Windows)\n    - Read output, verify scrollback contains 'hello'\n    - Resize session, verify no crash\n    - Kill session, verify state becomes Died\n    - Session move semantics\n11. Verify: ctest -R 'session' --output-on-failure\n12. Commit: 'phase3.3: port session + PTY with peer_id field'\n\nMUST follow TDD. This is complex — take your time. Test each piece.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'session' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

### Task 3.4: Port session registry (simplified) + persistence + logging — run after 3.3

**Dispatch:**
```
delegate_task(
  goal: "Phase 3.4: Port simplified SessionRegistry + persistence + logging into bridgesessions.cpp",
  context: "Work directory: C:/Users/Shadow/bridgesessions\n\nYour task:\n1. Open _archive/bs-server/src/session_manager.cpp\n2. Port a SIMPLIFIED session registry into bridgesessions.cpp:\n\n   class SessionRegistry {\n       mutable std::shared_mutex mutex_;\n       std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;\n       std::string persistence_path_;\n   public:\n       Session* attach(const std::string& name, const std::string& cmd,\n                       uint16_t cols, uint16_t rows, const std::string& term);\n       void detach(const std::string& name);\n       std::vector<SessionInfo> list() const;\n       Session* get(const std::string& name);\n       void kill(const std::string& name);\n       void reap_dead();\n       void prune_idle(std::chrono::seconds max_idle);\n       size_t count() const;\n       void load_persisted_sessions();\n       bool save_persisted_sessions() const;\n       Session* resurrect(const std::string& name, uint16_t cols, uint16_t rows, const std::string& term);\n   };\n\n   KEY SIMPLIFICATION: No more owner_id in keys. Session names locally unique.\n   peer_id on Session tracks WHO is attached (from TLS), not ownership.\n   Auto-restart: 3 failures in 60s window -> give up.\n   Persistence: atomic write (tmp + rename), load marks as Recoverable.\n\n3. Also port persistence.hpp and logging.hpp:\n   - save_sessions() / load_sessions() — JSON via nlohmann\n   - get_logger() / log_event() — spdlog rotating file at bs-mesh.log\n\n4. Write tests/tests/test_session_registry.cpp:\n   - Create session, verify in list\n   - Attach, detach, verify state transitions\n   - Kill session, verify removed\n   - Persist sessions, restart registry, verify resurrection\n   - Auto-restart: kill child, verify new child spawned\n   - Circuit breaker: kill child 4 times in 60s, verify Exited\n   - Prune idle: detach session, verify pruned after timeout\n5. Verify: ctest -R 'session_registry' --output-on-failure\n6. Commit: 'phase3.4: simplified session registry with persistence and auto-restart'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'session_registry' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 4: Config Layer

### Task 4.1: Config parser (key=value format) — run after Phase 1

**Dispatch:**
```
delegate_task(
  goal: "Phase 4: Implement MeshConfig struct + key=value config parser in bridgesessions.cpp",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead plan.md §4.2 for config format.\n\nYour task:\n1. Define MeshConfig struct + PeerEntry struct in bridgesessions.cpp (namespace bs::mesh):\n\n   struct PeerEntry {\n       std::string name;\n       std::string addr;       // 'host:port'\n       std::string pubkey_hex; // learned via Hello, empty until then\n       uint64_t last_seen = 0;\n   };\n\n   struct MeshConfig {\n       std::string node_name = 'unnamed';\n       std::string listen_addr = '0.0.0.0';\n       uint16_t listen_port = 19948;\n       size_t max_peers = 50;\n       int gossip_interval_secs = 30;\n       int reconnect_backoff_max_secs = 30;\n       int ping_interval_secs = 5;\n       int pong_timeout_secs = 30;\n       std::vector<PeerEntry> seeds;\n       std::vector<PeerEntry> discovered;\n       std::string authorized_keys_path = '~/.bridgesessions/authorized_keys';\n       std::string persistence_path = '~/.bridgesessions/sessions.json';\n       int scrollback_lines = 2000;\n       int idle_timeout_hours = 168;\n       std::string default_shell;\n       #ifdef _WIN32 default_shell = 'cmd.exe'; #else default_shell = '/bin/bash -l'; #endif\n       std::string terminal = 'xterm-256color';\n   };\n\n2. Write config parser for key=value format (matching existing hosts file style):\n   File: ~/.bridgesessions/config\n   Format:\n     node.name shadow\n     node.listen :19948\n     mesh.max_peers 50\n     seed linux-a 203.0.113.11:9948\n     seed linux-b 203.0.113.12:19948\n     sessions.default_shell cmd.exe\n\n   Parser: load_config(path) -> MeshConfig (fill defaults for missing keys)\n   Saver: save_config(path, cfg) -> writes back (preserves seeds, updates discovered)\n   Handle ~ expansion in paths.\n\n3. Write tests/tests/test_config.cpp:\n   - Load minimal config (only node.name + one seed)\n   - Verify all defaults filled in\n   - Load full config, round-trip save and reload\n   - Test ~ expansion\n   - Test missing file returns defaults\n   - Test duplicate seeds: last wins\n   - Save discovered peers, reload, verify preserved\n4. Verify: ctest -R 'config' --output-on-failure\n5. Commit: 'phase4: key=value config parser with MeshConfig struct'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'config' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 5: Mesh Connection Manager (CRITICAL PATH)

### Task 5.1: Mesh connection manager core

**Dispatch:**
```
delegate_task(
  goal: "Phase 5: Implement mesh connection manager — accept, connect, gossip, duplicate resolution",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead plan.md §5 and todo.md §Phase 5. This is the CORE of the mesh rewrite.\n\nYour task — implement ALL of this in bridgesessions.cpp (namespace bs::mesh):\n\n1. MeshConn struct:\n   struct MeshConn {\n       std::string peer_name;\n       std::string peer_pubkey;  // learned via Hello\n       std::string peer_addr;\n       SslPtr ssl;\n       #ifdef _WIN32 SOCKET sock_fd = INVALID_SOCKET; #else int sock_fd = -1; #endif\n       bool is_outbound;\n       std::chrono::steady_clock::time_point last_pong;\n       Session* attached_session = nullptr;\n       std::string remote_session;\n   };\n\n2. accept_loop():\n   - Create listen socket (dual-stack IPv4/IPv6, SO_REUSEADDR)\n   - On accept: TLS handshake via create_node_tls(Listen), verify against authorized_keys\n   - Extract peer pubkey, check duplicate (same pubkey already connected?)\n   - If duplicate and our_pubkey < their_pubkey: drop new, keep existing\n   - If duplicate and our_pubkey > their_pubkey: drop existing, accept new\n   - Exchange Hello messages (send ours, receive theirs)\n   - Store peer info, add to config.discovered, save config\n\n3. connect_to_peer(addr):\n   - Resolve address, TCP connect, TLS via create_node_tls(Connect) with TOFU\n   - Exchange Hello messages\n   - Check duplicate resolution\n   - On TOFU mismatch: log warning, reject\n\n4. event_loop():\n   - Single select() (Windows) across: listen socket + all peer sockets\n   - Timers: gossip every gossip_interval_secs, ping every ping_interval_secs\n   - For each ready socket: read_frame(), dispatch to message handler\n   - Check pong_timeout per connection (disconnect if >30s)\n   - Reconnect seeds with exponential backoff\n\n5. Gossip:\n   - Every gossip_interval_secs: build GossipMsg with all known peers, send to all connected\n   - On receiving GossipMsg: merge unknown peers into config.discovered, save config\n   - Try to connect to newly discovered peers (if under max_peers)\n\n6. Hello exchange:\n   - On connect (both directions): send HelloMsg with node info + known peers\n   - On receiving HelloMsg: store peer name/pubkey, merge known_peers into discovered\n\n7. Duplicate connection resolution:\n   - When two connections exist between same pubkeys: keep one where our_pubkey < their_pubkey\n\n8. Write tests/tests/test_mesh.cpp:\n   - Start two mesh nodes in separate threads within same process (different ports, different configs)\n   - Verify both connect, Hello exchanged, peer names match\n   - Verify ping/pong keepalive\n   - Verify gossip propagates peer list\n   - Kill one node, verify timeout detection\n   - Restart, verify reconnection with backoff\n\n9. Verify: ctest -R 'mesh' --output-on-failure\n10. Commit: 'phase5: mesh connection manager with accept/connect/gossip/duplicate resolution'\n\nMUST follow TDD. This is the hardest phase. Test thoroughly.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'mesh' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 6: Unified Relay

### Task 6.1: Unified relay (bidirectional session handling)

**Dispatch:**
```
delegate_task(
  goal: "Phase 6: Implement unified relay — handle all 22 message types bidirectionally",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead todo.md §Phase 6.\n\nYour task — implement in bridgesessions.cpp (namespace bs::mesh):\n\n1. handle_inbound_session(conn, msg):\n   - On AttachMsg from peer: register with SessionRegistry, set conn.attached_session, send scrollback\n   - On KeystrokeMsg: write to session PTY\n   - On ResizeMsg: resize PTY\n   - On DetachMsg: detach session\n   - On SignalMsg: send signal to child process\n   - On ClipboardMsg: write bracketed paste, echo hash\n\n2. handle_outbound_session(conn, msg):\n   - On OutputMsg: write to local stdout\n   - On ClipboardMsg: write to local clipboard\n   - On ExitCodeMsg/SessionDiedMsg: clear conn.remote_session, notify\n\n3. common_message_handler(conn, msg):\n   - PingMsg -> PongMsg, PongMsg -> update last_pong\n   - ScrollbackMsg -> write to output, ack\n   - ImageDataMsg/ImageFrameMsg -> render, ack\n   - SessionListMsg -> format and display\n   - HelloMsg -> process peer info\n   - GossipMsg -> merge peers\n\n4. pty_output_poller():\n   - For each session with attached peer: read PTY output, write to ring buffer\n   - OSC 52 scan, send OutputMsg + optional ClipboardMsg\n   - Check child exit -> reap, send SessionDiedMsg\n\n5. MERGE both current relay loops (Windows 2-thread, POSIX poll) into event_loop()\n   that handles ALL connections — inbound, outbound, local terminal.\n\n6. Write tests/tests/test_relay.cpp:\n   - Start two mesh nodes\n   - Node B requests shell on Node A\n   - Send keystrokes, verify output received\n   - Resize terminal, verify PTY resizes\n   - Ctrl+C, verify signal delivered\n   - Detach/re-attach, verify scrollback\n\n7. Verify: ctest -R 'relay' --output-on-failure\n8. Commit: 'phase6: unified relay handling all 22 message types bidirectionally'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'relay' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 7: CLI + main()

### Task 7.1: CLI with subcommands (parallel with Phase 8)

**Dispatch:**
```
delegate_task(
  goal: "Phase 7: Implement CLI with daemon/shell/keygen/authorize/peers/sessions subcommands",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead todo.md §Phase 7.\n\nYour task:\n1. Implement main() in bridgesessions.cpp using CLI11:\n\n   Subcommands:\n   - (default, no subcommand): daemon mode\n   - keygen: generate ed25519 keypair\n   - authorize <hex-pubkey>: add to authorized_keys\n   - shell <peer>[:session] [--cmd <command>]: connect to peer, attach session, enter relay\n   - sessions [<peer>|--all]: list sessions\n   - peers [list|add <name> <addr>|remove <name>]: manage peers\n   - health <peer>: ping/pong check\n   - stats [--json]: connection statistics\n   - image <file>: preview image\n   - anim <file>: preview animated GIF\n\n2. Daemon mode (default):\n   - Load config (--config flag override)\n   - Bootstrap identity\n   - Create node TLS\n   - Start listen socket\n   - Connect to seeds with backoff\n   - Enter event loop\n   - Graceful shutdown on SIGINT: detach sessions, save config\n\n3. shell subcommand:\n   - Load config, resolve peer name, connect via TLS\n   - Send AttachMsg, enter local terminal relay loop\n\n4. Port keygen and authorize from _archive/bs-client/src/keygen.cpp\n\n5. Write tests/tests/test_cli.cpp:\n   - Test --version, --help\n   - Test keygen creates files\n   - Test authorize appends to authorized_keys\n   - Test peers add/remove/list\n   - Test config parsing from CLI-specified path\n\n6. Verify: ctest -R 'cli' --output-on-failure\n7. Commit: 'phase7: CLI with daemon/shell/keygen/authorize/peers/sessions/stats'\n\nMUST follow TDD.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'cli' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 8: Client Support Layer

### Task 8.1: Terminal raw mode, clipboard, image render, peer management (parallel with Phase 7)

**Dispatch:**
```
delegate_task(
  goal: "Phase 8: Port terminal raw mode, clipboard bridge, image render, peer management",
  context: "Work directory: C:/Users/Shadow/bridgesessions\nRead todo.md §Phase 8.\n\nYour task — port from _archive/ into bridgesessions.cpp (namespace bs::mesh):\n\n1. Terminal raw mode (from _archive/bs-client/src/terminal_raw.cpp):\n   - enable_raw_mode(), restore_terminal(), get_winsize()\n   - Windows: SetConsoleMode + ENABLE_VIRTUAL_TERMINAL_INPUT\n   - POSIX: tcgetattr + cfmakeraw\n\n2. Clipboard bridge (from _archive/bs-client/src/clipboard_windows.cpp + clipboard_bridge.hpp):\n   - Windows: OpenClipboard/GetClipboardData/SetClipboardData\n   - Dedup via SHA-256 hash, poll in event loop\n\n3. Image render (from _archive/bs-client/src/image_render.cpp):\n   - read_binary_file(), detect_image_format(), parse_gif_metadata()\n   - make_image_data_message(), make_image_frame_message()\n   - render_image_message() — chafa on POSIX, text placeholder on Windows\n   - DELETE broken is_gif_magic(), keep only is_gif_magic_alt()\n\n4. Peer/host management (from _archive/bs-client/src/host_config.cpp):\n   - load_peers(), save_peers(), upsert_peer(), remove_peer(), find_peer()\n\n5. Write tests:\n   - tests/test_terminal.cpp: enable raw mode, verify echo disabled\n   - tests/test_clipboard.cpp: mock clipboard, verify poll detects change\n   - tests/test_image.cpp: detect PNG/JPEG/GIF magic bytes\n   - tests/test_peer_mgmt.cpp: add, find, remove peers\n\n6. Verify: ctest -R 'terminal|clipboard|image|peer_mgmt' --output-on-failure\n7. Commit: 'phase8: terminal raw mode, clipboard bridge, image render, peer management'\n\nMUST follow TDD for each component.\n\nBuild: cd C:/Users/Shadow/bridgesessions && cmake --build build/windows-msvc-debug\nTest: cd C:/Users/Shadow/bridgesessions && ctest -R 'terminal|clipboard|image|peer_mgmt' --output-on-failure",
  toolsets: ["terminal", "file"]
)
```

---

## Phase 9: Integration Testing

### Task 9.1: Two-node mesh integration test

**Execution:** Hermes parent direct or sub-agent
**Duration:** 10 min

1. Start bridgesessions on port 19948 with config seeding port 19949
2. Start bridgesessions on port 19949 with config seeding port 19948
3. Verify: both connect within 10 seconds
4. Run: `bridgesessions shell node-b --cmd "echo hello"`
5. Verify: "hello" appears in output
6. Kill node B, verify node A detects disconnect
7. Restart node B, verify reconnection within 30 seconds
8. Write integration test script: `tests/integration/test_two_node_mesh.sh`

### Task 9.2: Three-node gossip test

1. A seeds B, B seeds C (chain topology)
2. Verify A discovers C via gossip from B within 60 seconds
3. Verify A connects to C
4. Verify all three can shell into each other
5. Write: `tests/integration/test_three_node_gossip.sh`

### Task 9.3: Cross-platform verification

1. Build on Windows (MSVC): `cmake --preset windows-msvc-debug && cmake --build build/windows-msvc-debug`
2. Verify: all tests pass (`ctest --test-dir build/windows-msvc-debug --output-on-failure`)
3. If Linux available: build and test on Linux

---

## Phase 10: Documentation & Cleanup

1. Write `README.md` — build instructions, config format, quickstart, CLI reference
2. Verify `_archive/` contains old modules
3. Delete remaining build artifacts from old multi-module build
4. Verify single-file build: `cl /std:c++latest /EHsc /Fe:bridgesessions.exe bridgesessions.cpp /I C:/vcpkg/installed/x64-windows/include /link openssl.lib zstd.lib ws2_32.lib`
5. Final commit: "phase10: README, cleanup, single-file build verified"

---

## Execution Order

```
Phase 0 (parent executes directly)
  │
  ├─ [BATCH] Phase 1.1 + Phase 3.1 + Phase 3.2 ─── 3 parallel delegate_task
  │
  ├─ [BATCH] Phase 1.2 + Phase 2.1 + Phase 3.3 ─── 3 parallel delegate_task
  │
  ├─ [BATCH] Phase 2.2 + Phase 3.4 + Phase 4 ───── 3 parallel delegate_task
  │
  ├─ [SERIAL] Phase 2.3 ─── single delegate_task
  │
  ├─ [SERIAL] Phase 5 (mesh) ─── single delegate_task (CRITICAL)
  │
  ├─ [SERIAL] Phase 6 (relay) ─── single delegate_task
  │
  ├─ [BATCH] Phase 7 + Phase 8 ─── 2 parallel delegate_task
  │
  ├─ [SERIAL] Phase 9 (integration) ─── parent or sub-agent
  │
  └─ Phase 10 (cleanup) ─── parent
```

### Verification after every phase

```
cd C:/Users/Shadow/bridgesessions
cmake --build build/windows-msvc-debug
ctest --test-dir build/windows-msvc-debug --output-on-failure
```

Red = DO NOT PROCEED. Fix before next phase.
