# Bridge Panel

Bridge Panel is a small web surface bundled with BridgeSessions. It lets a node
publish Markdown documents into a session so peers can browse and read them in a
browser — with edit, save, copy, and a breadcrumb that mirrors the mesh tree.

It is served by the BridgePanel process and bound to the node's VPN/Tailscale
address (not localhost) so other mesh peers can reach it.

## Features

- **Session tree** — the left sidebar lists sessions → types → files, exactly
  as they exist on the node.
- **Breadcrumb** — `session / type / file`, aligned with the "Sessions" header.
- **Tools bar** — outlined icon buttons:
  - **Edit** — switch a document into an inline Markdown editor.
  - **Save** — write the edited Markdown back to disk under `sessions/`.
  - **Cancel** — discard edits and return to read view.
  - **Copy** — copy the raw Markdown to the clipboard.
- **Markdown rendering** — GitHub-flavored Markdown with code highlighting.
- **Build tag** — the titlebar shows the build number, injected server-side.

## Publishing a file

From the mesh CLI:

```bash
bridgesessions pane publish <file.md> \
    --session default --type documents --title "My Note"
```

This copies the local file into the node's BridgePanel surface under
`default / documents`, where any peer who can reach the panel can read it.

## Layout

```
┌──────────────┬──────────────────────────────────────┐
│ BRIDGE PANEL  │  default / documents / note.md       │  ← breadcrumb
│  build tag    ├──────────────────────────────────────┤
│ SESSIONS      │  [ Edit ] [ Copy ]                    │  ← tools bar
│ ▸ default     ├──────────────────────────────────────┤
│   ▸ documents │  # My Note                           │
│     note.md   │  Rendered markdown body…             │
└──────────────┴──────────────────────────────────────┘
```

## Editing model

When a file is editable, the **Edit** button swaps the rendered document for a
textarea. **Save** persists it via the same API the panel uses to read content;
**Cancel** reverts. **Copy** works in both read and edit views and copies the
raw source.

## Security notes

- The panel is bound to the VPN address, not `localhost`, so only mesh peers on
  the VPN can reach it.
- Editing requires the file to be writable by the panel process; read-only
  documents show only **Copy**.
- No authentication is performed by the panel itself — access control is the
  mesh/VPN boundary. Do not expose the port publicly.
