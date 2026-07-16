# BridgeSessions Documentation

BridgeSessions is a mesh terminal relay: a single C++23 binary that gives you
persistent, encrypted, multi-session terminal access to remote Linux hosts over
a TLS 1.3 mesh — no SSH, no mosh, no tmux/zellij in the path.

## Contents

| Document | What it covers |
|---|---|
| [Architecture](architecture.md) | Design, components, ADRs, and the `bs://` protocol model. |
| [Building](building.md) | Compile from source on Linux, Windows (MinGW), and macOS. |
| [Usage](usage.md) | Command-line reference and everyday workflows. |
| [Configuration](configuration.md) | Server and client config file reference. |
| [Protocol](protocol.md) | Wire format, message types, and stream multiplexing. |
| [Bridge Panel](bridge-panel.md) | The `bridgepanel` web surface for publishing documents to peers. |

## Quick orientation

- **`bridgesessions`** — the all-in-one binary (client + server + doctor).
- **`bs-server`** — the remote Linux daemon (session multiplexer).
- **`bs-client`** — the relay agent that attaches a local terminal to a session.
- **`bs-transport` / `bs-protocol`** — the shared TLS transport and codec libraries.

See the [README](../README.md) for a 60-second start.
