# BridgeSessions

**Persistent shells, verified files, and computer-use automation across a trusted peer mesh—one C++23 executable.**

`bridgesessions` (usually `bs`) is both daemon and CLI on Linux, macOS, and Windows. Sessions keep their PTY/ConPTY when clients disconnect.

> **Beta software.** An authorized peer has near-interactive host access. Use it only on machines and networks you control; read [SECURITY.md](SECURITY.md).

## Features

- Named persistent sessions, scrollback, and multi-attach
- Ed25519 mutual TLS with explicit peer pins
- Resumable SHA-256-verified file transfer
- Linux/macOS/Windows peers in one mesh
- Remote screenshots and input through `bs cua`
- Optional loopback Bridge Panel for Markdown review

## Install

Release binaries and `SHA256SUMS` are GitHub Release assets. Installers fail closed on verification errors.

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

## Join and use

```bash
# Existing pinned seed
bs invite

# New node
bs join <seed-address>:19949 <single-use-token> --start

bs peers list
bs health <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'hostname && uptime'
bs file send <peer> ./artifact.bin --wait
bs file recv <peer> received/report.md --to ./report.md --wait
bs run-script <peer> ./task.sh
bs cua capture <peer> -o screen.png
```

`Ctrl-D` detaches without killing the remote session. Reuse the same `--name` to reattach.

## Security model

- One Ed25519 identity per node
- Inbound keys must be in `authorized_keys`, except during a bounded invite join
- Outbound certificate, Hello key/name, and configured pin must agree
- Local daemon IPC is loopback-only and token-authenticated
- Remote file serving is confined to `receive_dir` unless explicitly weakened
- Authorization is host-level, not per-command
- Current cross-platform compatibility profile negotiates TLS 1.2

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/prepublish-scan.sh
```

## Documentation

[Quickstart](docs/QUICKSTART.md) · [Usage](docs/usage.md) · [Configuration](docs/configuration.md) · [Design](docs/design.md) · [Protocol](docs/protocol.md) · [CUA](docs/cua.md) · [Building](docs/building.md) · [Audit](AUDIT.md)

Generated binaries, app bundles, archives, checksums, and SBOMs are ignored by git and published through GitHub Releases.

## License

Business Source License 1.1. See [LICENSE](LICENSE).
