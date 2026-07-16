# BridgeSessions

**Mesh terminal relay** — a single C++23 binary that gives you persistent,
encrypted, multi-session terminal access to remote Linux hosts over a TLS 1.3
mesh. No SSH, no mosh, no tmux/zellij in the connection path.

- **Persistent sessions** — disconnect and reattach; your PTY keeps running.
- **Encrypted mesh** — ed25519 mutual TLS 1.3, forward secrecy.
- **One binary** — client, server, and `doctor` in `bridgesessions`.
- **Clipboard** — two-way, hash-deduped, no echo loops.
- **Bridge Panel** — publish Markdown to peers over a web surface.

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