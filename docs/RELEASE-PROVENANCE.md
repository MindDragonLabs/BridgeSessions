# Release provenance

A BridgeSessions release is the reviewed source tree plus the build outputs that the source produces. Git holds the source and the automation. GitHub Releases hold the artifacts. The release script enforces the boundary.

## What a release is

A release is a Git tag plus the artifacts attached to it.

- The tag is an annotated tag that points at the reviewed source commit.
- The artifacts are the platform binaries, the checksum manifest, and the SBOM.
- Optional source archives are produced with `git archive` from the same tag.

The release script refuses a dirty tree, a tag/HEAD mismatch, an origin-tag mismatch, a missing asset, changed staged checksums, invalid artifact formats or versions, or an implicit replacement of an existing release. Every failure path is explicit so the operator can fix the cause instead of overriding the script.

## Required assets

| Asset | Platform |
|---|---|
| `bridgesessions-linux-x86_64` | Linux x86_64 |
| `bridgesessions-macos-arm64` | macOS arm64 |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 |
| `bs_tray.ps1` | verified Windows tray companion |
| `bridgesessions-VERSION-source.tar.gz` / `.zip` | source archives from the tagged commit |
| `SHA256SUMS` | checksum manifest (basename-keyed) |
| `SBOM-binaries.json` | CycloneDX artifact and dependency record |

The `VERSION` file at the repo root stamps the build. CMake reads it and bakes it into the binary. The release notes, the binary, and the installer all agree on the stamp.

## Build requirements

A release build must satisfy all of the following:

- clean source tree,
- `VERSION` matches the intended tag,
- supported dependency lines: OpenSSL 3.5 LTS, spdlog newer than 1.15.1, fmt 12.2, Catch2 3.15, CLI11 2.7, nlohmann-json 3.12,
- release hardening flags enabled,
- no operator paths, private hosts, credentials, or debug symbols in public artifacts,
- macOS Developer ID signature and notarization,
- Windows PE with ASLR and NX linker flags validated,
- Linux PIE, RELRO/NX, stack-protector, and fortified libc validated.

The `scripts/prepublish-scan.sh` script blocks private addresses, key material, and operator names from a local blocklist before tagging. The blocklist covers home paths, Tailscale CGNAT ranges, common operator hostnames, and 64-hex patterns that are not placeholders. A release that fails the scan does not publish. A companion private allowlist (`~/.config/bridgesessions/publish-allowlist`, outside the repo) exempts legitimate baked-in strings — currently the Developer ID certificate subject embedded in codesigned macOS binaries, which this gate requires. Blocklist matching is case-sensitive (hostnames are lowercase).

## Stage locally

```bash
scripts/package-release.sh --release
scripts/release-checksums.sh
```

`dist/` is local staging. Git ignores it. The script writes the platform binaries, the checksum manifest, and the SBOM into `dist/`. Publishing is a separate step.

## Verify before publishing

```bash
# Source and tests
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
bash scripts/prepublish-scan.sh

gitleaks dir . --redact --no-banner

# Assets
(cd dist && shasum -a 256 -c SHA256SUMS)
file dist/bridgesessions-*
strings dist/bridgesessions-linux-x86_64 | grep -E '^/home/|^/Users/' && exit 1 || true
```

Run each binary on its target OS and compare `--version` with `VERSION`. Run the affected E2E layers from the [E2E Framework](E2E-FRAMEWORK.md). The release gate fails the build when any required layer does not pass.

## macOS signing and notarization

`scripts/sign-macos.sh` signs a Mach-O with the first Developer ID Application identity in the keychain. The script refuses ad-hoc signing.

```bash
./scripts/sign-macos.sh build/bridgesessions dist/bridgesessions-macos-arm64
codesign --verify --strict --verbose=2 dist/bridgesessions-macos-arm64
```

Set `BS_DEV_ID` if more than one identity exists.

If the build Mac has no Developer ID certificate, build there and sign on a Mac that does. Copy only the unsigned binary and the entitlements. Do not email an unprotected `.p12`.

The release workflow imports `MACOS_SIGNING_P12_B64` and
`MACOS_SIGNING_P12_PASSWORD`, then requires `APP_STORE_CONNECT_KEY_ID`,
`APP_STORE_CONNECT_ISSUER`, and `APP_STORE_CONNECT_KEY_P8` to sign and notarize
the macOS artifact. Missing credentials fail the macOS job and prevent the
publish job from running. Store the App Store Connect private key as a protected
GitHub Actions secret; do not commit it or place it in a release artifact.

