# bridgesessions — Implementation Plan

**Design:** [GUIDELINE.md](./GUIDELINE.md)
**Architecture:** [ARCHITECTURE.md](./ARCHITECTURE.md)
**Active TODO:** [TODO.md](./TODO.md)
**Language:** C++23 | **Build:** Single-file, MSVC/g++/clang + vcpkg | **Deps:** OpenSSL 3+, zstd, CLI11, spdlog, nlohmann/json, Catch2

---

## Where we are

Single-file peer-to-peer mesh relay, validated **v1.4.0** across 4 nodes (Windows/Linux/macOS).
22 message types (0x00–0x17), TLS 1.3 mTLS with ed25519 TOFU, multi-attach sessions,
mDNS + gossip discovery, daemon health IPC, 1009/1009 test assertions on Windows.

The mesh substrate is **proven stable** — pong_timeout=0, config_reload=0, no flap.
Everything in v1.5 builds on this same wire protocol and daemon architecture.

---

## Shipped: v1.4.0 — mesh substrate

| Area | Deliverable | Status |
|------|------------|--------|
| Mesh core | Single-binary peer-to-peer, TLS 1.3 mTLS, ed25519 TOFU | ✅ |
| Cross-platform | Windows MSVC, Linux g++, macOS clang — one source | ✅ |
| Session lifecycle | Multi-attach, detach, kill, resurrect, auto-restart circuit breaker | ✅ |
| Discovery | mDNS LAN + seed config + gossip peer propagation | ✅ |
| Config | seed/pubkey/token parsing, CRLF-safe, dedup by name | ✅ |
| Health IPC | Loopback :19980 on all platforms, event-driven in select() | ✅ |
| Duplicate conns | Deterministic tie-break (smaller pubkey keeps outbound) | ✅ |
| Backoff scheduling | Per-addr timer, one dial per loop, no starvation | ✅ |
| Frame-stall guard | 10s recv timeout on peer sockets | ✅ |
| Single-instance guard | Probe IPC :19980 before starting | ✅ |
| Test suite | 1009/1009, 16 suites, isolated USERPROFILE | ✅ |
| 4-node validation | Shadow + linux-a + linux-b + macos-peer — 12/12 health green | ✅ |

---

## v1.5 — Current sprint

All items build on the existing wire protocol, daemon architecture, CLI parser,
and test harness. No new dependencies. The goal is one release with all 7 items.

### Sequencing and dependencies

```
file send/recv (P1) ──► restart signal (P2) ──► render_hint (P3)
                                     │
                                     ▼
                   edit (P4, uses transfer from P1)
                                     │
                                     ▼
                   virtual folders (P5, uses transfer + edit patterns)
                                     │
                                     ▼
                   stats IPC (P7, independent, can slot anywhere)
```

**Critical path:** P1 → P4 → P5. P2/P3 are small and slot in gaps.
**P7 is independent** — no deps on other features, same :19980 pattern as HEALTH.

---

### P1: `file send` / `file recv` — mesh-native peer-to-peer transfer

**Why:** The GUIDELINE demands file transfer between any two nodes without scp/sftp.
Current mesh has session data + clipboard; file data needs a new message family.

**Wire changes:**
- New message types:
  - `FileMeta` (0x18) — filename, size, mtime, checksum (SHA-256 of content)
  - `FileChunk` (0x19) — offset, total_chunks, chunk_index, data (zstd-compressed)
  - `FileAck` (0x1A) — chunk_index received, next_requested
- Stream routing: File frames go over stream ID 0 (control) for now; dedicated file
  stream IDs are a v2 optimization.
- Flow control: sender waits for FileAck before sending next chunk (per-chunk ACK,
  not sliding window — simple, adequate for sub-100ms latency).
- Resume: FileMeta carries total_chunks and checksum. If receiver already has partial
  file (from a prior interrupted transfer), FileAck can request `next_requested` > 0.

**CLI changes:**
```bash
bridgesessions file send linux-b /local/path/file.txt                 # → ~/linux-b/
bridgesessions file send linux-b /local/path/file.txt /remote/path/   # explicit dir
bridgesessions file recv linux-b /remote/path/file.txt                # → ./
bridgesessions file recv linux-b /remote/path/file.txt /local/dir/    # explicit dir
```

