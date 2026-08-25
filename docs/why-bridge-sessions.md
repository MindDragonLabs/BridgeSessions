# Why BridgeSessions

Remote work often combines a transport, a multiplexer, a file copier, platform-specific Windows automation, and ad hoc agent tooling. BridgeSessions puts the common path behind one peer identity and one CLI. The point is not "yet another remote shell." The point is that the identity, the session, and the file move all live on the same pinned mesh.

## Compared to a traditional stack

| Need | Traditional stack | BridgeSessions |
|---|---|---|
| Encrypted shell | SSH | Mutual-TLS mesh shell with pinned Ed25519 |
| Survive disconnect | `tmux` or `zellij` on top of SSH | Server-owned named PTY/ConPTY, attachments are temporary |
| Copy files | SCP/SFTP | Resumable, hash-verified file protocol |
| Run a script on a peer | `ssh host 'bash -s'` | `bs run-script` ships the file and runs it |
| Windows automation | WinRM, RDP helpers | Same peer mesh plus CUA helper |
| Desktop capture | separate RDP/VNC session | `bs cua capture`, `bs capture-video` |
| Agent artifacts | paste, base64, chat uploads | Files, captures, and the optional Bridge Panel |

Each row is a real workflow that operators assemble from parts. BridgeSessions is the assembly done.

## The useful difference: the connection is not the session

The remote daemon owns the terminal and the child process. Clients attach and detach. A laptop sleep, a Wi-Fi change, or a client restart does not require restarting the shell or the agent TUI inside it. The same named session is waiting on the other side.

```bash
bs shell build-peer --name agent
# network drops, laptop sleeps, you walk away
# come back later
bs shell build-peer --name agent
```

The remote state survives. The agent inside the shell kept running.

## Files and desktop use the same identity

Files, desktop captures, and CUA input travel on the same pinned peer identity. There is no separate service to authenticate to and no second authorization boundary to maintain.

```bash
bs file send build-peer ./artifact.bin --wait
bs cua capture desktop-peer -o screen.png
```

The CLI, the daemon, and the helper all check the same Ed25519 pin.

## Built for operator-controlled meshes

- One Ed25519 identity per node.
- Explicit seed pins and inbound `authorized_keys`.
- One TCP port for mesh traffic.
- Linux, macOS, and Windows peers.
- Structured progress and output that scripts and agents can parse.

This is not a public multi-tenant remote-access service. Authorizing a peer is close to granting shell access. The security boundary is the peer key and the host account.

## A typical agent loop

A coding agent or a build pipeline usually repeats the same shape:

1. Attach to a durable build or session peer with `--name`.
2. Run or upload work in that session.
3. Capture a remote desktop when the workflow needs GUI context.
4. Transfer the artifact back, wait for the final `OK`.
5. Publish a concise report to Bridge Panel.

Each step uses the same mesh identity and the same CLI. There is no glue to write between SSH, SCP, RDP, and a clipboard sync tool.

## When BridgeSessions is the wrong tool

- You need a public multi-tenant service. BridgeSessions assumes operator-controlled peer sets.
- You have a single machine and no need for persistent sessions. A local shell is enough.
- You need MOSH-style UDP roaming across aggressive NAT. BridgeSessions uses TCP/TLS and application-level reattach.
- You want a managed SaaS. There is no managed service to subscribe to.

## Trade-offs

- TCP/TLS favors deployability over MOSH-style UDP roaming. Sleep recovery works through application-level reattach, not transport-level migration.
- A single event loop keeps reasoning simple but caps concurrency per process. The mesh scales by adding nodes, not threads.
- Authorization is host-level, not capability-scoped. Adding per-command policy is a feature for later; today the peer key is the boundary.
- CUA depends on user-session helpers and OS permissions. The desktop lane is the most setup-heavy part of the system.
- A compromised authorized node can affect every node that trusts it. The mesh does not pretend to be safer than the keys in `authorized_keys`.
- Beta releases require active upgrade discipline. The protocol is additive, but operators upgrade.

## Operating posture

The mesh assumes you control the peers and the network between them. A typical deployment runs on a private LAN or a VPN. Exposing port `19949` to the public internet is allowed only when the operator accepts the corresponding risk.

## Related

- [Quickstart](QUICKSTART.md) — install, join, first shell
- [Design](design.md) — architecture and core decisions
- [Protocol](protocol.md) — wire format and handshake
- [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md) — trust boundary and reporting
- [Building](building.md) — compile and sign