#!/usr/bin/env bash
set -eu
# BridgeSessions one-line install + upgrade (Linux / macOS)
#
#   curl -fsSL https://github.com/MindDragonLabs/BridgeSessions/releases/download/26.08.10-beta2/scripts/install.sh | bash
#
# Or join a mesh in one command:
#
#   curl ... | bash -s -- join <host-addr> <invite-code>
#
# On Windows (PowerShell):
#   irm https://github.com/MindDragonLabs/BridgeSessions/releases/download/26.08.10-beta2/scripts/install.ps1 | iex

TAG="${BRIDGESESSIONS_TAG:-26.08.10-beta2}"
BASE="https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/v${TAG}/dist"
INSTALL_DIR="${HOME}/.local/bin"
VERSION_FILE="${INSTALL_DIR}/.bridgesessions-version"
FORCE_UPDATE="${BRIDGESESSIONS_FORCE:-0}"

os=$(uname -s)
arch=$(uname -m)
case "${os}-${arch}" in
  Linux-x86_64)
    BIN="bridgesessions-linux-x86_64"
    BIN_NAME="bridgesessions"
    ;;
  Darwin-arm64)
    BIN="bridgesessions-macos-arm64"
    BIN_NAME="bridgesessions"
    ;;
  *)
    echo "Unsupported platform: ${os}-${arch}" >&2
    echo "On Windows, download manually:" >&2
    echo "  ${BASE}/bridgesessions-windows-x86_64.exe" >&2
    exit 1
    ;;
esac

mkdir -p "${INSTALL_DIR}"

validate_binary() {
  candidate=$1
  if ! command -v file >/dev/null 2>&1; then
    echo "ERROR: 'file' is required to validate the downloaded artifact" >&2
    return 1
  fi
  description=$(file -b "${candidate}")
  case "${os}-${arch}" in
    Linux-x86_64)
      echo "${description}" | grep -qE '^ELF 64-bit LSB.*x86-64' || {
        echo "ERROR: downloaded artifact is not a Linux x86-64 ELF executable: ${description}" >&2
        return 1
      }
      ;;
    Darwin-arm64)
      echo "${description}" | grep -qE '^Mach-O 64-bit.*arm64' || {
        echo "ERROR: downloaded artifact is not a macOS arm64 Mach-O executable: ${description}" >&2
        return 1
      }
      ;;
  esac
}

# ── 1. Stop existing daemon before update ──────────────────────────
stop_daemon() {
  case "${os}" in
    Darwin)
      launchctl bootout "gui/$(id -u)/com.bridgesessions.mesh" 2>/dev/null || true
      pkill -f "bridgesessions.*--config" 2>/dev/null || true
      ;;
    Linux)
      systemctl --user stop bridgesessions.service 2>/dev/null || true
      pkill -f "bridgesessions.*--config" 2>/dev/null || true
      ;;
  esac
  sleep 1
}

start_daemon() {
  case "${os}" in
    Darwin)
      launchctl bootstrap "gui/$(id -u)" "${HOME}/Library/LaunchAgents/com.bridgesessions.mesh.plist" 2>/dev/null || \
        launchctl load -w "${HOME}/Library/LaunchAgents/com.bridgesessions.mesh.plist" 2>/dev/null || true
      ;;
    Linux)
      systemctl --user daemon-reload 2>/dev/null || true
      systemctl --user enable bridgesessions.service 2>/dev/null || true
      systemctl --user start bridgesessions.service 2>/dev/null || true
      ;;
  esac
}

CURRENT=""
[ -f "${VERSION_FILE}" ] && CURRENT="$(cat "${VERSION_FILE}")" || true

# Determine if we need to download
NEEDS_DOWNLOAD=0
if [ "${FORCE_UPDATE}" = "1" ]; then
  NEEDS_DOWNLOAD=1
elif [ "${CURRENT}" != "${TAG}" ] || [ ! -x "${INSTALL_DIR}/${BIN_NAME}" ]; then
  NEEDS_DOWNLOAD=1
fi

