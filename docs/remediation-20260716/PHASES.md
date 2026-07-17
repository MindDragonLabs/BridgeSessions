# Remediation phases (ordered)

Maps independent review “Recommended repair sequence” + fleet operational work.

## Phase 1 — Security blockers (must complete first)

| # | Work | TODO file | Gate |
|---|------|-----------|------|
| 1.1 | Centralize outbound peer verification | TODO-P0-SECURITY P0-1 | Attacker e2e green |
| 1.2 | Remove accept-all TOFU on mesh | P0-1 | No always-true tofu_cb |
| 1.3 | Require pinned seeds | P0-1 | Fail closed without pubkey |
| 1.4 | Bind Hello ↔ cert key | P0-1 | Unit + e2e |
| 1.5 | TLS version policy | P0-2 | Docs == binary |
| 1.6 | File transfer containment | P0-3 | Traversal suite |
| 1.7 | Attacker-oriented regressions | P0-1/3 | CTest in CI |

**Exit:** Public materials may say “preview with pinned mesh” only after 1.1–1.7.

## Phase 2 — One buildable product

| # | Work | TODO file |
|---|------|-----------|
| 2.1 | Monolith vs modular decision + archive | TODO-P1-ENGINEERING P1-1 |
| 2.2 | CMake/docs/CI match | P1-2 |
| 2.3 | Sanitizers/fuzz real or unclaim | P1-2 |
| 2.4 | `--config-dir` isolation | P1-3 |

**Exit:** Fresh clone builds one product on 3 OS in CI.

## Phase 3 — Release hardening + ops

| # | Work | TODO file |
|---|------|-----------|
| 3.1 | BridgePanel body limit + write auth | P1-4 |
| 3.2 | Metadata reconcile | TODO-P2-RELEASE P2-1 |
| 3.3 | Checksums / SBOM / signatures | P2-2 |
| 3.4 | Tag PowerShell/oneshot fleet release | TODO-P1-OPERATIONAL OP-1 |
| 3.5 | Peer naming gates | OP-2 |

**Exit:** Signed release with verify instructions; fleet matrix published.

## Parallelizable without waiting on Phase 1
- BridgePanel body-size check (P1-4a) — low risk
- Metadata inventory (P2-1a)
- PowerShell e2e tests once v2.0.2 tagged (OP-1)
- Doc-only TLS claim inventory (P0-2c if choosing Option B)

## Do not parallelize with Phase 1
- Modular migration (P1-1 Option B) while rewriting TLS/auth in monolith
- Public “secure SSH replacement” marketing
