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
./scripts/package-release.sh
./scripts/release-checksums.sh
```

## What we ship (2.0.5-alpha2)

| Artifact | Platform | Build notes |
|----------|----------|-------------|
| `bridgesessions-linux-x86_64` | Linux x86_64 | CMake RelWithDebInfo / shipping root |
| `bridgesessions-windows-x86_64.exe` | Windows x86_64 | MinGW static cross-compile (OpenSSL+zstd; FMT header-only) |
| `bridgesessions-macos-arm64` | macOS arm64 | clang++ on Apple Silicon; Homebrew OpenSSL/zstd/fmt/spdlog dylibs |
| `bridgesessions-2.0.5-alpha2-source.tar.gz` | Source archive | Deterministic `scripts/package-release.sh` |
| `SHA256SUMS` | Checksums | SHA-256 of every dist artifact above |
| `SBOM-binaries.json` | SBOM | CycloneDX 1.5 of release artifacts |

Do not copy forward binaries from earlier tags. Every artifact must embed the
contents of [`VERSION`](../VERSION) and pass `scripts/release-checksums.sh`.

Full dependency SBOMs for system packages (OpenSSL, etc.) are host-specific;
document the build OS in forge release notes if you republish.

### Platform verification (alpha2)

| Platform | How verified |
|----------|----------------|
| Linux | `./binary --version`, full CTest 237/237, ASan suite |
| Windows | PE32+ x86-64; embedded version; peer `--version` smoke |
| macOS | Mach-O arm64; `./binary --version` on Apple Silicon build host |

### SHA256SUMS (2.0.5-alpha2)

```
a59fe63c8ed89ea7ff64c9098a43d67c677db54f72ca76ec15071c9f98e721a9  bridgesessions-2.0.5-alpha2-source.tar.gz
0df6c28c41437495937c60cdfe223a7207e0dcbc7080ea305b265d2a5117c69c  bridgesessions-linux-x86_64
4c034783692185aaa5d50864c3e1ac6096f614af488b27dd5179a5e17e2ce7d9  bridgesessions-macos-arm64
67d342ed47c970cd73acdc3115257c38a1e44ac0f095229332752c092f18d4ad  bridgesessions-windows-x86_64.exe
```

(If you rebuild, regenerate checksums — do not trust this table after local rebuilds.)

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
./scripts/package-release.sh
./scripts/release-checksums.sh
```
