# bridgesessions — TODO

**Status:** v1.4.0 released · v1.5 in progress
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

## v1.5 — Current sprint

### P1: `file send` / `file recv` — mesh-native peer-to-peer transfer

**Summary:** New CLI subcommands + 3 new message types for direct peer-to-peer file
transfer over the mesh wire. Per-chunk ACK flow control, resume support, progress logging.

**Tasks:**

- [ ] **P1.1** Define message types in `MessageType` enum
  - `FileMeta = 0x18` — filename, size, checksum (SHA-256), total_chunks
  - `FileChunk = 0x19` — chunk_index, data (zstd-compressed), total_chunks
  - `FileAck = 0x1A` — chunk_index, next_requested (for resume)
- [ ] **P1.2** Add structs + `operator==` for all three types
- [ ] **P1.3** Add to `Message` variant in correct order
- [ ] **P1.4** Add to `index_to_type[]` array (expand from 23 to 26)
- [ ] **P1.5** Implement `serialize_msg` overloads for each new type
- [ ] **P1.6** Add `case 0xNN:` entries in deserializer switch
- [ ] **P1.7** Create `test_file_protocol.cpp` — encode/decode roundtrip for all 3 types
  - Edge cases: 0-byte payload, max-size chunk, unicode filename
- [ ] **P1.8** Daemon: `handle_file_meta()` dispatcher in `MeshController::dispatch_message()`
  - Open output file, prepare for chunk writes
- [ ] **P1.9** Daemon: `handle_file_chunk()` — write chunk to disk, send FileAck
  - Path: `~/bridgesessions/received/<peer>/<filename>` or user-specified dir
  - Atomic: write to `.part` file, rename on completion
- [ ] **P1.10** Daemon: `handle_file_ack()` — advance send window, send next chunk
- [ ] **P1.11** CLI: `file_cmd_app` subcommand in CLI11 setup
  - `file send <peer> <local> [remote-dir]`
  - `file recv <peer> <remote> [local-dir]`
- [ ] **P1.12** `file_send()` implementation:
  - Read local file, compute SHA-256, chunk into 64KB blocks (zstd per chunk)
  - Build FileMeta, connect_to_peer_impl() or use IPC
  - Send FileMeta, iterate chunks waiting for FileAck per chunk
  - Print summary on completion: filename, size, chunks, checksum
- [ ] **P1.13** `file_recv()` implementation:
  - Request file from peer (inverse FileMeta request or explicit CLI flow)
  - Await FileMeta, create output path, receive chunks
  - Verify SHA-256 on completion, clean up .part file
- [ ] **P1.14** Resume support:
  - On `file recv`, check if `.part` file exists + has matching checksum prefix
  - Send `next_requested` = last complete chunk + 1 in FileAck
- [ ] **P1.15** Integration test: `test_file_transfer_integration.cpp`
  - Spawn two test nodes, send small file (README.md), verify checksum match
  - Send 0-byte file, verify empty file created
  - Send file with non-ASCII name, verify roundtrip
- [ ] **P1.16** Resume integration test:
  - Configure test to interrupt after chunk N, reconnect, verify resume from N+1
- [ ] **P1.17** Error handling: network drop mid-transfer → close partial file, log
- [ ] **P1.18** Error handling: disk full on receiver → FileAck with error flag, sender cancels
- [ ] **P1.19** Progress logging: `file_transfer_start`, `file_chunk_sent`, `file_transfer_complete`

**Verification:**
```bash
bridgesessions file send linux-b README.md /tmp/
# → "sent README.md (12KB, 3 chunks, sha256:ab12...)"
bridgesessions file recv linux-b /tmp/README.md /tmp/copy.md
# → "received README.md (12KB, sha256:ab12...), 100% match"
sha256sum README.md && sha256sum /tmp/copy.md  # identical
```

**Effort:** Moderate (~7 hours + tests)

---

### P2: `restart` signal — kill + respawn processes over mesh

**Summary:** Extend existing SignalMsg with Restart variant. No new message type.
Kill current child process, spawn fresh one in same session. Session stays alive.

**Tasks:**

- [ ] **P2.1** Extend `SignalMsg` struct: add `std::string process` field
- [ ] **P2.2** Add `Restart` to the Signal enum (existing enum, new value)
- [ ] **P2.3** Update `serialize_msg` / deserializer for SignalMsg (backward-compat: empty process = old behavior)
- [ ] **P2.4** Daemon: `handle_signal()` — add `case Restart:` branch
- [ ] **P2.5** Implement `restart_process(Session& session)`:
  - Send SIGTERM to child_pid, wait 2s, send SIGKILL if still alive
  - Wait for child to fully exit (waitpid on POSIX, WaitForSingleObject on Win)
  - Spawn new process from `session.command` (or `signal.process` override)
  - Update session.child_pid and session.master_fd
  - Log: `session_restart {name, pid_old→pid_new}`
