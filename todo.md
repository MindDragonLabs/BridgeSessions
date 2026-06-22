# bridgesessions — v1.5.0 mesh file services

**Status:** 🟢 **v1.5.0 released** — all P1-P6 items shipped, 4-node validated.
**Date:** 2026-06-22

---

## v1.4.0 — mesh substrate

**Status:** 🟢 **v1.4.0 released** — all items closed, docs reconciled, 4-node validated.
**Date:** 2026-06-22

See Git history for v1.4 release log.

## v1.5.0 — mesh file services

### Deliverables

| # | Feature | Notes | Status |
|---|---------|-------|--------|
| P1 | `file send` / `file recv` | FileMeta(0x18), FileChunk(0x19), FileAck(0x1A), FileRequest(0x1F) bidirectional. Live SHA-256 verified Shadow↔linux-b | ✅ |
| P2 | `restart` signal | SignalMsg::Restart enum + process field. Kill+respawn handler. 36/36 session tests | ✅ |
| P3 | `render_hint` flag | FLAG_RENDER_MARKDOWN=0x04, OutputMsg.render_markdown bool, heuristic detection, config override. 156/156 message tests | ✅ |
| P4 | `edit <peer>:<path>` | Route through daemon IPC (EDIT_DL/EDIT_UP). Download to temp, open $EDITOR, verify SHA-256, upload diffs. Live verified | ✅ |
| P5 | Virtual folder mapping | `vfolder.<name>.*` config keys. CLI: add/list/sync. Config-driven polling sync over daemon IPC | ✅ |
| P6 | `stats` IPC parity | STATS IPC command → JSON. Live daemon state: 3 peers, uptime, bytes. Verified | ✅ |

### Test suite

| Suite | Layer | Assertions |
|-------|-------|-----------|
| test_message | unit | 156 |
| test_codec | unit | 155 |
| test_frame_io | unit | 42 |
| test_osc52 | unit | 37 |
| test_ring_buffer | unit | 49 |
| test_identity | unit | 84 |
| test_config | unit | 73 |
| test_tls | integration | 146 |
| test_tls_reliability | integration | 9 |
| test_authorized_keys_reload | integration | 23 |
| test_session | integration | 36 |
| test_session_registry | integration | 75 |
| test_relay | integration | 28 |
| test_multi_attach | integration | 48 |
| test_mesh | integration | 69 |
| test_mesh_reliability | integration | 26 |
| test_file_transfer | integration | 18 |
| **Total** | **17 suites** | **1074** |

### 4-node cluster validation

```
shadow → linux-a/linux-b/macos-peer : healthy / healthy / healthy
linux-a  → shadow/linux-b/macos-peer: healthy / healthy / healthy
linux-b  → shadow/linux-a/macos-peer: healthy / healthy / healthy
macos-peer→ shadow/linux-a/linux-b  : healthy / healthy / healthy
```

- file send Shadow→linux-b: SHA-256 match
- file send linux-b→Shadow: SHA-256 match
- edit download from linux-b: confirmed
- stats IPC: 3 peers, real uptime
- vfolder add + list: config persist verified

### Commits

```
12ba28d P6: stats IPC parity — live JSON over daemon IPC
d384e62 P5: virtual folder mapping — config + CLI + polling sync
15f90e4 P4: edit <peer>:<path> IPC routing — download + editor + verify + upload
f4756e8 P4: edit <peer>:<path> — remote file editing with delta upload
fae8ece P3: render_hint flag — markdown vs raw terminal detection
a580564 P1 cleanup: resume support, integration tests, MSVC libs fix + P2: restart signal
9ab6c90 P1.8-P1.19: file transfer daemon + CLI + IPC routing + live cluster test
81ef77e P1.1-P1.7: file transfer protocol types + codec + unit tests
```

### Next

v1.7: Platform filesystem watchers + bidirectional vfolder sync (tracked in `docs/TODO.md`)
