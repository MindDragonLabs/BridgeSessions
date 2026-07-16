# Changelog

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
