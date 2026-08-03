# BridgeSessions 2.0.16-alpha6

**Hotfix: file transfer between v2.0.14/2.0.15 nodes was broken. Receivers now
sniff the zstd magic and accept both raw (v2.0.14+) and legacy double-compressed
(pre-2.0.14) chunk payloads.**

## Changes since 2.0.15-alpha6

### The bug
2.0.14-alpha6 converted all 6 send paths to raw bytes (single frame-layer
`encode()` compression) but left the 3 receiver sites running the compensating
manual `zstd_decompress()` — so a 2.0.14/2.0.15 receiver aborted every transfer
from a 2.0.14+ sender with `zstd: invalid frame` on the first chunk.

### The fix
- New `decompress_chunk_payload()` sniffs the zstd magic (`28 B5 2F FD`) at all
  3 receiver sites (mesh-relay recv, direct recv, pull path). Raw payloads pass
  through; legacy double-compressed payloads are unwrapped. Magic collisions on
  raw data fall back to raw — the end-to-end sha256 remains the integrity
  backstop and fails loudly rather than corrupting silently.
- Regression test covers raw / legacy / magic-collision shapes.
- The version-canonical test no longer hardcodes the release string (reads the
  repo `VERSION` file via `BS_VERSION_FILE_PATH`); building only the
  `bridgesessions` target no longer lets stale test binaries mask a version
  mismatch.

### Interop matrix

| Sender → Receiver | Result |
|---|---|
| any → **2.0.16** | ✅ works (sniffing receiver) |
| ≥2.0.14 → ≤2.0.13 | ❌ receiver aborts (`zstd: invalid frame`) — upgrade the receiver |
| ≤2.0.13 → ≤2.0.13 | ✅ (legacy double path, as before) |

**Upgrade all receivers; 2.0.16 nodes interop with everything.**

### Known issue (tracked for 2.0.17)
Long-running daemons leak `/dev/ptmx` FDs (~1 per shell/health session) and
eventually hit the 1024 soft limit, after which every `open()` fails
(e.g. `ERROR cannot hash …` on sends). Restarting the daemon clears it.

## Assets

```text
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.16-alpha6/dist/bridgesessions-linux-x86_64
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.16-alpha6/dist/bridgesessions-windows-x86_64.exe
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.16-alpha6/dist/bridgesessions-macos-arm64
```

## Verification

```bash
./bridgesessions --version   # → 2.0.16-alpha6
```

330/330 CTest green (3 consecutive runs). Live-verified on the fleet:
Linux ↔ Windows nodes 1 MiB transfer, both directions, hash-matched.
