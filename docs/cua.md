# CUA — Computer-Use Automation (BridgeSessions 26.08.10-beta2)

Remote desktop automation over the `bs://` mesh. Seven subcommands: screen query,
screenshot capture, mouse click/move/scroll, text typing, and HID key press.
No VNC/RDP/streaming server required — input injection and capture happen in the
peer's user session via the `--cua-helper` process.

## Architecture

```
bs cua <subcommand> <peer>     ← CLI (operator machine)
    │
    ▼ direct TLS to peer
┌──────────────┐        ┌─────────────────────┐
│  peer daemon │ ──IPC──▶│  bs --cua-helper    │  (user session)
│  (Session 0) │        │  input + capture    │
└──────────────┘        └─────────────────────┘
```

- **Linux:** the daemon already runs in the user session — `--cua-helper` is a
  no-op (prints "not needed on Linux").
- **Windows:** the daemon typically runs as SYSTEM (Session 0). `--cua-helper`
  must run in the interactive user session for input injection and screen capture.
- **macOS:** `--cua-helper` uses ScreenCaptureKit for capture and CGEvent for
  input injection. Requires TCC (Screen Recording / Accessibility) approval.

## Setup: `--cua-helper`

### Linux

No setup needed. The daemon runs in the user session and performs capture/input
directly.

### Windows

Run the helper in the user session (e.g., at logon via a scheduled task or
Startup folder):

```powershell
# Interactive (for testing)
bridgesessions --cua-helper

# As a scheduled task at logon (production)
schtasks /create /tn "BS-CUA-Helper" /tr "bridgesessions --cua-helper" /sc onlogon /rl highest
schtasks /run /tn "BS-CUA-Helper"
```

The helper:
- Listens on `127.0.0.1:19986` (localhost only — not exposed to the network).
- Generates a random auth token written to `~/.bridgesessions/cua-helper-token`.
- The daemon reads this token and forwards CUA requests to the helper via IPC.

### macOS

```bash
# Run in the user session
bridgesessions --cua-helper
```

**TCC permissions required** (System Settings → Privacy & Security):
- **Screen Recording** — for `capture-video` and `cua capture`.
- **Accessibility** — for mouse/keyboard input injection (`click`, `move`,
  `type`, `key`, `scroll`).

> Rebuilt ad-hoc binaries get a new CDHash. You must remove and re-add the
> binary under Screen & System Audio Recording and Accessibility after every
> rebuild, then restart the launchd daemon.

Verify:
```bash
# Should print: cua-helper: listening on 127.0.0.1:19986
bridgesessions --cua-helper
```

---

## Subcommand reference

### 1. `cua screen` — Get screen dimensions

```bash
$ bs cua screen test-pc1
1920x1080
```

Returns `WIDTHxHEIGHT` in pixels. Use this to validate coordinates before
clicking.

### 2. `cua capture` — Screenshot

```bash
# Save to file
$ bs cua capture test-pc1 -o screenshot.png
Saved 184320 bytes to screenshot.png

# Binary to stdout (pipe to file or vision tool)
$ bs cua capture test-pc1 > shot.png
184320 bytes (png)

# JPEG format with quality
$ bs cua capture test-pc1 -o shot.jpg --format 2 --quality 60
```

| Flag | Default | Description |
|------|---------|-------------|
| `--format` | `1` (png) | `1`=png, `2`=jpeg |
| `--quality` | `80` | JPEG quality 1–100 (ignored for png) |
| `--output`, `-o` | stdout | Output file path |

### 3. `cua click` — Mouse click

```bash
$ bs cua click test-pc1 --x 960 --y 540
Clicked at (960,540) button=left

$ bs cua click test-pc1 --x 100 --y 200 --button right
Clicked at (100,200) button=right
```

| Flag | Default | Description |
|------|---------|-------------|
| `--x` | *(required)* | X coordinate (pixels) |
| `--y` | *(required)* | Y coordinate (pixels) |
| `--button` | `left` | `left`, `right`, `middle` |

### 4. `cua move` — Move cursor

```bash
$ bs cua move test-pc1 --x 500 --y 300
Moved to (500,300)
```

Moves the cursor without clicking. Useful before a `click` or to position for
hover-based UI.

### 5. `cua type` — Type text

```bash
$ bs cua type test-pc1 --text "hello world"
Typed 11 chars
```

Types UTF-8 text at the current cursor position. Use for form fields, terminal
input, and text editors.

### 6. `cua key` — Press HID key

```bash
# Press Enter (HID usage 0x28 = 40)
$ bs cua key test-pc1 --code 40
Pressed HID key 0x28

# Ctrl+S (HID usage 0x16 = 22 = 's')
$ bs cua key test-pc1 --code 22 --modifiers ctrl
Pressed HID key 0x16
```

