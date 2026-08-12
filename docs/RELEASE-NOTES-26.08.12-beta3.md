# BridgeSessions 26.08.12-beta3

Date: 2026-08-12

## Highlights

- Windows CUA capture: GDI-only BMP path (no GDI+ AV); Session-0 skip PS fallback;
  single-instance helper; SYSTEM-readable helper token ACL.
- Tray/menubar B logo on macOS (BSMenubar), Windows (`bs_tray.ps1`), Linux (`bs_tray.py`)
  with Fleet Status, Restart Daemon, Open Logs.
- Mesh as service: launchd / systemd user (`LD_LIBRARY_PATH`) / Windows scheduled tasks.
- Static icons: `assets/icon-b.png` / `icon-b.ico`.
- E2E L3 checks for tray process, helper count, systemd/launchd posture.

## Upgrade

```bash
# Linux/macOS
curl -fsSL https://github.com/MindDragonLabs/BridgeSessions/raw/main/scripts/install.sh | bash

# Windows
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

Verify: `bridgesessions --version` → `26.08.12-beta3`.
