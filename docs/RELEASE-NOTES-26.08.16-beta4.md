# BridgeSessions 26.08.16-beta4

Date: 2026-08-16

## Highlights

- **BridgePanel UI redesign + tray/menubar polish** — dark/light theme toggle,
  machine search, three-tab layout (Documents / Output / Files), harness picker,
  first-machine auto-select, `#3FA9E0` accent unification.
- **Session reflectiveness** — new `kind` field (`user` / `harness` / `probe`)
  classified server-side at spawn; machine badges count live sessions only;
  internal probes collapse into a dim summary.
- **Fleet directory** — offline/stale seeds render instead of vanishing;
  discovered peers TTL-prune via `mesh.discovered_ttl_secs`.
- **`bs upgrade` self-update fixes** — portable SHA256 verification
  (`sha256sum` fallback for Arch), `--tag` leading-`v` normalization.

## Artifacts

| Platform | Artifact |
|----------|----------|
| Linux x86_64 | `bridgesessions-linux-x86_64` |
| Windows x86_64 | `bridgesessions-windows-x86_64.exe` |
| macOS arm64 | `bridgesessions-macos-arm64` |

Verify: `bridgesessions --version` → `26.08.16-beta4`

## Checksums

```bash
sha256sum -c SHA256SUMS
```

See `CHANGELOG.md` and `docs/RELEASE-PROVENANCE.md` for full release evidence.