| Flag | Default | Description |
|------|---------|-------------|
| `--code` | *(required)* | USB HID usage ID (decimal) |
| `--modifiers` | *(none)* | `ctrl`, `shift`, `alt`, `meta` (comma-separated) |

#### Common HID usage codes

| Key | HID (hex) | HID (dec) |
|-----|-----------|-----------|
| Enter | 0x28 | 40 |
| Escape | 0x29 | 41 |
| Backspace | 0x2A | 42 |
| Tab | 0x2B | 43 |
| Space | 0x2C | 44 |
| CapsLock | 0x39 | 57 |
| F1 | 0x3A | 58 |
| a | 0x04 | 4 |
| s | 0x16 | 22 |
| Left Arrow | 0x50 | 80 |
| Right Arrow | 0x4F | 79 |
| Up Arrow | 0x52 | 82 |
| Down Arrow | 0x51 | 81 |

Full table: [USB HID Usage Tables §10](https://usb.org/document-library/hid-usage-tables-122)
(Keyboard/Keypad page 0x07).

### 7. `cua scroll` — Mouse wheel

```bash
$ bs cua scroll test-pc1 --direction down --amount 5
Scrolled down 5 ticks

$ bs cua scroll test-pc1   # default: up 3 ticks
Scrolled up 3 ticks
```

| Flag | Default | Description |
|------|---------|-------------|
| `--direction` | `up` | `up` or `down` |
| `--amount` | `3` | Scroll ticks |

---

## Workflows

### Capture → analyze → click (vision loop)

```bash
# 1. Capture screen
bs cua capture test-pc1 -o /tmp/screen.png

# 2. Analyze (agent or vision model finds the button)
# 3. Click the target
bs cua click test-pc1 --x 742 --y 386
```

### Fill a form

```bash
bs cua click test-pc1 --x 300 --y 200          # focus name field
bs cua type test-pc1 --text "Jefferson"
bs cua key test-pc1 --code 43                   # Tab to next field
bs cua type test-pc1 --text "jeff@example.com"
bs cua key test-pc1 --code 40                   # Enter to submit
```

### Scroll and re-capture

```bash
bs cua capture test-pc1 -o /tmp/top.png
bs cua scroll test-pc1 --direction down --amount 10
bs cua capture test-pc1 -o /tmp/scrolled.png
```

---

## Security considerations

1. **Helper is localhost-only.** The `--cua-helper` binds `127.0.0.1:19986` —
   not exposed to the network. The daemon connects locally with a random token.
2. **Token auth.** Every helper request carries a token from
   `~/.bridgesessions/cua-helper-token`. A process without the token is rejected.
3. **Spectator guard.** Read-only (spectator) attach role cannot send CUA
   requests — enforced at the daemon before any input injection. (P0 fix from
   the 2.0.8 security audit.)
4. **POSIX `sq()` quoting.** All CUA wire payloads use proper shell quoting —
   no injection from crafted text/type payloads.
5. **Coordinate validation.** `click`/`move` coordinates are clamped to the
   reported screen dimensions. Out-of-bounds coordinates are not silently
   dropped — the response includes the actual screen size.
6. **TCC (macOS).** Screen Recording and Accessibility permissions must be
   granted per-binary (CDHash). Rebuilt binaries require re-approval.
7. **No keystroke logging.** The helper does not persist typed text or capture
   data. Screenshots are returned to the requesting CLI only.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `ERROR: cua helper not running` | Start `bridgesessions --cua-helper` in the user session |
| `ERROR: cua helper token mismatch` | Restart both daemon and helper; token regenerates on helper start |
| macOS capture returns black image | Grant Screen Recording TCC permission; restart daemon after approval |
| macOS input injection does nothing | Grant Accessibility TCC permission for the binary |
| Windows: helper not found | Ensure `bridgesessions.exe` is on PATH in the user session |
| Wrong coordinates clicked | Run `bs cua screen <peer>` first; coordinates are absolute screen pixels |
| Helper port conflict | Port 19986 is in use — kill the stale helper process |

---

## Wire protocol (for developers)

CUA requests ride on the existing `CuaRequest` (0x26) / `CuaResponse` (0x27)
wire messages. The action byte selects the operation:

| Action | Operation |
|--------|-----------|
| 0 | Screen dimensions |
| 1 | Key press (HID code + modifiers) |
| 2 | Type text (UTF-8) |
| 3 | Mouse move (x, y) |
| 4 | Mouse click (x, y, button) |
| 5 | Mouse scroll (y delta) |
| 6 | Screenshot capture (format:quality in text field) |

See `bs-protocol.h` for struct definitions and `bs-cua-helper.h` for the
helper-side implementation.