In daemon mode, the CLI subcommand builds a transient `Conn` to the peer (same pattern
as `health`, `shell`), sends FileMeta + chunks, reports progress. Future: daemon
background transfer queue with `transfer list` / `transfer cancel`.

**Daemon changes:**
- New `handle_file_meta()`, `handle_file_chunk()`, `handle_file_ack()` dispatchers
  in `MeshController::dispatch_message()`.
- On receiving FileMeta: prepare output path, open file, await chunks.
- On receiving FileChunk: write to disk, send FileAck.
- Progress logging via spdlog.
- Resume support: check for partial file at destination, compare checksum prefix.

**CLI subcommand implementation:**
- New `file_cmd_app` in CLI11 setup (~20 lines)
- `file_send()`: read local file, SHA-256 hash, build FileMeta, connect_to_peer_impl(),
  send FileMeta, iterate chunks with ACK-wait, close.
- `file_recv()`: connect, send FileMeta request (or inverse), await chunks.

**Tests:**
- `test_file_transfer.cpp` — protocol roundtrip (encode/decode FileMeta/FileChunk/FileAck)
- Integration: spawn two test nodes, send small file, verify checksum match
- Resume test: send half, disconnect, reconnect, verify resume from correct offset
- Size edge cases: 0-byte file, 50MB image, non-ASCII filename

**Verification:**
```bash
bridgesessions file send linux-b README.md /tmp/
# → "sent README.md (12KB, 3 chunks, sha256:ab12...)"
bridgesessions file recv linux-b /tmp/README.md /tmp/copy.md
# → "received README.md (12KB, sha256:ab12...)"
sha256sum README.md && sha256sum /tmp/copy.md  # → identical
```

**Effort:** Moderate (5–7 hours code + tests)

---

### P2: `restart` signal — kill + respawn processes over mesh

**Why:** Currently restarting hermes or codex on a remote node requires SSH-in-kill-ssh-in-run.
A mesh signal does it instantly from the GUI. Core to the swarm model.

**Wire changes:**
- Extend `SignalMsg` struct: add `std::string process = ""` field.
- New variant `Restart` in existing Signal enum (no new message type).
- `Signal{Restart, process="hermes"}` over the session stream.

**Daemon changes:**
- `handle_signal()` switch: case `Restart` → call `restart_process(session)`.
- `restart_process(Session&)`: kill current child (SIGTERM, wait 2s, SIGKILL),
  spawn new process with same command/config, update `child_pid` / `master_fd`.
- Session stays in RUNNING/ATTACHED state throughout; output buffer continues.
- On respawn failure: send SessionDied, transition to Died state (existing path).

**Process resolution order (matching ADR-007):**
1. `process` field from SignalMsg (override)
2. Session's original `command` from creation
3. Config `sessions.default_shell`

**CLI changes:**
- No new subcommand — restart is a session signal, not a CLI command.

**Tests:**
- `test_session_restart.cpp`: create session running `sleep 60`, send Restart signal,
  verify child_pid changes, session still Running, new process responds
- Error: restart with bad cmd → verify SessionDied fires
- Error: restart on dead session → no-op, log warning

**Verification:**
```bash
bridgesessions shell linux-b -n hermes -x "hermes --tui"   # session running
# (in another pane, send restart via protocol)
# → session continues, new hermes process spawns
```

**Effort:** Small (2–3 hours + tests)

---

### P3: `render_hint` flag — markdown vs raw terminal

**Why:** hermes agent output is heavily markdown. The bridgemind.ai GUI has a browser
engine that can render it as HTML. Currently it all looks like raw terminal text.
A single flag bit tells the GUI "this is markdown, render it."

**Wire changes:**
- Single flag bit in frame header: bit 2 in the `flags` byte (bits: 0=compressed,
  1=control, **2=render_markdown**).
