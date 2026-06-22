# bridgesessions — TODO

**Status:** v1.4.0 — peer mesh tested and green on 4 nodes (Windows/Linux/macOS)
**Plan:** [PLANS.md](./PLANS.md) · **Design:** [GUIDELINE.md](./GUIDELINE.md)

---

## Completed: v1.4.0 — mesh substrate

All core mesh, transport, session, config, and reliability items shipped:

- Single-file C++23, MSVC/g++/clang, one build command per platform
- TLS 1.3 mTLS with ed25519 + TOFU
- 22 message types (0x00–0x17)
- Session lifecycle: CREATED → RUNNING → DETACHED → ATTACHED → DIED → KILLED
- Multi-attach (vector peer_ids), multi-hop session routing (SessionSearch 0x17)
- mDNS LAN discovery + gossip peer propagation + seed config
- Daemon health IPC on loopback :19980 (all platforms, event-driven)
- CLI: shell, sessions, peers, keygen, authorize, health, stats, image, anim
- Config: key=value with `#` comments, CRLF-safe, pubkey token parsing, dedup
- Session persistence: v1:plain JSON, atomic tmp+rename, legacy loader
- Ring buffer (thread-safe CircularBuffer), scrollback replay
- Clipboard: OSC 52 capture + ClipboardPut
- Duplicate-conn resolution: deterministic tie-break by pubkey
- Reconnect backoff: per-addr scheduling, one dial per event-loop pass
- Single-instance guard (probe IPC :19980 before starting)
- Steady-state recv timeout (10s) on peer sockets to bound frame-stall
- Tests: 1009 assertions across 16 suites (unit + integration), isolated USERPROFILE
- 4-node production validation: Shadow + linux-a + linux-b + macos-peer, 12/12 health matrix

---

## v1.5 — In progress/sprintable

| # | Item | Wire | Status |
|---|------|------|--------|
| 1 | `file send <peer> <local> [remote]` — peer-to-peer transfer | New FileTransfer/FileAck/FileChunk message types | `[ ]` |
| 2 | `file recv <peer> <remote> [local]` — inverse transfer | Same types | `[ ]` |
| 3 | `restart` signal — kill+respawn bash/hermes/codex/claude over mesh | Extend SignalMsg (0x0D) with Restart variant | `[ ]` |
| 4 | `render_hint` flag on OutputMsg — GUI renders markdown vs raw terminal | Flag bit in frame header | `[ ]` |
| 5 | `edit <peer>:<path>` — open remote file locally, save delta patch | Uses existing transfer; no new wire types | `[ ]` |
| 6 | Virtual folder mapping — local↔remote live sync via inotify/FSEvents | New daemon filesystem-watch thread | `[ ]` |
| 7 | `stats` IPC parity — expose daemon conns+sessions over :19980 | Parse in cli_ipc_accept_one (same pattern as HEALTH) | `[ ]` |

**v1.5 release:** all 7 items done, built, cross-platform tested, 4-node health re-validated.

---

## v2 — Future

| # | Feature | Notes |
|---|---------|-------|
| 1 | QUIC via msquic transport backend | Library swap, same protocol layer — eliminates TCP HoL blocking |
| 2 | Nonblocking TLS handshakes in event loop | Kill the last blocking call for total async |
| 3 | Session recording + replay | ImageFrame already carries sequential metadata |
| 4 | Read-only session spectators (fan-out mode) | New attach flag for read-only |
| 5 | SRV record peer discovery | DNS-based, no seed config needed for fleet |
| 6 | Dictionary-trained zstd compression | Higher ratio on ANSI terminal output |
| 7 | Production CI smoke test over live Tailscale nodes | Automated health matrix run |

---

## shadow-agent workstream

Not bridgesessions — separate daemon at `PLANS-shadow-agent.md`. bridgesessions is the mesh transport; shadow-agent is the Shadow-specific tool surface (shell, CUA, FS, Hermes, Roblox, Win32) exposed as one MCP-over-HTTP endpoint. See `TODO-shadow-agent.md` in project root.

---

## Docs

| Doc | Purpose | Status |
|-----|---------|--------|
| `GUIDELINE.md` | Design sketch, vision | ✅ Current |
| `ARCHITECTURE.md` | Deep ADR docs, wire format | ⚠️ Diagrams need 4-node mesh update vs old client/server |
| `PLANS.md` | Implementation plan | ✅ Current |
| `README.md` | Quickstart + commands | ✅ Current |
| `todo.md` (root) | Reliability deployment log | ✅ Current |
| `PLANS-shadow-agent.md` | Shadow agent plan | ✅ Current |
