# Release provenance

## Verify a binary

```bash
cd dist
sha256sum -c SHA256SUMS
# Linux:
./bridgesessions-linux-x86_64 --version   # → matches VERSION file
# Windows/macOS: compare embedded version via strings or run on-target
```

Regenerate source archive, checksums, and SBOM from a clean tree:

```bash
./scripts/package-release.sh --release
./scripts/release-checksums.sh
```

## What we ship

| Artifact | Platform | Build notes |
|----------|----------|-------------|
| `bridgesessions-linux-x86_64` | Linux x86_64 | CMake RelWithDebInfo / shipping root |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 | MinGW static cross-compile (OpenSSL+zstd) |
| `bridgesessions-macos-arm64` | macOS arm64 | clang++ on Apple Silicon; Homebrew OpenSSL/zstd/fmt/spdlog dylibs |
| `bridgesessions-<VERSION>-source.tar.gz` | Source archive | Deterministic `scripts/package-release.sh` |
| `bridgesessions-<VERSION>-source.zip` | Source archive | Deterministic `git archive` ZIP |
| `SHA256SUMS` | Checksums | SHA-256 of every release artifact, including the SBOM |
| `SBOM-binaries.json` | SBOM | CycloneDX 1.5 of release artifacts |

Do not copy forward binaries from earlier tags. Every artifact must embed the
contents of [`VERSION`](../VERSION) and pass `scripts/release-checksums.sh`.

Full dependency SBOMs for system packages (OpenSSL, etc.) are host-specific;
document the build OS in forge release notes if you republish.

Release-bundle hashes live in the generated `dist/SHA256SUMS`. The checksum,
SBOM, and source archives are generated after the exact tag and are
intentionally not tracked in that tag: committing them would change the tag's
Git object and therefore change the exact-object source archive they describe.

## Platform verification

| Platform | How verified |
|----------|----------------|
| Linux | `./binary --version`, full CTest, ASan suite |
| Windows | MinGW x86_64 build; all 18 test executables run natively on Windows 11 24H2 (252 cases / 1,430 assertions), including ConPTY queue and resize coverage; binary reports `2.0.6` |
| macOS | Native Apple Silicon build + full CTest |

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
python3 -m pytest tests/test_release.py -q
./scripts/package-release.sh --release
./scripts/release-checksums.sh
```
