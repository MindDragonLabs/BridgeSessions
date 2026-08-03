# BridgeSessions 2.0.17-alpha6

**Hotfix follow-on: frame reads tolerate WANT_READ/WANT_WRITE — Windows-source
pulls no longer die at TLS record boundaries. Carries the 2.0.16 zstd-magic
receiver sniff.**

## Changes since 2.0.16-alpha6

- **`bs file recv` (pull) aborted mid-transfer** with
  `SSL_read header failed: SSL error 2` (WANT_READ): `select()` readiness
  guarantees bytes, not a complete TLS record; multi-record chunk frames
  surfaced WANT_READ inside `read_frame`, which treated any `SSL_read_ex <= 0`
  as fatal. `read_frame` now retries WANT_READ/WANT_WRITE with a bounded budget
  (400 × 25 ms = 10 s cap) — benefits every frame consumer (transfers, health
  probes, edit sync).

## Why 2.0.17 exists one hour after 2.0.16

2.0.16 fixed the *receiver payload* layer (zstd double-decompress). Live
verification then exposed the next layer down — the *frame read* layer — on the
Windows→Linux pull path. Rather than rewrite a public tag, 2.0.17 fast-follows.
**Skip 2.0.16 entirely; deploy 2.0.17.** (2.0.16 is not harmful, it just still
breaks on Windows-source pulls.)

## Interop matrix (unchanged from 2.0.16)

| Sender → Receiver | Result |
|---|---|
| any → **2.0.17** | ✅ works |
| ≥2.0.14 → ≤2.0.13 | ❌ receiver aborts — upgrade the receiver |
| ≤2.0.13 → ≤2.0.13 | ✅ (legacy double path) |

## Assets

```text
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.17-alpha6/dist/bridgesessions-linux-x86_64
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.17-alpha6/dist/bridgesessions-windows-x86_64.exe
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.17-alpha6/dist/bridgesessions-macos-arm64
```

## Verification

```bash
./bridgesessions --version   # → 2.0.17-alpha6
```

330/330 CTest green. Live-verified on the fleet: Linux ↔ Windows nodes,
1 MiB both directions, sha256-matched (push `e9a84dfa…`, pull `75c5e630…`).

### Known issue (tracked for 2.0.18)
Long-running daemons leak `/dev/ptmx` FDs (~1 per shell/health session) and
eventually hit the 1024 soft limit, after which every `open()` fails
(e.g. `ERROR cannot hash …` on sends). Restarting the daemon clears it.
