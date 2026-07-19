# Release notes — BridgeSessions 2.0.6

**Date:** 2026-07-19  
**Tag:** `v2.0.6`  
**Status:** Security-audited public multi-platform **alpha** — **not** a production-secure SSH replacement

## Download

| Platform | Asset |
|----------|--------|
| Linux x86_64 | `bridgesessions-linux-x86_64` |
| Windows x86_64 | `bridgesessions-windows-x86_64.exe` |
| macOS arm64 | `bridgesessions-macos-arm64` |
| Source | `bridgesessions-2.0.6-source.tar.gz`, `bridgesessions-2.0.6-source.zip` |
| Checksums | `SHA256SUMS` |
| SBOM (CycloneDX 1.5) | `SBOM-binaries.json` |

```bash
# Verify (Linux)
cd /path/to/downloaded-release-assets
sha256sum -c SHA256SUMS
chmod +x bridgesessions-linux-x86_64
./bridgesessions-linux-x86_64 --version   # → 2.0.6
```

Full provenance: [docs/RELEASE-PROVENANCE.md](RELEASE-PROVENANCE.md)  
Historical audit baseline: [docs/AUDIT-2.0.5-alpha2.md](AUDIT-2.0.5-alpha2.md)  
Changelog: [CHANGELOG.md](../CHANGELOG.md)

## What changed (short)

- mDNS is disabled by default and cannot establish trust from multicast.
- Local daemon IPC requires an owner-only per-process token.
- TLS/Hello handshakes are nonblocking and deadline-bounded.
- Long file operations run on bounded joinable workers instead of the mesh loop.
- Duplicate Hello identity rebinding is rejected.
- PTY input handles partial nonblocking writes with ordered backpressure.
- Windows ConPTY input is handed to a bounded dedicated writer queue instead of
  blocking the mesh event loop in `WriteFile`.
- Async receive destinations are per connection.
- Release scripts hardened for deterministic, reproducible packaging.
- Source archives are produced with deterministic `git archive` tar/ZIP output
  plus `gzip -n` for the compressed tarball.
- Release mode requires a clean tree and an exact `v2.0.6` tag.
- SBOM uses a unique CycloneDX UUID on every run; SBOM hash is included in `SHA256SUMS`.
- Codeberg release script is draft-first, supports explicit `--draft-only`
  staging, and never mutates published assets.

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

- Remote `edit` and vfolder sync commands are disabled for this release rather
  than running blocking work on the mesh event loop.
- Image protocol message IDs remain reserved, but large image fragmentation and
  receiver rendering are not implemented; send media through file transfer.
- Linux, native Apple Silicon, and native Windows runtime suites pass; see the
  exact counts in [CHANGELOG.md](../CHANGELOG.md).
- See CHANGELOG for Homebrew-linked macOS binary and alpha-posture details.