if [ "${NEEDS_DOWNLOAD}" = "1" ]; then
  # Stop daemon before swapping binary (prevents "Text file busy")
  echo "→ Stopping existing daemon..."
  stop_daemon

  echo "→ Downloading bridgesessions ${TAG} for ${os}-${arch}..."
  TMP_BIN="${INSTALL_DIR}/.${BIN_NAME}.download.$$"
  trap 'rm -f "${TMP_BIN:-}"' EXIT INT TERM
  curl -fsSL --progress-bar "${BASE}/${BIN}" -o "${TMP_BIN}"
  chmod +x "${TMP_BIN}"
  validate_binary "${TMP_BIN}"

  # SHA-256 checksum verification
  TMP_SUMS="${INSTALL_DIR}/.SHA256SUMS.$$"
  curl -fsSL "${BASE}/SHA256SUMS" -o "${TMP_SUMS}" 2>/dev/null || true
  if [ -f "${TMP_SUMS}" ]; then
    EXPECTED_HASH=$(grep " ${BIN}\$" "${TMP_SUMS}" | awk '{print $1}')
    if [ -n "${EXPECTED_HASH}" ]; then
      ACTUAL_HASH=$(sha256sum "${TMP_BIN}" 2>/dev/null | awk '{print $1}' || shasum -a 256 "${TMP_BIN}" | awk '{print $1}')
      if [ "${ACTUAL_HASH}" != "${EXPECTED_HASH}" ]; then
        echo "ERROR: SHA-256 mismatch!" >&2
        echo "  expected: ${EXPECTED_HASH}" >&2
        echo "  actual:   ${ACTUAL_HASH}" >&2
        rm -f "${TMP_BIN}" "${TMP_SUMS}"
        exit 1
      fi
      echo "→ SHA-256 verified."
    fi
  else
    echo "→ WARNING: could not download SHA256SUMS — skipping checksum verification." >&2
  fi
  rm -f "${TMP_SUMS}"

  REPORTED_VERSION=$("${TMP_BIN}" --version)
  EXPECTED_VERSION=${TAG#v}
  if [ "${REPORTED_VERSION}" != "${EXPECTED_VERSION}" ]; then
    echo "ERROR: downloaded binary reports ${REPORTED_VERSION}; expected ${EXPECTED_VERSION}" >&2
    exit 1
  fi

  # Atomic swap: mv over the old binary (handles "Text file busy" since daemon is stopped)
  mv -f "${TMP_BIN}" "${INSTALL_DIR}/${BIN_NAME}"
  echo "${TAG}" > "${VERSION_FILE}"
  trap - EXIT INT TERM
  echo "→ Binary updated."
else
  validate_binary "${INSTALL_DIR}/${BIN_NAME}"
  echo "→ bridgesessions ${TAG} already installed."
fi

"${INSTALL_DIR}/${BIN_NAME}" --version

if ! echo "${PATH}" | grep -q "${INSTALL_DIR}"; then
  echo "→ Add to PATH: export PATH=\"${INSTALL_DIR}:\$PATH\""
  echo "   (or restart your shell)"
fi

# ── 2. App dirs + default config ───────────────────────────────────
APP_HOME="${HOME}/.bridgesessions"
RECEIVE_DIR="${APP_HOME}/received"
mkdir -p "${APP_HOME}" "${RECEIVE_DIR}"

# Clean received/ dir (prevents macOS daemon crash from binary files — known issue)
RECEIVE_COUNT=$(find "${RECEIVE_DIR}" -type f 2>/dev/null | wc -l | tr -d ' ')
if [ "${RECEIVE_COUNT}" -gt 50 ]; then
  echo "→ Cleaning ${RECEIVE_COUNT} files from received/ (prevents daemon crash)..."
  find "${RECEIVE_DIR}" -type f -delete 2>/dev/null || true
fi

CONFIG_PATH="${APP_HOME}/config"
if [ ! -f "${CONFIG_PATH}" ]; then
  echo "→ Creating default config at ${CONFIG_PATH}..."
  HOSTNAME_SHORT="$(hostname -s 2>/dev/null || echo node)"
  cat > "${CONFIG_PATH}" <<EOF
# BridgeSessions config — generated by install.sh
node.name ${HOSTNAME_SHORT}
node.listen 0.0.0.0:19949
receive_dir ${RECEIVE_DIR}
EOF
  chmod 600 "${CONFIG_PATH}"
fi

# ── 3. System service setup ────────────────────────────────────────
BIN_ABS="${INSTALL_DIR}/${BIN_NAME}"
case "${os}" in
  Darwin)
    PLIST_DIR="${HOME}/Library/LaunchAgents"
    PLIST_PATH="${PLIST_DIR}/com.bridgesessions.mesh.plist"
    mkdir -p "${PLIST_DIR}"
    echo "→ Installing launchd service..."
    cat > "${PLIST_PATH}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.bridgesessions.mesh</string>
  <key>ProgramArguments</key>
  <array>
    <string>${BIN_ABS}</string>
    <string>--config</string>
    <string>${CONFIG_PATH}</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>${APP_HOME}/daemon.log</string>
  <key>StandardErrorPath</key>
  <string>${APP_HOME}/daemon.err</string>
