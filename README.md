# BridgeSessions

[![Release](https://img.shields.io/github/v/release/MindDragonLabs/BridgeSessions?include_prereleases&label=release)](https://github.com/MindDragonLabs/BridgeSessions/releases)
[![License: BUSL-1.1](https://img.shields.io/badge/license-BUSL--1.1-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-informational)](docs/building.md)

**Persistent shells, verified files, and desktop automation across a trusted peer mesh.**

`bridgesessions` (command: `bs`) is one C++23 program. It is the mesh daemon and the CLI. It runs on Linux, macOS, and Windows. A remote session keeps its PTY or ConPTY when the client disconnects.

> **Beta software.** An authorized peer has near-interactive host access. Use BridgeSessions only on machines and networks that you control. Read [SECURITY.md](SECURITY.md) before you join a mesh.

Current release tag: **`26.09.01-release`**.

---

## Table of contents

- [What it does](#what-it-does)
- [Install](#install)
- [First mesh](#first-mesh)
- [Always-online seed](#always-online-seed)
- [Everyday commands](#everyday-commands)
- [Bridge Panel](#bridge-panel)
- [Install for AI agents](#install-for-ai-agents)
- [Security model](#security-model)
- [Build from source](#build-from-source)
- [Documentation](#documentation)
- [License](#license)

## What it does

| Need | What `bs` provides |
|---|---|
| Remote shell | Named persistent PTY/ConPTY. Detach with `Ctrl-D`. Reattach by name. |
| File copy | Resumable transfer with SHA-256 check. Final `OK` is the success signal. |
| Remote script | `bs run-script` sends a file and runs it with the right interpreter. |
| Desktop control | `bs cua` captures the screen and sends input on Windows and macOS. |
| Review UI | Optional Bridge Panel lists inbox files on each peer. |

One Ed25519 identity identifies each node. Mesh traffic uses mutual TLS on TCP port **19949**.

## Install

Release binaries and `SHA256SUMS` are GitHub Release assets. The installer fails closed if the checksum or the embedded version does not match.

### Linux and macOS

```bash
curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash
```

The script installs `bridgesessions` to `~/.local/bin` and creates a `bs` symlink. Add `~/.local/bin` to `PATH` if `bs` is not found.

### Windows (PowerShell)

```powershell
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

### Pin a tag

```bash
# Linux / macOS
BRIDGESESSIONS_TAG=26.09.01-release \
  bash -c 'curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash'

# Windows PowerShell
$env:BRIDGESESSIONS_TAG = '26.09.01-release'
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

### Check the install

```bash
bs --version
bs doctor
```

The version string must contain `26.09.01-release`.

macOS release binaries are Developer ID signed. The installer does not re-sign the file. Re-signing on a machine without the Developer ID certificate strips the seal and can make Gatekeeper kill the process.

## First mesh

You need one **seed** node and one **joining** node.

**On the seed** (a machine that already has a pinned identity):

```bash
bs invite
```

The command prints a listen address and a single-use token. Use the token at once. The join window is short.

**On the new node:**

```bash
bs join <seed-address>:19949 <single-use-token> --start
```

`--start` starts the local daemon after the join succeeds. The seed signs a mesh-directory enrollment. Other peers learn the new key by gossip. You do not copy keys by hand.

**Check the link:**

```bash
bs peers list
bs health <peer>
```

`health` must report data-plane health. A local IPC reply alone is not enough.

Only a pinned seed can issue an enrollment that the mesh accepts. `bs enroll` is an out-of-band admin path. It is not the normal install flow.

## Always-online seed

Pick one machine that stays powered and reachable. Call it the seed. Other nodes join that seed.

### Choose the host

Use a small always-on computer. A home lab server, a VPS that you control, or a desk Mac Mini are typical. The seed must:

1. Stay on across laptop sleep.
2. Have a stable address on your private network or VPN.
3. Bind the mesh port on that address. Do not bind only `127.0.0.1` if other nodes must connect.

### Bind and persist

```ini
# ~/.bridgesessions/config  (example addresses are documentation-only)
node.name seed-a
node.listen 192.0.2.10:19949
mesh.require_seed_pins true
seed seed-a 192.0.2.10:19949 pubkey=<this-node-ed25519-public-key>
```

Start the daemon through the platform service, not through an interactive shell:

| Platform | Service |
|---|---|
| Linux | systemd user unit `bridgesessions.service` with lingering enabled for that user |
| macOS | launchd agent `com.bridgesessions.mesh` |
| Windows | Scheduled Task `BridgeSessions` with `ExecutionTimeLimit=0` |

On Linux, enable lingering so the user daemon survives logout:

```bash
loginctl enable-linger "$USER"
systemctl --user enable --now bridgesessions.service
```

Do not persist-disable the unit during an upgrade. A disable plus a failed resume leaves the seed with no listener. Inbound `bs shell` then fails with connection refused.

### How other nodes use the seed

1. On the seed, run `bs invite`.
2. On each new node, run `bs join <seed-address>:19949 <token> --start`.
3. Confirm `bs health seed-a` from the new node.
4. Confirm `bs peers list` shows the new name as a seed.

Keep `mesh.require_seed_pins` enabled. Do not turn it off on an untrusted network.

## Everyday commands

```bash
bs peers list
bs health <peer>
bs fleet
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'uname -a'
bs file send <peer> ./artifact.bin --wait
bs file recv <peer> received/report.md --to ./report.md --wait
bs run-script <peer> ./task.sh
bs cua capture <peer> -o screen.png
```

`Ctrl-D` detaches. The remote session stays alive. Reuse the same `--name` to reattach.

Success for a file transfer is the final `OK` after the SHA-256 check. A progress line is not success.

Stack dependent work in one remote command, or use `bs run-script`. Separate `bs shell --cmd` calls do not share working directory or environment.

## Bridge Panel

Bridge Panel is an optional local web UI. It is not required to run the mesh.

The left column lists mesh hosts. The middle column lists files. The right pane previews markdown, images, video, PDF, and text. You can edit files only in a writable root (the inbox, or an ACL outbox that you configure).

### Start the panel

From a clone of this repository:

```bash
python3 -m tools.bridgepanel
```

The default bind is loopback. A typical operator bind is the VPN address on port **9770**. Do not bind `0.0.0.0`. Do not put the panel on the public internet.

The process prints a URL that includes a bearer token. Open that URL in a browser on the same trusted network.

If the client address is in `BRIDGEPANEL_TRUSTED_IPS`, read calls may skip the token. Writes still require the bearer. Treat that list as a read-trust list.

### What you can do

- Select a host in the left column.
- Open the inbox (`received/`) or another allowed volume root.
- Filter by markdown, image, video, or search by name.
- Preview markdown (Toast UI) and code (CodeMirror).
- Create a file or folder in a writable root.
- Rename or move a file to trash in a writable root.
- Upload with the file picker or drag and drop.
- Download the selected file.

Writes stay in the inbox unless you add a writable root in `browse_roots.json`. Other volumes are read-only.

### Stay running

Use a systemd user unit or an equivalent supervisor. Restart the unit after you pull panel code. The mesh daemon and the panel are separate processes.

Full operator notes: [docs/bridge-panel.md](docs/bridge-panel.md).

## Install for AI agents

Give an agent these exact steps. Do not ask the agent to compile unless you want a development build.

1. Install the release binary with `scripts/install.sh` or `scripts/install.ps1`.
2. Confirm `bs --version` prints `26.09.01-release`.
3. If this node is new, join with `bs join <seed-address>:19949 <token> --start`.
4. If this node is the seed, run `bs invite` and give the token to the joining node only.
5. Confirm `bs peers list` and `bs health <peer>`.
6. Prefer `bs run-script` for multi-step remote work.
7. For file copy, wait for the final `OK`.
8. Optional: install the portable skill with `scripts/install-agent-skill.sh`.

The agent skill lives at `skills/bridgesessions/SKILL.md`. It follows the Agent Skills layout. `AGENTS.md` in the repo root is the always-on contract for coding agents that work in this tree.

Do not put private host names, VPN addresses, or keys in prompts that may be published.

## Security model

- One Ed25519 identity per node.
- Inbound keys must be in `authorized_keys`, except during a bounded invite join.
- Outbound certificate, Hello key/name, and configured pin must agree.
- Local daemon IPC is loopback-only and token-authenticated.
- Remote file serving is confined to `receive_dir` unless you weaken that on purpose.
- Authorization is host-level, not per-command.
- The current compatibility profile negotiates TLS 1.2.

Read [SECURITY.md](SECURITY.md) and [docs/configuration.md](docs/configuration.md).

## Build from source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/prepublish-scan.sh
```

Release platform notes: [docs/building.md](docs/building.md).

Generated binaries, app bundles, checksums, and SBOMs are not committed. They belong in GitHub Releases.

## Documentation

| Document | Contents |
|---|---|
| [Quickstart](docs/QUICKSTART.md) | Install, join, first shell |
| [Usage](docs/usage.md) | Command reference |
| [Configuration](docs/configuration.md) | Config file and directives |
| [Always-online seed](docs/always-online-seed.md) | How to run a central node |
| [Bridge Panel](docs/bridge-panel.md) | How to load and use the file UI |
| [Computer use](docs/cua.md) | Screen capture and input |
| [Design](docs/design.md) | Architecture |
| [Protocol](docs/protocol.md) | Wire protocol |
| [Building](docs/building.md) | Compile and sign |
| [Why BridgeSessions](docs/why-bridge-sessions.md) | Comparison and trade-offs |
| [Release provenance](docs/RELEASE-PROVENANCE.md) | How a release is built |
| [Audit](AUDIT.md) | Current audit notes |
| [Changelog](CHANGELOG.md) | User-visible changes |

## License

Business Source License 1.1. See [LICENSE](LICENSE).
