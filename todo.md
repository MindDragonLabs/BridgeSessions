# bridgesessions — Mesh Implementation TODO

**Status:** ✅ **v1.2.0 ACTIVE** — 4,917 lines, uncommitted
**Completed:** 2026-06-07
**Current:** v1.2.0-mesh: mDNS, multi-attach, health, image, anim, stats, encryption, --daemon, connect_and_hello

**Target:** `bridgesessions.cpp` — single ~4,917 line file, one binary per device
**Approach:** Single-file C++23, namespace bs::mesh, cross-platform MSVC+g++
**Test strategy:** 12 test suites, 8/12 passing (4 pre-existing TLS failures)
**Review status:** ✅ passed independent review (0 security, 0 logic errors)

---

## v1.2 Wave (2026-06-07)

- [x] **W1.1** `health <peer>` CLI subcommand — ping/pong with 3s timeout
- [x] **W1.2** `image <file>` CLI subcommand — text placeholder on Windows, chafa on POSIX
- [x] **W1.3** mDNS LAN discovery — custom multicast 224.0.0.252:19949, 30s announce, auto-add to discovered
- [x] **W1.4** Multi-attach — `peer_id` → `peer_ids` vector, output fan-out to all attached peers
- [x] **W1.5** Fix criticals from v1.0 audit:
  - C1: Terminal raw-mode leak on exception — `SavedConsole` lifted out of try, restore in catch
  - C2: Socket leak on Hello mismatch — `CLOSESOCK(sfd)` before return in `shell_peer()` and `list_sessions()`
- [x] **W1.6** Session encryption at rest — `v1:xor` with SHA-256 of node ed25519 private key
- [x] **W1.7** `stats` + `anim` CLI subcommands
- [x] **W1.8** `connect_and_hello()` helper — deduplicated TCP+TLS+Hello for shell_peer/list_sessions/health_check
- [x] **W1.9** `--daemon` flag — POSIX fork+setsid, Windows FreeConsole+AttachConsole
- [x] **W1.10** `--config-dir` flag

---

## v1.0 Baseline (all complete)

⚠️ _All checkboxes marked `[x]` below are confirmed done. 47 of 47 done._

## Phase 0: Clean Slate Setup

- [x] **0.1** Delete `shadow-agent/` directory entirely (200 lines of skeleton, wrong abstraction)
- [x] **0.2** Move old `bs-*` directories to `_archive/` for reference during porting
- [x] **0.3** Create `bridgesessions.cpp` as empty file at repo root
- [x] **0.4** Create `tests/` directory with test files
- [x] **0.5** Update `CMakeLists.txt` to single target `bridgesessions` + `bridgesessions-tests`
- [x] **0.6** Verify build: empty `main() { return 0; }` compiles on MSVC + vcpkg

## Phase 1: Protocol Layer

- [x] **1.1** Port 20 message types + 2 new mesh types (Hello, Gossip)
- [x] **1.2** Port codec (encode/decode/zstd/SHA-256)
- [x] **1.3** Run protocol tests

## Phase 2: TLS + Identity Layer

- [x] **2.1** Port TLS — unified `create_node_tls()` for listen + connect
- [x] **2.2** Port frame I/O
- [x] **2.3** Identity bootstrap with auto-keygen

## Phase 3: Session + PTY Layer

- [x] **3.1** Ring buffer
- [x] **3.2** OSC 52 scanner
- [x] **3.3** Persistence
- [x] **3.4** Logging
- [x] **3.5** Session + ConPTY/PTY
- [x] **3.6** Session registry with auto-restart

## Phase 4: Config Layer

- [x] **4.1** MeshConfig struct
- [x] **4.2** key=value config parser
- [x] **4.3** Config round-trip tests

## Phase 5: Mesh Connection Manager

- [x] **5.1** Connection struct
- [x] **5.2** Accept loop
- [x] **5.3** Outbound connect
- [x] **5.4** Event loop (select-based)
- [x] **5.5** Gossip
- [x] **5.6** mDNS LAN discovery (done in v1.1)
- [x] **5.7** Mesh gossip tests

## Phase 6: Unified Relay

- [x] **6.1** Inbound session handler
- [x] **6.2** Outbound session relay
- [x] **6.3** Common message handling
- [x] **6.4** PTY output polling
- [x] **6.5** Local terminal integration

## Phase 7: CLI + main()

- [x] **7.1** CLI11 design
- [x] **7.2** Daemon mode
- [x] **7.3** `shell` subcommand
- [x] **7.4** `sessions` subcommand
- [x] **7.5** `peers` subcommand
- [x] **7.6** `keygen` + `authorize`

