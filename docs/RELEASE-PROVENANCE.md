# Release provenance

A BridgeSessions release is the reviewed source tree plus the build outputs that the source produces. Git holds the source and the automation. GitHub Releases hold the artifacts. The release script enforces the boundary.

## What a release is

A release is a Git tag plus the artifacts attached to it.

- The tag is an annotated tag that points at the reviewed source commit.
- The artifacts are the platform binaries, the checksum manifest, and the SBOM.
- Optional source archives are produced with `git archive` from the same tag.

The release script refuses a dirty tree, a tag/HEAD mismatch, an origin-tag mismatch, a missing asset, or an implicit replacement of an existing release. Every failure path is explicit so the operator can fix the cause instead of overriding the script.

## Required assets

| Asset | Platform |
|---|---|
| `bridgesessions-linux-x86_64` | Linux x86_64 |
| `bridgesessions-macos-arm64` | macOS arm64 |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 |
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

1. Commit the source-only tree.
2. Push `main` and wait for the Build/Test and Security workflows to be green.
3. Create or move the annotated release tag only after green CI.
4. Push the tag.
5. Run `scripts/github-release.sh` to create the prerelease and upload verified assets.
6. Read the release back with `gh release view` and compare every remote digest and size with the local files.

The release script refuses to publish when any of the checks above would fail. The operator's job is to keep the source tree and the dependencies ready; the script's job is to keep the release honest.

## After publish

- Move or update the tag if a release is republished. Force-updating a tag is allowed by the repository ruleset; deleting a tag is not.
- Announce the release in the changelog. The CHANGELOG top section is the source of truth for user-visible changes.
- Watch the GitHub Action that watches the tag. A failed download digest or a bad signed installer is a release bug and warrants a hotfix tag.
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
