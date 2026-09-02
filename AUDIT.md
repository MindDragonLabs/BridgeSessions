# BridgeSessions Audit — 2026-09-02

Re-audit of shipping `main` at `6f18581` (`VERSION` = `26.09.01-release`). This pass reviewed the C++ monolith, installers, Bridge Panel, CI, and the 2026-08-20 audit notes against current source. Prior P0/P1 remediations in the [historical section](#bridgesessions-audit--2026-08-20) remain in code. New issues below are current-tree findings.

## Verdict

**Trusted-operator mesh, public-beta quality, with one P1 operator-path defect and several under-documented residuals.** No confirmed authentication bypass when defaults stay on (`mesh.require_seed_pins true`, `transfer.allow_sensitive_paths false`). An authorized peer is still near-interactive host access. CI does not gate TLS tests, Python installer tests, or live E2E.

This tree remediates the P1 installer default (it pointed at GitHub-labeled broken `v26.08.26-r2`) and the invite-token `RAND_bytes` check. Remaining items are documented for follow-up; they are not silent Critical holes.

## Scope

- Shipping C++: `main.cpp`, `bs-protocol.h`, `bs-session.h`, `bs-session-worker.h`, `bs-cua-helper.h`, `macos-capture.mm`
- Install / upgrade: `scripts/install.sh`, `scripts/install.ps1`, `bs upgrade`
- Bridge Panel: `tools/bridgepanel/`
- Tests, `.github/workflows/`, `SECURITY.md`, prior `AUDIT.md`

Threat model used: operator-controlled mesh (matches `SECURITY.md`). Hostile multi-tenant RBAC is out of scope.

## Material findings

| Sev | ID | Finding | Disposition |
|---|---|---|---|
| P1 | A1 | `scripts/install.sh` / `install.ps1` defaulted to tag `26.08.26-r2`. GitHub marks that release **BROKEN / do not use**. `curl \| bash` without `BRIDGESESSIONS_TAG` installed a known-bad binary two lines behind `26.09.01-release`. macOS `CFBundleVersion` was also frozen at that tag. | **fixed in this tree**: default tag follows `VERSION`; plist uses `${TAG}`; regression test added |
| P2 | A2 | Invite token generation called `RAND_bytes` without checking the return. IPC token generation already failed closed. A CSPRNG failure could mint a weak join token. | **fixed in this tree** |
| P2 | A3 | Version stamps drifted: `AGENTS.md` / skill metadata / `SECURITY.md` still said `26.08.27-r1` while `VERSION` and README said `26.09.01-release`. | **fixed in this tree** |
| P2 | A4 | README and `docs/bridge-panel.md` claimed trusted IPs may skip the token on **writes**. `server.py` sets `require_token=True` on POST; only GET can bypass. | **docs fixed in this tree** |
| P2 | A5 | Join invite TTL is 2 hours (`kInviteTtl`), but the TLS join window that accepts unknown certs is `mesh.join_window_max_secs` (default 300s). After the window closes, a still-unexpired token cannot be redeemed by an unknown cert. Operators who treat the token as a 2-hour capability will fail closed. | open (behavior is conservative; docs should say 300s) |
| P2 | A6 | During an open join window, inbound TLS verify accepts **any** unknown cert so the peer can present a token. A network attacker who **answers** the joiner's TCP session obtains the single-use token and can redeem it at the real seed. Joiner does bind `JoinReply.host_pubkey` to the TLS cert (blocks pin injection). Token is not bound to the intended seed pubkey. | open (design residual; bind token to seed pin) |
| P2 | A7 | Outbound TLS verify callback is accept-all; pin checks run in `connect_and_hello()` / `verify_outbound_peer_identity()`. Empty pin without TOFU is refused. Future connect paths that skip the app-layer check inherit TOFU. | open (defense in depth) |
| P2 | A8 | `mesh.auto_upgrade` defaults **true**. A reachable newer node can dispatch `bridgesessions upgrade` on older peers (shell-safe name + execution-time pin re-check). Integrity is GitHub HTTPS + `SHA256SUMS`, not a detached signature. | documented beta; keep default visible in config.example |
| P3 | A9 | `read_frame` accepts `FLAG_LENGTH_U32` without requiring Hello `+frm2`. Encode path gates large frames. Payload still capped at 4 MiB. | open (DoS/compat footgun, not unbounded alloc) |
| P3 | A10 | Linux in-process CUA (`xdotool`) does not reject `hid_key > 0xFF`; Windows/macOS helpers do. Interpolation is numeric-only (not shell injection). | open |
| P3 | A11 | Session-worker Unix socket is `0600` only; no IPC token / `SO_PEERCRED`. Same-UID local processes can drive the PTY. Matches "trust the host account". | accepted |
| P3 | A12 | Default mesh listen is `0.0.0.0`. Firewall/VPN is operator-owned (`SECURITY.md`). | accepted |
| P3 | A13 | Signed directory enrollments are fresh for 24h wall-clock. Seed-key compromise remains mesh-wide enrollment authority. | documented |
| P3 | A14 | `bs join` may set `listen_addr` from `tailscale ip -4` when present. Wrong interface advertisement skips or mis-gossips auto-enroll. | open |
| I | A15 | CI excludes `test_tls` and builds with `-DBRIDGESESSIONS_PYTHON=OFF`, so installer regressions and TLS tests are not merge gates. No Windows Catch2 job. No ASan/UBSan in GitHub Actions. E2E L2–L4 remain operator-run. | open |
| I | A16 | `bs-protocol.h` is ~834 KB / ~18k LOC. Auth, path confinement, PTY, CUA, and upgrade share one translation unit. Review and blast radius stay high. | structural |
| I | A17 | Panel CSP allows `script-src 'unsafe-inline'`. Markdown renderer HTML-escapes raw markup (tests assert `<script>` stripped). Still a browser XSS budget if preview HTML is ever concatenated unsafely. | accepted with tests |
| I | A18 | 2026-08-20 audit listed `select()` FD ceiling as a product limit. Main mesh loop now uses `poll()` / `WSAPoll`; helper paths still `select()` and reject `fd >= FD_SETSIZE`. | docs updated |

## Security properties re-verified

| Property | Evidence |
|---|---|
| Inbound Ed25519 must be in `authorized_keys`, except join window | `server_cert_verify_cb` (`bs-protocol.h`) |
| Join-window unknown peers stay in `ReadJoinRequest`; other message types drop the handshake | `advance_handshakes` ReadJoinRequest branch |
| Invite tokens single-use (`claimed_by`) | `process_join_request` |
| Only pinned seed keys may issue accepted `DirectoryEnroll` | `apply_directory_enroll` |
| Outbound cert, Hello key, and pin must agree; empty pin refused unless TOFU join | `connect_and_hello` |
| File serve confined to `receive_dir` incl. symlink/prefix; sensitive basenames denied | `file_request_on_transport` + `tests/test_file_path_sanitization.cpp` |
| Spectator CUA / keystroke / signal rejected when the flag is set (client-declared) | `handle_inbound_session` |
| Local daemon IPC loopback + owner-only token + const-time prefix match | `cli_ipc_init`, `const_time_token_match` |
| Frame/zstd payloads capped (u16 / 4 MiB u32) | `read_frame`, `encode` |
| Upgrade/install require `SHA256SUMS` + embedded version before swap | `scripts/install.sh`, `main.cpp` upgrade path |
| CUA text to `xdotool` uses POSIX single-quote escaping | Linux CUA dispatch (prior P0) |

## CI and test posture (this tree)

Catch2 coverage for pins, path containment, enrollment issuer, spectator denial, upgrade tag charset, and systemd unit names is strong. Gaps that matter:

1. `.github/workflows/ci.yml` runs `--exclude-regex "test_tls$"` on Linux and macOS.
2. Same workflow sets `-DBRIDGESESSIONS_PYTHON=OFF`, so `tests/test_regression_install.py` would not have caught A1 in CI.
3. Windows is a MinGW cross-build + Wine `--version` smoke, not Catch2.
4. Release SBOM (`scripts/release-checksums.sh`) is artifact hashes, not a dependency CVE graph.
5. macOS Developer ID signing is in `release-builds.yml`; notarization remains an operator step.
6. Fleet e2e (`scripts/e2e-fleet-test.sh`, `tests/e2e/runner.py`) is required by `docs/E2E-FRAMEWORK.md` for release, not by GitHub Actions.

## Recommended follow-ups (not done here)

1. Put `test_tls` and Python installer/panel tests back in CI, or document a concrete flake reason next to the exclude.
2. Bind invite tokens to the seed pubkey (or print the pin in `bs invite` output and require the joiner to pass it).
3. Align invite TTL with `join_window_max_secs` in both code and operator docs.
4. Default `mesh.auto_upgrade` to false in `config.example`, or require an explicit pin in the example file.
5. Reject `FLAG_LENGTH_U32` unless the peer advertised `+frm2`.
6. HID range-check on the Linux CUA path.
7. Dependency SBOM + OSV/Grype in the release workflow.
8. Split `bs-protocol.h` along TLS / transfer / session / CUA boundaries.

## Verification for this pass

See the walkthrough artifacts attached to the PR for command transcripts. Local gates run on this branch:

- security-tagged Catch2 tests
- path / enroll / join-window / upgrade-validation tests
- Python installer regression including default-tag == `VERSION`

---

# BridgeSessions Audit — 2026-08-20

Historical audit. Retained as provenance. Test counts, FD-ceiling wording, and release hashes below are as of 2026-08-20 and are **not** restated as current.

## Verdict (historical)

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
| Linux static release build | pass: GCC 13, OpenSSL 3.5.7, PIE, RELRO/NOW, NX; SHA-256 `d770c0d1…` |
| macOS arm64 release build | pass: static third-party deps, Developer ID signed, notarization accepted (`2b09a380-d350-42dc-9b85-ff54b12ae551`) |
| Windows x86_64 release build | pass: PE32+, HIGH_ENTROPY_VA, DYNAMIC_BASE, NX_COMPAT; SHA-256 `f20832db…` |

## Repository outcome

- Dead modular prototype trees and orphaned tests removed.
- Generated `dist/`, app bundles, binaries, SBOMs, and checksums removed from git and ignored.
- Obsolete root installers/build wrappers, service templates, unrelated setup tools, and legacy forge release scripts removed.
- Remaining executable helpers are classified as production/release automation, E2E harnesses, or Bridge Panel source/tests.
- Markdown consolidated from 48 files / ~5,700 lines to 22 files / ~1,500 lines. Release-note, RCA, and historical audit duplication was folded into `CHANGELOG.md` and this report.
- GitHub Security workflow adds CodeQL and Gitleaks; Dependabot covers Actions/Docker metadata.

## Accepted beta limitations

1. Helper I/O paths still use `select()` and inherit the platform FD ceiling; the main mesh loop uses `poll()`. BridgeSessions targets small trusted meshes.
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
