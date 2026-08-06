# BridgeSessions

**Mesh terminal relay** — one C++23 binary that replaces the usual remote stack
(**SSH + MOSH + SCP + tmux/Zellij + WinRM**) with a single secure mesh
(`bs://` over TLS 1.2+, prefer 1.3). Built for humans **and** AI agents: durable PTYs,
file transfer for media artifacts, Windows CUA peers, and **Bridge Panel** for long
Markdown reviews.

> **Beta status:** `26.08.05-beta1` is a feature-complete beta. The canonical
> shipping implementation is [`main.cpp`](main.cpp) + [`bs-protocol.h`](bs-protocol.h) +
> [`bs-session.h`](bs-session.h); macOS capture backend: [`macos-capture.mm`](macos-capture.mm).
> See [LEGACY_CODE.md](LEGACY_CODE.md) for the non-shipping modular experiment.

- **Persistent sessions** — disconnect and reattach; the PTY keeps running (tmux's job, built in).
- **Encrypted mesh** — ed25519 mutual TLS 1.2+ (prefer 1.3), forward secrecy (SSH's job, fleet-native).
- **Files + media** — on-protocol transfer for logs, screenshots, and video (SCP + vision I/O). Pipelined for high-latency links (8-chunk batching, 4–8× throughput).
- **Windows + Linux + macOS** — one mesh for shells and desktop automation (WinRM-class peers).
- **CUA automation** — remote screen capture, mouse, keyboard, scroll (`bs cua …`).
- **Run-script** — send and execute scripts remotely with auto-detected interpreter (`bs run-script`).
- **Fuzzy peer resolution** — 4-tier matching: exact → suffix/prefix → Levenshtein (typo-tolerant).
- **Bridge Panel** — agent-friendly Markdown surface (Edit / Save / Copy), not chat paste.
- **One binary** — client, server, and `doctor` in `bridgesessions`.

**Read the full story:** [docs/why-bridge-sessions.md](docs/why-bridge-sessions.md)

![Bridge Panel](docs/assets/bridgepanel-read.png)

### Product demo

[![BridgeSessions product demo](docs/assets/demo-install-ai-mesh.gif)](docs/assets/demo-install-ai-mesh.mp4)

*HyperFrames storyboard (22s) — install → mesh → AI CUA → media/vision → Bridge Panel.*  
Full quality: **[MP4 · 1080p · 2.7 MB](docs/assets/demo-install-ai-mesh.mp4)** · [file view with player](https://codeberg.org/Mind-Dragon/BridgeSessions/src/branch/main/docs/assets/demo-install-ai-mesh.mp4)

> License: **Business Source License 1.1** (BSL-1.1). Production and commercial
> use are permitted with one carve-out (you may not operate it as a hosted
> remote-terminal service). Converts to **Apache-2.0** on **2030-07-16**. See
> [LICENSE](LICENSE).


## For AI agents (Hermes, Codex, Claude, OpenCode, Cursor, Grok, …)

This repo ships multi-harness agent instructions:

| File | Role |
|------|------|
| [`AGENTS.md`](AGENTS.md) | Always-on project rules (AGENTS.md standard) |
| [`CLAUDE.md`](CLAUDE.md) | Symlink → `AGENTS.md` for Claude Code |
| [`skills/bridgesessions/SKILL.md`](skills/bridgesessions/SKILL.md) | Portable **Agent Skills** skill (YAML frontmatter + body) |
| `.claude/skills/bridgesessions` | Symlink for Claude Code discovery |
| `.opencode/skills/bridgesessions` | Symlink for OpenCode discovery |
| `.agents/skills/bridgesessions` | Symlink for generic Agent Skills discovery |

**Load the skill** when operating `bs` mesh, Windows peers, large file transfer, or release hardening.
Release security changes are summarized in [`CHANGELOG.md`](CHANGELOG.md) and
the support/reporting policy lives in [`SECURITY.md`](SECURITY.md).

Install/refresh harness links: `./scripts/install-agent-skill.sh`.


## Install from binary

Current release: **`26.08.05-beta1`** (git tag `v26.08.05-beta1`).
Platform binaries live under `dist/`; checksums and the SBOM are generated from
the exact tag.

| Platform | Artifact |
|----------|----------|
| Linux x86_64 | `bridgesessions-linux-x86_64` |
| Windows x86_64 | `bridgesessions-windows-x86_64.exe` |
| macOS arm64 | `bridgesessions-macos-arm64` |
| Source | `bridgesessions-26.08.05-beta1-source.tar.gz`, `bridgesessions-26.08.05-beta1-source.zip` |

```bash
cd /path/to/downloaded-release-assets
sha256sum -c SHA256SUMS
# Linux example
install -m 0755 bridgesessions-linux-x86_64 ~/.local/bin/bridgesessions
ln -sfn ~/.local/bin/bridgesessions ~/.local/bin/bs
bridgesessions --version   # → 26.08.05-beta1
bridgesessions keygen
```

| Platform | Notes |
|----------|--------|
| Linux | Dynamic link to system OpenSSL/zstd/fmt/spdlog |
| Windows | MinGW-static OpenSSL+zstd; place `.exe` on `PATH` |
| macOS arm64 | Homebrew OpenSSL/fmt/spdlog dylibs; zstd linked statically (or rebuild) |

Provenance and build notes: [docs/RELEASE-PROVENANCE.md](docs/RELEASE-PROVENANCE.md) ·  
Release notes: [CHANGELOG.md](CHANGELOG.md) (26.08.05-beta1 entry)

## Build from source

```bash
# Linux / macOS (needs OpenSSL, zstd, fmt, spdlog, CLI11, nlohmann-json)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bridgesessions --version  # → 26.08.05-beta1
```

Or: `./build.sh` on Linux. Windows MinGW and macOS flags:
[docs/building.md](docs/building.md). Release artifacts must pass
`scripts/release-checksums.sh`.

## Quickstart

```bash
# 1. Generate a keypair (once)
bridgesessions keygen

# 2. Point a node at a seed peer and run it
bridgesessions --config ~/.bridgesessions/config
```

`~/.bridgesessions/config`:

```ini
node.name   my-laptop
node.listen 192.0.2.10:19949
seed peer-a <seed-peer-host>:19949 pubkey=<64-hex-ed25519-public-key>
sessions.default_shell /bin/bash -l
```

```bash
# 3. Attach from anywhere
bridgesessions shell <server> --name hms
# data-plane health (not IPC-only)
bridgesessions health <peer>   # → healthy (data-plane ok)
```

See [docs/usage.md](docs/usage.md) for the full command reference and
[docs/configuration.md](docs/configuration.md) for the config reference.

## What's new in 26.08.05-beta1

### CUA — Computer-Use Automation

Remote desktop automation over the mesh. Seven subcommands — no VNC/RDP needed.

```bash
bs cua screen <peer>                          # get screen dimensions
bs cua capture <peer> -o shot.png             # screenshot to file (or stdout)
bs cua click <peer> --x 500 --y 300           # left/right/middle click
bs cua move <peer> --x 500 --y 300            # move cursor
bs cua type <peer> --text "hello world"       # type UTF-8 text
bs cua key <peer> --code 40 --modifiers ctrl  # press HID key code with modifiers
bs cua scroll <peer> --direction down --amount 5  # scroll mouse wheel
```

**Windows/macOS:** requires `--cua-helper` running in the user session (input
injection + capture). See [docs/cua.md](docs/cua.md) for full setup.

### Run-script — Remote Script Execution

Send a local script to a peer and execute it with auto-detected interpreter
(bash, PowerShell, Python). Script body is base64-encoded — no escaping issues.

```bash
bs run-script <peer> deploy.sh                # auto-detect interpreter from extension/shebang
bs run-script <peer> script.py --interpreter python3
echo 'Get-Process | Select -First 5' | bs run-script <peer> - --interpreter powershell
```

### Peer Resolution — 4-Tier Fuzzy Matching

Peer names resolve through four tiers so you don't need exact matches:

| Tier | Method | Example |
|------|--------|---------|
| 1 | Exact (case-insensitive) | `test-pc1` → `test-pc1` |
| 2 | *(reserved for config aliases)* | — |
| 3 | Hyphen-segment suffix/prefix | `shadow` → `windows-peer` |
| 4 | Levenshtein ≤ 2 (typo-tolerant) | `shadwo` → `shadow` |

Ambiguous matches return suggestions instead of guessing.

### File Transfer — Pipelined + Direct TLS

- **Pipelining:** 8-chunk (384 KB) batching per ack round-trip — 4–8× faster on
  high-latency links.
- **Direct TLS:** `file recv` and `capture-video` route over direct pinned TLS
  to the target peer, not daemon IPC.

## Documentation

| Document | What it covers |
|---|---|
| **[docs/why-bridge-sessions.md](docs/why-bridge-sessions.md)** | Why it replaces SSH/MOSH/SCP/tmux/WinRM + AI workflows. |
| [docs/design.md](docs/design.md) | Design, ADRs, component model. |
| [docs/building.md](docs/building.md) | Compile on Linux / Windows / macOS. |
| [docs/usage.md](docs/usage.md) | CLI reference and workflows. |
| [docs/cua.md](docs/cua.md) | CUA commands: screen, capture, click, move, type, key, scroll. |
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | Quick reference for LLMs and humans. |
| [docs/configuration.md](docs/configuration.md) | Config file reference. |
| [docs/protocol.md](docs/protocol.md) | The `bs://` wire protocol. |
| [docs/bridge-panel.md](docs/bridge-panel.md) | The Bridge Panel web surface. |
| [docs/cua-signal-scenarios.md](docs/cua-signal-scenarios.md) | Ctrl-C and signal-forward/detach behavior. |
| [docs/RELEASE-PROVENANCE.md](docs/RELEASE-PROVENANCE.md) | Release provenance and build matrix. |

## Releases

Release candidates are accepted only when:

1. Embedded version equals [`VERSION`](VERSION)
2. `sha256sum -c SHA256SUMS` passes in the downloaded release bundle
3. The downloaded `SBOM-binaries.json` is valid CycloneDX 1.5

**26.08.05-beta1** ships Linux x86_64, Windows x86_64, and macOS arm64 from this
source. Prefer the annotated git tag `v26.08.05-beta1` over floating branch tips.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bug reports and pull requests are
welcome.

## Security

Found a vulnerability? Please follow the disclosure process in
[SECURITY.md](SECURITY.md).
