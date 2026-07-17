# Release notes — BridgeSessions 2.0.5-alpha2

**Date:** 2026-07-17  
**Tag:** `v2.0.5-alpha2`  
**Status:** Public multi-platform **alpha** (security-audited candidate — **not** a production-secure SSH replacement)

## Download

| Platform | Asset |
|----------|--------|
| Linux x86_64 | `bridgesessions-linux-x86_64` |
| Windows x86_64 | `bridgesessions-windows-x86_64.exe` |
| macOS arm64 | `bridgesessions-macos-arm64` |
| Source | `bridgesessions-2.0.5-alpha2-source.tar.gz` |
| Checksums | `SHA256SUMS` |
| SBOM (CycloneDX 1.5) | `SBOM-binaries.json` |

```bash
# Verify (Linux)
sha256sum -c SHA256SUMS
chmod +x bridgesessions-linux-x86_64
./bridgesessions-linux-x86_64 --version   # → 2.0.5-alpha2
```

Full provenance: [docs/RELEASE-PROVENANCE.md](RELEASE-PROVENANCE.md)  
Audit narrative: [docs/AUDIT-2.0.5-alpha2.md](AUDIT-2.0.5-alpha2.md)  
Changelog: [CHANGELOG.md](../CHANGELOG.md)

## What changed (short)

- Mesh identity hardening (pin ↔ cert ↔ Hello; direct CLI requires pins)
- Transfer integrity (per-connection state, size/chunk enforcement, hash-before-publish)
- Single `VERSION` across CLI / CMake / packaging / SBOM
- All three platform binaries rebuilt from this source (no stale cross-platform reuse)

## Install sketch

```bash
# Linux
install -m 0755 bridgesessions-linux-x86_64 ~/.local/bin/bridgesessions
ln -sfn ~/.local/bin/bridgesessions ~/.local/bin/bs
bridgesessions keygen
# edit ~/.bridgesessions/config — every seed needs pubkey=…
bridgesessions --config ~/.bridgesessions/config
```

Windows: place the `.exe` on PATH; generate keys with `bridgesessions keygen`.  
macOS arm64: binary expects Homebrew OpenSSL/zstd/fmt/spdlog at standard
`/opt/homebrew/opt/...` paths, or rebuild from source (see [building.md](building.md)).

## Security expectations

- Prefer pinned seeds; do not disable `mesh.require_seed_pins` on untrusted networks
- Treat `id_ed25519.pem` like an SSH private key
- Do not expose BridgePanel to the public internet
- Report vulnerabilities privately — see [SECURITY.md](../SECURITY.md)

## Known limitations

See CHANGELOG “Known limitations” for this tag (Homebrew-linked macOS binary,
Windows cross-compile soak, unsigned-by-default checksums, alpha posture).