- Only meaningful on OutputMsg (0x02), ignored on other types.
- Daemon sets the flag automatically when it detects markdown in the first ~200 bytes
  (heuristic: presence of `# `, `## `, `- `, `` ``` ``, `| ` patterns). Config override
  `output.render_hint auto|markdown|raw`.

**CLI/daemon changes:**
- New config key: `output.render_hint` = `auto` (default), `markdown`, `raw`.
- In `dispatch_message()` on OutputMsg: apply heuristic → set flag.
- CLI health/stats outputs don't need it (raw terminal by default).

**Tests:**
- `test_output_render_hint.cpp`: set `render_hint=markdown`, send markdown text,
  verify flag bit IS set. Send plain text, verify flag NOT set.
- Heuristic test: mix of markdown/non-markdown, verify detection accuracy.
- Config override test: `render_hint=raw` → never set flag.

**Verification:** Capture OutputMsg on wire, check bit 2 of flags byte.

**Effort:** Trivial (1 hour + tests)

---

### P4: `edit <peer>:<path>` — remote file editing with delta patches

**Why:** Editing a config on linux-b should feel local. Current workflow: scp out, edit,
scp back. Mesh-native: open a whole file or a remote editor view, save sends diffs.

**Wire changes:**
- Reuses P1's file transfer infrastructure for initial fetch + final write-back.
- No new message types. Pattern:
  1. `file recv` the remote file to a temp local path
  2. User edits (vim/nano/helix/notepad++)
  3. On save: compute diff (or full content for binary), `file send` back
  4. Daemon applies patch atomically (write temp, rename over original)

**CLI changes:**
```bash
bridgesessions edit linux-b:/etc/nginx/nginx.conf
# → downloads to /tmp/bsedit-XXXX/nginx.conf
# → opens $EDITOR (or notepad++ on Windows)
# → on editor exit: diffs, uploads, confirms
```

**Implementation details:**
- `edit_cmd_app` in CLI11 (new subcommand).
- `bsedit_file()`: parse `peer:path` pair, resolve peer name to addr,
  download via P1 `file recv` to temp dir, launch editor (`$EDITOR` or `notepad++` on Win,
  `vim` as fallback), wait for editor exit, diff against original.
- If file changed: upload via P1 `file send` to original path (or a `.bak` backup first).
- If no change: print "no changes" and clean up temp dir.
- Backup: before overwriting remote, daemon copies `file → file.bsbak` (last one).

**Limitations:**
- Binary files: no diff, full file upload on every save.
- Concurrent edits: last-writer-wins (same as scp workflow). v2: lock protocol.
- Large files (>50MB): warn user, suggest scp.

**Tests:**
- `test_edit_integration.cpp`: create temp file on test peer, edit it via CLI,
  verify remote file matches edited version.
- No-change case: open + close editor without saving → verify no upload.

**Verification:**
```bash
bridgesessions edit linux-b:/home/agent/myconfig.yaml
# → "downloaded myconfig.yaml (2KB)"
# → opens $EDITOR
# → (save and quit)
# → "file changed, uploading... pushed 1 delta"
```

**Effort:** Moderate (4–6 hours + tests)

---

### P5: Virtual folder mapping — local↔remote live sync

**Why:** A folder on the GUI (e.g. `~/projects/`) should be automatically synced to
a remote path on linux-b. Not rsync — mesh-aware, change-driven, compressed.

**Architecture:**
- New daemon thread per mount: filesystem watcher (inotify on Linux,
  ReadDirectoryChangesW on Windows, FSEvents on macOS).
- On local file change: zstd-compress + send FileChunk series to remote peer.
- Remote peer writes to mapped path.
- Conflicts: last-writer-wins with `.bsconflict.<peer>` copy for both sides.
- Sync triggers: file create, modify, delete, rename. Debounced 2s window.

**Config format (new section):**
```ini
[vfolder "projects"]
local = ~/projects
remote = linux-b:/home/agent/projects
sync_interval_secs = 30
direction = bidirectional      # push, pull, bidirectional
```

**CLI changes:**
```bash
bridgesessions vfolder list          # → active mounts
bridgesessions vfolder sync projects  # → force sync now
bridgesessions vfolder pause projects # → stop watcher, keep mount
```

**Implementation considerations:**
- Thread safety: watcher thread pushes change events to a queue; main loop
  processes them (callbacks to `file_send`).
- Initial sync: on mount start, rsync-style scan + hash all files, transfer
  any that differ.
- Ignore patterns: `.git/`, `node_modules/`, `*.swp`, `.DS_Store`.
- State persistence: mapping config in `~/.bridgesessions/vfolders.json`.

**Tests:**
- `test_vfolder_local_change.cpp`: create watcher, write file in watched dir,
  verify FileChunk sent to mock peer.
- `test_vfolder_initial_sync.cpp`: pre-populate remote dir with different files,
  verify sync brings them in sync.
- `test_vfolder_conflict.cpp`: simultaneous edit on both sides → produce .bsconflict.

**Verification:**
```bash
# Setup mapping, start daemon
echo "new content" > ~/projects/test.txt
# → log: "vfolder projects: test.txt changed, sending to linux-b"
# On linux-b: cat /home/agent/projects/test.txt → "new content"
```

**Effort:** Large (10–15 hours + tests, mostly platform watcher APIs)

---

### P7: `stats` IPC parity — expose daemon connections + sessions

**Why:** `bridgesessions stats` currently opens a fresh TLS connection (ephemeral CLI)
that races the daemon. Should query daemon IPC like `health` does.

**Wire changes:**
- None (new IPC command string, not a new protocol message).
- IPC request: `STATS\n`
- IPC response: JSON blob on one line:
```
STATS {"conns":3,"sessions":5,"uptime_secs":86400,"bytes_in":1234,"bytes_out":5678,"relay_count":0,"known_seeds":3,"known_discovered":2}
```

**Daemon changes:**
- Add `"STATS"` branch to `cli_ipc_accept_one()` parser.
- Build JSON from `conns_`, `sessions_.count()`, uptime counter.
- Same response format over TCP :19980.

**CLI changes:**
- `stats` subcommand: try `daemon_health_via_ipc`-style query (but for STATS),
  fall back to ephemeral dial if no daemon answers.

**Tests:**
- `test_ipc_stats.cpp`: start daemon, connect IPC, send "STATS\n", parse JSON,
  verify conn count matches expected.

**Verification:**
```bash
bridgesessions stats
# → "connections: 3 | sessions: 2 | uptime: 2h 14m"
```

**Effort:** Small (2–3 hours + tests)

---

## v1.5 Release criteria

All must pass for v1.5 tag:

- [ ] 7 items complete with test coverage
- [ ] Suite total ≥ 1100 assertions (current 1009 + new tests)
- [ ] All v1.4 regression tests still pass (1009/1009)
- [ ] `file send`/`recv` roundtrip verified on test cluster
- [ ] `restart` signal verified on at least 2 platforms
- [ ] `edit` command verified with text file on remote node
- [ ] `stats` IPC returns live daemon state
- [ ] 4-node health matrix still green
- [ ] Version bumped `1.4.0` → `1.5.0`
- [ ] All docs cross-reference updated

---

## Future: v2 (post v1.5)

| Feature | Depends on | When |
|---------|-----------|------|
| QUIC via msquic | — | After v1.5 stable on all platforms |
| Nonblocking TLS handshakes | — | Concurrent with QUIC |
| Session recording + replay | P1 file transfer infra | After QUIC |
| Read-only spectators (fan-out) | — | After session recording |
| SRV record discovery | — | Low priority |
| Dictionary-trained zstd | — | Performance pass |

---

## shadow-agent (separate workstream)

bridgesessions is the **mesh transport**. shadow-agent is a **separate daemon**
(`PLANS-shadow-agent.md`) that wraps Shadow's desktop surface (CUA, FS, Hermes chat,
Roblox Studio, Win32) behind one mTLS MCP-over-HTTP endpoint on `:9100`.

The v1.5 features here (file transfer, edit, restart) feed into shadow-agent's
`shell.*` and `fs.*` tool surface — shadow-agent doesn't reimplement transport,
it reuses bridgesessions' proven wire.

---

## Docs

| Doc | Purpose | Status |
|-----|---------|--------|
| `GUIDELINE.md` | Design sketch, vision, v1.5/v2 feature map | ✅ Current |
| `ARCHITECTURE.md` | Deep ADR docs, wire format, daemon architecture, deployment | ✅ Current |
| `PLANS.md` | Implementation plan — this file | ✅ Current |
| `TODO.md` | Task checklist with verification criteria | ✅ Current |
| `README.md` | Quickstart + commands | ✅ Current |
| `AUTONOMOUS.md` | Agent dispatch rules | ✅ Current |
| `todo.md` (root) | v1.4 release log | ✅ Current |