- [ ] **P2.6** Error handling: respawn failure → send SessionDied, transition to Died
- [ ] **P2.7** Error handling: Restart on dead session → log warning, no-op
- [ ] **P2.8** Error handling: Restart on detached session → restart anyway (process respawns)
- [ ] **P2.9** Tests: `test_session_restart.cpp`
  - Create session `sleep 60`, send Restart signal
  - Verify child_pid changed (old pid != new pid)
  - Verify session.state still Running
  - Verify new process responds (write keystroke, read output)
- [ ] **P2.10** Test: Restart with bad command → verify SessionDied fires
- [ ] **P2.11** Test: Restart on dead session → no crash, log warning
- [ ] **P2.12** Test: Restart on DETACHED session → process respawns correctly

**Verification:**
```bash
bridgesessions shell linux-b -n hermes -x "hermes --tui"
# Send Restart through session stream
# → log "session_restart" with pid transition
# → new hermes process running, session responsive
```

**Effort:** Small (~3 hours + tests)

---

### P3: `render_hint` flag — markdown vs raw terminal

**Summary:** Single flag bit in frame header tells GUI "this OutputMsg should render
as markdown HTML, not raw terminal text." Heuristic detection or config override.

**Tasks:**

- [ ] **P3.1** Define flag constant: `RENDER_MARKDOWN = 0x04` (bit 2 in flags byte)
- [ ] **P3.2** Add to frame header encode/decode (flag already exists in framing — just set it)
- [ ] **P3.3** Config: add `output.render_hint = auto|markdown|raw` to `MeshConfig`
- [ ] **P3.4** Implement markdown heuristic detection:
  - Check first 200 bytes of OutputMsg payload
  - True if: `^# `, `^## `, `^### `, `- ` list items, `` ``` ``, `| ` table markers
  - Counters: at least 2 markdown-like lines in first 20 lines = markdown
  - Config `auto` = run heuristic. `markdown` = always set flag. `raw` = never set.
- [ ] **P3.5** Daemon: in `dispatch_message()` for OutputMsg, apply heuristic → set flag
- [ ] **P3.6** Test: `test_output_render_hint.cpp`
  - `render_hint=markdown`, verify flag bit IS set on markdown output
  - `render_hint=raw`, verify flag NOT set even on markdown output
  - Heuristic: send plain `ls -la` output → flag NOT set
  - Heuristic: send hermes agent output → flag IS set
- [ ] **P3.7** Config override test: auto vs markdown vs raw all produce correct flag

**Verification:** Capture frame on wire, check bit 2 of flags byte.

**Effort:** Trivial (~1 hour + tests)

---

### P4: `edit <peer>:<path>` — remote file editing with delta patches

**Summary:** Open any file on any peer in local editor, save sends delta. Uses P1
transfer infra for initial fetch + final write-back.

**Tasks:**

- [ ] **P4.1** CLI: `edit_cmd_app` subcommand — parse `peer:path` argument
- [ ] **P4.2** Download: `file_recv` remote file to `$TMPDIR/bsedit-XXXX/<basename>`
- [ ] **P4.3** Determine editor: `$EDITOR` env var → `notepad++` on Win → `vim` fallback
- [ ] **P4.4** Launch editor as subprocess, wait for exit
- [ ] **P4.5** Compute diff on exit: if file unchanged → clean up temp, print "no changes"
- [ ] **P4.6** Upload: if file changed, `file_send` back to original path
- [ ] **P4.7** Daemon: on receiving file to existing path, backup original to `.bsbak`
- [ ] **P4.8** Cleanup: remove temp dir on success or failure
- [ ] **P4.9** Binary files: warn user, upload full file (no diff)
- [ ] **P4.10** Large files (>50MB): warn, suggest scp, ask confirmation
- [ ] **P4.11** Test: `test_edit_integration.cpp`
  - Create temp file on test peer, edit via CLI, verify remote matches edit
  - No-change case: open + close editor without saving → no upload
  - Backup: edit file, verify `.bsbak` created on remote
- [ ] **P4.12** Error handling: editor returns non-zero exit → abort upload, warn

**Verification:**
```bash
bridgesessions edit linux-b:/home/agent/myconfig.yaml
# → "downloaded myconfig.yaml (2KB)" → opens $EDITOR
# → (save + quit)
# → "file changed, uploading... pushed delta (1 patch)"
# On linux-b: cat /home/agent/myconfig.yaml → matches edited version
```

**Effort:** Moderate (~5 hours + tests)

---

### P5: Virtual folder mapping — local↔remote live sync

**Summary:** Filesystem watchers (inotify/FSEvents/ReadDirectoryChangesW) per mount
sync changes to remote peer. Change-driven, zstd-compressed, bidirectional.

**Tasks:**

- [ ] **P5.1** Config: add `[vfolder]` section parser to `load_config()`/`save_config()`
  - Fields: name, local_path, remote_peer, remote_path, direction, sync_interval_secs
- [ ] **P5.2** VFolder mount structure: name, local, remote, thread handle, event queue, state
- [ ] **P5.3** Filesystem watcher abstraction (platform-specific):
  - `#ifdef _WIN32`: `ReadDirectoryChangesW` thread
  - `#ifdef __linux__`: inotify thread
  - `#ifdef __APPLE__`: FSEvents thread
  - Common: debounce 2s window, filter ignories (`.git/`, `node_modules/`, `*.swp`)
