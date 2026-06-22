# bridgesessions — Reliability Hardening TODO

**Status:** 🟡 **v1.4 in progress** — local reliability fixes pass targeted tests; production cross-node health still needs live Tailscale validation.
**Date:** 2026-06-21

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

## Remaining production validation

1. Build/deploy new binary on Shadow, linux-a, linux-b.
2. Confirm each node uses correct production config:
   - Shadow listen: `100.124.169.66:19949` or `0.0.0.0:19949`
   - linux-a/linux-b seeds include Shadow + each other with valid 64-hex pubkeys.
   - `authorized_keys` contains other nodes' pubkeys.
3. Network checks both directions:
   - linux-b → Shadow `100.124.169.66:19949`
   - Shadow → linux-b/linux-a `:19949`
   - linux-a ↔ linux-b `:19949`
4. Daemon checks:
   - logs show `mesh_listening`
   - logs show `tls_verify_* result=accept`
   - logs show `mesh_peer_connected` / `mesh_peer_connected_outbound`
5. CLI checks:
   - `bridgesessions.exe health <peer>` from each node
   - `bridgesessions.exe stats` on daemon/IPC path when available
6. If CLI `health` still times out while daemon has live conns, implement daemon IPC health query (Unix socket / named pipe) so health reuses live mesh state instead of opening fresh TLS.

## PASS criteria

- Full local suite green.
- Shadow/linux-a/linux-b daemons form mesh within 60s.
- Cross-node `health` succeeds from all nodes.
- Cross-node attach/session search works across Windows/Linux/macOS path.

## Addressed this wave (review follow-ups)

- Partial-frame event-loop stall: `check_conn_read`'s drain loop relied on `SSL_pending()`, which guarantees only >=1 buffered byte, not a whole frame. A frame split across TLS records (front half buffered) made `read_frame` block in `SSL_read_ex` on the rest, freezing the single-threaded loop. Fixed by setting a 10s steady-state recv timeout (`kPeerRecvTimeoutMs`) on established peer sockets at both push sites (inbound + outbound); a stall now degrades to drop+reconnect via the existing catch + backoff.

## Deferred (v2+)

- Nonblocking TLS handshakes in main loop instead of bounded blocking calls.
- Daemon IPC health/stats API as primary operational path.
- Better TOFU persistence/pinning instead of accept-all connect callback.
- Production CI smoke test over real Tailscale nodes.
