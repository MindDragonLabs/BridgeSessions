# BridgeSessions Audit — 2026-08-20

## Verdict

**Source is release-candidate quality after remediation, with explicit beta limitations.** No confirmed P0/P1 issue remains in the reviewed diff. Release still requires green GitHub CI and target-platform artifact verification.

## Scope

Reviewed the shipping C++ monolith, installers/upgrader, CMake/release builders, Bridge Panel, all tracked Python/shell/PowerShell helpers, GitHub configuration, release artifacts, and every Markdown file.

Audit dimensions:

- PII, secrets, and build-path leakage
- TLS identity, enrollment, IPC, command/path injection
- protocol bounds and decompression
- session/PTY/process lifetime
- event-loop blocking and backpressure
- reconnect, file transfer, and resume behavior
- dependency support/CVEs
- repository and documentation hygiene
- GitHub CI/release policy

Four independent MiniMax M3/Hermes passes covered security, reliability/performance, repository hygiene, and documentation. Model findings were treated as leads and checked against source/tests.

## Material findings and disposition

| Severity | Finding | Disposition |
|---|---|---|
| P0 | Remote file requests could resolve arbitrary absolute paths | fixed: canonical `receive_dir` confinement, symlink/prefix-collision tests |
| P0 | `Session` move assignment wrote into destroyed members | fixed: placement-new reconstruction + regression test |
| P0 | Script alias/path traversal and raw argv shell injection | fixed: strict aliases/hashes, canonical target checks, per-argument quoting |
| P1 | Blocking file ACK/control writes on the single event loop | fixed: bounded non-blocking per-connection TX queues |
| P1 | Fresh receive performed a full-file hash on event-loop completion | fixed: incremental SHA-256; resumed transfers retain final re-hash |
| P1 | Worker PTY input and controller output could block indefinitely | fixed: non-blocking input queue, high-water disconnect, send deadline |
| P1 | Worker PTY/session ownership could double-close/reap incorrectly | fixed: explicit ownership, process-group kill, bounded wait |
| P1 | Valid partial transfers were deleted on connection loss | fixed: `.part`/sidecar survive transport loss; invalid data is removed |
| P1 | Unbounded detached auto-upgrade threads and unsafe peer interpolation | fixed: shell-safe names and fixed worker pool |
| P1 | Unsigned/unverified installer replacement | fixed: GitHub Release SHA-256 + embedded-version verification before swap |
| P1 | Reconnect jitter used unsigned subtraction and could underflow | fixed: signed jitter calculation |
| P1 | Linux CI treated zombies as live process-group survivors | fixed: Linux `/proc` state-aware test |
| P2 | Static release dependencies were stale/EOL/vulnerable | fixed: OpenSSL 3.5.7 LTS, spdlog 1.17, fmt 12.2, current headers/tests |
| P2 | Daemon-mediated edit/vfolder paths blocked the event loop | fixed: CLI-owned dedicated direct-TLS operations; obsolete IPC paths removed |
| P2 | Repo contained dead prototypes, wrappers, services, binaries, and per-tag prose | fixed: removed; history remains in git |
| P2 | Public tests/docs exposed private fleet identifiers and local paths | fixed: sanitized fixtures, external private blocklist, clean scans |

## Security properties verified

- inbound and outbound Ed25519 pin binding,
- enrollment issuer restricted to configured seed keys,
- bounded invite window and single-use token flow,
- canonical file containment including symlinks and sibling-prefix paths,
- sensitive identity/token/config path denial,
- spectator CUA denial and HID range validation,
- bounded u16/u32 frames and decompressed payloads,
- bounded TLS RX/TX, IPC line, PTY input, output, and worker queues,
- mandatory installer checksum/version ordering,
- no tracked secret shape, CGNAT address, private fleet name, or personal home path.

## Verification evidence

| Gate | Result |
|---|---|
| macOS RelWithDebInfo build | pass |
| CTest | **474/474 pass** |
| ASan + UBSan CTest | **474/474 pass** |
| targeted security/reliability regressions | pass |
| Python release/installer/BridgePanel | **84 pass, 2 skip** |
| Ruff | pass |
| tracked Python compile + shell syntax | pass |
| Gitleaks history | pass, 304 commits |
| Gitleaks current tracked tree | pass |
| prepublish PII/secret scan | pass (review-only keyword warnings remain) |
| tracked-file PII regex sweep | pass, zero hits |
| MkDocs strict build | pass |
| aggressive AppleClang warning build | builds successfully; 89 conversion/unused warnings remain as P3 debt |
| Linux static build (GCC 13, OpenSSL 3.5.7) | see final release evidence / GitHub CI |

## Repository outcome

- Dead modular prototype trees and orphaned tests removed.
- Generated `dist/`, app bundles, binaries, SBOMs, and checksums removed from git and ignored.
- Obsolete root installers/build wrappers, service templates, unrelated setup tools, and legacy forge release scripts removed.
- Remaining executable helpers are classified as production/release automation, E2E harnesses, or Bridge Panel source/tests.
- Markdown consolidated from 48 files / ~5,700 lines to 22 files / ~1,500 lines. Release-note, RCA, and historical audit duplication was folded into `CHANGELOG.md` and this report.
- GitHub Security workflow adds CodeQL and Gitleaks; Dependabot covers Actions/Docker metadata.

## Accepted beta limitations

1. `select()` retains the platform FD ceiling; BridgeSessions targets small trusted meshes.
2. Peer authorization is host-level, not capability-scoped.
3. TLS is capped at 1.2 for current mixed-platform compatibility; keys remain explicitly pinned.
4. A resumed transfer performs one final streaming full-file hash; a fresh transfer does not.
5. Session destruction is bounded but can briefly wait while registry work is in progress.
6. The warning-cleanliness backlog is non-zero; current warnings are dominated by explicit socket/numeric conversions and disabled-feature parameters.
7. Windows/macOS CUA remains dependent on a correctly signed user-session helper and OS permissions.

## Release gate

Before publishing a tag/release:

1. commit and push the source-only tree,
2. observe Build/Test, Security, and Docs workflows to success,
3. build all three artifacts from that exact commit,
4. verify target format, hardening, version, signatures, and no embedded PII,
5. generate SHA256SUMS/SBOM,
6. publish through `scripts/github-release.sh`,
7. read back remote asset size/digest and exact tag/HEAD.