`scripts/notarize-macos.sh` uploads the signed bundle to Apple's notary service and staples the ticket. The release cannot be uploaded without a valid notarization record.

Re-signing the release artifact on a machine that does not have the Developer ID certificate strips the seal and can make Gatekeeper kill the process at launch. The installer does not re-sign.

## Linux hardening

`scripts/Dockerfile.static-linux` produces a static binary with OpenSSL 3 LTS and static third-party libraries. The binary may still link glibc depending on the build options. Linux release hardening validates:

- Position Independent Executable,
- RELRO with immediate bindings,
- NX stack,
- Stack protector,
- Fortified libc.

## Windows hardening

`scripts/build-windows-mingw.sh` produces a MinGW cross-build. Imports must be OS DLLs only. Windows release hardening validates:

- ASLR and NX linker flags,
- Control Flow Guard when the toolchain supports it.

The installer uses a verified temporary PE and atomic move. The installer verifies the GitHub Release `SHA256SUMS` entry before it replaces the running binary.

## Dependency evidence

Record the exact compiler, OS image, and dependency versions in the GitHub Release notes or in the SBOM. Do not copy binaries or checksum files forward from an older tag. Every tag is a fresh build from the source commit the tag points at.

## Publish

1. Commit the reviewed source and verify its tests and privacy scan. Use a
   candidate branch/PR for CI; keep installer defaults on `main` pointed at an
   already published release.
2. After operator authorization and green platform gates, create the annotated
   release tag on that exact source commit and push the tag. The release
   workflow builds, validates, and publishes from the verified tag commit.
3. For the alternative local publishing lane, build/sign all platform artifacts,
   run `scripts/package-release.sh --release` and `scripts/release-checksums.sh`,
   then run `scripts/github-release.sh`. It verifies the existing manifest and
   revalidates payloads before upload. Suffixed versions are prereleases;
   plain versions become Latest.
4. Read the release back with `gh release view` and compare every remote digest
   and size with the local files; verify a fresh download.
5. Promote the reviewed installer defaults only after the matching release
   exists. A fleet rollout follows publication and needs its own live upgrade
   and session acceptance evidence.

The release script refuses to publish when any of the checks above would fail. The operator's job is to keep the source tree and the dependencies ready; the script's job is to keep the release honest.

## After publish

- Keep a published tag and its assets immutable. Ship corrections under a new
  version so existing checksum manifests and installed versions remain useful.
- Announce the release in the changelog. The CHANGELOG top section is the source of truth for user-visible changes.
- Watch the GitHub Action that watches the tag. A failed download digest or a bad signed installer is a release bug and warrants a hotfix tag.

## 26.10.05 candidate

Unix CI and release jobs run native two-daemon session acceptance before the
full suite. The suite emits periodic process diagnostics and has a 25-minute
command deadline; CTest defaults otherwise unbounded individual tests to
120 seconds while preserving explicit per-test budgets. This makes a stalled
runner or test visible and prevents an indefinite release gate.

Prepared locally on 2026-10-10 from source base `d33e40b`, with release
changes committed on the `release/26.10.05` branch. This is an unpublished
candidate, not a deployed release. Read-only `gh release list` confirmed that
`v26.09.28` remains Latest; the earlier `v26.10.04` tag has no published release.

The review corrected legacy panel scope bypasses, inconsistent trusted-proxy
HTTPS detection, unvalidated local uploads, omitted panel tests, TLS fixture
file collisions, ignored Windows CMake options, and native Windows linking.
Regression tests reproduced the scope, proxy, and parallel TLS failures before
their fixes. CI now builds the native platform shells, and the release workflow
executes the exact uploaded Windows binary on Windows before publication.

The first candidate CI run exposed an AppKit property-setter collision in the
native macOS shell. The status-text method now has a distinct selector; an
Apple compiler syntax check on a live Mac passed. Fleet acceptance also now
requires authenticated session input and executed markers in each session's
own scrollback, rejects empty captures, and tests input/readback after the full
ten-second idle interval. Sixteen behavioral regression cases cover these
checks and the IPC protocol's canonical, unpadded Base64 encoding.
Both CI and release validation scan and execute the Windows artifact with
Defender antivirus and real-time protection enabled.

