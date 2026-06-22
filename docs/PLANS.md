# bridgesessions — Implementation Plan

**Design:** [GUIDELINE.md](./GUIDELINE.md)
**Architecture:** [ARCHITECTURE.md](./ARCHITECTURE.md)
**Active TODO:** [TODO.md](./TODO.md)
**Language:** C++23 | **Build:** Single-file, MSVC/g++/clang + vcpkg | **Deps:** OpenSSL 3+, zstd, CLI11, spdlog, nlohmann/json, Catch2

---

## Where we are

Single-file peer-to-peer mesh relay, validated **v1.5.0** across 4 nodes (Windows/Linux/macOS).
22 message types (0x00–0x17), TLS 1.3 mTLS with ed25519 TOFU, multi-attach sessions,
mDNS + gossip discovery, daemon health IPC, **1074/1074 test assertions** on Windows.

v1.5 added: file transfer (bidirectional, SHA-256 verified), restart signal, render_hint,
remote file editing, virtual folder mapping, live stats IPC. All verified on 4-node cluster.

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

## Shipped: v1.5.0 — mesh file services

All items build on the v1.4 wire protocol, daemon architecture, CLI parser, and test harness.
No new dependencies.

| Area | Deliverable | Status |
|------|------------|--------|
| P1 | `file send` / `file recv` — bidirectional mesh-native transfer | ✅ |
| P2 | `restart` signal — kill+respawn processes over mesh | ✅ |
| P3 | `render_hint` flag — markdown vs raw terminal detection | ✅ |
| P4 | `edit <peer>:<path>` — remote file editing with diff upload | ✅ |
| P5 | Virtual folder mapping — config + CLI + polling sync | ✅ |
| P6 | `stats` IPC parity — live JSON daemon state over :19980 | ✅ |
| Test suite | **1074/1074**, 17 suites, all P1-P6 features tested | ✅ |
| 4-node validation | P1-P6 live-tested on Shadow+linux-a+linux-b+macos-peer | ✅ |
| Version | `1.4.0` → `1.5.0` | ✅ |

---

## v1.7 — Current development

Next sprint after P1-P6. The mesh substrate is stable; these build on it.

### Sequencing & dependencies

```
vfolder watcher (W1-W4) ──► vfolder bidirectional (W5-W7)
                                  │
                                  ▼
            auto-restart gateway (G1) ──► session persistence (G2)
                                  │
                                  ▼
            rendering pipeline (R1-R3) ──► OOB message handling (O1)
```

| # | Feature | Effort | Depends on |
|---|---------|--------|-----------|
| W1 | Win: ReadDirectoryChangesW watcher | 4h | P5 polling |
| W2 | Linux: inotify watcher | 2h | P5 polling |
| W3 | macOS: FSEvents watcher | 3h | P5 polling |
| W4 | Watcher thread → event queue → daemon dispatch | 3h | W1-W3 |
| W5 | Bidirectional sync (receive remote changes) | 4h | P1 |
| W6 | Conflict detection (`.bsconflict` copies) | 2h | W5 |
| W7 | Ignore patterns (`node_modules/`, `.git/`, etc.) | 1h | W4 |
| G1 | Daemon auto-restart on crash (systemd watchdog) | 2h | P2 |
| G2 | Session persistence to disk + recovery | 3h | P6 |
| R1 | Image rendering via chafa/kitty protocol | 4h | — |
| R2 | OSC-52 clipboard integration | 2h | — |
| R3 | Terminal emulation state tracking | 6h | — |
| O1 | Out-of-band message injection | 2h | — |

### Release criteria

- [ ] Platform filesystem watchers (Win/Linux/macOS) all green
- [ ] Bidirectional vfolder sync verified on 4-node cluster
- [ ] Suite total ≥ 1150 assertions
- [ ] All v1.4 + v1.5 regression tests still pass
- [ ] Version bumped `1.5.0` → `1.7.0`

---

## Future: v2 (post v1.7)

| Feature | Depends on | When |
|---------|-----------|------|
| QUIC via msquic | — | After v1.7 stable on all platforms |
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
| `todo.md` (root) | v1.4/v1.5 release logs | ✅ Current |
