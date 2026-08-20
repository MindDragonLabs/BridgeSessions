# Bridge Panel

Bridge Panel is an optional local web UI for reviewing Markdown and session output produced by humans or agents.

## Start

From the repository root:

```bash
python3 -m tools.bridgepanel
```

It binds `127.0.0.1:9770` by default and talks to the local BridgeSessions daemon over token-authenticated loopback IPC.

## Publish a document

```bash
bs pane publish report.md \
  --session default \
  --type documents \
  --title 'Audit report'
```

The panel provides:

- session/document tree,
- breadcrumb navigation,
- rendered Markdown,
- raw copy,
- edit/save/cancel for writable documents,
- live session output where supported.

![Bridge Panel read view](assets/bridgepanel-read.png)

![Bridge Panel edit view](assets/bridgepanel-edit.png)

## Security

- Loopback is the default and recommended bind.
- Writes require a generated bearer token.
- A non-loopback bind is an explicit operator choice and must be protected by a trusted VPN/firewall and configured source controls.
- Do not expose the panel directly to the internet.
- Bridge Panel does **not** currently implement per-device mTLS pairing or OIDC.
- Panel files are constrained to its session/document roots; do not run it as a more privileged user than necessary.

## Development

```bash
python3 -m pytest tools/bridgepanel/test_bridgepanel.py -q
python3 -m tools.bridgepanel
```

The implementation lives in `tools/bridgepanel/`. Keep launchers there; do not add one-off wrapper scripts at the repository root.
