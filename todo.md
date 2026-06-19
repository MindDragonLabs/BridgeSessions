# bridgesessions — Reliability Hardening TODO

**Status:** 🟡 **v1.3 CODE COMPLETE** — live cluster R3.2 / R6.5 / R5.5 still need SSH deploy
**Date:** 2026-06-11
**Source:** ~5,450 lines, single-file C++23, `bridgesessions.cpp`

**Target:** 3-node mesh (Shadow + linux-b + FECv3) forms and stays formed. Windows↔Linux TLS handshake works reliably. Daemons don't crash-loop.

---

## v1.3: Reliability Hardening (HIGHEST PRIORITY)

### R1: TLS Debug Logging
- [x] **R1.1** Add `log_event("tls_verify_server", pubkey_hex + " result=" + (ok?"accept":"reject"))` in `server_cert_verify_cb`
- [x] **R1.2** Add `log_event("tls_verify_client", fp + " result=" + (ok?"accept":"reject"))` in `client_cert_verify_cb`
- [x] **R1.3** On connect error in `connect_to_peer_impl()`, log `SSL_get_error()` + `ERR_error_string()` BEFORE the catch block consumes the error queue
- [x] **R1.4** Log cert subject/issuer/pubkey fingerprint on both sides of handshake (accept path + connect path)
- [x] **R1.5** Add `log_event("mesh_tls_connect_error", addr + ": " + err_detail)` with actual error detail (currently logs empty `error:00000000:lib(0)::reason(0)`)

### R2: TLS Handshake Timeout
- [x] **R2.1** `connect_to_peer_impl()`: `setsockopt(SO_RCVTIMEO, 5000ms)` on socket before `SSL_connect()`
- [x] **R2.2** `connect_and_hello()`: `setsockopt(SO_SNDTIMEO/RCVTIMEO, 5000ms)` on socket before connect
- [x] **R2.3** `connect_and_hello()`: report clear error when timeout vs refused vs TLS rejected
- [x] **R2.4** Accept loop: `select()` with 3s timeout so daemon doesn't freeze; forces periodic health checks

### R3: Daemon Process Lifecycle
- [x] **R3.1** Linux: create systemd unit at `/etc/systemd/system/bsmesh.service` (Restart=always, RestartSec=5, User=agent) — template in `etc/bsmesh.service`
- [ ] **R3.2** Linux: install + enable systemd unit on linux-b + FECv3
- [x] **R3.3** Windows: create NSSM install script (`install-daemon.ps1`) with auto-restart on failure
- [ ] **R3.4** Windows: install NSSM service for bridgesessions daemon (run `install-daemon.ps1` on Shadow)
- [x] **R3.5** `mesh_listen_bind_failed` → log actual errno/WSAGetLastError() code
- [x] **R3.6** Add SO_REUSEADDR to connect sockets (not just listen socket)

### R4: Config Hot-Reload
- [x] **R4.1** Reload `authorized_keys` from disk on each inbound TLS accept (before cert verify callback fires)
- [x] **R4.2** Re-read config seeds list on SIGHUP (Linux) so new seeds take effect without restart
- [x] **R4.3** Re-read config seeds on file timestamp change every 30s (Windows — no SIGHUP)

### R5: CLI Robustness
- [x] **R5.1** `health <peer>`: explicit 10s timeout; report "timeout" / "refused" / "auth failed" / "unknown peer"
- [x] **R5.2** `shell <peer>`: validate peer exists in seeds+discovered before attempting connect (no SIGSEGV on unknown)
- [x] **R5.3** `peers list`: query running daemon for live connection state (connected/connecting/failed) — not just config dump
- [x] **R5.4** `stats`: show per-connection detail: peer name, addr, direction, uptime, bytes in/out, last pong latency
- [ ] **R5.5** `sessions`: test across all 3 nodes

### R6: Cross-Platform Build & Deploy
- [x] **R6.1** Create `build.sh` — one-command POSIX build (g++ with correct flags)
- [x] **R6.2** Create `build.ps1` — one-command Windows build (MSVC + vcpkg paths auto-detected)
- [x] **R6.3** Create `deploy.sh` — scp binary + config to linux-a/linux-b, issue systemctl restart
- [x] **R6.4** Version bump: `--version` outputs `1.3.0-reliability`
- [ ] **R6.5** Build + deploy to all 3 nodes, verify version

### R7: Integration Test Harness
- [x] **R7.1** `test_mesh_reliability.ps1`: kill all daemons, start on all 3 nodes via SSH, wait 30s, verify `stats` shows `connections: ≥ 2`
- [x] **R7.2** `test_cross_platform_shell.ps1`: `shell linux-b` → send "echo RELIABILITY_OK\n" → verify OutputMsg contains "RELIABILITY_OK"
- [x] **R7.3** Both scripts must be runnable from Shadow with existing SSH keys; report pass/fail clearly

### R8: Known-Issue Cleanup
- [x] **R8.1** K1: `(void)` cast all 4 `[[nodiscard]]` return values (`resize_pty`, `load_peers_from_file`, etc.)
- [x] **R8.2** K2: Fix `select(0, ...)` — compute actual `maxfd+1` from socket handles
- [x] **R8.3** K3: Store `AuthorizedKeys` instance inside `MeshController` instead of heap-alloc via `new` (eliminates context leak)
- [x] **R8.4** K4: Add header comment on `conns_` documenting single-threaded invariant

---

## Done (v1.0–v1.2)

47 of 47 baseline items done. 10 of 10 v1.2 wave items done.
See plan.md §v1.0 Baseline + §v1.2 Wave for full enumeration.

## Deferred (v2+)

| Feature | Priority | Status |
|---|---|---|
| Multi-hop session routing | High | Code written, not mesh-tested |
| Session recording / replay | Low | Not started |
| Mesh-wide session search | Low | Not started |
| WebRTC transport | Low | Code behind `BS_NO_WEBRTC` |
| DHT for >100 nodes | Low | Code behind `BS_NO_DHT` |
| NAT traversal (UPnP) | Low | Code behind `BS_NO_NAT` |
| Cross-platform test matrix | Medium | Not started |
| CMakeLists.txt unification | Low | Not started |
| Man page / full --help | Low | Not started |