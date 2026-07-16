# BridgePane

BridgePane turns BridgeSpace's embedded browser into a durable remote-agent output and file-transfer surface.

## Why

A PTY is a transport for commands, not a document viewer. Long agent responses scroll away, remote files are awkward to exchange, and BridgeSpace no longer exposes the plugin surface BridgeSessions originally targeted. BridgePane avoids that dead integration point:

- runs on the remote host;
- binds only to its Tailscale address;
- uses an unguessable URL token;
- renders agent Markdown and source files in a readable browser pane;
- refreshes the selected artifact automatically;
- supports direct downloads;
- accepts drag/drop file uploads and pasted text into a remote inbox.

No third-party Python packages are required.

## Install

BridgePane ships with BridgeSessions under `tools/bridgepane/`, including its
systemd unit and installer:

```bash
cd ~/bridgesessions
tools/bridgepane/install.sh \
  --trusted-ip 203.0.113.60 \
  --tailscale-serve
```

The installer detects the server's Tailscale address, installs the `bridgepane`
CLI, enables `bridgepane.service`, and can expose the pane through tailnet-only
HTTPS. Configuration is persisted at `~/.config/bridgepane/environment`.

## User workflow

1. From the trusted BridgeSpace Mac, open `http://<tailscale-ip>:9770/`. Other clients use the tokenized URL printed by `bridgepane.py url --bind <tailscale-ip>`.
2. Leave that pane open.
3. Tell Hermes **“pane mode”** or **“publish the details.”**
4. Hermes publishes long output with:

   ```bash
   python tools/bridgepane/bridgepane.py publish /path/to/report.md
   ```

5. Drop local files onto the pane to upload them. Uploaded files land in:

   ```text
   ~/.local/share/bridgepane/inbox/
   ```

Published artifacts live in:

```text
~/.local/share/bridgepane/artifacts/
```

## Commands

```bash
python bridgepane.py init --bind 100.x.y.z --port 9770
python bridgepane.py serve --bind 100.x.y.z --port 9770
python bridgepane.py url --bind 100.x.y.z --port 9770
python bridgepane.py publish REPORT.md
printf '# Report\n\nDone.' | python bridgepane.py note --title 'Current report.md'
```

## Security model

- The service binds to the remote host's Tailscale address, not `0.0.0.0`.
- Untrusted clients require a 192-bit URL token stored mode `0600`.
- Explicitly configured Tailscale source IPs may use the short root URL; their
  identity is authenticated by Tailscale/WireGuard.
- All storage is flat and filename-sanitized.
- Resolved paths must remain under the artifact or inbox directory; symlink escapes are rejected.
- Upload size is capped at 100 MiB.
- Uploaded HTML and SVG are never served as active HTML.
- Responses use `no-store`, `nosniff`, no-referrer, and a restrictive CSP.
- Keep the token URL private. Rotate it by stopping the service, deleting `~/.config/bridgepane/token`, and starting again.

## Tests

```bash
python test_bridgepane.py
```
