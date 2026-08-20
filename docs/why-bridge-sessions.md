# Why BridgeSessions

Remote work often combines a transport, a multiplexer, a file copier, platform-specific Windows automation, and ad hoc agent tooling. BridgeSessions puts the common path behind one peer identity and one CLI.

| Need | Traditional stack | BridgeSessions |
|---|---|---|
| encrypted shell | SSH | mutual-TLS mesh shell |
| survive disconnect | tmux/Zellij | server-owned named PTY/ConPTY |
| copy files | SCP/SFTP | resumable, hash-verified file protocol |
| Windows automation | WinRM/RDP helpers | same peer mesh plus CUA helper |
| agent artifacts | paste/base64/chat uploads | files, capture, and Bridge Panel |

## The useful difference

The network connection is not the session. The remote daemon owns the terminal and child process; clients attach and detach. A laptop sleep, Wi-Fi change, or client restart does not require restarting the shell or agent TUI.

```bash
bs shell build-peer --name agent
# detach, reconnect later
bs shell build-peer --name agent
```

Files and desktop captures use the same pinned peer identity:

```bash
bs file send build-peer ./artifact.bin --wait
bs cua capture desktop-peer -o screen.png
```

## Built for operator-controlled meshes

- One Ed25519 identity per node.
- Explicit seed pins and inbound `authorized_keys`.
- One TCP port for mesh traffic.
- Linux, macOS, and Windows peers.
- Structured progress/output that agents can parse.

This is not a public multi-tenant remote-access service. Authorizing a peer is close to granting shell access; the security boundary is the peer key and the host account.

## Agent workflow

A typical loop is:

1. attach to a durable build/session peer,
2. run or upload work,
3. capture a remote desktop when GUI context matters,
4. transfer the artifact,
5. publish a concise report to Bridge Panel.

Bridge Panel is optional and loopback-only by default. It is a document review surface, not a new trust root.

## Trade-offs

- TCP/TLS favors deployability over MOSH-style UDP roaming.
- `select()` and host-level authorization target small trusted fleets, not internet scale.
- CUA depends on user-session helpers and OS permissions.
- A compromised authorized node can affect other nodes that trust it.
- Beta releases require active upgrade discipline.

See [Design](design.md), [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md), and [Quickstart](QUICKSTART.md).
