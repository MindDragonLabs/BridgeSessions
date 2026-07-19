# Configuration

BridgeSessions reads its config from a path passed with `--config` (or the
default location). The file is plain key/value, one directive per line.

## Example

```ini
# ── Mesh identity ──
node.name          node-a
node.listen        192.0.2.10:19949
mesh.pong_timeout_secs 30
mesh.reconnect_backoff_max_secs 30

# ── Bootstrap peers ──
seed node-b <seed-peer-host>:<port> pubkey=<64-hex-ed25519-public-key>

# ── Session defaults ──
# Set this for the server OS: Windows `pwsh.exe -NoLogo`,
# macOS `/bin/zsh -il`, Linux `/bin/bash -l`.
sessions.default_shell /bin/bash -l

# ── Persistence ──
sessions.persistence_path ~/.bridgesessions/sessions.json
sessions.authorized_keys_path ~/.bridgesessions/authorized_keys
```

## Directive reference

| Directive | Meaning |
|---|---|
| `node.name` | Human-readable name for this node. |
| `node.listen` | IPv4 address and TCP port to bind for the mesh (default `0.0.0.0:19949`; prefer a VPN address). |
| `mesh.pong_timeout_secs` | Disconnect if no pong within this window. |
| `mesh.reconnect_backoff_max_secs` | Ceiling for reconnect backoff. |
| `seed <name> <host>:<port> pubkey=<hex>` | A pinned bootstrap peer. Repeatable. |
| `sessions.default_shell` | Shell spawned for a session (per-server OS). |
| `sessions.persistence_path` | Where session metadata is persisted. |
| `sessions.authorized_keys_path` | File of authorized ed25519 public keys (hex). |

## Per-session commands

A session's command resolves in this order (ADR-007):

1. Client `--cmd` flag (always wins)
2. `session.<name>.command` in the server config
3. `sessions.default_shell`

## Example config (production-shaped)

See [`config.example`](../config.example) in the repository root for a sanitized
template you can copy and adapt.

## Local IPC port

The daemon and CLI use loopback port `19980` by default. For multiple isolated
instances on one host, set the same override for each daemon and its CLI calls:

```bash
BRIDGESESSIONS_IPC_PORT=20081 bridgesessions --config-dir /srv/bs-node-a
BRIDGESESSIONS_IPC_PORT=20081 bridgesessions --config-dir /srv/bs-node-a health node-b
```

The override must be an integer from 1 through 65535; invalid values fall back
to `19980`.
