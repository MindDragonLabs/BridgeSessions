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
receive_retention_hours 24
transfer.max_bytes 8589934592
transfer.allow_sensitive_paths false
file.dest_allow_home false
file.copy_scope anywhere
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
| `mesh.discovered_ttl_secs` | How long a runtime-learned peer survives silence |
| `mesh.auto_upgrade` | Offer `bs upgrade` to peers that reconnect with an older version |
| `sessions.default_shell` | Remote shell command |
| `session.<name>.command` | Named-session command |
| `sessions.idle_timeout_hours` | Idle session expiry |
| `sessions.authorized_keys_path` | Inbound trusted keys |
| `receive_dir` | Inbox and default served-file root |
| `receive_retention_hours` | Hours a received file stays in the inbox before removal. `0` keeps them forever. Default 24. |
| `transfer.max_bytes` | Per-file limit |
| `transfer.allow_sensitive_paths` | Arbitrary path access. High risk. |
| `file.dest_allow_home` | Allow `--dest` outside the receive dir (`~`, `/tmp`) |
| `file.copy_scope` | Direct copy/list scope: `anywhere` (default) or `receive_dir` |

## Bind rules

- Bind the address that other nodes can reach.
- Loopback-only listen hides the node from the mesh.
- A wildcard bind needs a host firewall.

See [Always-online seed](always-online-seed.md) for a central node.

## Local IPC

The CLI talks to the local daemon on loopback port **19980**. The channel uses an owner-only token. Isolated tests may set `BRIDGESESSIONS_IPC_PORT` for both the daemon and the CLI.

## Files and inbox

`receive_dir` is the inbox. Bridge Panel lists that directory by default. Legacy `bs file recv` serving stays inside this root unless you set `transfer.allow_sensitive_paths`. That flag removes a major safeguard.

`file.copy_scope` controls the paths a peer daemon accepts for `bs cp` and
`bs file ls`:

- `anywhere` (default): direct reads, writes, and listings may use paths outside
  `receive_dir`, including absolute and `~/` paths. File permissions and the
  existing sensitive-path checks still apply.
- `receive_dir`: confines direct source reads, destination writes, and directory
  listings to that daemon's configured `receive_dir`, including checks against
  symlink escapes. Set it on each peer that should restrict direct file access.

Direct copies write to the requested destination without inbox staging.
`file.copy_scope` leaves the legacy `file send` / `file recv` staging rules
unchanged. Security follows the existing shell trust model: an authorized pinned
peer can already read and write files through `bs shell` as the daemon's user.
The default `anywhere` mirrors that access; `receive_dir` confines these file
operations but does not restrict shell access.

The inbox is a **staging area** for legacy transfers. A staged copy left behind after writing the caller's destination doubles the disk cost of that transfer. `receive_retention_hours` (default 24) expires it. Partial transfers (`.part`, `.part.bsmeta`) are never removed, so a slow transfer cannot be interrupted by housekeeping.

See [`config.example`](https://github.com/MindDragonLabs/BridgeSessions/blob/main/config.example).
