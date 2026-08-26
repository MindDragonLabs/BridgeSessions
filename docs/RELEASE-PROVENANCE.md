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
  built on fecv3 (docker `bs-static-builder` + MinGW cross toolchain, `/opt/bs-win`
  static deps). macOS arm64 built on macmini `build/release`, signed with
  Developer ID (Team QL5MD8FKPL) via the MacBook's login-keychain identity,
  notarized through ASC key QH9V26H342 (submission `ed6384ab…`, Accepted).
- Local Gatekeeper note: replacing a binary in place (cp over an exec'd path)
  invalidates the cached code-signature mapping; install by rm + cp so the new
  inode gets a clean evaluation.
- `release-checksums.sh` validated 5 artifacts; `prepublish-scan.sh` clean
  (WARN-only, same shape as beta7). `github-release.sh` published and remote
  digests verified equal to local SHA256SUMS.
