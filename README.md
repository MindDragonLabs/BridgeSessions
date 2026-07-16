# BridgeSessions

**Mesh terminal relay** — one C++23 binary that replaces the usual remote stack
(**SSH + MOSH + SCP + tmux/Zellij + WinRM**) with a single secure mesh
(`bs://` over TLS 1.3). Built for humans **and** AI agents: durable PTYs,
file/image/video transfer, Windows CUA peers, and **Bridge Panel** for long
Markdown reviews.

- **Persistent sessions** — disconnect and reattach; the PTY keeps running (tmux’s job, built in).
- **Encrypted mesh** — ed25519 mutual TLS 1.3, forward secrecy (SSH’s job, fleet-native).
- **Files + media** — on-protocol transfer for logs, screenshots, and video (SCP + vision I/O).
- **Windows + Linux + macOS** — one mesh for shells and desktop automation (WinRM-class peers).
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

## Install from binary

Download the latest release for your platform:

- Linux (x86_64): `bridgesessions`
- macOS (arm64): `bridgesessions-macos-arm64`
- Windows (x86_64): `bridgesessions-windows-x86_64.exe`

Place the binary in your `$PATH` and run:

```bash
bridgesessions --version   # → 2.0.0
bridgesessions keygen
```

## Build from source

```bash
# Linux / macOS (needs libssl, zstd, fmt, spdlog)
./build.sh
./bridgesessions --version        # → 2.0.0
```

Windows (x86_64) cross-compiles from Linux with MinGW. Full instructions,
including CMake and the modular `bs-*` tree, are in
[docs/building.md](docs/building.md).

## Quickstart

```bash
# 1. Generate a keypair (once)
bridgesessions keygen

# 2. Point a node at a seed peer and run it
bridgesessions --config ~/.bridgesessions/config
```

`~/.bridgesessions/config`:

```ini
mesh.node_name   my-laptop
mesh.listen      19949
seed <seed-peer-host>:19949
sessions.default_shell /bin/bash -l
```

```bash
# 3. Attach from anywhere
bridgesessions shell <server> --name hms
```

See [docs/usage.md](docs/usage.md) for the full command reference and
[docs/configuration.md](docs/configuration.md) for the config reference.

## Documentation

| Document | What it covers |
|---|---|
| **[docs/why-bridge-sessions.md](docs/why-bridge-sessions.md)** | Why it replaces SSH/MOSH/SCP/tmux/WinRM + AI workflows. |
| [docs/design.md](docs/design.md) | Design, ADRs, component model. |
| [docs/building.md](docs/building.md) | Compile on Linux / Windows / macOS. |
| [docs/usage.md](docs/usage.md) | CLI reference and workflows. |
| [docs/configuration.md](docs/configuration.md) | Config file reference. |
| [docs/protocol.md](docs/protocol.md) | The `bs://` wire protocol. |
| [docs/bridge-panel.md](docs/bridge-panel.md) | The Bridge Panel web surface. |

## Releases

Prebuilt binaries for **Linux (x86_64)**, **Windows (x86_64)**, and
**macOS (arm64)** ship in [`dist/`](dist/) and with each Codeberg release tag
(`vX.Y.Z`).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bug reports and pull requests are
welcome.

## Security

Found a vulnerability? Please follow the disclosure process in
[SECURITY.md](SECURITY.md).