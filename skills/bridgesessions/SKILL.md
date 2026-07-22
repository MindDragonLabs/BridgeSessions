---
name: bridgesessions
description: >-
  Operate and develop BridgeSessions (bs) mesh terminal/file relay across
  Linux, macOS, and Windows. Use when running bs shell/file/health, fleet
  deploy, PowerShell $_ quoting, large file transfer with PROGRESS lines,
  Windows vs Linux commands, peer pin security, Codeberg publish (deploy-key SSH +
  dist/), or release hardening after the 2026-07 security audit. Do NOT use for
  raw SSH/SCP when a healthy bs mesh path exists. Do NOT confuse public Codeberg
  publish with local user Forgejo.
license: BUSL-1.1
compatibility: Requires bridgesessions CLI (bs) or build from this repo; OpenSSL; optional WinRM for Windows gameplay Session-1.
metadata:
  version: "2.0.8-alpha3"
  product: BridgeSessions
  harnesses: "hermes,codex,claude-code,opencode,cursor,grok,copilot"
  related: "docs/RELEASE-PROVENANCE.md"
  release: "v2.0.8-alpha3"
  forge: "codeberg.org/Mind-Dragon/BridgeSessions"
---

# BridgeSessions — Agent Skill

Portable skill for **Hermes**, **OpenAI Codex**, **Claude Code**, **OpenCode**,
**Cursor**, **Grok/xAI**, and other SKILL.md / AGENTS.md-aware harnesses.

**Repo root always-on context:** also read [`AGENTS.md`](../../AGENTS.md) when present.

## What this product is

- One C++ binary: mesh daemon + CLI (`bridgesessions` / `bs`).
- Replaces ad-hoc SSH + SCP + tmux + WinRM for **agent-native** shells and files.
- Default mesh port **19949**; CLI IPC **19980** (local).
- **Current public release: `v2.0.8-alpha3`** (multi-platform alpha on Codeberg).
- Canonical shipping source: `bridgesessions.cpp` (monolith). Modular `bs-*` trees
  are non-shipping — see `LEGACY_CODE.md`.
- Always probe live: `bs --version` / `bridgesessions --version` (do not trust memory alone).

## Non-negotiables

1. Prefer **bs** over raw SSH/SCP when mesh is healthy.
2. **Pinned peers:** seeds need `pubkey=…`; `mesh.require_seed_pins` defaults true.
3. **CLI health** = data-plane (`healthy (data-plane ok)`). IPC HEALTH alone is not enough.
4. **Windows peers are Windows** — not Linux, not "MinGW ≈ Linux".
5. **Stack commands** in one shell; do not open one `bs shell` per micro-step.
6. Credentials: never commit secrets; use env / operator vaults.
7. **Alpha posture:** public alpha is **not** a production-secure SSH replacement.
   See `SECURITY.md` and `.audit/moa-2.0.8a3/AUDIT.md`.
8. **Public forge = Codeberg only** for this product repo. Do **not** route BridgeSessions
   release/publish through local user **Forgejo** or invent a Forgejo requirement.
9. **Binaries ship in git `dist/`** and are published by **`git push` over SSH**.
   No API token is required to make binaries downloadable.
10. **Pre-push security hook** blocks secrets/IPs before Codeberg pushes
    (`scripts/prepublish-scan.sh`). Do not bypass it.

## Public release (`v2.0.8-alpha3`)

| Fact | Value |
|------|--------|
| Tag | `v2.0.8-alpha3` (commit `2c36201`, 2026-07-21) |
| Branch | `main` |
| Repo | https://codeberg.org/Mind-Dragon/BridgeSessions |
| Artifacts | Linux x86_64, Windows x86_64 PE, macOS arm64 (`dist/`) |
| Tests | 329/329 CTest green (25 test files, 13,079 SLOC) |
| Audit | MoA 4-lane: 4 P0 + 10 P1 + 5 P2 fixed (`.audit/moa-2.0.8a3/AUDIT.md`) |
| Notes | `docs/RELEASE-2.0.8-alpha3.md` · provenance `docs/RELEASE-PROVENANCE.md` |

### New in v2.0.8-alpha3

- **Multi-attach + spectator** (P1): same-source multiple connections, per-attach detach, MIN-geometry, read-only spectator role
- **Conversation store** (P4): `ConversationAppend` (0x23) / `ConversationQuery` (0x24) / `ConversationBatch` (0x25) with mesh relay, server seq authority, bounded (10k/conv, 1024 convs)
- **Streaming hardening** (P3): per-conn output queues + `OutputGap` (0x22) backpressure, RingBuffer alignment fix, IPC `SCROLLBACK`
- **Cross-platform CUA** (P5): `CuaRequest` (0x26) / `CuaResponse` (0x27), Linux/macOS/Windows backends, USB HID keycodes
- **Display harness** (P2): 11 geometry/glyph tests, `doctor` display check
- **BridgePanel v3**: token-auth IPC, 3-column UI, `MESH_TREE` gossip, incremental scrollback sync
- **MoA security audit**: spectator SignalMsg RCE, conversation body u8→u16, RingBuffer slot corruption, CUA shell injection, gossip JSON envelope, IPC framing/RST drain, geometry floor, store bounds, seq authority, token race

