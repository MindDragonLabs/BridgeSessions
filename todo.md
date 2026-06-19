# bridgesessions — Reliability Hardening TODO

**Status:** 🟡 **v1.3** — daemon mesh logs show peer connects; **CLI `health` still timeout** (connect path, not pong-only)
**Date:** 2026-06-19

---

## Latest code (`f2eb055` + health commit)

- `close_conn` fixes (CLOSE_WAIT leak)
- `ssl_connect_blocking`, 15s socket timeout, WSAETIMEDOUT → **timeout** (not tls_rejected)
- Inbound firewall **19949** on Shadow
- Production config: `config.shadow.production.example` + `~/.bridgesessions/config` (Shadow/19949/seeds)
- **health:** `wait_for_pong` (Gossip-tolerant), case-insensitive peer names, `bootstrap_identity` on health CLI

## Verified

| Item | Status |
|------|--------|
| Unit/integration tests | **1010/1010** |
| All nodes version | **1.3.0-reliability** |
| Shadow daemon log | historical **`mesh_peer_connected`** linux-b/linux-a |
| linux-b log | outbound **Shadow**, **linux-a** connects (stale ts) |
| CLI `health` (Shadow→Linux, Linux cross) | **timeout** — `connect_and_hello` / TCP (logs: `tls_connect_failed` **10060** Shadow, **ssl_err=2** Linux) |
| `stats` connections | **0** on ephemeral CLI (expected); daemon may still dial |

## Root cause (current)

1. **Ephemeral CLI** opens new TLS to peer; often **cannot complete connect** within 15s (Tailscale latency, concurrent daemon dials, or Windows **10060**).
2. **Not** primarily “wrong Pong handler” — connect fails before ping when timeout.
3. **Tests** use localhost loopback — production Tailscale path untested in CI.

## To reach 100% green

1. **Network:** linux-b `nc`/`curl` to `100.124.169.66:19949` and `203.0.113.11:19949` (both directions).
2. **Code (next):** optional `health` via **Unix socket / named pipe** to running daemon (reuse live `conns_`); or raise `kConnectTimeoutMs` to 30s for CLI only.
3. **R7 PASS criteria:** daemon log `mesh_peer_connected` + Linux `linux-b health linux-a` when (2) network OK.

## Deferred (v2+)

Unchanged.