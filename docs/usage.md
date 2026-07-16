# Usage

The `bridgesessions` binary is the all-in-one client + server + `doctor`.
The `bs-client` and `bs-server` binaries expose the same surface as separate
components.

## Connect to a session

```bash
bridgesessions shell <server>                  # attach to default session
bridgesessions shell <server> --name hms       # attach to session "hms"
bridgesessions shell <server> --name logs --cmd="journalctl -f"
```

The short form reuses your SSH `Host` aliases for address discovery only:

```bash
bs dev                  # ≡ bs-client --server=dev
bs dev hms             # ≡ bs-client --server=dev --name=hms
```

## List / manage sessions

```bash
bridgesessions shell <server> --list           # list sessions
bridgesessions shell <server> --name hms --kill   # kill a session
```

## Health & diagnostics

```bash
bridgesessions doctor            # config + key presence checks
bridgesessions --version         # → 2.0.0
```

## Keys

```bash
bridgesessions keygen                        # write ~/.bridgesessions/id_ed25519.pem
bridgesessions authorize <hex-pubkey>       # append to server authorized_keys
```

## Server (bs-server)

```bash
bs-server --daemon --config ~/.bridgesessions/config
bs-server health              # exit 0 if healthy
bs-server status --json       # structured status
```

Install as a systemd user service:

```bash
bs-server install --user
systemctl --user enable --now bs-server
```

## Bridge Panel

`bridgesessions pane publish <file>` copies a local Markdown file into a
BridgePanel session so peers can read it in the web surface. See
[Bridge Panel](bridge-panel.md).

## Notes

- A disconnected client never kills the session — reattach any time.
- Keystrokes are sent with `TCP_NODELAY` for lowest latency.
- Clipboard is two-way and hash-deduped to avoid echo loops.
