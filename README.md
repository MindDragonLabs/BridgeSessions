# BridgeSessions

[![Release](https://img.shields.io/github/v/release/MindDragonLabs/BridgeSessions?include_prereleases&label=release)](https://github.com/MindDragonLabs/BridgeSessions/releases)
[![License: BUSL-1.1](https://img.shields.io/badge/License-BUSL--1.1-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-informational)](docs/building.md)

**Persistent shells, verified files, and desktop automation across a trusted peer mesh.**

`bridgesessions` (command: `bs`) is one C++23 program. It is the mesh daemon and the CLI. It runs on Linux, macOS, and Windows. A remote session keeps its PTY or ConPTY when the client disconnects.

> **Beta software.** An authorized peer has near-interactive host access. Use BridgeSessions only on machines and networks that you control. Read [SECURITY.md](SECURITY.md) before you join a mesh.

The current release tag is on the [releases page](https://github.com/MindDragonLabs/BridgeSessions/releases). `VERSION` in the repo root is the version built from source. Do not trust a hardcoded version string in documentation; run `bs --version`.

---

## Table of contents

- [What it does](#what-it-does)
- [Supported platforms](#supported-platforms)
- [Install](#install)
- [First mesh](#first-mesh)
- [Always-online seed](#always-online-seed)
- [Commands](#commands)
- [Files and the receive directory](#files-and-the-receive-directory)
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
| Folder mirroring | `bs sync pair` and `bs vfolder` keep directories aligned across hosts. |
| Multi-step jobs | `bs job run` executes an ordered JSON command list on a peer. |
| Remote editing | `bs edit` opens a peer's file in your local editor and writes it back. |
| Review UI | Optional Bridge Panel lists and previews files on each peer. |

One Ed25519 identity identifies each node. Mesh traffic uses mutual TLS on TCP port **19949**.

## Supported platforms

| Platform | Artifact | Notes |
|---|---|---|
| Linux x86_64 | `bridgesessions-linux-x86_64` | Built on Ubuntu 22.04. Runs on glibc 2.35 and newer: Debian 12+, Ubuntu 22.04/24.04, Arch, Fedora 38+. |
| macOS arm64 | `bridgesessions-macos-arm64` | Developer ID signed. macOS 13 and newer. |
| Windows x86_64 | `bridgesessions-windows-x86_64.exe` | Statically linked. Imports only OS DLLs. Windows 10 and newer. |

The Linux artifact links `libssl.so.3` and `libcrypto.so.3` from the host. Every supported distribution provides those; nothing else is needed at runtime.

`bs cua` needs a helper process inside the interactive desktop session on Windows and macOS (`bs --cua-helper`). On Linux the daemon already runs in the user session, so no helper is required.

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
BRIDGESESSIONS_TAG=<tag> \
  bash -c 'curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash'

# Windows PowerShell
$env:BRIDGESESSIONS_TAG = '<tag>'
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

### Check the install

```bash
bs --version
bs doctor
```

The installer also removes any stale copy of the binary that appears earlier on `PATH`. Two versions on `PATH` is the most common cause of a node that reports the wrong version.

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

## Commands

Full reference with every flag: [docs/cli.md](docs/cli.md). It is generated from the binary's own `--help`, so it cannot drift.

### Everyday use

```bash
bs peers list                     # known peers and their state
bs health <peer>                  # TLS + identity + data-plane check
bs fleet                          # live directory with CPU, memory, disk, load
bs shell <peer>                   # attach, or start a session
bs shell <peer> --name agent      # named session; reattach with the same name
bs shell <peer> --cmd 'uname -a'  # one-shot command
bs connect                        # pick a peer, then a launch harness
```

`Ctrl-D` detaches. The remote session stays alive. Reuse the same `--name` to reattach.

Stack dependent work in one remote command, or use `bs run-script`. Separate `bs shell --cmd` calls do not share working directory or environment.

### All 31 commands

| Command | What it does |
|---|---|
| `shell` | Open an interactive shell, or run one command, on a peer. |
| `connect` | Two-step picker: choose a peer, then a launch harness. |
| `sessions` | List sessions here, or on a peer. |
| `run-script` | Send a script to a peer and execute it there. |
| `job` | Run an ordered JSON command list on a peer. |
| `script` | Content-addressed script cache. |
| `edit` | Edit a peer's file in your local editor, then write it back. |
| `file` | `send` and `recv` with SHA-256 verification and resume. |
| `sync` | `sync pair` mirrors a local directory to a peer, with a dry-run manifest. |
| `vfolder` | Virtual folder sync for a local directory. |
| `cua` | Screen capture and input injection: `screen`, `capture`, `click`, `move`, `type`, `key`, `scroll`. |
| `capture-video` | Record a peer's screen to a video file. |
| `image` | Render an image inline in the terminal. |
| `anim` | Play a GIF inline in the terminal. |
| `pane` | Publish content to the peer's Bridge Panel. |
| `api` | Query the daemon JSON API. |
| `peers` | List, add, and remove seed peers. |
| `health` | Ping/pong health check against a peer. |
| `fleet` | Live fleet directory gathered by the daemon. |
| `reconnect` | Tear down and re-handshake one peer. |
| `stats` | Local daemon statistics. |
| `telemetry` | Transfer telemetry and byte counters. |
| `invite` | Generate a single-use invite token. |
| `join` | Join a mesh with an invite token. |
| `enroll` | Vouch for a new member out of band. |
| `authorize` | Authorize a peer's public key for direct connections. |
| `keygen` | Generate this node's Ed25519 identity. |
| `rotate-identity` | Regenerate identity keys. Peers must re-pin. |
| `doctor` | Check local configuration. |
| `upgrade` | Self-update from GitHub releases. |
| `--cua-helper` | Run the desktop helper in the user session (Windows, macOS). |

## Files and the receive directory

A peer serves and accepts files under its **receive directory**, `~/.bridgesessions/received` by default, or whatever `receive_dir` sets.

```bash
bs file send <peer> ./artifact.bin --wait
bs file send <peer> ./artifact.bin --dest reports/artifact.bin --wait
bs file recv <peer> reports/artifact.bin --to ./artifact.bin --wait
```

- Success is the final `OK` line after the SHA-256 check. A progress line is not success.
- `--dest` is a path **under the peer's receive directory**, not an absolute path, unless the peer allows more (`file.dest_allow_home`).
- The `OK` line reports `dest=` as the path relative to that directory, so the file is always findable. When the peer is too old to confirm the destination, the sender prints a warning instead of guessing.
- The receive directory is a **staging area**, not storage. Every transfer is also written to the caller's own destination, so a copy left behind doubles the disk cost. Files older than `receive_retention_hours` (default 24) are removed by an hourly sweep. Set it to `0` to keep them forever. Partial transfers (`.part`, `.part.bsmeta`) are never removed.

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
2. Confirm `bs --version` prints the version you installed.
3. If this node is new, join with `bs join <seed-address>:19949 <token> --start`.
4. If this node is the seed, run `bs invite` and give the token to the joining node only.
5. Confirm `bs peers list` and `bs health <peer>`.
6. Prefer `bs run-script` for multi-step remote work.
7. For file copy, wait for the final `OK`, and read the `dest=` path it reports.
8. Optional: install the portable skill with `scripts/install-agent-skill.sh`.

The agent skill lives at [skills/bridgesessions/SKILL.md](skills/bridgesessions/SKILL.md). It follows the Agent Skills layout. `scripts/install-agent-skill.sh` links it into the harness search paths on the machine you run it on.

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

One script builds every platform. It resolves all dependencies itself: a fresh clone needs a compiler, CMake 3.25 or newer, git, and network access. Nothing else is hand-installed.

```bash
./build.sh linux --distro ubuntu:22.04    # release Linux artifact (glibc 2.35 floor)
./build.sh linux                          # native build on this host
./build.sh macos                          # macOS (must run on macOS)
./build.sh windows                        # cross-compile with mingw-w64
./build.sh all                            # every target this host can produce
./build.sh package                        # build all, stage dist/ and SHA256SUMS
./build.sh test                           # configure, build, run ctest
./build.sh deps                           # print the pinned dependency set
```

Dependencies are pinned in [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake) and built from source by default, so the binary does not depend on the host's `libspdlog.so.1`, `libfmt.so.8`, or any other distribution-specific soname. spdlog is built with its bundled fmt for that reason. `bs` links only `libssl.so.3` and `libcrypto.so.3` from the system.

Useful options:

```bash
./build.sh linux --deps system    # fast local iteration against installed packages
./build.sh linux --no-tests       # skip ctest
./build.sh windows --no-strip     # keep debug symbols
./build.sh --help
```

Platform notes and signing: [docs/building.md](docs/building.md).
Release procedure: [docs/RELEASE-PROVENANCE.md](docs/RELEASE-PROVENANCE.md).

Generated binaries, app bundles, checksums, and SBOMs are not committed. They belong in GitHub Releases.

## Documentation

| Document | Contents |
|---|---|
| [Command reference](docs/cli.md) | Every command and flag, generated from `--help` |
| [Quickstart](docs/QUICKSTART.md) | Install, join, first shell |
| [Usage](docs/usage.md) | Task-oriented walkthrough |
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
