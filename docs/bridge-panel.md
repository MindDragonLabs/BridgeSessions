# Bridge Panel

Bridge Panel is an optional web UI for files on the mesh. It does not replace `bs`. It does not start the mesh daemon.

Use it to browse the inbox on each host, preview documents, and edit files in a writable root.

## Load the panel

### From a source checkout

```bash
cd /path/to/BridgeSessions
python3 -m tools.bridgepanel
```

The process prints a URL. The URL includes a bearer token. Open that URL in a browser that can reach the bind address.

### Bind address

The default bind is loopback. That is the safe default.

To reach the panel from another machine on your private network or VPN, bind the VPN address. Example:

```bash
# documentation address only
BRIDGEPANEL_BIND=192.0.2.10 BRIDGEPANEL_PORT=9770 python3 -m tools.bridgepanel
```

Rules:

- Do not bind `0.0.0.0`.
- Do not put the panel on the public internet.
- Do not run the panel as a more privileged user than you need.

### Token and trusted addresses

Writes need the bearer token unless the client IP is in `BRIDGEPANEL_TRUSTED_IPS`.

Put only addresses that you control on that list. A trusted address can change files in writable roots.

### Keep it running

Use a systemd user unit, launchd, or an equivalent supervisor. The panel and the mesh daemon are separate. Restart only the panel after a UI change. Restart the daemon only when you replace `bridgesessions`.

## Use the panel

The window has three columns.

| Column | Role |
|---|---|
| Left | Mesh hosts. Online and offline seeds appear. Select one host. |
| Middle | Files on that host. Volume chips pick the root. The default root is the inbox. |
| Right | Preview or editor. |

### Open a host

1. Click a host name in the left column.
2. The middle column lists that host's inbox.
3. The breadcrumb shows `host / root / folder`.

The inbox is `~/.bridgesessions/received` on Unix. On Windows it is the per-user `received` directory under the BridgeSessions home.

### Move around

- Click a folder to enter it.
- Click a breadcrumb segment to go up.
- Use the volume chips to switch root (`Inbox`, then allowed disks).
- Use the search box to filter by file name.
- Use the chips **All**, **Markdown**, **Images**, and **Video** to filter by type.

### Preview

| Kind | Viewer |
|---|---|
| Markdown | Toast UI preview. Edit, Save, and Copy when the root is writable. |
| Code | CodeMirror 6. Language comes from the name or a picker. |
| Image | Inline preview. |
| Video | Inline player. |
| PDF | Inline embed. |

### Create, rename, delete

These actions work only in a writable root. The inbox is writable. Other volumes are read-only unless you add them to the ACL file.

- **New file** / **New folder** — buttons in the toolbar.
- **Rename** / **Delete** — the ⋮ menu on a row.
- Delete moves the item to a trash directory. It does not wipe the disk.

Remote writes go through the mesh file path. Large files belong in `bs file send`, not in the browser editor.

### Upload

Use the file picker or drag files onto the list. The upload lands in the current folder. The current root must be writable. The default size cap is 10 MiB for browser uploads.

## Security

- Paths cannot leave the selected root. `..` and absolute paths are rejected.
- Hidden and sensitive names stay out of listings.
- Content-Security-Policy limits scripts to the same origin plus the vendored editor files.
- The panel is not a public multi-tenant file server.

## Develop

```bash
python3 -m unittest discover -s tools/bridgepanel -q
python3 -m tools.bridgepanel
```

Code lives in `tools/bridgepanel/`. Static editor files live in `tools/bridgepanel/static/`.

## Related

- [Always-online seed](always-online-seed.md) — run a central node
- [Usage](usage.md) — CLI file commands
- [Configuration](configuration.md) — `receive_dir` and transfer limits
