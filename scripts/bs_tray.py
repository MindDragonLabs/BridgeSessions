#!/usr/bin/env python3
"""BridgeSessions Linux system tray app (pystray + Pillow).

Creates a tray icon with fleet status polling, daemon restart, and settings.
Falls back to daemon-only monitor if no system tray is available.
"""

import os
import sys
import subprocess
import threading
import time
import textwrap

# ── Auto-install dependencies if missing ──────────────────────────
def _ensure_deps():
    try:
        import pystray  # noqa: F401
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401
    except ImportError:
        print("→ Installing pystray + Pillow...", file=sys.stderr)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--user", "pystray", "Pillow"])
        # Re-import after install
        import importlib
        importlib.import_module("pystray")
        importlib.import_module("PIL")

_ensure_deps()

from PIL import Image, ImageDraw, ImageFont
import pystray
from pystray import MenuItem, Menu

# ── Constants ─────────────────────────────────────────────────────
BIN_NAME = os.environ.get("BS_BIN", "bridgesessions")
CONFIG_DIR = os.path.expanduser("~/.config/autostart")
DESKTOP_FILE = os.path.join(CONFIG_DIR, "bridgesessions-tray.desktop")
POLL_INTERVAL = 10  # seconds
ICON_SIZE = 64

# ── Icon generation ───────────────────────────────────────────────
def create_icon_image():
    """Load static branded B icon if present; else draw blue-square 'B' with PIL."""
    for cand in (
        os.path.expanduser("~/.local/share/bridgesessions/icon-b.png"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "icon-b.png"),
        "/usr/share/bridgesessions/icon-b.png",
    ):
        if os.path.isfile(cand):
            try:
                return Image.open(cand).convert("RGBA").resize((ICON_SIZE, ICON_SIZE))
            except Exception:
                pass
    img = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    # Rounded blue background
    margin = 4
    draw.rounded_rectangle(
        [margin, margin, ICON_SIZE - margin, ICON_SIZE - margin],
        radius=12,
        fill=(63, 169, 224),
    )
    # Draw "B" centered
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 40)
    except (IOError, OSError):
        font = ImageFont.load_default()
    bbox = draw.textbbox((0, 0), "B", font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (ICON_SIZE - tw) // 2 - bbox[0]
    y = (ICON_SIZE - th) // 2 - bbox[1]
    draw.text((x, y), "B", fill=(255, 255, 255), font=font)
    return img

# ── Fleet status polling ──────────────────────────────────────────
_fleet_cache = []
_fleet_lock = threading.Lock()

def poll_fleet():
    """Run `bridgesessions fleet` and cache the output."""
    global _fleet_cache
    try:
        result = subprocess.run(
            [BIN_NAME, "fleet"],
            capture_output=True, text=True, timeout=8,
        )
        lines = result.stdout.strip().splitlines() if result.returncode == 0 else []
        with _fleet_lock:
            _fleet_cache = lines
    except Exception as e:
        with _fleet_lock:
            _fleet_cache = [f"(poll error: {e})"]

def fleet_loop():
    """Background thread: poll every POLL_INTERVAL seconds."""
    while True:
        poll_fleet()
        time.sleep(POLL_INTERVAL)

def get_fleet_status():
    """Return cached fleet status as formatted text."""
    with _fleet_lock:
        if not _fleet_cache:
            return "No fleet data yet."
        return "\n".join(_fleet_cache)

# ── Notification helper ───────────────────────────────────────────
def notify(title, body):
    """Send a desktop notification (libnotify / notify-send)."""
    try:
        subprocess.run(
            ["notify-send", title, body],
            timeout=5, check=False,
        )
    except FileNotFoundError:
        # No notify-send; fall through silently
        pass
    except Exception:
        pass

# ── Menu actions ──────────────────────────────────────────────────
def on_fleet_status(icon, item):
    """Show fleet status via desktop notification."""
    status = get_fleet_status()
    # notify-send truncates; show first 5 lines in body
    lines = status.splitlines()
    summary = lines[0] if lines else "No fleet data"
    body = "\n".join(lines[1:6]) if len(lines) > 1 else ""
    notify(f"BridgeSessions — {summary}", body)
    # Also print to stdout for debugging
    print(f"--- Fleet Status ---\n{status}\n---", flush=True)

def on_restart_daemon(icon, item):
    """Restart the systemd user service."""
    notify("BridgeSessions", "Restarting daemon...")
    try:
        subprocess.run(
            ["systemctl", "--user", "restart", "bridgesessions"],
            timeout=10, check=True,
        )
        notify("BridgeSessions", "Daemon restarted.")
    except subprocess.CalledProcessError:
        notify("BridgeSessions", "ERROR: Failed to restart daemon.")
    except FileNotFoundError:
        notify("BridgeSessions", "ERROR: systemctl not found.")

def on_settings(icon, item):
    """Toggle autostart .desktop entry."""
    if os.path.isfile(DESKTOP_FILE):
        os.remove(DESKTOP_FILE)
        notify("BridgeSessions Tray", "Autostart disabled.")
    else:
        install_autostart()
        notify("BridgeSessions Tray", "Autostart enabled.")

def on_quit(icon, item):
    """Stop the tray icon."""
    icon.stop()

def on_open_logs(icon, item):
    """Open the BridgeSessions log / config directory in the file manager."""
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, ".bridgesessions"),
        os.path.join(home, ".local", "share", "bridgesessions"),
        "/tmp",
    ]
    path = next((p for p in candidates if os.path.isdir(p)), home)
    for opener in ("xdg-open", "gio"):
        try:
            subprocess.Popen([opener, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except FileNotFoundError:
            continue
    notify("BridgeSessions", f"Logs: {path}")

# ── Autostart .desktop ────────────────────────────────────────────
def install_autostart():
    """Create the autostart .desktop file."""
    os.makedirs(CONFIG_DIR, exist_ok=True)
    script_path = os.path.expanduser("~/.local/bin/bs_tray.py")
    content = textwrap.dedent(f"""\
        [Desktop Entry]
        Type=Application
        Name=BridgeSessions Tray
        Comment=BridgeSessions fleet status tray app
        Exec={script_path}
        Icon=bridgesessions
        Terminal=false
        X-GNOME-Autostart-enabled=true
        Categories=Network;
    """)
    with open(DESKTOP_FILE, "w") as f:
        f.write(content)

def autostart_label(item):
    """Dynamic menu label for settings."""
    return "Disable Autostart" if os.path.isfile(DESKTOP_FILE) else "Enable Autostart"

# ── Tooltip / title update ────────────────────────────────────────
def update_tooltip(icon):
    """Update icon tooltip with peer count from fleet status."""
    while icon.visible or not icon._running if hasattr(icon, "_running") else True:
        with _fleet_lock:
            lines = _fleet_cache
        if lines:
            # Count peer lines (skip header lines)
            peer_lines = [l for l in lines if l.strip() and not l.startswith("#")]
            icon.title = f"BridgeSessions ({len(peer_lines)} peers)"
        else:
            icon.title = "BridgeSessions"
        time.sleep(POLL_INTERVAL)

# ── Main entry ────────────────────────────────────────────────────
def main():
    # Start background fleet polling
    t = threading.Thread(target=fleet_loop, daemon=True)
    t.start()

    icon_image = create_icon_image()

    menu = Menu(
        MenuItem("Fleet Overview", on_fleet_status, default=True),
        MenuItem("Restart Daemon", on_restart_daemon),
        MenuItem("Open Logs", on_open_logs),
        MenuItem(autostart_label, on_settings),
        Menu.SEPARATOR,
        MenuItem("Quit", on_quit),
    )

    icon = pystray.Icon("bridgesessions", icon_image, "BridgeSessions", menu)

    # Start tooltip updater
    tooltip_thread = threading.Thread(target=update_tooltip, args=(icon,), daemon=True)
    tooltip_thread.start()

    print("→ BridgeSessions tray app starting...", flush=True)

    try:
        icon.run()
    except Exception as e:
        # No system tray available — run as daemon monitor only
        print(f"⚠ No system tray detected ({e}). Running as daemon monitor.", file=sys.stderr)
        print("→ Polling fleet status every 10s. Press Ctrl+C to exit.", file=sys.stderr)
        try:
            while True:
                status = get_fleet_status()
                print(f"[{time.strftime('%H:%M:%S')}] {status}", flush=True)
                time.sleep(POLL_INTERVAL)
        except KeyboardInterrupt:
            print("\n→ Exiting.", file=sys.stderr)

if __name__ == "__main__":
    main()
