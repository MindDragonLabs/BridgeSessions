# BridgeSessions Documentation

BridgeSessions is a mesh terminal relay: a single C++23 binary that gives you
persistent, encrypted, multi-session terminal access across Linux, macOS, and
Windows — no SSH, no mosh, no tmux/zellij in the connection path.

## Start here

| Document | What it covers |
|---|---|
| **[Why BridgeSessions](why-bridge-sessions.md)** | Replaces SSH + MOSH + SCP + tmux/Zellij + WinRM; AI media; Bridge Panel. |
| [Design](design.md) | Design, components, ADRs, and the `bs://` protocol model. |
| [Building](building.md) | Compile from source on Linux, Windows (MinGW), and macOS. |
| [Usage](usage.md) | Command-line reference and everyday workflows. |
| [Configuration](configuration.md) | Server and client config file reference. |
| [Protocol](protocol.md) | Wire format, message types, and stream multiplexing. |
| [Bridge Panel](bridge-panel.md) | The Bridge Panel web surface for publishing documents to peers. |

## Quick orientation

- **`bridgesessions`** — the all-in-one binary (client + server + doctor).
- **`bs-server`** — the remote Linux daemon (session multiplexer).
- **`bs-client`** — the relay agent that attaches a local terminal to a session.
- **`bs-transport` / `bs-protocol`** — the shared TLS transport and codec libraries.

## Screenshots — Bridge Panel

![Bridge Panel — read view](assets/bridgepanel-read.png)

*Sessions tree, breadcrumb, Edit/Copy tools, rendered Markdown.*

![Bridge Panel — edit view](assets/bridgepanel-edit.png)

*Inline Markdown editor with Save / Cancel for agent-human document review.*

## Demo trailer

[![BridgeSessions product demo](assets/demo-install-ai-mesh.gif)](assets/demo-install-ai-mesh.mp4)

Product demo — the old stack collapses into one mesh; Windows CUA + media transfer + vision path; Bridge Panel.  
Full quality: **[MP4 · 1080p](assets/demo-install-ai-mesh.mp4)** · [file view with player](https://github.com/MindDragonLabs/BridgeSessions/blob/main/docs/assets/demo-install-ai-mesh.mp4)

See the [README](../README.md) for a 60-second start.
