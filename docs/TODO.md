# bridgesessions — TODO

**Status:** v1.5.0 released · v1.7 in planning
**Plan:** [PLANS.md](./PLANS.md) · **Design:** [GUIDELINE.md](./GUIDELINE.md) · **Architecture:** [ARCHITECTURE.md](./ARCHITECTURE.md)

---

## Legend

| Mark | Meaning |
|------|---------|
| `[ ]` | Not started |
| `[/]` | In progress (partially implemented) |
| `[x]` | Complete, tested, verified |
| `[~]` | Blocked / deferred |
| `✅` | Shipped and validated in production |

---

## v1.4.0 — COMPLETE ✅

All mesh substrate items shipped and validated 4-node cross-platform.

| # | Item | Tests | Status |
|---|------|-------|--------|
| M1 | Single-file C++23, cross-platform build | `_run_tests.ps1` 16 suites | ✅ |
| M2 | TLS 1.3 mTLS with ed25519 + TOFU | `test_tls`, `test_tls_reliability` | ✅ |
| M3 | 22 message types (0x00–0x17) | `test_message` | ✅ |
| M4 | Session lifecycle + multi-attach | `test_session_registry`, `test_session` | ✅ |
| M5 | Multi-hop routing (SessionSearch) | `test_relay`, `test_mesh` | ✅ |
| M6 | mDNS + gossip + seed discovery | `test_mesh` | ✅ |
| M7 | Daemon health IPC :19980 (all platforms) | `test_mesh_reliability` | ✅ |
| M8 | Config: CRLF-safe, pubkey token, dedup | `test_config` | ✅ |
| M9 | Deterministic duplicate-conn resolution | `test_mesh_reliability` | ✅ |
| M10 | Per-addr reconnect backoff scheduling | `test_mesh_reliability` | ✅ |
| M11 | Single-instance guard | Manual | ✅ |
| M12 | Bounded TLS handshake (select + deadline) | `test_tls_reliability` | ✅ |
| M13 | Steady-state recv timeout (10s) | Manual | ✅ |
| M14 | 1009/1009 test assertions, 16 suites | Self-referential | ✅ |
| M15 | 4-node validation (Shadow/linux-a/linux-b/macos-peer) | `matrix.sh` | ✅ |
| M16 | Docs reconciled (6 documents) | N/A | ✅ |

---

## v1.5.0 — COMPLETE ✅

### P1: `file send` / `file recv` — mesh-native peer-to-peer transfer

**Wire:** FileMeta (0x18), FileChunk (0x19), FileAck (0x1A), FileRequest (0x1F)
**CLI:** `file send <peer> <local>`, `file recv <peer> <remote>`
**Tests:** 18/18 in `test_file_transfer.cpp` (codec + sha256)
**Live:** Bidirectional SHA-256 verified Shadow↔linux-b

**Tasks:**

- [x] **P1.1** Define message types in `MessageType` enum
- [x] **P1.2** Add structs + `operator==` for all three types + FileRequest
- [x] **P1.3** Add to `Message` variant in correct order
- [x] **P1.4** Add to `index_to_type[]` array
- [x] **P1.5** Implement `serialize_msg` overloads for each new type
- [x] **P1.6** Add `case 0xNN:` entries in deserializer switch
- [x] **P1.7** Create `test_file_transfer.cpp` — encode/decode roundtrip for all types
- [x] **P1.8** Daemon: `handle_file_meta()` dispatcher
- [x] **P1.9** Daemon: `handle_file_chunk()` — write chunk to disk, send FileAck
- [x] **P1.10** Daemon: `handle_file_ack()` — advance send window
- [x] **P1.11** CLI: `file_cmd_app` subcommand in CLI11 setup
- [x] **P1.12** `file_send()` implementation (read, hash, chunk, send via IPC)
- [x] **P1.13** `file_recv()` implementation (request, await chunks, verify SHA-256)
- [x] **P1.14** Resume support (`.part` detection, append mode)
- [x] **P1.15** Integration test — bidirectional transfer verified
- [x] **P1.19** Progress logging

### P2: `restart` signal — kill + respawn processes over mesh

**Wire:** New `Restart=3` in SignalMsg enum, `std::string process` field
**Daemon:** Kill child (SIGTERM/TerminateProcess), spawn new session, bump generation
**Tests:** 36/36 in `test_session.cpp` (restart test included)

- [x] **P2.1** Extend `SignalMsg` struct: add `std::string process` field
- [x] **P2.2** Add `Restart` to the Signal enum
- [x] **P2.3** Update `serialize_msg` / deserializer for SignalMsg
- [x] **P2.4** Daemon: `handle_signal()` — add `case Restart:` branch
- [x] **P2.5** Implement kill+respawn logic (POSIX + Win32)
- [x] **P2.6** Error handling: respawn failure → stay in Died
- [x] **P2.9** Tests: 36/36 session assertions

### P3: `render_hint` flag — markdown vs raw terminal

