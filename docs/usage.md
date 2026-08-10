# Usage

The shipping `bridgesessions` binary is the all-in-one client, daemon, and
diagnostic tool. Older `bs-client`/`bs-server` modular documents are historical.

## Connect to a session

```bash
bridgesessions shell <server>                  # attach to default session
bridgesessions shell <server> --name hms       # attach to session "hms"
bridgesessions shell <server> --name logs --cmd="journalctl -f"
```

The short form reuses your SSH `Host` aliases for address discovery only:

```bash
bs shell dev            # short alias for bridgesessions shell dev
bs dev hms              # positional quick-connect to session "hms"
```

## List / manage sessions

```bash
bridgesessions sessions <server>
```

## Health & diagnostics

```bash
bridgesessions doctor            # config + key presence checks
bridgesessions --version         # → 26.08.10-beta2
```

## Keys

```bash
bridgesessions keygen                        # write ~/.bridgesessions/id_ed25519.pem
bridgesessions authorize <hex-pubkey>       # append to server authorized_keys
```

## Daemon

```bash
bridgesessions --daemon --config ~/.bridgesessions/config
bridgesessions health <peer>
bridgesessions stats
```

Install as a systemd user service:

```bash
cp docs/service/bs-server.service ~/.config/systemd/user/bridgesessions.service
systemctl --user daemon-reload
systemctl --user enable --now bridgesessions
```

## Bridge Panel

`bridgesessions pane publish <file>` copies a local Markdown file into a
BridgePanel session so peers can read it in the web surface. See
[Bridge Panel](bridge-panel.md).

## Notes

- A disconnected client never kills the session — reattach any time.
- Keystrokes are sent with `TCP_NODELAY` for lowest latency.
- Clipboard integration is Windows-only in 26.08.10-beta2.
- `image` and `anim` are local terminal previews; media transfer uses `file`.
- Remote `edit` and `vfolder sync` fail closed in 26.08.10-beta2 pending a dedicated
  transfer transport.
