# bridgesessions — v1.4.0 reliability hardening

**Status:** 🟢 **v1.4.0 released** — all items closed, docs reconciled, 4-node validated.
**Date:** 2026-06-22

---

## Deliverables

| Item | Status |
|------|--------|
| Single-instance guard (probe IPC :19980 before start) | ✅ |
| TLS 1.2+ fallback (not 1.3-only — cross-platform edge) | ✅ |
| Bounded blocking handshake (select + deadline, not spin) | ✅ |
| Backoff scheduling (per-addr, one dial per loop) | ✅ |
| Startup IPC init before seed dials | ✅ |
| Empty-pubkey guard on Gossip/Hello | ✅ |
| Duplicate-conn tie-break (deterministic, symmetric) | ✅ |
| Discovered peers not persisted (kill config_reload churn) | ✅ |
| IPC fd in select() + event-driven accept | ✅ |
| HEALTH = last_pong freshness, not synchronous ping | ✅ |
| Bounded recv on IPC accept (2s) | ✅ |
| bootstrap_identity path fix (bs_dir, not home_dir_) | ✅ |
| Daemon health IPC un-gated (all platforms) | ✅ |
| macOS PTY include (util.h vs pty.h) | ✅ |
| CRLF-safe config parsing + seed pubkey token parsing | ✅ |
| Peer pubkey dedup in dial loop (don't re-dial connected) | ✅ |
| Steady-state recv timeout (10s, kPeerRecvTimeoutMs) | ✅ |
| `--version` bumped 1.3.0-reliability → 1.4.0 | ✅ |
| Test suite: 1009/1009 (16 suites, isolated USERPROFILE) | ✅ |

## Test failures found & fixed

| Suite | Symptom | Fix |
|-------|---------|-----|
| `test_config` | Asserted default port 19948; code changed to 19949 | Updated assertion |

## Production validation

All 4 nodes (Shadow/Win + linux-b + linux-a + macos-peer) deployed 1.4.0, mesh formed
within ~100s, live `health` = `healthy` in all 12 directions.

```
shadow → linux-a/linux-b/macos-peer : healthy / healthy / healthy
linux-a  → shadow/linux-b/macos-peer: healthy / healthy / healthy
linux-b  → shadow/linux-a/macos-peer: healthy / healthy / healthy
macos-peer→ shadow/linux-a/linux-b  : healthy / healthy / healthy
```

## Docs reconciliation (same session)

All architecture docs rewritten to match peer-mesh reality:

| Doc | What changed |
|-----|-------------|
| `GUIDELINE.md` | Client/server sketch → mesh swarm vision |
| `PLANS.md` | Windows port phases → v1.4/v1.5/v2 roadmap |
| `TODO.md` (docs/) | 13-phase checklist → v1.4 done / v1.5 sprint / v2 deferred |
| `README.md` | Old two-binary setup → peer mesh quickstart |
| `AUTONOMOUS.md` | Stale lib targets → single-file reality, pitfalls |
| `ARCHITECTURE.md` | Client-server spec → peer mesh with ADR-003 |

## Move forward

v1.5 sprint items tracked in `docs/TODO.md`:
- `file send`/`file recv` — mesh-native transfer
- `restart` signal — kill+respawn processes over mesh
- `render_hint` flag — markdown vs raw terminal
- `edit` subcommand — remote file editing
- Virtual folder mapping — local↔remote live sync
- `stats` IPC parity

---

**Commits:** `232eaf7` → `e0387ee` → `7f05bc8` → `9b4b359` → `62fddb9` → `405c181`
**Next:** v1.5 — start with `file send`/`file recv`.
