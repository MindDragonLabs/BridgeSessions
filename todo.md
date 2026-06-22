# bridgesessions — Reliability Hardening TODO

**Status:** 🟢 **v1.4.0 validated** — local suite 1009/1009; 4-node cross-platform mesh (Shadow/Windows + linux-b/linux-a/Linux + macos-peer/macOS) forms within ~100s and live `health` returns `healthy` in all 12 directions.
**Date:** 2026-06-22

---

## Latest code (`working tree after 232eaf7`)

- macOS PTY include fixed: `<util.h>` on Apple, `<pty.h>` elsewhere.
- TLS relaxed to TLS 1.2+ for cross-platform OpenSSL/SChannel edge compatibility.
- Bounded TLS handshake helper waits on socket readiness instead of spinning on `SSL_ERROR_WANT_READ/WRITE`.
- Health CLI timeout restored to **30s** for Tailscale/cross-platform paths.
- Inbound TLS accept timeout capped at **5s** so slow/half-open clients do not stall mesh loop.
- Identity bootstrap fixed: `MeshController` now writes/loads cert/key under `~/.bridgesessions/` consistently.
- Seed/discovered reconnect backoff fixed: retries resume after scheduled delay; one failed bounded dial per loop prevents accept/read starvation.
- Startup no longer dials seeds before CLI IPC init.
- Gossip/Hello skip peers with empty pubkeys.
- BS_TESTING still blocks production config writes.

## Verified locally (after test_config default-port fix)

Built every suite against the working tree and ran them (Windows MSVC, isolated test HOME).

| Item | Status |
|------|--------|
| `git diff --check` | **PASS** |
| `test_mesh_reliability` | **26/26 PASS** |
| `test_tls_reliability` | **9/9 PASS** |
| `test_config` | **73/73 PASS** (was 1 failing: asserted stale default port 19948; updated to 19949 to match `listen_port` default fix) |
| Full test suite (16 suites) | **1009/1009 PASS** |

> Note: version string bumped `1.3.0-reliability` → `1.4.0` (`--version`).

## Production validation — DONE (2026-06-22, 4-node live)

Deployed 1.4.0 to all four nodes (Shadow local MSVC; linux-b/linux-a g++; macos-peer clang),
truncated all logs, restarted all daemons, waited ~100s.

| Check | Result |
|-------|--------|
| All nodes report `--version` `1.4.0` | ✅ |
| All nodes LISTEN on 19949 (Shadow also 19980 IPC) | ✅ |
| Each node has 3 live conns (full mesh) | ✅ |
| `pong_timeout` across all nodes | **0** (no flap) |
| `config_reload` across all nodes | **0** (churn-loop fix holds) |
| Live `health` matrix — all 12 directions | **all `healthy`** |

Health matrix (live IPC, not stale logs):
```
shadow ->linux-a/linux-b/macos-peer : healthy / healthy / healthy
linux-a  ->shadow/linux-b/macos-peer : healthy / healthy / healthy
linux-b  ->shadow/linux-a/macos-peer : healthy / healthy / healthy
macos-peer->shadow/linux-a/linux-b   : healthy / healthy / healthy
```

> accept_fail/connect_fail counts are nonzero but **startup-only** simultaneous-dial
> races (duplicate-conn dedup); counts stop climbing once mesh settles. Reproduce
> with `bash matrix.sh`.

## Remaining (optional)

- Cross-node attach / session-search exercise across Windows/Linux/macOS (health proven; interactive shell path not re-tested this wave).
- `bridgesessions.exe stats` IPC parity (health IPC done; stats still ephemeral-CLI).

## Addressed this wave (review follow-ups)

- Partial-frame event-loop stall: `check_conn_read`'s drain loop relied on `SSL_pending()`, which guarantees only >=1 buffered byte, not a whole frame. A frame split across TLS records (front half buffered) made `read_frame` block in `SSL_read_ex` on the rest, freezing the single-threaded loop. Fixed by setting a 10s steady-state recv timeout (`kPeerRecvTimeoutMs`) on established peer sockets at both push sites (inbound + outbound); a stall now degrades to drop+reconnect via the existing catch + backoff.

## Deferred (v2+)

- Nonblocking TLS handshakes in main loop instead of bounded blocking calls.
- Daemon IPC health/stats API as primary operational path.
- Better TOFU persistence/pinning instead of accept-all connect callback.
- Production CI smoke test over real Tailscale nodes.
