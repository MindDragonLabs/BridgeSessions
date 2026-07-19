# BridgeSessions — Remediation Index (Independent Review 2026-07-16)

**Source review:** Independent Technical Review, base `b2742f5`, tag `v2.0.1`  
**Security base:** identity + path + TLS policy (landed through 2.0.3-era work)  
**Transfer/AI:** streaming hash, size-aware timeouts, PROGRESS (2.0.4-era)  
**Current candidate:** **v2.0.6** (trust-boundary, event-loop, IPC, PTY, and provenance hardening)

| Doc | Scope |
|------|--------|
| [TODO-AUDIT-CLOSURE.md](./TODO-AUDIT-CLOSURE.md) | **Master checklist** (authoritative status) |
| [TODO-P0-SECURITY.md](./TODO-P0-SECURITY.md) | P0 security detail |
| [TODO-P1-ENGINEERING.md](./TODO-P1-ENGINEERING.md) | Dual stack, CMake, config-dir, BridgePanel |
| [TODO-P1-OPERATIONAL.md](./TODO-P1-OPERATIONAL.md) | Peer naming, oneshot |
| [TODO-P2-RELEASE.md](./TODO-P2-RELEASE.md) | Metadata, provenance |
| [TODO-TRANSFER-AI.md](./TODO-TRANSFER-AI.md) | Large files + AI progress |
| [TODO-AI-WINDOWS-OPS.md](./TODO-AI-WINDOWS-OPS.md) | Command stacking + Windows reminders |
| [PHASES.md](./PHASES.md) | Phase order |

### Quick status (2.0.6)

| Area | State |
|------|--------|
| Mesh pin/Hello/path | Fixed + unit/attacker tests |
| Large transfer + PROGRESS | Worker-isolated + Linux/macOS regression coverage |
| BridgePanel auth/body limit | Fixed |
| VERSION / provenance | Single `VERSION` drives CLI, CMake, checksums, SBOM |
| Platform gates | Linux and macOS native suites pass; Windows native gate still required before release |
| Production-secure claim | **Not yet** — public alpha only |