</dict>
</plist>
EOF
    # Always (re)start the daemon after install/update
    echo "→ Restarting daemon..."
    stop_daemon
    start_daemon
    sleep 2
    if pgrep -f "bridgesessions.*--config" >/dev/null 2>&1; then
      echo "→ Daemon running (PID $(pgrep -f 'bridgesessions.*--config' | head -1))."
    else
      echo "→ WARNING: Daemon not running. Start manually:"
      echo "   ${BIN_ABS} --config ${CONFIG_PATH}"
    fi

    # ── 3a. CUA helper launchd agent (macOS) ─────────────────────
    # The CUA helper needs to run in the user GUI session (not under
    # the daemon's launchd context) for Screen Recording + input injection.
    CUA_PLIST="${PLIST_DIR}/com.bridgesessions.cua-helper.plist"
    cat > "${CUA_PLIST}" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.bridgesessions.cua-helper</string>
  <key>ProgramArguments</key>
  <array>
    <string>${BIN_ABS}</string>
    <string>--config</string>
    <string>${CONFIG_PATH}</string>
    <string>--cua-helper</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>${APP_HOME}/cua-helper.log</string>
  <key>StandardErrorPath</key>
  <string>${APP_HOME}/cua-helper.err</string>
</dict>
</plist>
EOF
    launchctl bootstrap "gui/$(id -u)" "${CUA_PLIST}" 2>/dev/null || \
      launchctl load -w "${CUA_PLIST}" 2>/dev/null || true
    echo "→ CUA helper launchd agent installed."

    # ── 3b. BSMenubar.app install (macOS) ────────────────────────
    APP_DIR="/Applications"
    MENUBAR_APP="${APP_DIR}/BSMenubar.app"
    # Check if we have a bundled app to deploy (from repo's dist/)
    REPO_APP="$(cd "$(dirname "$0")/.." && pwd)/dist/BSMenubar.app"
    if [ ! -d "${REPO_APP}" ]; then
      REPO_APP="$(cd "$(dirname "$0")" && pwd)/../dist/BSMenubar.app"
    fi
    if [ -d "${REPO_APP}" ]; then
      echo "→ Installing BSMenubar.app to ${APP_DIR}/..."
      rm -rf "${MENUBAR_APP}" 2>/dev/null
      cp -R "${REPO_APP}" "${APP_DIR}/"
      codesign --force --sign - "${MENUBAR_APP}" 2>/dev/null || true
      echo "→ BSMenubar.app installed."

      # Launch if not already running
      if ! pgrep -f "BSMenubar" >/dev/null 2>&1; then
        open "${MENUBAR_APP}" 2>/dev/null && echo "→ BSMenubar.app launched."
      else
        echo "→ BSMenubar.app already running."
      fi
    else
      echo "→ NOTE: BSMenubar.app not found in dist/ — skipping menubar install."
      echo "  The CUA helper is running via launchd. For a menubar UI,"
      echo "  build BSMenubar.app separately."
    fi

    # ── 3c. TCC Permissions guidance (macOS) ─────────────────────
    # Reset TCC for the new binary identity (adhoc signing changes CDHash)
    tccutil reset ScreenCapture com.bridgesessions.mesh 2>/dev/null || true
    tccutil reset Accessibility com.bridgesessions.mesh 2>/dev/null || true
    echo "→ TIP: Grant Screen Recording + Accessibility permissions:"
    echo "  System Settings → Privacy & Security → Screen Recording → Add bridgesessions"
    echo "  System Settings → Privacy & Security → Accessibility → Add bridgesessions"
    echo "  (Or open BSMenubar.app → Settings → Check Permissions)"
    ;;
  Linux)
    UNIT_DIR="${HOME}/.config/systemd/user"
    UNIT_PATH="${UNIT_DIR}/bridgesessions.service"
    mkdir -p "${UNIT_DIR}"
    echo "→ Installing systemd user service..."
    cat > "${UNIT_PATH}" <<EOF
[Unit]
Description=BridgeSessions mesh daemon
After=network.target

