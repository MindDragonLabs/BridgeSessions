# Release provenance

## Verify a binary

```bash
cd dist
sha256sum -c SHA256SUMS
# optional: compare to signed release notes / git-tagged source
./bridgesessions-linux-x86_64 --version
```

Build the deterministic source archive, then regenerate checksums/SBOM:

```bash
./scripts/package-release.sh
./scripts/release-checksums.sh
```

## What we ship (2.0.5-alpha2)

| Artifact | Platform | Build notes |
|----------|----------|-------------|
| `bridgesessions-linux-x86_64` | Linux x86_64 | RelWithDebInfo/CMake on test-pc1 |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 | MinGW static cross-compile (OpenSSL+zstd static; FMT header-only) |
| `bridgesessions-macos-arm64` | macOS arm64 | clang++ on Apple Silicon; Homebrew OpenSSL/zstd/fmt/spdlog dylibs |
| `bridgesessions-2.0.5-alpha2-source.tar.gz` | Source archive | Deterministic `scripts/package-release.sh` |

Do not copy forward binaries from earlier tags. Every artifact must embed
`VERSION` = `2.0.5-alpha2` and pass `scripts/release-checksums.sh`.

`SBOM-binaries.json` is a CycloneDX 1.5 manifest of the release artifacts
(SHA-256 + size). Full dependency SBOMs for OpenSSL/zstd system packages are
host-specific; document the build OS in the release notes.

### Platform verification (alpha2)

| Platform | How verified |
|----------|----------------|
| Linux | `./binary --version`, full CTest 237/237, ASan suite |
| Windows | PE32+ x86-64, embedded version string `2.0.5-alpha2`; smoke on Windows peer when available |
| macOS | Mach-O arm64, `./binary --version` on build host |

## Signing (recommended operator step)

```bash
# Example minisign / cosign — keys not stored in this repo
minisign -Sm SHA256SUMS
# or
cosign sign-blob --bundle SHA256SUMS.cosign.bundle SHA256SUMS
```

Tags should be annotated; prefer signed tags when publishing.

## Tests for a release

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/test_config "[security]"
```
