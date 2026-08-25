# Design

BridgeSessions is one executable with three roles: mesh daemon, client CLI, and persistent session multiplexer. The same binary runs as a service on each peer and as the operator's command on a workstation. There is no separate "server" build.

```text
CLI ── mutual TLS ── peer daemon ── PTY / ConPTY
                            ├── files
                            ├── desktop helper
                            └── peer/session gossip
```

## Core decisions

### Pinned identity

Each node owns an Ed25519 key. Trust is the raw pinned key, not a certificate subject and not a public CA. The certificate carries the public key in the SAN and the Hello frame repeats it; the daemon compares the configured pin, the certificate, and the Hello and refuses the connection if they disagree. Adding or removing a peer is an edit to `authorized_keys` or a fresh invite token. There is no revocation list because there is no certificate hierarchy.

### Persistent sessions

The server owns the terminal and the child process. Client attachments are temporary. Detaching a `bs shell` does not kill the session. The same `--name` reattaches to the same server-side PTY or ConPTY. This is the contract that makes laptop sleep, Wi-Fi change, or a client restart transparent to the remote work.

### Single event loop

Established sockets are non-blocking. Outgoing frames flow through bounded queues. The same loop polls the TLS sockets, the IPC channel, and the PTY worker pipe. There is one thread of execution per process. Cooperative yields replace blocking reads.

### Exclusive TLS ownership

Long-running operations on a transport borrow it through `exec_busy`. No two threads touch the same `SSL*`. This rule keeps the OpenSSL state machine coherent under load and removes a class of races that show up only on multi-core hosts.

### Verified transfers

File metadata is bounded, paths are canonicalized, partial writes use `.part` files, and SHA-256 verification happens before atomic publish. A transport failure that leaves a valid partial preserves it for resume. A validation or write failure removes the partial. The success signal for an operator or a script is the final `OK` line. Progress lines are not success.

### Backpressure

Wire lengths, receive work per tick, TLS transmit, PTY input, worker IPC, and logical output queues are all bounded. A slow peer cannot starve memory. The bounded queues also make it possible to expose failure modes as explicit errors instead of silently dropped bytes.

### Desktop isolation

Windows and macOS CUA uses a token-authenticated loopback helper in the interactive user session. The mesh daemon never injects input or reads the screen directly. The helper is a separate process owned by the logged-in user; the daemon talks to it over loopback IPC. Spectator attachments are rejected before any CUA action.

### Artifact separation

Git contains source, tests, scripts, and docs. Binaries, app bundles, checksums, and SBOMs are GitHub Release assets. The repository never carries compiled artifacts. The release script refuses a dirty tree and a tag/HEAD mismatch.

## Source map

| File | Responsibility |
|---|---|
| `main.cpp` | CLI dispatch, self-upgrade, daemon mode |
| `bs-protocol.h` | codec, TLS, mesh, transfers, IPC |
| `bs-session.h` | session lifetime and scrollback |
| `bs-session-worker.h` | optional per-session child process |
| `bs-cua-helper.h` | desktop helper |
| `bs-logging.h` | structured logging |
| `macos-capture.mm` | macOS ScreenCaptureKit capture |
| `tools/bridgepanel/` | optional web UI for the inbox |

`bs-protocol.h` is the authoritative enum, variant, serializer, and decoder source. Any new message is incomplete until the four agree and round-trip tests cover it. The wire format itself is summarized in [Protocol](protocol.md).

## Operating model

### Bootstrap

Invite/join is the normal flow. A pinned seed runs `bs invite`. The CLI prints a listen address and a single-use token. The new node runs `bs join <addr> <token> --start`. The seed signs a directory enrollment. The mesh gossips the new key; every peer auto-trusts the joiner without a roster edit.

### Mesh maintenance

Peers gossip on a fixed cadence. Liveness combines a `Ping`/`Pong` heartbeat with the data-plane health probe. `bs health <peer>` requires data-plane success; a local IPC reply alone is not enough.

### Sessions

A session is created on first attach or by `bs shell --name`. The server-side process and PTY stay alive across attachments. `Ctrl-D` detaches. `exit` ends the session. The session registry persists session names and last-used metadata in `~/.bridgesessions/sessions.json` so a daemon restart can resume the listing.

### File transfer

The sender publishes metadata and the receiver decides the first chunk to ask for. The sender streams a bounded window. The receiver validates order and size, writes the partial, and acknowledges progress. SHA-256 verification precedes atomic rename.

### Upgrade

`bs upgrade` resolves the latest GitHub Release tag, downloads the binary, checks `SHA256SUMS`, and compares the embedded version before it replaces the running binary. The installer pauses the service with a runtime mask during the swap and re-enables it on every failure path.

## Security model in one paragraph

A peer key in `authorized_keys` is near-interactive host access. The mesh refuses unpinned seeds when `mesh.require_seed_pins` is enabled. Local IPC is loopback-only and token-authenticated. File serving is confined to `receive_dir` unless the operator weakens that on purpose. CUA is rejected for spectator attachments. Release artifacts are Developer ID signed on macOS and SHA-verified on every install path.

## Current limits

- `select()` inherits the platform FD ceiling. The target is small trusted fleets, not thousands of peers on a single daemon.
- Authorization is host-level, not capability-scoped. The mesh does not implement fine-grained per-command policy.
- Cross-platform compatibility currently caps TLS at 1.2. TLS 1.3 is not yet negotiated.
- Windows ConPTY and macOS permissions require platform E2E testing. The unit suite covers contracts; the E2E suite covers reality.

## Trade-offs

- TCP/TLS favors deployability over MOSH-style UDP roaming. Sleep recovery works through application-level reattach, not transport-level migration.
- A single event loop keeps reasoning simple but caps concurrency per process. BridgeSessions scales by adding nodes, not threads.
- The mesh carries identity, files, sessions, and CUA on one transport. Splitting them would simplify each lane at the cost of more certificate handling and more ports to firewall.

See [Protocol](protocol.md), [Security](../SECURITY.md), and [Building](building.md) for the next layer of detail.