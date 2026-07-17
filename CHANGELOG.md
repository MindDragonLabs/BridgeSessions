# Changelog

All notable public releases are documented here.

## [2.0.5-alpha2] — 2026-07-17

**Public multi-platform alpha** — security/reliability closeout after the 2026-07
independent review. **Not** a production-secure SSH replacement.

Tag: `v2.0.5-alpha2` · License: BSL-1.1 (→ Apache-2.0 on 2030-07-16)

### Highlights

- Hardened mesh identity: pin ↔ TLS cert ↔ Hello binding; direct CLI requires pins
- Transfer integrity: per-connection receive state, exact size/chunk checks,
  streaming SHA-256 before publish
- Canonical `VERSION` drives CLI, CMake, Hello, BridgePanel, packaging, SBOM
- Release artifacts for **Linux x86_64**, **Windows x86_64**, and **macOS arm64**
  from the same source tree

### Security

- Require complete Hello key/name identity; reject inbound configured name/key
  collisions (peer-name impersonation)
- Reject direct CLI connections without a pinned peer key before DNS/TCP
- Reject malformed hexadecimal authorized keys
- Create identity and authorization files as owner-only (`0600`) and app homes as
  `0700` from first write (including legacy migration)
- Launch local editors with argv paths (no shell-parsed remote filenames)
- Reject hostile frame lengths without pointer-arithmetic overflow
- TLS policy: minimum 1.2, maximum/prefer 1.3 (shipping binary)

### Reliability

- Isolate file receive state per connection
- Enforce declared file size/chunk shape; overflow-safe metadata
- Hash partial files before final rename; corrupt/incomplete transfers never land
  under the destination name
- Bounded nonblocking TCP connect on mesh and direct CLI paths
- Honor explicit config paths on daemon reload; logs stay under `--config-dir`
- Fix cumulative duplicate `PROGRESS` lines on long IPC transfers
- Close BridgePanel IPC sockets on failure
- Size-aware + idle transfer timeouts (no fixed 120s wall for large AI assets)

### Release engineering

- Single `VERSION` file is source of truth
- Deterministic source packaging (`scripts/package-release.sh`) excludes
  operator-only and non-shipping modular trees (see `LEGACY_CODE.md`)
- `scripts/release-checksums.sh` validates embedded version, writes `SHA256SUMS`
  and CycloneDX 1.5 `SBOM-binaries.json` (works for PE/Mach-O on Linux hosts)
- Multi-harness agent packaging: `AGENTS.md` + `skills/bridgesessions/`

### Artifacts

| File | Notes |
|------|--------|
| `bridgesessions-linux-x86_64` | ELF x86_64 |
| `bridgesessions-windows-x86_64.exe` | PE32+ MinGW static (OpenSSL+zstd) |
| `bridgesessions-macos-arm64` | Mach-O arm64 (Homebrew OpenSSL/zstd/fmt/spdlog) |
| `bridgesessions-2.0.5-alpha2-source.tar.gz` | Deterministic source |
| `SHA256SUMS` / `SBOM-binaries.json` | Provenance |

Verify: `cd dist && sha256sum -c SHA256SUMS` — see [docs/RELEASE-PROVENANCE.md](docs/RELEASE-PROVENANCE.md).

### Validation (release host)

- CTest **237/237**
- BridgePanel HTTP **13/13**
- ASan/UBSan serial suite green
- Targeted TSan (ring + Hello)
- clang-tidy / scan-build clean
- Windows peer `--version` smoke
- macOS build-host `--version`
- 500 MB transfer soak (byte-identical)

### Known limitations

- Public **alpha** — do not treat as a production SSH replacement
- macOS binary links Homebrew dylibs (not fully static)
- Windows artifact is cross-compiled; full Windows CI suite is operator soak
- Checksums ship; forge/minisign signing is optional operator step
- Modular `bs-*` trees remain in git as non-shipping history only

### Upgrade notes

From v2.0.0 / v2.0.1 binaries:

1. Replace the binary; keep `~/.bridgesessions/` (identity + config)
2. Ensure every `seed` line has `pubkey=<64-hex>` (`mesh.require_seed_pins` defaults true)
3. Re-run `bridgesessions doctor` and `health <peer>` (must say `healthy (data-plane ok)`)

---

## [2.0.1] — 2026-07-16

- Windows one-shot shell/health pipe capture fix
- Stale seed hygiene
- Prebuilt Linux / macOS / Windows binaries on the public tree

## [2.0.0] — 2026-07-16

First public release.

### Added

- Business Source License 1.1 (Change Date 2030-07-16 → Apache-2.0)
- Full `docs/` tree (design, building, usage, configuration, protocol, bridge-panel)
- Public README, CONTRIBUTING, SECURITY
- Prebuilt binaries in `dist/`: Linux x86_64, macOS arm64, Windows x86_64
- Bridge Panel web surface + `bridgesessions pane publish`
- Stale-exec watchdog (force-release stuck `exec_busy` after 90s)

### Changed

- Version unified to 2.0.0
- Public tree stripped of internal notes, fleet configs, and build cruft
- Example configs use placeholder hosts only (no real infrastructure IPs)

### Prior history

Earlier internal tags (v1.7.x–v1.8.x) were development releases and are not part
of this public history (orphan rewrite for a clean initial public commit).
