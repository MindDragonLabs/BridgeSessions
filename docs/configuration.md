# Configuration

BridgeSessions reads its config from a path passed with `--config` (or the
default location). The file is plain key/value, one directive per line.

## Example

```ini
# ── Mesh identity ──
mesh.node_name     test-pc1
mesh.listen        19949
mesh.pong_timeout_secs 30
mesh.reconnect_backoff_max_secs 30

# ── Bootstrap peers ──
seed <seed-peer-host>:<port>   # Seed peer to connect to on startup

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
| `mesh.node_name` | Human-readable name for this node. |
| `mesh.listen` | TCP port to bind for the mesh (default 19949). |
| `mesh.pong_timeout_secs` | Disconnect if no pong within this window. |
| `mesh.reconnect_backoff_max_secs` | Ceiling for reconnect backoff. |
| `seed <host>:<port>` | A bootstrap peer to connect to on startup. Repeatable. |
| `sessions.default_shell` | Shell spawned for a session (per-server OS). |
| `sessions.persistence_path` | Where session metadata is persisted. |
| `sessions.authorized_keys_path` | File of authorized ed25519 public keys (hex). |

## Per-session commands

A session's command resolves in this order (ADR-007):

1. Client `--cmd` flag (always wins)
2. Server `config` per-session `command`
3. Server `default_command`
4. Hardcoded default: `hermes --tui`

## Example config (production-shaped)

See `config.shadow.production.example` in the repository root for a filled-in
template you can copy and adapt.