- [ ] **P5.4** Event queue: watcher thread → change events → main-loop processing
  - Thread-safe SPSC queue. Events: CreateFile, ModifyFile, DeleteFile, RenameFile
- [ ] **P5.5** Initial sync: on mount start, scan local dir, hash all files, compare with remote via `file recv` manifest, transfer diffs
- [ ] **P5.6** On local change event: zstd file content, send FileChunk series to remote peer (reuses P1)
- [ ] **P5.7** On remote file change: daemon receives FileChunk from peer, writes to local mapped path
- [ ] **P5.8** Conflict handling: simultaneous edit detection → save as `.bsconflict.<peer>.<timestamp>`, log warning
- [ ] **P5.9** Ignore list: `.git/`, `node_modules/`, `venv/`, `__pycache__/`, `.DS_Store`, `*.o`, `*.exe`, `*.swp`
- [ ] **P5.10** CLI: `vfolder list` — show active mounts (name, peer, path, state)
- [ ] **P5.11** CLI: `vfolder sync <name>` — force sync now
- [ ] **P5.12** CLI: `vfolder pause <name>` / `vfolder resume <name>`
- [ ] **P5.13** Persistence: save active vfolder state to `~/.bridgesessions/vfolders.json`
- [ ] **P5.14** Test: `test_vfolder_local_change.cpp` — create watcher, write file, verify FileChunk sent to mock peer
- [ ] **P5.15** Test: `test_vfolder_initial_sync.cpp` — pre-populate remote, verify sync brings in sync
- [ ] **P5.16** Test: `test_vfolder_conflict.cpp` — simultaneous edits → .bsconflict created
- [ ] **P5.17** Test: `test_vfolder_watcher_win.cpp` / `test_vfolder_watcher_linux.cpp` — platform watcher launches, detects change, fires event
- [ ] **P5.18** Performance: measure sync latency for 1000 small files, ensure <5s

**Verification:**
```bash
# Config: [vfolder "projects"] local=~/projects remote=linux-b:/home/agent/projects
# Start daemon, watch log
echo "new content" > ~/projects/test.txt
# → log: "vfolder projects: test.txt changed → sending to linux-b"
# On linux-b: cat /home/agent/projects/test.txt → "new content"
```

**Effort:** Large (~15 hours + tests)

---

### P7: `stats` IPC parity — query daemon state over :19980

**Summary:** `bridgesessions stats` queries daemon IPC like `health` does, not
ephemeral fresh TLS. Daemon returns JSON with conn/session/uptime state.

**Tasks:**

- [ ] **P7.1** IPC: add `"STATS\n"` handler to `cli_ipc_accept_one()`
- [ ] **P7.2** Build JSON response: `conns: conns_.size()`, `sessions: sessions_.count()`,
        `uptime_secs: now - start_time`, `bytes_in/bytes_out`, `known_seeds`, etc.
- [ ] **P7.3** CLI: `stats` subcommand tries IPC first → parse JSON → display formatted
        Fallback: ephemeral dial if no daemon (same pattern as health)
- [ ] **P7.4** Display format:
  ```
  connections: 3
  sessions:    2
  uptime:      2h 14m 32s
  data in:     1.2 MB
  data out:    3.4 MB
  ```
- [ ] **P7.5** Test: `test_ipc_stats.cpp` — start daemon, connect IPC, send `STATS\n`,
        parse JSON, verify fields match expected
- [ ] **P7.6** Error handling: if daemon returns empty or malformed JSON → print error, fall back

**Verification:**
```bash
bridgesessions stats
# → "connections: 3 | sessions: 2 | uptime: 2h 14m | in: 1.2MB out: 3.4MB"
```

**Effort:** Small (~2 hours + tests)

---

## v1.5 Release checklist

- [ ] All 7 items complete
- [ ] Suite total ≥ 1100 assertions
- [ ] `_run_tests.ps1` full suite green (all existing 1009 + new)
- [ ] `file send`/`recv` roundtrip verified on test cluster
- [ ] `restart` signal verified on ≥2 platforms
- [ ] `edit` command verified with text file on remote node
- [ ] `stats` IPC returns live daemon state
- [ ] 4-node health matrix still green (`bash matrix.sh`)
- [ ] Version bumped `1.4.0` → `1.5.0` in `bridgesessions.cpp`
- [ ] All docs cross-references updated (PLANS.md, ARCHITECTURE.md, GUIDELINE.md, README.md)
- [ ] `todo.md` (root) updated with release log

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
| `todo.md` (root) | v1.4 release log | ✅ Current |
