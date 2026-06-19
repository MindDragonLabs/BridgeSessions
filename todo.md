# bridgesessions — Reliability Hardening TODO

**Status:** 🟡 **v1.3 DEPLOYED** — all 3 nodes on `1.3.0-reliability`; **CLI outbound health still `tls_rejected` (ssl_err=5)** while inbound mesh accepts Shadow cert on linux-b (see logs). Needs TLS client/TOFU or duplicate-listener follow-up — not checkbox-complete for R5.5/R7 live PASS.
**Date:** 2026-06-19
**Source:** ~5,458 lines, single-file C++23, `bridgesessions.cpp`

---

## v1.3: Reliability Hardening

### R1–R8 code + scripts
All code-local items **done** (see git `de746ec` + cluster wave below).

### Cluster (2026-06-19)

| Item | Status |
|------|--------|
| **R3.2** | linux-b + FECv3 `bsmesh` **active**, unit on FECv3 refreshed from `etc/bsmesh.service` |
| **R3.4** | NSSM service **bridgesessions** installed on Shadow; `AppEnvironmentExtra` USERPROFILE/HOME → `C:\Users\Shadow` |
| **R6.5** | Built/deployed Linux via scp + g++; **all three** report `1.3.0-reliability` |
| **Shadow config** | Fixed production mesh: `node.name Shadow`, `0.0.0.0:19949`, seeds linux-b/linux-a |
| **R5.5** | **Blocked** — `sessions linux-b/linux-a` fails `tls_rejected` from Shadow CLI |
| **R7.1** | Script runs; **FAIL** on health until outbound TLS fixed; versions PASS |
| **R7.2** | Script fixed (PS syntax); **FAIL** until health healthy |

### Evidence (inbound OK, outbound broken)

- linux-b log: `tls_verify_server` **accept** for `e702d6ad...` (Shadow pubkey) on CLI connect attempt
- Shadow log: recent `tls_verify_server` **accept** for linux-b pubkey `358e0bb8...`
- CLI still reports `tls_rejected` / `ssl_err=5` after server accept → investigate post-handshake (Hello frame, client cert presentation, or second TLS layer)

### Next debug steps

1. Single listener on Shadow:19949 (`nssm` only); kill strays
2. Trace `connect_and_hello` after `SSL_connect` on Windows → linux-b
3. Compare daemon mesh outbound (works?) vs CLI ephemeral `MeshController`

---

## Deferred (v2+)

Unchanged — see previous `todo.md` deferred table.