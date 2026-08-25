# Configuration

The normal config file is `~/.bridgesessions/config`. Override the path with `--config`.

The format is one directive per line.

```ini
node.name node-a
node.listen 192.0.2.10:19949
mesh.require_seed_pins true
mesh.mdns_enabled false
mesh.reconnect_backoff_max_secs 300
mesh.join_window_max_secs 300
seed node-b 192.0.2.11:19949 pubkey=<64-hex-ed25519-public-key>
sessions.default_shell /bin/bash -l
sessions.persistence_path ~/.bridgesessions/sessions.json
sessions.authorized_keys_path ~/.bridgesessions/authorized_keys
session.agent.command /bin/bash -lc 'exec hermes --tui'
receive_dir ~/.bridgesessions/received
transfer.max_bytes 8589934592
transfer.allow_sensitive_paths false
file.dest_allow_home false
```

The addresses above are documentation-only. Use addresses that belong to your network.

| Directive | Purpose |
|---|---|
| `node.name`, `node.listen` | Node identity and mesh bind |
| `seed ... pubkey=` | Pinned bootstrap and enrollment authority |
| `mesh.require_seed_pins` | Reject unpinned seeds. Keep this enabled. |
| `mesh.ping_interval_secs`, `mesh.pong_timeout_secs` | Liveness |
| `mesh.reconnect_backoff_max_secs` | Retry ceiling |
| `mesh.join_window_max_secs` | Unknown-cert join window cap |
| `sessions.default_shell` | Remote shell command |
| `session.<name>.command` | Named-session command |
| `sessions.authorized_keys_path` | Inbound trusted keys |
| `receive_dir` | Inbox and default served-file root |
| `transfer.max_bytes` | Per-file limit |
| `transfer.allow_sensitive_paths` | Arbitrary path access. High risk. |

## Bind rules

- Bind the address that other nodes can reach.
- Loopback-only listen hides the node from the mesh.
- A wildcard bind needs a host firewall.

See [Always-online seed](always-online-seed.md) for a central node.

## Local IPC

The CLI talks to the local daemon on loopback port **19980**. The channel uses an owner-only token. Isolated tests may set `BRIDGESESSIONS_IPC_PORT` for both the daemon and the CLI.

## Files and inbox

`receive_dir` is the inbox. Bridge Panel lists that directory by default. Remote `bs file` serving stays inside this root unless you set `transfer.allow_sensitive_paths`. That flag removes a major safeguard.

See [`config.example`](https://github.com/MindDragonLabs/BridgeSessions/blob/main/config.example).