**Wire:** `FLAG_RENDER_MARKDOWN = 0x04`, `OutputMsg.render_markdown` bool
**Heuristic:** `looks_like_markdown()` checks first 200 bytes for headers, lists, fences, tables
**Tests:** 13 new assertions in `test_message.cpp` (156/156 total)

- [x] **P3.1** Define flag constant: `FLAG_RENDER_MARKDOWN = 0x04`
- [x] **P3.2** Add to `OutputMsg` struct + serialize/deserialize
- [x] **P3.3** Config: `output.render_hint = auto|markdown|raw`
- [x] **P3.4** Implement markdown heuristic detection (`looks_like_markdown`)
- [x] **P3.5** Daemon: set flag when creating OutputMsg from PTY output
- [x] **P3.6** Tests: encode/decode roundtrip, heuristic detection, FLAG constant

### P4: `edit <peer>:<path>` — remote file editing

**Architecture:** Route through daemon IPC (EDIT_DL / EDIT_UP), download to temp, open editor, upload diffs
**Live:** Download verified from linux-b, upload path uses same proven P1 wire

- [x] **P4.1** CLI: `edit_cmd_app` subcommand — parse `peer:path` argument
- [x] **P4.2** Download via daemon IPC `EDIT_DL` → `daemon_edit_dl()`
- [x] **P4.3** Editor: `$EDITOR` → notepad (Win) → vim (POSIX)
- [x] **P4.5** SHA-256 compare: unchanged → "no changes", changed → upload
- [x] **P4.6** Upload via daemon IPC `EDIT_UP` → `daemon_edit_up()`
- [x] **P4.7-8** IPC handlers: EDIT_DL receives chunks to temp, EDIT_UP sends FileMeta+chunks

### P5: Virtual folder mapping — config + CLI + polling sync

**Config:** `vfolder.<name>.*` keys (local, peer, remote, direction, interval)
**CLI:** `vfolder add`, `vfolder list`, `vfolder sync` (via daemon IPC)
**Daemon:** `daemon_vfolder_sync()` scans dir, sends FileMeta+chunks per file

- [x] **P5.1** Config parser/saver: `vfolder.<name>.*` keys
- [x] **P5.2** VFolderEntry struct in MeshConfig
- [x] **P5.10** CLI: `vfolder list` — shows mappings from config
- [x] **P5.11** CLI: `vfolder sync` — via VFOLDER_SYNC IPC → daemon sync
- [x] **P5.17** daemon_vfolder_sync(): recursive dir scan, hash, transfer
- [x] Live: `vfolder add` + `vfolder list` verified

### P6: `stats` IPC parity — query daemon state over :19980

**Wire:** `STATS\n` IPC command → JSON response
**Daemon:** `daemon_stats_json()` — node info, connections (name, addr, uptime, pong, bytes), session count
**Live:** Returns real-time state from daemon (3 peers shown)

- [x] **P6.1** IPC: add `"STATS"` handler to `cli_ipc_accept_one()`
- [x] **P6.2** Build JSON response from `conns_`, `sessions_`, counters
- [x] **P6.3** CLI: `stats` subcommand routes through `daemon_simple_ipc()`
- [x] **P6.5** Live: verified 3 peers, uptime, bytes_in/bytes_out

---

## v1.7 — Platform watchers + bidirectional sync

### Release criteria

- [ ] Platform filesystem watchers (Win/Linux/macOS) all green
- [ ] Bidirectional vfolder sync verified on 4-node cluster
- [ ] Suite total ≥ 1150 assertions
- [ ] All v1.4 + v1.5 regression tests still pass
- [ ] Version bumped `1.5.0` → `1.7.0`

---

## v2 — Future (no active work)

| # | Feature | Depends on | Notes |
|---|---------|-----------|-------|
| 1 | QUIC via msquic | — | Library swap, same protocol layer |
| 2 | Nonblocking TLS handshakes | — | Concurrent with QUIC |
| 3 | Session recording + replay | P1 file transfer | ImageFrame has frame-order metadata |
| 4 | Read-only spectators (fan-out) | — | New Attach flag |
| 5 | SRV record peer discovery | — | DNS-based fleet discovery |
| 6 | Dictionary-trained zstd | — | Higher compression on ANSI output |
| 7 | CI smoke test over Tailscale | — | Automated `matrix.sh` on schedule |

---

## Docs

| Doc | Purpose | Status |
|-----|---------|--------|
| `GUIDELINE.md` | Design sketch, vision | ✅ Current |
| `ARCHITECTURE.md` | ADR docs, wire format, daemon architecture | ✅ Current |
| `PLANS.md` | Implementation plan with feature detail | ✅ Current |
| `TODO.md` | Task checklist — this file | ✅ Current |
| `README.md` | Quickstart + commands | ✅ Current |
| `AUTONOMOUS.md` | Agent dispatch rules | ✅ Current |
| `todo.md` (root) | v1.4 + v1.5 release logs | ✅ Current |
