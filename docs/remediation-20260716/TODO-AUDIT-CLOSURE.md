# Audit closure backlog — 2.0.5-alpha2

**Goal:** every finding from the 2026-07-16 independent review and follow-on fleet RCAs is either **fixed with evidence** or **explicitly deferred with owner/date**.  
**Release target:** **v2.0.5-alpha2** (Linux + Windows + macOS artifacts from this source).

Legend: `[x]` done with test/evidence · `[ ]` open · `[~]` partial · `[D]` deferred (documented)

---

## P0 — Security

| ID | Item | Status | Evidence |
|----|------|--------|----------|
| P0-1a | Central `verify_outbound_peer_identity` | [x] | unit `[security]` |
| P0-1b | Mesh outbound pin↔cert↔Hello before merge | [x] | connect_to_peer_impl |
| P0-1c | `mesh.require_seed_pins` default true | [x] | config + dial skip |
| P0-1d | Inbound Hello↔cert bind | [x] | accept_inbound |
| P0-1e | Remove “Accept all for now” identity meaning | [x] | provisional self-signed only |
| P0-1f | Attacker MITM/forged Hello unit cases | [x] | `[attacker]` in test_config |
| P0-1g | TOFU store for interactive first contact | [D] | pins cover fleet; optional later |
| P0-2a | TLS policy coherent (1.2+ prefer 1.3) | [x] | min/max set in shipping binary |
| P0-2b | Docs claim match binary (no false 1.3-only) | [x] | README/protocol/design/why + man pages |
| P0-2c | Modular tree non-shipping label | [x] | LEGACY_CODE.md + export-ignore |
| P0-3a | sanitize_transfer_filename | [x] | unit |
| P0-3b | path_is_inside_directory | [x] | unit |
| P0-3c | transfer.max_bytes | [x] | default large for AI assets |
| P0-3d | Streaming checksum (no full-file RAM) | [x] | wait + send + edit_dl + file_request |
| P0-3e | Checksum fail quarantine | [x] | hash before rename; delete .part on fail |
| P0-3f | Large-file timeout not fixed 120s | [x] | size-aware + idle |

---

## P1 — Engineering

| ID | Item | Status | Notes |
|----|------|--------|-------|
| P1-1 | One canonical implementation | [x]/[D] | Monolith ships; modular retained as non-shipping history (not dual-stack) |
| P1-2 | CMake/docs/CI match shipping root | [x] | VERSION drives CMake + CLI |
| P1-3 | `--config-dir` isolation everywhere | [x] | AppPaths + isolation tests |
| P1-4 | BridgePanel body limit + write auth | [x] | 10 MiB + token on all writes |

---

## P2 — Release

| ID | Item | Status |
|----|------|--------|
| P2-1 | Metadata/version/port drift checker | [x] | VERSION + release-checksums.sh |
| P2-2 | SHA256SUMS / SBOM / signed tags | [~] | checksums+SBOM ship; signing is operator step |
| P2-3 | Multi-platform artifacts from same source | [x] | Linux x86_64, Windows x86_64, macOS arm64 for 2.0.5-alpha2 |

---

## Operational / AI product

| ID | Item | Status |
|----|------|--------|
| OP-T1 | 500MB+ file transfer reliable | [x] | soak evidence in AUDIT doc |
| OP-T2 | Progress every ~10s | [x] | PROGRESS lines |
| OP-T3 | Streaming SHA-256 send & recv | [x] | |
| OP-T4 | IPC wait timeout size-aware | [x] | |
| OP-C1 | Command stacking docs | [x] | skill + AGENTS.md |
| OP-W1–W3 | Windows peer reminders | [x] | skill + remediation TODO-AI-WINDOWS-OPS |

---

## Alpha2 definition of done

1. Every non-deferred P0 row is `[x]`.
2. Shipping binary reports `2.0.5-alpha2` on all three platform artifacts.
3. `scripts/release-checksums.sh` validates all dist artifacts against `VERSION`.
4. CTest green on Linux build host.
5. Docs do not claim production-secure SSH replacement.

Until signed tags + broader fleet soak: still **public alpha**, not production SSH replacement.

Canonical audit narrative: [`docs/AUDIT-2.0.5-alpha2.md`](../AUDIT-2.0.5-alpha2.md).
