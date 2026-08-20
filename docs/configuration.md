# Configuration

The normal config is `~/.bridgesessions/config`; override with `--config`.

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

| Directive | Purpose |
|---|---|
| `node.name`, `node.listen` | node identity and mesh bind |
| `seed ... pubkey=` | pinned bootstrap/enrollment authority |
| `mesh.require_seed_pins` | reject unpinned seeds; keep enabled |
| `mesh.ping_interval_secs`, `mesh.pong_timeout_secs` | liveness |
| `mesh.reconnect_backoff_max_secs` | retry ceiling |
| `mesh.join_window_max_secs` | unknown-cert join window cap |
| `sessions.default_shell` | remote shell command |
| `session.<name>.command` | named-session command |
| `sessions.authorized_keys_path` | inbound trusted keys |
| `receive_dir` | inbox and default served-file root |
| `transfer.max_bytes` | per-file limit |
| `transfer.allow_sensitive_paths` | arbitrary/sensitive path access; high risk |

Local daemon IPC uses loopback port 19980 plus an owner-only token. Isolated tests may set `BRIDGESESSIONS_IPC_PORT` consistently for daemon and CLI.

See [`config.example`](https://github.com/MindDragonLabs/BridgeSessions/blob/main/config.example).