### Download (raw from tag — preferred)

```text
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.8-alpha3/dist/bridgesessions-linux-x86_64
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.8-alpha3/dist/bridgesessions-windows-x86_64.exe
https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.8-alpha3/dist/bridgesessions-macos-arm64
```

Tree: https://codeberg.org/Mind-Dragon/BridgeSessions/src/tag/v2.0.8-alpha3/dist

```bash
curl -fL -o bridgesessions \
  https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.8-alpha3/dist/bridgesessions-linux-x86_64
chmod +x bridgesessions
./bridgesessions --version   # → 2.0.8-alpha3
```

### Build matrix (how the 3 platform binaries are produced)

All three are **portable static** (no runtime dylib/DLL deps beyond OS libs):

| Platform | Built on | Method |
|----------|----------|--------|
| Linux x86_64 | test-pc1 via `ubuntu:22.04` container | static OpenSSL/zstd/fmt/spdlog + `-static-libstdc++ -static-libgcc`; glibc kept dynamic (DNS). Dockerfile `/tmp/bs-static/Dockerfile` (see `bridgesessions-static-build` skill). |
| macOS arm64 | **test-pc5** (Apple clang 17) | native build, static deps into `~/local`, `cmake -DCMAKE_OSX_ARCHITECTURES=arm64`. Links only system `libc++`/`libSystem`. |
| Windows x86_64 PE | **test-pc1 cross-compile** (`x86_64-w64-mingw32-g++`) | static deps into `~/bs-win`; compile `bridgesessions.cpp` directly with `-static`. PE imports only OS DLLs (KERNEL32/USER32/WS2_32/ADVAPI32/CRYPT32 + UCRT). |

- **test-pc7 (Win11) has NO sshd** — ship the PE via **WinRM** (port 5985, NTLM, credentials in test-pc1 `~/.ssh/config` note / local vault). Host a temp `python3 -m http.server` on test-pc1's TS IP, then `curl.exe` it from a WinRM `run_ps`.
- mac/win release binaries ARE committed to `dist/` (the `.gitignore` only ignores the dev `bridgesessions.exe` + `*.o`/`*.obj`). `SHA256SUMS`/`SBOM` stay gitignored (downloader regenerates).
- Re-tag after changing `dist/`: `git tag -f v2.0.8-alpha3 HEAD && git push --force codeberg main && git push --force codeberg v2.0.8-alpha3`.

### Publish to Codeberg (SSH only)

On the operator host that has the **Mind-Dragon** Codeberg key:

```bash
# Working identity: ~/.ssh/deploy-key  (default id_ed25519 is denied for this account)
export GIT_SSH_COMMAND='ssh -i ~/.ssh/deploy-key -o IdentitiesOnly=yes -o BatchMode=yes'
# Or permanent: Host codeberg.org → IdentityFile ~/.ssh/deploy-key in ~/.ssh/config

cd /path/to/BridgeSessions   # often ~/bridgesessions
git push codeberg main
git push codeberg v2.0.8-alpha3
```

Probe: `ssh -i ~/.ssh/deploy-key -o IdentitiesOnly=yes -T git@codeberg.org`  
→ "Hi there, Mind-Dragon!"

**Do not:**

- Use local user Forgejo for this public product release
- Block on `FORGEJO_TOKEN` / Codeberg "Releases" API for binary delivery
- Claim binaries missing if `dist/` on the tag is already pushed (raw URLs return 200)
- Bypass the pre-push security hook (`.git/hooks/pre-push`)

Optional Codeberg "Releases" UI assets are cosmetics only; **git `dist/` is the source of truth**.

## Quick commands

```bash
export PATH="$HOME/.local/bin:$PATH"
bs --version                          # expect 2.0.8-alpha3
bs health <peer>                      # must say healthy (data-plane ok)
bs shell <peer> --cmd '…'             # one-shot; exit code propagates
bs file send <peer> /local/path --wait
bs file recv <peer> /remote/path --to ./out --wait
bs doctor
```

### Command stacking (required pattern)

```bash
# Windows cmd
bs shell windows-peer --cmd 'cmd /c "hostname && dir C:\temp && type C:\temp\a.txt"'

# Windows PowerShell (bash single-quotes protect $_)
bs shell windows-peer --cmd 'powershell -NoProfile -Command "Get-Process | Select-Object -First 3 | ForEach-Object { $_.ProcessName }"'

# Linux/macOS
bs shell linux-peer --cmd "bash -lc 'hostname && df -h && uptime'"
```

**Bad:** three separate `bs shell` calls for dependent steps.

### Large files (v2.0.5+)

- Prefer `bs file … --wait` over scp on mesh peers.
- Streams AI-parseable progress ~every 10s:

```text
PROGRESS phase=recv file=x.bin chunks=a/b bytes=c/d pct=P rate_mibs=R eta_sec=E
```

- Timeouts are size-aware + idle (not a fixed 120s wall).
- Streaming SHA-256 (no full-file RAM). Default `transfer.max_bytes` is large (8 GiB).

### PowerShell `$_` / pipes

