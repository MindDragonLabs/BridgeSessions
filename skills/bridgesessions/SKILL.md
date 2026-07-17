---
name: bridgesessions
description: >-
  Operate and develop BridgeSessions (bs) mesh terminal/file relay across
  Linux, macOS, and Windows. Use when running bs shell/file/health, fleet
  deploy, PowerShell $_ quoting, large file transfer with PROGRESS lines,
  Windows vs Linux commands, peer pin security, or
  reading remediation TODOs after the 2026-07 security audit. Do NOT use for
  raw SSH/SCP when a healthy bs mesh path exists.
license: BSL-1.1
compatibility: Requires bridgesessions CLI (bs) or build from this repo; OpenSSL; optional WinRM for Windows gameplay Session-1.
metadata:
  version: "2.0.5-alpha2"
  product: BridgeSessions
  harnesses: "hermes,codex,claude-code,opencode,cursor,grok,copilot"
  related: "docs/remediation-20260716/INDEX.md"
---

# BridgeSessions — Agent Skill

Portable skill for **Hermes**, **OpenAI Codex**, **Claude Code**, **OpenCode**,
**Cursor**, **Grok/xAI**, and other SKILL.md / AGENTS.md-aware harnesses.

**Repo root always-on context:** also read [`AGENTS.md`](../../AGENTS.md) when present.

## What this product is

- One C++ binary: mesh daemon + CLI (`bridgesessions` / `bs`).
- Replaces ad-hoc SSH + SCP + tmux + WinRM for **agent-native** shells and files.
- Default mesh port **19949**; CLI IPC **19980** (local).
- Current public release: **v2.0.5-alpha2**.
- Public tag may lag; always run `bs --version` / `bridgesessions --version` live.

## Non-negotiables

1. Prefer **bs** over raw SSH/SCP when mesh is healthy.
2. **Pinned peers:** seeds need `pubkey=…`; `mesh.require_seed_pins` defaults true (v2.0.3+).
3. **CLI health** = data-plane (`healthy (data-plane ok)`). IPC HEALTH alone is not enough.
4. **Windows peers are Windows** — not Linux, not “MinGW ≈ Linux”.
5. **Stack commands** in one shell; do not open one `bs shell` per micro-step.
6. Credentials: never commit secrets; use env / operator vaults.
7. Security claims: do **not** call this production-secure SSH replacement until
   `docs/remediation-20260716/TODO-AUDIT-CLOSURE.md` P0 rows are all `[x]`.

## Quick commands

```bash
export PATH="$HOME/.local/bin:$PATH"
bs --version                          # expect 2.0.5-alpha2
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
| Linux ELF as `.exe` | Native PE build / MinGW **only** for this project’s Windows binary |
| Treat MinGW as Linux userspace | MinGW builds PE; not apt/systemd/Linux ABI |

Gameplay GUI / desktop input: **WinRM Session-1** (or documented Session-1 helper). BS one-shots often run as **SYSTEM/Session 0**.

## Security posture (v2.0.3+)

- Outbound mesh: pin ↔ TLS cert Ed25519 key ↔ Hello before trusting peer / `merge_peers`.
- File recv: basename sanitize + path containment; reject absolute/traversal/device names.
- TLS: min 1.2, max 1.3 (prefer 1.3; not 1.3-only for fleet self-signed compatibility).
- Backlog: `docs/remediation-20260716/TODO-AUDIT-CLOSURE.md`.

## Develop / build

```bash
# Linux
cmake -S . -B build && cmake --build build -j
./build/bridgesessions --version
ctest --test-dir build --output-on-failure   # may have rare PTY flake

# Unit security helpers
./build/test_config "[security]"
```

Primary source: `bridgesessions.cpp` (monolith). Modular `bs-*` trees are **not** the shipping root build unless decision flips.

## Deploy (fleet sketch)

1. Never overwrite live `~/.bridgesessions/{config,authorized_keys}` with empty templates.
2. Order typical: build host → peers; Windows via WinRM + HTTP file serve, not scp of Linux ELF.
3. After deploy: `--version`, `health <peer>`, `shell <peer> --cmd hostname`.

## Harness install map

Copy or symlink this directory so each tool discovers it:

| Harness | Discovery path |
|---------|----------------|
| **Claude Code** | `.claude/skills/bridgesessions/SKILL.md` |
| **OpenCode** | `.opencode/skills/bridgesessions/SKILL.md` |
| **Codex / AGENTS** | repo `AGENTS.md` (always-on) + optional skill dir |
| **Cursor** | skills under project / `.cursor` + `AGENTS.md` |
| **Hermes** | `~/.hermes/skills/**/bridgesessions*/SKILL.md` or `skill_view` after install |
| **Generic Agent Skills** | `.agents/skills/bridgesessions/SKILL.md` |

Canonical in-repo path: **`skills/bridgesessions/`**. Other locations should symlink here.

```bash
# From repo root — recreate harness links
ln -sfn ../../skills/bridgesessions .claude/skills/bridgesessions
ln -sfn ../../skills/bridgesessions .opencode/skills/bridgesessions
ln -sfn ../../skills/bridgesessions .agents/skills/bridgesessions
# Hermes (operator machine)
mkdir -p ~/.hermes/skills/devops
ln -sfn /path/to/BridgeSessions/skills/bridgesessions ~/.hermes/skills/devops/bridgesessions
```

## Progressive docs (load on demand)

- Remediation index: `docs/remediation-20260716/INDEX.md`
- Audit 100% checklist: `docs/remediation-20260716/TODO-AUDIT-CLOSURE.md`
- Transfer/PROGRESS: `docs/remediation-20260716/TODO-TRANSFER-AI.md`
- Windows + stacking: `docs/remediation-20260716/TODO-AI-WINDOWS-OPS.md`
- Why BridgeSessions: `docs/why-bridge-sessions.md`
- Config: `docs/configuration.md`
- Protocol: `docs/protocol.md`

## Verification before claiming success

```bash
bs --version
bs health <peer>          # healthy (data-plane ok)
bs shell <peer> --cmd "…" # real stdout, correct host
# Large file:
bs file send <peer> /tmp/big.bin --wait   # see PROGRESS then OK
```

Subagent claims are not evidence — re-probe in this session.
