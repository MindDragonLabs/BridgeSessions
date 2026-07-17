# Changelog

## [2.0.5-alpha2] — 2026-07-17

Public alpha security and release-hardening candidate.

### Security
- Require complete Hello key/name identity and reject inbound configured
  name/key collisions.
- Reject direct CLI connections without a pinned peer key before opening TCP.
- Reject malformed hexadecimal authorized keys.
- Create identity and authorization files as owner-only (`0600`) and identity
  directories as `0700` from first write, including legacy migration.
- Launch local editors without shell-parsing remote filenames.
- Reject hostile frame lengths without pointer-arithmetic overflow.

### Reliability
- Isolate receive state per connection and enforce declared file size/chunk
  shape on every transfer.
- Hash partial files before final rename; corrupt transfers are never published
  under the destination filename.
- Use bounded nonblocking TCP connect for daemon and direct CLI paths.
- Honor explicit config paths during daemon reload and keep logs under the
  authoritative `--config-dir`.
- Fix cumulative duplicate `PROGRESS` output in long IPC transfers.
- Close BridgePanel IPC sockets on failure.

### Release
- Canonical `VERSION` now drives CMake, CLI, mesh Hello, BridgePanel, artifacts,
  checksums, and CycloneDX metadata.
- Deterministic source packaging excludes operator-only and non-shipping legacy
  trees without deleting them.
- Multi-platform release artifacts from this source:
  - Linux x86_64 (`bridgesessions-linux-x86_64`)
  - Windows x86_64 MinGW-static (`bridgesessions-windows-x86_64.exe`)
  - macOS arm64 Homebrew-linked (`bridgesessions-macos-arm64`)
- Validation: 237 CTest cases, 13 BridgePanel HTTP tests, ASan/UBSan, targeted
  TSan, clang-tidy, scan-build, Windows peer `--version` smoke, macOS build-host
  `--version`, and byte-identical 500 MB transfer soak.

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
Earlier internal tags (v1.7.x–v1.8.x) were development releases and are not
part of this public history (orphan rewrite for a clean initial public commit).