[Service]
ExecStart=${BIN_ABS} --config ${CONFIG_PATH}
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF
    echo "→ Restarting daemon..."
    start_daemon
    sleep 2
    if systemctl --user is-active bridgesessions.service >/dev/null 2>&1; then
      echo "→ Daemon running."
    else
      echo "→ WARNING: Daemon not running. Start manually:"
      echo "   systemctl --user start bridgesessions"
    fi

    # ── 3b. System tray app (Linux) ──────────────────────────────
    echo "→ Installing system tray app..."

    # Install bs_tray.py to ~/.local/bin/
    TRAY_SCRIPT_SRC="$(cd "$(dirname "$0")" && pwd)/bs_tray.py"
    TRAY_SCRIPT_DEST="${INSTALL_DIR}/bs_tray.py"
    if [ -f "${TRAY_SCRIPT_SRC}" ]; then
      cp "${TRAY_SCRIPT_SRC}" "${TRAY_SCRIPT_DEST}"
      chmod +x "${TRAY_SCRIPT_DEST}"
      echo "  → Tray script installed to ${TRAY_SCRIPT_DEST}"
    else
      echo "  → WARNING: bs_tray.py not found in scripts/ — skipping tray install."
    fi

    # Install Python dependencies for tray app
    if command -v pip3 >/dev/null 2>&1; then
      echo "  → Installing pystray + Pillow..."
      pip3 install --user pystray Pillow 2>/dev/null || \
        echo "  → WARNING: pip3 install failed — tray app may not work."
    else
      echo "  → WARNING: pip3 not found — install pystray + Pillow manually for tray app."
    fi

    # Create autostart .desktop entry
    AUTOSTART_DIR="${HOME}/.config/autostart"
    AUTOSTART_FILE="${AUTOSTART_DIR}/bridgesessions-tray.desktop"
    mkdir -p "${AUTOSTART_DIR}"
    cat > "${AUTOSTART_FILE}" <<EOF
[Desktop Entry]
Type=Application
Name=BridgeSessions Tray
Comment=BridgeSessions fleet status tray app
Exec=${TRAY_SCRIPT_DEST}
Icon=bridgesessions
Terminal=false
X-GNOME-Autostart-enabled=true
Categories=Network;
EOF
    echo "  → Autostart entry created at ${AUTOSTART_FILE}"

    # Start tray app in background (if not already running)
    if ! pgrep -f "bs_tray.py" >/dev/null 2>&1; then
      if [ -x "${TRAY_SCRIPT_DEST}" ]; then
        nohup python3 "${TRAY_SCRIPT_DEST}" >/dev/null 2>&1 &
        echo "  → Tray app started (PID $!)."
      fi
    else
      echo "  → Tray app already running."
    fi

    # ── 3c. Verify xdotool for CUA ───────────────────────────────
    if ! command -v xdotool >/dev/null 2>&1; then
      echo "→ Installing xdotool for CUA desktop automation..."
      if command -v apt >/dev/null 2>&1; then
        sudo apt install -y xdotool 2>/dev/null || echo "  → WARNING: apt install xdotool failed."
      elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y xdotool 2>/dev/null || echo "  → WARNING: dnf install xdotool failed."
      elif command -v pacman >/dev/null 2>&1; then
        sudo pacman -S --noconfirm xdotool 2>/dev/null || echo "  → WARNING: pacman install xdotool failed."
      else
        echo "  → WARNING: Could not auto-install xdotool. Install manually for CUA support."
      fi
    else
      echo "→ xdotool already installed."
    fi
    ;;
esac

# ── 4. CUA helper instructions ─────────────────────────────────────
echo ""
case "${os}" in
  Linux)
    echo "→ CUA helper: xdotool installed above (Linux daemon uses xdotool directly)."
    ;;
  *)
    echo "→ CUA helper (desktop automation):"
    echo "   For screen capture + input injection on desktop sessions, run:"
    echo "   ${BIN_ABS} --cua-helper &"
    ;;
esac
echo ""

# ── 5. Join mode ───────────────────────────────────────────────────
if [ $# -ge 2 ] && [ "$1" = "join" ]; then
  shift
  # Ensure old daemon is stopped before join (prevents conflicts)
  echo "→ Stopping existing daemon before join..."
  stop_daemon
  sleep 1
  echo "→ Joining mesh: bridgesessions join $@ --start"
  exec "${INSTALL_DIR}/${BIN_NAME}" join "$@" --start
fi

echo "→ Ready."
echo "   To join a mesh, run:"
echo "   ${INSTALL_DIR}/${BIN_NAME} join <host-addr> <invite-code> --start"