- v2.0.2+: powershell/pwsh skip broken `cmd /c` quote destruction.
- Still quote carefully: bash double-quotes expand `$_` — use single quotes or `\$_`.
- Prefer `powershell -NoProfile -Command "…"` or `-EncodedCommand` for complex scripts.

## Peer naming

- Resolve peer names from the active config; do not guess aliases.
- Require a pinned public key for every seed and direct command.
- Keep private fleet names and VPN addresses out of public skills and examples.

## Windows vs Linux (read every time)

| Never on Windows | Use instead |
|------------------|-------------|
| Default `tar xvf` / apt / yum | `Expand-Archive`, `winget`, `msiexec` |
| `chmod +x`, shebang scripts | `.ps1` / `.cmd` / `.bat` |
| `/tmp`, `/home` | `%TEMP%`, `C:\Users\…` |
| Linux ELF as `.exe` | Native PE build / MinGW **only** for this project's Windows binary |
| Treat MinGW as Linux userspace | MinGW builds PE; not apt/systemd/Linux ABI |

Gameplay GUI / desktop input: **WinRM Session-1** (or documented Session-1 helper). BS one-shots often run as **SYSTEM/Session 0**.

## Security posture (v2.0.8-alpha3)

- Outbound mesh: pin ↔ TLS cert Ed25519 key ↔ Hello before trusting peer / `merge_peers`.
- Direct CLI: reject unpinned peers before DNS/TCP.
- File recv: basename sanitize + path containment; hash `.part` before publish.
- TLS: min 1.2, max 1.3 (prefer 1.3; not 1.3-only for fleet self-signed compatibility).
- Spectator guard: read-only role cannot inject SignalMsg/CUA/Keystroke (P0 fixed).
- IPC: token-auth (BridgePanel v3), 128 KiB framing, RST-safe drain, `send_all` replies.
- Gossip: JSON envelope validator on receive (injection fix).
- Conversation store: seq authority + bounds (10k/conv, 1024 convs), body u16-prefixed.
- CUA: POSIX `sq()` quoting (shell-injection fix).
- Audit: `.audit/moa-2.0.8a3/AUDIT.md` · release evidence: `docs/RELEASE-PROVENANCE.md` and `CHANGELOG.md`

## Develop / build

```bash
# Linux
cmake -S . -B build && cmake --build build -j
./build/bridgesessions --version   # 2.0.8-alpha3
ctest --test-dir build --output-on-failure
./build/test_config "[security]"

# Or one-shot:
./build.sh
```

Cross-platform build notes: `docs/building.md`.  
Release packaging: `scripts/package-release.sh` + `scripts/release-checksums.sh`.

Primary source: `bridgesessions.cpp` (monolith).

## Deploy (fleet sketch)

1. Never overwrite live `~/.bridgesessions/{config,authorized_keys}` with empty templates.
2. Order typical: build host → peers; Windows via PE binary (not Linux ELF).
3. After deploy: `--version`, `health <peer>`, `shell <peer> --cmd hostname`.
4. Seeds without `pubkey=` are skipped when `require_seed_pins` is true.

## Harness install map

| Harness | Discovery path |
|---------|----------------|
| **Claude Code** | `.claude/skills/bridgesessions/SKILL.md` |
| **OpenCode** | `.opencode/skills/bridgesessions/SKILL.md` |
| **Codex / AGENTS** | repo `AGENTS.md` (always-on) + optional skill dir |
| **Cursor** | skills under project / `.cursor` + `AGENTS.md` |
| **Hermes** | `~/.hermes/skills/**/bridgesessions*/SKILL.md` or `skill_view` |
| **Generic Agent Skills** | `.agents/skills/bridgesessions/SKILL.md` |

Canonical in-repo path: **`skills/bridgesessions/`**. Symlink others here.

```bash
# From repo root — recreate harness links
./scripts/install-agent-skill.sh
# Hermes (prefer symlink to this repo tree)
mkdir -p ~/.hermes/skills/devops
ln -sfn "$(pwd)/skills/bridgesessions" ~/.hermes/skills/devops/bridgesessions
```

## Progressive docs (load on demand)

- Release notes: `docs/RELEASE-2.0.8-alpha3.md`
- Provenance / checksums: `docs/RELEASE-PROVENANCE.md`
- Audit: `.audit/moa-2.0.8a3/AUDIT.md`
- Usage and transfer workflows: `docs/usage.md`
- Cross-platform builds: `docs/building.md`
- Why BridgeSessions: `docs/why-bridge-sessions.md`
- Config: `docs/configuration.md`
- Protocol: `docs/protocol.md`
- Publish pitfalls: `references/codeberg-publish.md`

## Verification before claiming success

```bash
bs --version                          # 2.0.8-alpha3
bs health <peer>                      # healthy (data-plane ok)
bs shell <peer> --cmd "…"             # real stdout, correct host
bs file send <peer> /tmp/big.bin --wait   # PROGRESS then OK
# Public tag contains the reviewed platform binaries:
curl -fsI https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.8-alpha3/dist/bridgesessions-linux-x86_64
```

Subagent claims are not evidence — re-probe in this session.
