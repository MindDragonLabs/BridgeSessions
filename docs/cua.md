# Computer-use automation (CUA)

`bs cua` and `bs capture-video` capture and control a trusted peer's interactive desktop through the BridgeSessions mesh. The capability works only against peers in `authorized_keys`. Spectator attachments are rejected before any CUA action, including video capture.

## What CUA covers

| Action | Command family | Notes |
|---|---|---|
| Read the screen | `bs cua screen`, `bs cua capture` | Returns a single frame as PNG or JPEG. |
| Move the mouse | `bs cua move` | Coordinates are absolute, in screen pixels. |
| Click | `bs cua click` | Buttons: `left`, `right`, `middle`. |
| Type text | `bs cua type` | UTF-8 text, no control characters. |
| Press a key | `bs cua key` | USB HID usage ID plus optional modifiers. |
| Scroll | `bs cua scroll` | Direction and amount in wheel notches. |
| Record a video | `bs capture-video` | Bounded duration. Returns MP4. |

Capture first, then act. Always run `bs cua screen` (or capture to an image) and confirm dimensions before clicking. Coordinates from an old capture do not survive a window move or resolution change.

## Commands

```bash
# read the current desktop
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua capture <peer> -o screen.jpg --format 2 --quality 70

# pointer and buttons
bs cua move <peer> --x 500 --y 300
bs cua click <peer> --x 500 --y 300 --button left
bs cua scroll <peer> --direction down --amount 5

# keyboard
bs cua type <peer> --text 'hello'
bs cua key <peer> --code 40 --modifiers ctrl

# bounded desktop recording
bs capture-video <peer> --duration 10 -o capture.mp4
```

The image format defaults to PNG. JPEG accepts `--quality` from 1 to 100. Video is encoded on the peer with the platform's native pipeline (ScreenCaptureKit on macOS, Media Foundation on Windows, an external tool on Linux) and returned as a single MP4.

## Architecture

```text
operator CLI ── mutual TLS (19949) ── peer daemon ── token-auth loopback IPC ── user-session helper
```

Three processes cooperate per action.

1. **Operator CLI** sends a CUA request over the mesh transport. The transport is mutual TLS with a pinned Ed25519 peer key.
2. **Peer daemon** validates the request, checks the caller is not a spectator, and forwards the request over loopback IPC.
3. **User-session helper** runs as the logged-in user. It owns the desktop session and can capture the screen and inject input.

The helper listens only on loopback (default port `19986`). Every request is authenticated with an owner-only random token stored under `~/.bridgesessions/`. The mesh daemon never touches the desktop directly.

### Platform notes

- **Linux.** Capture and input usually work from the user daemon session. Desktop tools such as `xdotool`, `grim`, or `ffmpeg` may be required depending on the compositor. The helper is one process per interactive session.
- **Windows.** The mesh daemon often runs in Session 0. Start one `bridgesessions --cua-helper` in the logged-in user's session to reach the interactive desktop.
- **macOS.** The helper must run in the logged-in user's session. Capture uses ScreenCaptureKit; input uses accessibility APIs. Permission grants are sticky and survive helper restarts.

## Setup

### Linux

```bash
bridgesessions --cua-helper
```

The helper process writes its token file under `~/.bridgesessions/`. The mesh daemon reads the same token to authenticate loopback IPC.

### Windows

```powershell
bridgesessions --cua-helper
```

Create an at-logon task in the interactive user's context for persistent use. Do not run multiple helpers against one token file. Each helper writes its own token; two helpers racing one token cause auth mismatch.

### macOS

```bash
bridgesessions --cua-helper
```

The installed, Developer ID-signed app and helper need:

- **Screen Recording** for capture and video,
- **Accessibility** for mouse and keyboard.

Grant both. Restart the helper after granting. Do not run `tccutil reset` for the BridgeSessions bundle during routine upgrades. The reset wipes the grants and breaks capture until the user re-grants.

## HID keys

`cua key` uses USB HID keyboard usage IDs. Common values:

| Key | Decimal |
|---|---:|
| `a` | 4 |
| `s` | 22 |
| `Enter` | 40 |
| `Escape` | 41 |
| `Backspace` | 42 |
| `Tab` | 43 |
| `Space` | 44 |
| Right arrow | 79 |
| Left arrow | 80 |
| Down arrow | 81 |
| Up arrow | 82 |

Modifiers are comma-separated. The accepted names are `ctrl`, `shift`, `alt`, `meta`. Combine them as needed, for example `ctrl,shift`. The helper translates the names and HID code into the platform-native key event.

## Security

- Spectator attachments are rejected before any CUA action, including video capture.
- Helper IPC is loopback-only and token-authenticated. The token is owner-readable only.
- Coordinates, payload sizes, and durations are validated. Out-of-range values fail before the helper acts.
- Capture and input results return to the requesting peer. The helper does not intentionally persist them.
- CUA is not a low-privilege capability. A trusted peer can observe and control the desktop. Treat an authorized peer as having full interactive access to the user session that hosts the helper.

## Failure modes and recovery

| Symptom | Likely cause | Action |
|---|---|---|
| Helper unavailable, auth mismatch | duplicate helpers against one token | stop duplicates; restart one helper and the daemon |
| Black or empty macOS capture | Screen Recording grant missing or stale | grant, then restart the helper |
| macOS input ignored | Accessibility grant missing | grant, then restart the helper |
| Windows input hits wrong desktop | helper in a different session than the target | start the helper in the intended interactive session |
| Click lands in the wrong place | screen resized since `cua screen` | run `cua screen`, capture again, recalculate |
| Port `19986` busy | stale helper from a previous user or container | terminate the stale process; restart one helper |

For automated desktop verification, use the [E2E Framework](E2E-FRAMEWORK.md). The harness wraps `bs cua` and `bs capture-video` with screenshot diffs and structured assertions.