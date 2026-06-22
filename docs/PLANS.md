# bridgesessions — Implementation Plan

**Design:** [GUIDELINE.md](./GUIDELINE.md)
**Architecture:** [ARCHITECTURE.md](./ARCHITECTURE.md)
**Active TODO:** [TODO.md](./TODO.md)
**Language:** C++23 | **Build:** Single-file, MSVC/g++/clang + vcpkg | **Deps:** OpenSSL 3+, zstd, CLI11, spdlog, nlohmann/json, Catch2

---

## Where we are

Single-file peer-to-peer mesh relay, validated 1.4.0 across 4 nodes (Windows/Linux/macOS). 22 message types, TLS 1.3 mTLS, ed25519 TOFU, multi-attach sessions, mDNS+gossip discovery, daemon health IPC, 1009/1009 tests on Windows.

## What changed

The old plan (two binaries, three libs, BridgeSpace macOS front-end, Windows port sprint) has been **superseded** by a simpler reality:

- **Single-file** `bridgesessions.cpp` — no libs, no CMake library targets, one `cl`/`g++` command builds all platforms.
- **Peer mesh, not client-server** — every node runs the same binary; the GUI (bridgemind.ai) is a mesh peer.
- **Windows port done** — the single-file includes native Winsock2 + ConPTY + MSVC build. The dedicated `PLANS-WINDOWS.md` is archived.
- **No BridgeSpace.app** — the bridgemind.ai browser-based terminal is the operator console.

---

## Shipped: v1.4.0 (this wave)

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
| Single-instance guard | Probe IPC port before starting | ✅ |
| Test suite | 1009/1009, 16 suites, isolated USERPROFILE | ✅ |
| 4-node validation | Shadow + linux-a + linux-b + macos-peer — 12/12 health green | ✅ |

---

## v1.5 — Current sprint

Build the GUIDELINE features on top of the proven mesh substrate. All items reuse the existing wire protocol, daemon architecture, and cross-platform transport.

| Priority | Feature | Wire impact | Effort |
|----------|---------|-------------|--------|
| 1 | `file send` / `file recv` — mesh-native peer-to-peer transfer | New message types (FileTransfer, FileAck, FileChunk) + new CLI subcommand | Moderate |
| 2 | `restart` signal — kill + respawn bash/hermes/codex/claude over mesh | Extend SignalMsg (0x0D) with Restart variant | Small |
| 3 | `render_hint` flag — tell GUI "this OutputMsg is markdown, render it" | Single flag bit in frame header | Trivial |
| 4 | `edit` subcommand — open remote file locally, save delta patch | No new wire types; scp+patch pattern over existing transfer | Moderate |
| 5 | Virtual folder mapping — local↔remote live sync | New daemon thread per mount, inotify/FSEvents/ReadDirectoryChanges | Large |
| — | `stats` IPC parity — expose daemon conns/sessions over :19980 | Parse in cli_ipc_accept_one, same as HEALTH | Small |

### v1.5 sequencing

```
Priority 1  ─► file send/recv ─► Priority 2  ─► restart signal ─► Priority 3  ─► render_hint
                                                  │
                                                  ▼
                                    Priority 4/5 ─► edit, virtual folders (parallel-able)
```

---

## Future v2 (post v1.5)

| Feature | Rationale |
|---------|-----------|
| QUIC via msquic | Library swap, same protocol — eliminates TCP head-of-line blocking on high-latency links |
| Nonblocking TLS handshakes | Kill the last blocking call in the event loop |
| Session recording + replay | Protocol messages already exist (`ImageFrame` → `RecordedFrame`) |
| Read-only spectators (fan-out) | Share a session view without write access |
| SRV record peer discovery | DNS-based, no config needed for fleet nodes |
| Dictionary-trained zstd | Higher compression on ANSI-heavy terminal output |

---

## shadow-agent — separate workstream

See `PLANS-shadow-agent.md` (project root). bridgesessions is the **mesh substrate**; shadow-agent is a **different binary** that wraps Shadow's desktop surface (CUA, FS, Hermes chat, Roblox) behind one mTLS endpoint. The GUIDELINE's file transfer, edit, and restart features here feed into shadow-agent's `shell.*` / `fs.*` tool surface.

---

## Docs audit

| Doc | Status |
|-----|--------|
| `GUIDELINE.md` | ✅ Current — rewritten for mesh vision |
| `ARCHITECTURE.md` | ⚠️ Two-binary client/server — still accurate at protocol level, needs front-end diagram update |
| `TODO.md` | ✅ Rewritten — maps to v1.4/v1.5/v2 |
| `PLANS.md` | ✅ Rewritten — this file |
| `README.md` | ✅ Rewritten — matches mesh reality |
| `AUTONOMOUS.md` | ⚠️ Agent dispatch rules OK; library targets stale |
| `PLANS-WINDOWS.md` | 🗄️ Superseded — archived in git history |
| `PLANS-shadow-agent.md` | ✅ Current (separate workstream) |
| `TODO-shadow-agent.md` | ✅ Current (separate workstream) |
| `todo.md` (root) | ✅ Current (reliability deployment) |