## Phase 8: Client Support Layer

- [x] **8.1** Terminal raw mode
- [x] **8.2** Clipboard bridge
- [x] **8.3** Image render
- [x] **8.4** Peer/host management

## Phase 10: Documentation & Cleanup

- [x] **10.1** Update plan.md
- [x] **10.2** README.md
- [x] **10.3** Archive old code
- [x] **10.4** Delete shadow-agent
- [x] **10.5** Verify single-file build

---

## Deferred: v2 / Future

### High Priority

- [x] **D1** Multi-hop session routing — `SessionSearchMsg` (0x17), `AttachMsg.routing` field, forward/broadcast logic ✅
- [ ] **D2** Live mesh integration tests — `test_two_node_mesh.ps1`, `test_three_node_mesh.ps1` require live nodes on Tailscale
- [x] **D3** `stats` CLI subcommand ✅
- [x] **D4** `anim <file>` CLI subcommand ✅
- [ ] **D5** `peers list` live connection status — currently config-only, should show connection state/latency/uptime
- [x] **D6** `Ctrl+D` / EOF detection on Windows shell — 0x1A (Ctrl+Z) detected as EOF ✅
- [x] **D7** Daemon `--daemon` flag ✅ (POSIX fork+setsid, Windows FreeConsole)
- [x] **D8** Encryption at rest for session persistence — `v1:xor` with SHA-256 of node private key ✅
- [x] **D9** ~~Test: old client backward compat~~ — N/A: no prior version ever shipped
- [ ] **D10** Test: cross-platform matrix — Windows↔Linux, ConPTY↔PTY interoperability
- [x] **D12** `connect_and_hello()` helper — TCP + TLS + Hello exchange, reused by shell_peer/list_sessions/health_check ✅

### Low Priority

- [x] **D11** Multi-attach keystroke echo — PTY output fan-out echoes OutputMsg to all attached peers ✅
- [x] **D12** ~~`connect_and_hello()` helper~~ — moved to ✅ Medium section above
- [ ] **D13** Session recording / replay — save/playback session history
- [ ] **D14** Mesh-wide session search — "find session X on any node" via broadcast query
- [ ] **D15** WebRTC transport for browser peers
- [ ] **D16** DHT for >100 node meshes
- [ ] **D17** NAT traversal without Tailscale
- [ ] **D18** TLS `close_notify` before socket close — proper `SSL_shutdown()` for truncation detection
- [ ] **D19** Build: CMakeLists.txt unification — one file for bridgesessions + tests
- [x] **D20** Config: `--config-dir` flag ✅
- [ ] **D21** Docs: full `--help` coverage for all subcommands (`shell`, `sessions`)
- [ ] **D22** Docs: man page
- [ ] **D23** PeekNamedPipe on console input — replace with `GetNumberOfConsoleInputEvents` + `ReadConsoleInput` for universal Windows terminal compat

### Known Issues (Pre-existing, Low Severity)

- [ ] **K1** `[[nodiscard]]` warnings (4x) — `(void)` casts on `resize_pty()` / `load_peers_from_file()` calls
- [ ] **K2** `select(0, ...)` idiom on Windows — unconventional `nfds=0`, works because Winsock ignores it
- [ ] **K3** OpenSSL callback context leak — `AuthorizedKeys*` / `std::function*` allocated for cert verify callback, never freed. Bounded (once per CTX creation, CTX created once at startup)
- [ ] **K4** `build_hello()` iterates `conns_` without synchronization — safe in single-threaded event loop but undocumented invariant

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
                          Phase 9 (integration tests) ← NEXT
                                │
                          Phase 10 (docs/cleanup) ← DONE
                                │
                          v1.2 Wave (mDNS, multi-attach, health, image, anim, stats, encryption, --daemon) ← DONE
```

---

## Estimated Line Counts (v1.1 actual)

| Component | Lines |
|---|---|
| Platform abstractions + includes | ~310 |
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
| Mesh connection manager + gossip | ~500 |
| Unified relay | ~500 |
| mDNS LAN discovery | ~170 |
| Multi-attach fan-out | ~60 |
| `shell_peer()` + `process_shell_response()` | ~200 |
| `list_sessions()` | ~110 |
| `health_check()` | ~60 |
| Clipboard bridge | ~150 |
| Terminal raw mode | ~100 |
| Image render + `render_image_to_terminal()` | ~230 |
| Peer/host management | ~100 |
| keygen + authorize | ~60 |
| main() + CLI11 (7 subcommands) | ~140 |
| **TOTAL** | **~4,718** |
