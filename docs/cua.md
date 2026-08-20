# Computer-use automation (CUA)

`bs cua` captures and controls a trusted peer's interactive desktop through the BridgeSessions mesh.

## Commands

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua capture <peer> -o screen.jpg --format 2 --quality 70
bs cua move <peer> --x 500 --y 300
bs cua click <peer> --x 500 --y 300 --button left
bs cua type <peer> --text 'hello'
bs cua key <peer> --code 40 --modifiers ctrl
bs cua scroll <peer> --direction down --amount 5
bs capture-video <peer> --duration 10 -o capture.mp4
```

Always capture first and confirm screen dimensions before clicking.

## Architecture

```text
operator CLI ── mutual TLS ── peer daemon ── token-auth loopback IPC ── user-session helper
```

- **Linux:** capture/input can run in the user's daemon session; common desktop tools may be required.
- **Windows:** the mesh daemon may run in Session 0. Start one `bridgesessions --cua-helper` in the logged-in user's session.
- **macOS:** run the helper in the logged-in user's session. Capture uses ScreenCaptureKit; input uses accessibility APIs.

The helper listens only on loopback (default 19986) and authenticates every request with an owner-only random token under `~/.bridgesessions/`.

## Setup

### Windows

```powershell
bridgesessions --cua-helper
```

For persistent use, create an at-logon task in the interactive user's context. Do not run multiple helpers against one token file.

### macOS

```bash
bridgesessions --cua-helper
```

Grant the installed, Developer ID-signed app/binary:

- Screen Recording (capture/video),
- Accessibility (mouse/keyboard).

Restart the helper after changing permissions. Do not reset TCC during routine upgrades.

## HID keys

`cua key` uses USB HID keyboard usage IDs.

| Key | Decimal |
|---|---:|
| `a` | 4 |
| `s` | 22 |
| Enter | 40 |
| Escape | 41 |
| Backspace | 42 |
| Tab | 43 |
| Space | 44 |
| Right / Left / Down / Up | 79 / 80 / 81 / 82 |

Modifiers are comma-separated: `ctrl`, `shift`, `alt`, `meta`.

## Security

- Spectator attachments are rejected before any CUA action.
- Helper IPC is loopback-only and token-authenticated.
- Coordinates and payload sizes are validated.
- Capture/input results are returned to the requesting peer; the helper does not intentionally persist them.
- A trusted peer can still observe or control the desktop. CUA is not a low-privilege capability.

## Troubleshooting

| Symptom | Check |
|---|---|
| helper unavailable/auth mismatch | stop duplicate helpers; restart one helper and the daemon |
| black/empty macOS capture | Screen Recording grant and helper restart |
| macOS input ignored | Accessibility grant |
| Windows input hits wrong desktop | helper must be in the intended interactive session |
| wrong click target | run `cua screen`, capture, then recalculate coordinates |
| port 19986 busy | terminate stale helper |

For automated desktop verification, use [E2E Framework](E2E-FRAMEWORK.md).
