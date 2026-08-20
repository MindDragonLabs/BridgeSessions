# Release provenance

Generated executables, app bundles, archives, checksums, and SBOMs are release artifacts. They are ignored by git and attached to a GitHub Release whose tag points to the reviewed source commit.

## Required assets

| Asset | Platform |
|---|---|
| `bridgesessions-linux-x86_64` | Linux x86_64 |
| `bridgesessions-macos-arm64` | macOS arm64 |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 |
| `SHA256SUMS` | checksum manifest |
| `SBOM-binaries.json` | CycloneDX artifact/dependency record |

Optional source archives are generated from the exact tag with `git archive`; GitHub also publishes automatic source archives.

## Build requirements

- clean source tree,
- `VERSION` matches the intended tag,
- supported dependency lines (OpenSSL 3.5 LTS; spdlog newer than 1.15.1),
- release hardening flags enabled,
- no operator paths, private hosts, credentials, or debug symbols in public artifacts,
- macOS Developer ID signature and notarization,
- Windows PE ASLR/NX validation,
- Linux PIE/RELRO/NX/stack-protector validation.

## Stage locally

```bash
scripts/package-release.sh --release
scripts/release-checksums.sh
```

`dist/` is local staging and is ignored by git.

## Verify

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

Run each binary on its target OS and compare `--version` with `VERSION`. Run the affected E2E layers from [E2E Framework](E2E-FRAMEWORK.md).

## Publish

1. Commit the source-only tree.
2. Push `main` and wait for Build/Test and Security workflows.
3. Create or move the annotated release tag only after green CI.
4. Push the tag.
5. Run `scripts/github-release.sh` to create the prerelease and upload verified assets.
6. Read the release back with `gh release view` and compare every remote digest/size with local files.

The release script refuses a dirty tree, tag/HEAD mismatch, origin-tag mismatch, missing asset, or implicit replacement of an existing release.

## Dependency evidence

Record exact compiler, OS image, and dependency versions in the GitHub Release notes or SBOM. Do not copy binaries or checksum files forward from an older tag.
