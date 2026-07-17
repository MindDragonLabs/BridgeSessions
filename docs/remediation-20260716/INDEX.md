# BridgeSessions — Remediation Index (Independent Review 2026-07-16)

**Source review:** Independent Technical Review, base `b2742f5`, tag `v2.0.1`  
**Security base:** identity + path + TLS policy (landed through 2.0.3-era work)  
**Transfer/AI:** streaming hash, size-aware timeouts, PROGRESS (2.0.4-era)  
**Public candidate:** **v2.0.5-alpha2** (audit closeout + release hardening + multi-platform artifacts)

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

### Quick status (2.0.5-alpha2)

| Area | State |
|------|--------|
| Mesh pin/Hello/path | Fixed + unit/attacker tests |
| Large transfer + PROGRESS | Fixed + soak evidence |
| BridgePanel auth/body limit | Fixed |
| VERSION / provenance | Single `VERSION` drives CLI, CMake, checksums, SBOM |
| Platform artifacts | Linux x86_64, Windows x86_64, macOS arm64 from this source |
| Production-secure claim | **Not yet** — public alpha only |