The runtime gate exposed GitHub's disabled-Defender runner defaults. A separate
preparation step, restricted to disposable GitHub-hosted runners, enables
protection, removes scan exclusions, and updates signatures. The validator
still rejects disabled protection or scan exclusions; fleet security settings
are not modified by this preparation.

The rollout review also reproduced an unmatched quote in Linux's detached
upgrade command. The complete child command is now quoted as one shell
argument, tags are validated before command construction, and behavioral tests
execute a harmless updater with spaces, apostrophes, and substitution syntax
in its paths. They check literal arguments, unset mesh markers, closed stdin,
and absence of injected commands; all 22 focused upgrade cases passed.

Native Windows Server 2022 diagnostics reproduced redirected standard handles
bypassing ConPTY: CMD output reached the parent stream and a daemon with closed
input lost its interactive shell. Explicit null standard handles with
`STARTF_USESTDHANDLES` kept CMD alive and routed executed output through ConPTY,
matching [Microsoft's documented inheritance issue](https://github.com/microsoft/terminal/discussions/15814).
Windows restarts also transfer the replacement's kill-on-close job handle;
temporary session cleanup previously closed that job and killed the child.
CI and release validation now require real CMD and PowerShell session
input/readback, isolation, idle survival, and execution of the native restart
regression. The restart fixture cross-compiles successfully; final Windows
runtime checks remain a publication gate.

Release checksum scripts accept native macOS `shasum -a 256` when GNU
`sha256sum` is absent. All eight integrity cases passed on a live Mac, and all
25 focused integrity/publisher cases passed on Linux.

| Local validation | Result |
|---|---|
| Release build, native client enabled; `ctest --test-dir build --parallel 8 --output-on-failure` | 650/650 passed after the detached-upgrade regressions, without retrying failures |
| `python3 -m pytest tests tools/bridgepanel -q` | 248 passed, 30 subtests passed; one macOS-only `otool` check skipped on Linux |
| Session acceptance, `ctest --test-dir build -R '^panel_session_acceptance$' --repeat until-fail:10 --output-on-failure` | 10 consecutive passes |
| Parallel TLS regressions, `ctest --test-dir build -R 'R[12]' --parallel 8 --repeat until-fail:10 --output-on-failure` | All five cases passed ten times; two cases aborted before unique temporary filenames were introduced |
| Native client fixture with AddressSanitizer and UndefinedBehaviorSanitizer | Passed |
| Standard-library-only Python | All eight panel suites passed (167 tests); CMake registered these suites and real-PTY acceptance without pytest |
| Ubuntu 22.04 container build with fetched, pinned dependencies | Built Linux daemon and native shell; staged daemon reports `26.10.05`, and version/help run in a fresh Ubuntu 22.04 container |
| MinGW build with `--extra -DBS_BUILD_NATIVE=ON` | Daemon, native core, smoke CLI, and Win32 shell built; PE imports are OS DLLs only, with ASLR/NX flags |
| Release scripts and workflow syntax | All 22 tracked shell scripts parse; changed release scripts pass ShellCheck; both workflow YAML files parse and have valid job dependencies |
| Privacy gates | `bash scripts/prepublish-scan.sh` passed; the three staged candidate payloads passed the same build-path and private-blocklist checks |

Linux hardening checks found PIE, RELRO with immediate binding, an NX stack,
stack protection, and fortified libc calls. The largest required glibc symbol
version is `GLIBC_2.34`; execution on the supported Ubuntu 22.04 floor passed.
The staged portable binary also passed the two-daemon session acceptance
harness and all 11 real-PTY menu/session cases.
Ruff found no new findings in the changed Python files. Existing repository
lint debt is retained rather than folded into this release.

Local Linux and Windows artifacts, the verified tray companion, `SHA256SUMS`,
and `SBOM-binaries.json` are staged under ignored `dist/26.10.05-candidate/`.
The checksum validator accepted all three payloads. Source archives must be
generated from the eventual tagged source commit.
Detailed local test logs are kept outside git under
`release/26.10.05/evidence/`. The same local release folder contains source
patch snapshots and `cleanup-manifest.json`, which records every archived path
and its original location.

Local folder cleanup moved historical build trees, scratch/audit material, and
older mixed-version `dist/` files into the sibling archive
`../bridgesessions-local-archive/2026-10-10-26.10.05/`. The active root `build/`,
portable Linux and Windows build trees, MinGW dependency prefix, and native
sanitizer build remain in place. Git worktrees and runtime state were preserved.
The candidate staging folder contains only the verified `26.10.05` payloads
and their checksum/SBOM records; older binaries must not be copied into it.

Publication still requires the final committed source and green platform CI,
a fresh macOS build with Developer ID signing/notarization, and Windows runtime
and Defender acceptance of the new PE. The previous Defender quarantine is
historical evidence, not proof that this new binary passes. Real Windows/Mac
session input, readback, isolation, and in-band upgrades have not been run for
this candidate. The Windows runtime gate first rejected an unprotected GitHub
runner; the corrected disposable-runner preparation subsequently passed a real
Defender scan and execution of that candidate. The final updated Windows PE
must pass the protected runtime and interactive-session gates before publication.

Keep candidate installer defaults off `main` until matching assets exist.
The earlier audit's invite seed-binding limit remains documented in `README.md`
and `AUDIT.md`; this release does not claim to close it. No tag was created,
GitHub write performed, fleet rolled, or security setting relaxed during local
preparation.

## Release record — v26.08.26-r1 (2026-08-26)

- Tag `v26.08.26-r1` → commit `bbe7410` (annotated, pushed before CI green:
  GitHub Actions was in a partial outage; run list for the commit stayed empty).
- Built from tagged source only. Linux x86_64 + Windows x86_64 + source archives
  built on the Linux build host (docker `bs-static-builder` + MinGW cross
  toolchain, `/opt/bs-win` static deps). macOS arm64 built on the macOS signing
  host `build/release`, signed with Developer ID (Team QL5MD8FKPL) via the
  operator Mac's login-keychain identity, notarized through the App Store
  Connect API key (submission `ed6384ab…`, Accepted).
- Local Gatekeeper note: replacing a binary in place (cp over an exec'd path)
  invalidates the cached code-signature mapping; install by rm + cp so the new
  inode gets a clean evaluation.
- `release-checksums.sh` validated 5 artifacts; `prepublish-scan.sh` clean
  (WARN-only, same shape as beta7). `github-release.sh` published and remote
  digests verified equal to local SHA256SUMS.

## Release record — v26.08.26-r2 (2026-08-26)

- Tag `v26.08.26-r2` → commit `134174d` (same commit as `main` tip; the tag is
  the current shipping release).
- Published as a GitHub **pre-release** on 2026-08-26 23:00:09Z (created
  22:08:46Z). Full changelog: `v26.08.26-r1...v26.08.26-r2`.
- 6 assets: `bridgesessions-26.08.26-r2-source.tar.gz`,
  `bridgesessions-26.08.26-r2-source.zip`, `bridgesessions-linux-x86_64`,
  `bridgesessions-windows-x86_64.exe`, `SBOM-binaries.json`, `SHA256SUMS`.
- Content: session-worker hosting (shells survive daemon restart/upgrade),
  Ctrl-D detach + TUI-mode reset on transport loss, `bs connect` selector fixes,
  self-connect guard, `bs shell -i` interactive path, `scripts/build-local.sh`.
- Build/provenance: same build hosts and toolchain as r1 (Linux x86_64 +
  Windows x86_64 on the `bs-static-builder` Docker + MinGW cross toolchain,
  macOS arm64 on the signing host). `prepublish-scan.sh` clean; fleet hostnames
  scrubbed from provenance/build-local before publish.

## Release record — v26.08.27-r1 (2026-08-27)

- Tag `v26.08.27-r1` → commit `68c20a8`. Published as a GitHub pre-release on
  2026-08-27 16:01:46Z. 7 assets: `bridgesessions-linux-x86_64`,
  `bridgesessions-macos-arm64`, `bridgesessions-windows-x86_64.exe`,
  source tar.gz/zip, `SHA256SUMS`, `SBOM-binaries.json` — every remote digest
  verified equal to the local `SHA256SUMS` (SHA256SUMS compared by direct hash).
- Content: security, privacy, and reliability hardening (see `CHANGELOG.md`
  `## 26.08.27-r1`):
  - **Security:** TLS handshake enforces known peer pins; join-window cert
    acceptance scoped to the mesh listener (per-listener context, not a
    process-global); home/temp transfer destinations moved to an allowlist
    posture with hidden-path denial; BridgePanel requires auth on every write,
    accepts bearer tokens, no longer prints tokenized startup URLs, and uses
    symlink-aware path containment for local inbox resolution.
  - **Privacy:** join token accepted via stdin/`--token-file` (argv form kept but
    deprecated); command secrets redacted from session persistence and logs;
    persisted-session store restricted to 0600; TLS identity logging truncated
    to 12 hex chars; operational logs default to `info` with a sink-level
    redaction hook (`BRIDGESESSIONS_LOG_LEVEL`).
  - **Reliability:** slow session-worker READY responses retried with liveness
    probe before socket unlink (no orphan shells); worker `select`/`FD_SET`
    replaced with `poll`/`WSAPoll`; frame read/write retries are
    cancellation-aware; stale-exec watchdog always timestamped; silent catch
    blocks around persistence/adoption now log/report.
- Verification: full build passes; full test suite **492/492 (100%)** on the
  macOS arm64 signing host. Fleet e2e (`scripts/e2e-fleet-test.sh`) against
  Linux mesh peers 1–5 and a macOS mesh peer: 52 pass / 0 real fail
  (two load-window flakes re-verified passing on direct run); 4 CUA skips on
  headless Linux. Deployed live on the primary macOS build host + 5 Linux peers
  (all healthy on `26.08.27-r1`) before publish.
- v26.08.26-r2 retirement: tag protected by a repo rule (undeletable); release
  page replaced with a tombstone pointing here. The broken r2 packaging
  (missing macos-arm64 asset) is corrected in this release — macos-arm64 is
  included, Developer ID signed (Team QL5MD8FKPL) and notarized (submission
  `06940c32…`, Accepted 2026-08-27).

## Release record — v26.09.01-release (2026-09-01)

- Tag `v26.09.01-release` → commit `3ebe252` (PR #16). Published on 2026-09-01
  as a GitHub **pre-release**. 7 assets: the three platform binaries, source
  tar.gz/zip, `SHA256SUMS`, `SBOM-binaries.json` — every digest verified
  against the local `SHA256SUMS` after upload.
- Content: spawn reliability — fully-timed-out systemd-run worker spawns no
  longer orphan the scope; deterministic unit names, sanitized to systemd's
  charset, stopped on every never-adopted failure path (see `CHANGELOG.md`
  `## 26.09.01-release`). Covered by `tests/test_unit_name_sanitization.cpp`.
- First release produced end-to-end by the gated one-command pipeline
  (`builder/release.sh`, run `rel-20260901-172418`): parity gate → build
  dispatch (linux/windows local docker+mingw, macOS on a fleet build host) →
  assemble →
  PR → blocking CI + Greptile → squash merge → tag → publish → re-download
  e2e. Builder gate fix landed mid-run: Greptile clean verdicts are check-runs,
  not review objects (builder `3e62170`).
- Verification: Linux rebuild is deterministic — a second full build produced
  the identical `bridgesessions-linux-x86_64` sha256. The Windows cross-build
  was reproduced hermetically in an ubuntu:24.04 container with a pinned dep
  prefix (OpenSSL 3.5.7, zstd 1.5.7, fmt 12.2.0, spdlog 1.17.0, CLI11 2.7.2,
  nlohmann-json 3.12.0): PE build succeeds, import table is OS DLLs only. PR
  #16 checks all green, including a clean Greptile pass (0 annotations). Full
  test suite 489/492 in the Linux build container; the 3 failures are known
  container-environment flakes, identical on pristine main.
- Fleet e2e: 87 pass / 7 fail / 8 skip of 102. All 7 failures were probed
  individually and classified environmental or harness-quoting issues (transient
  peer health, a PATH quirk, a harness PowerShell `\$`-escape bug, one
  progress-line truncation); manual re-tests pass. No fleet rollout occurred —
  peers remain on prior versions pending a separate rollout decision.

## Release record — v26.09.01-release CI runners (2026-09-01)

Starting with the next release, the three platform release binaries are built
by GitHub-hosted runners (`.github/workflows/release-builds.yml`,
`workflow_dispatch` + `v*` tags only; fork PRs never run it). The Windows job
bootstraps its MinGW static dep prefix with `scripts/ci-win-deps.sh`.
`build_dispatch.py` / `builder/release.sh BS_BUILD_BACKEND=local` remain the
local testing lane.
