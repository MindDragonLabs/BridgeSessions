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
      # Stop systemd service FIRST to prevent auto-restart from re-locking the binary
      systemctl --user stop bridgesessions.service 2>/dev/null || true
      # Mask temporarily to prevent respawn during swap
      systemctl --user disable bridgesessions.service 2>/dev/null || true
      pkill -f "bridgesessions.*--config" 2>/dev/null || true
      ;;
  esac
  sleep 1
  # Verify daemon is actually stopped
  if pgrep -f "bridgesessions.*--config" >/dev/null 2>&1; then
    # Force kill if still running
    pkill -9 -f "bridgesessions.*--config" 2>/dev/null || true
    sleep 1
  fi
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
      # Fallback: if systemd not available, start manually
      if ! systemctl --user is-active bridgesessions.service >/dev/null 2>&1; then
        if ! pgrep -f "bridgesessions.*--config" >/dev/null 2>&1; then
          nohup "${BIN_ABS}" --config "${CONFIG_PATH}" >/dev/null 2>&1 &
        fi
      fi
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

  # Atomic swap: rename old binary out of the way, then install new one.
  # This avoids "Text file busy" (ETXTBSY) on Linux when the daemon
  # process is still holding the file open during shutdown.
  if [ -f "${INSTALL_DIR}/${BIN_NAME}" ]; then
    mv -f "${INSTALL_DIR}/${BIN_NAME}" "${INSTALL_DIR}/${BIN_NAME}.old" 2>/dev/null || true
  fi
  mv -f "${TMP_BIN}" "${INSTALL_DIR}/${BIN_NAME}"
  chmod +x "${INSTALL_DIR}/${BIN_NAME}"

  # Create 'bs' symlink for CLI shorthand (was only created in the .app branch,
  # so bare-binary installs ended up with 'bs: command not found').
  ln -sf "${INSTALL_DIR}/${BIN_NAME}" "${INSTALL_DIR}/bs"

  # Clean up old binary after successful swap
  rm -f "${INSTALL_DIR}/${BIN_NAME}.old" 2>/dev/null || true
  echo "${TAG}" > "${VERSION_FILE}"
  trap - EXIT INT TERM
  echo "→ Binary updated."
else
  validate_binary "${INSTALL_DIR}/${BIN_NAME}"
  echo "→ bridgesessions ${TAG} already installed."
fi

"${INSTALL_DIR}/${BIN_NAME}" --version

# ── 1c. Ensure INSTALL_DIR is on PATH (macOS/Linux) ────────────────
# A fresh Mac won't have ~/.local/bin on PATH — bs would be 'command not found'
# after install. Append an export line to the user's shell rc automatically.
if ! echo "${PATH}" | grep -q "${INSTALL_DIR}"; then
  # Pick the login shell's rc file: macOS zsh → .zshrc, Linux bash → .bashrc,
  # fall back to .profile if neither exists yet.
  RC_FILE=""
  if [ -n "${ZSH_VERSION:-}" ] || [ "$(basename "$SHELL" 2>/dev/null)" = "zsh" ]; then
    RC_FILE="${HOME}/.zshrc"
  elif [ "$(basename "$SHELL" 2>/dev/null)" = "bash" ]; then
    RC_FILE="${HOME}/.bashrc"
  fi
  if [ -z "${RC_FILE}" ]; then
    [ -f "${HOME}/.zshrc" ] && RC_FILE="${HOME}/.zshrc"
    [ -z "${RC_FILE}" ] && [ -f "${HOME}/.bashrc" ] && RC_FILE="${HOME}/.bashrc"
    [ -z "${RC_FILE}" ] && RC_FILE="${HOME}/.profile"
  fi

  if ! grep -q "export PATH=\"${INSTALL_DIR}:\$PATH\"" "${RC_FILE}" 2>/dev/null; then
    # Don't append if the file already has a .local/bin line in any form
    if ! grep -q '\.local/bin' "${RC_FILE}" 2>/dev/null; then
      {
        echo ""
        echo "# BridgeSessions CLI on PATH"
        echo "export PATH=\"${INSTALL_DIR}:\$PATH\""
      } >> "${RC_FILE}"
      echo "→ Added '${INSTALL_DIR}' to PATH in ${RC_FILE}"
    fi
  fi
  echo "→ Restart your shell (or run: source ${RC_FILE}) to use 'bs'"
else
  echo "→ PATH OK: ${INSTALL_DIR} is on PATH"
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

    # ── 3c. Proper .app bundle with Developer ID signing (macOS TCC fix) ──
    # TCC tracks permissions by code signature. Adhoc signing produces a new
    # CDHash on every rebuild, silently invalidating Screen Recording and
    # Accessibility grants. A Developer ID-signed .app bundle gives TCC a
    # stable identity (TeamIdentifier + CFBundleIdentifier) that persists.
    APP_BUNDLE="/Applications/BridgeSessions.app"
    APP_BIN="${APP_BUNDLE}/Contents/MacOS/bridgesessions"

    # Check if we have a pre-signed .app bundle in dist/
    REPO_APP="$(cd "$(dirname "$0")/.." && pwd)/dist/BridgeSessions.app"
    if [ ! -d "${REPO_APP}" ]; then
      REPO_APP="$(cd "$(dirname "$0")" && pwd)/../dist/BridgeSessions.app"
    fi

    if [ -d "${REPO_APP}" ]; then
      echo "→ Installing BridgeSessions.app (Developer ID signed) to /Applications/..."

      # Purge ALL old TCC entries for bridgesessions (ad hoc identities)
      tccutil reset ScreenCapture bridgesessions 2>/dev/null || true
      tccutil reset Accessibility bridgesessions 2>/dev/null || true
      tccutil reset ScreenCapture com.minddragon.bridgesessions 2>/dev/null || true
      tccutil reset Accessibility com.minddragon.bridgesessions 2>/dev/null || true

      # Stop daemons before swap
      launchctl bootout "gui/$(id -u)/com.bridgesessions.mesh" 2>/dev/null || true
      launchctl bootout "gui/$(id -u)/com.bridgesessions.cua-helper" 2>/dev/null || true
      pkill -9 -f bridgesessions 2>/dev/null || true
      sleep 1

      # Install the .app bundle
      rm -rf "${APP_BUNDLE}"
      cp -R "${REPO_APP}" /Applications/

      # Symlink for CLI compatibility (~/.local/bin/bridgesessions → .app binary)
      ln -sf "${APP_BIN}" "${BIN_ABS}"

      # Update launchd plists to use .app bundle binary
      cat > "${PLIST_DIR}/com.bridgesessions.mesh.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.bridgesessions.mesh</string>
  <key>ProgramArguments</key>
  <array><string>${APP_BIN}</string><string>--config</string><string>${CONFIG_PATH}</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${APP_HOME}/daemon.log</string>
  <key>StandardErrorPath</key><string>${APP_HOME}/daemon.err</string>
</dict>
</plist>
EOF

      # CUA helper plist (runs in user session for GUI access)
      cat > "${PLIST_DIR}/com.bridgesessions.cua-helper.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.bridgesessions.cua-helper</string>
  <key>ProgramArguments</key>
  <array><string>${APP_BIN}</string><string>--config</string><string>${CONFIG_PATH}</string><string>--cua-helper</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${APP_HOME}/cua-helper.log</string>
  <key>StandardErrorPath</key><string>${APP_HOME}/cua-helper.err</string>
</dict>
</plist>
EOF

      # Load both services
      launchctl bootstrap "gui/$(id -u)" "${PLIST_DIR}/com.bridgesessions.mesh.plist" 2>/dev/null || \
        launchctl load -w "${PLIST_DIR}/com.bridgesessions.mesh.plist" 2>/dev/null || true
      launchctl bootstrap "gui/$(id -u)" "${PLIST_DIR}/com.bridgesessions.cua-helper.plist" 2>/dev/null || \
        launchctl load -w "${PLIST_DIR}/com.bridgesessions.cua-helper.plist" 2>/dev/null || true
      sleep 2

      echo "→ BridgeSessions.app installed (Developer ID: QL5MD8FKPL)"
      echo "→ Daemons restarted from .app bundle"

      # Open System Settings for TCC permission grant
      open "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture" 2>/dev/null
      echo ""
      echo "⚠️  ACTION REQUIRED — Grant permissions on this Mac's screen:"
      echo "   1. System Settings → Privacy & Security → Screen Recording"
      echo "      → Find 'BridgeSessions' → Toggle ON"
      echo "   2. System Settings → Privacy & Security → Accessibility"
      echo "      → Find 'BridgeSessions' → Toggle ON"
      echo "   3. Restart: launchctl kickstart -k gui/\$(id -u)/com.bridgesessions.cua-helper"
      echo ""
      echo "   (Developer ID signing ensures permissions persist across updates)"
    else
      echo "→ NOTE: BridgeSessions.app not found — creating local .app wrapper for TCC"
      # Create a minimal .app bundle so TCC (Screen Recording, Accessibility)
      # can track a stable bundle identity. Without a bundle, macOS won't list
      # the bare binary in System Settings → Privacy & Security.
      LOCAL_APP="${HOME}/Applications/BridgeSessions.app"
      mkdir -p "${LOCAL_APP}/Contents/MacOS"
      # Resolve symlinks — BIN_ABS may point back to a previous .app install
      REAL_BIN="${BIN_ABS}"
      if [ -L "${BIN_ABS}" ]; then
        REAL_BIN=$(readlink -f "${BIN_ABS}" 2>/dev/null || readlink "${BIN_ABS}")
        # readlink -f not available on older macOS — resolve manually
        if [ -z "${REAL_BIN}" ] || [ ! -f "${REAL_BIN}" ]; then
          REAL_BIN=$(cd "$(dirname "${BIN_ABS}")" && readlink "$(basename "${BIN_ABS}")")
          case "${REAL_BIN}" in
            /*) ;; # absolute — ok
            *) REAL_BIN="$(dirname "${BIN_ABS}")/${REAL_BIN}" ;;
          esac
        fi
      fi
      # Copy the real binary, not the symlink
      cp -f "${REAL_BIN}" "${LOCAL_APP}/Contents/MacOS/bridgesessions"
      chmod 755 "${LOCAL_APP}/Contents/MacOS/bridgesessions"
      cat > "${LOCAL_APP}/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>BridgeSessions</string>
  <key>CFBundleDisplayName</key><string>BridgeSessions</string>
  <key>CFBundleIdentifier</key><string>com.mindragon.bridgesessions</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleExecutable</key><string>bridgesessions</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSUIElement</key><true/>
  <key>NSDesktopFolderUsageDescription</key>
  <string>BridgeSessions needs access to automate desktop tasks.</string>
  <key>NSCameraUsageDescription</key>
  <string>BridgeSessions captures the screen for remote automation.</string>
</dict>
</plist>
EOF
      # The downloaded binary from dist/ is already Developer ID signed.
      # Do NOT re-sign — that would strip the signature (Rana's machine
      # has no Developer ID cert). Just copy the signed binary as-is.
      # TCC tracks the TeamIdentifier (QL5MD8FKPL) from the embedded signature.
      echo "→ .app bundle created (preserving signed binary)"
      # Point CLI symlink at the .app binary
      ln -sf "${LOCAL_APP}/Contents/MacOS/bridgesessions" "${BIN_ABS}"
      APP_BIN="${LOCAL_APP}/Contents/MacOS/bridgesessions"

      # Update launchd plists to use .app binary
      cat > "${PLIST_DIR}/com.bridgesessions.mesh.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.bridgesessions.mesh</string>
  <key>ProgramArguments</key>
  <array><string>${APP_BIN}</string><string>--config</string><string>${CONFIG_PATH}</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${APP_HOME}/daemon.log</string>
  <key>StandardErrorPath</key><string>${APP_HOME}/daemon.err</string>
</dict>
</plist>
EOF

      # CUA helper plist (runs in user session for GUI access)
      CUA_PLIST="${PLIST_DIR}/com.bridgesessions.cua-helper.plist"
      cat > "${CUA_PLIST}" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.bridgesessions.cua-helper</string>
  <key>ProgramArguments</key>
  <array><string>${APP_BIN}</string><string>--config</string><string>${CONFIG_PATH}</string><string>--cua-helper</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>${APP_HOME}/cua-helper.log</string>
  <key>StandardErrorPath</key><string>${APP_HOME}/cua-helper.err</string>
</dict>
</plist>
EOF
      launchctl bootstrap "gui/$(id -u)" "${CUA_PLIST}" 2>/dev/null || \
        launchctl load -w "${CUA_PLIST}" 2>/dev/null || true
      echo "→ CUA helper launchd agent installed (.app bundle wrapper)."

      # Purge old TCC entries and register the new bundle identity
      tccutil reset ScreenCapture com.mindragon.bridgesessions 2>/dev/null || true
      tccutil reset Accessibility com.mindragon.bridgesessions 2>/dev/null || true

      # Register the bundle with LaunchServices so TCC can resolve it,
      # then restart both agents so they run from the .app bundle
      # (the mesh daemon started earlier is still on the bare path).
      LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister"
      [ -x "${LSREGISTER}" ] && "${LSREGISTER}" -f "${LOCAL_APP}" 2>/dev/null || true

      launchctl bootout "gui/$(id -u)/com.bridgesessions.mesh" 2>/dev/null || true
      launchctl bootout "gui/$(id -u)/com.bridgesessions.cua-helper" 2>/dev/null || true
      sleep 1
      launchctl bootstrap "gui/$(id -u)" "${PLIST_DIR}/com.bridgesessions.mesh.plist" 2>/dev/null || \
        launchctl load -w "${PLIST_DIR}/com.bridgesessions.mesh.plist" 2>/dev/null || true
      launchctl bootstrap "gui/$(id -u)" "${CUA_PLIST}" 2>/dev/null || \
        launchctl load -w "${CUA_PLIST}" 2>/dev/null || true
      sleep 2
      echo "→ Daemons restarted from .app bundle."

      # Do NOT 'open' the .app — that launches the binary bare (no
      # --config). TCC entries are created by the CUA helper calling
      # AXIsProcessTrustedWithOptions + CGRequestScreenCaptureAccess at
      # startup, which shows the grant prompts automatically.
      echo ""
      echo "⚠️  ACTION REQUIRED — Grant permissions on this Mac's screen:"
      echo "   The CUA helper will pop system prompts for:"
      echo "   1. Screen Recording   → click 'Open System Settings' → toggle ON"
      echo "   2. Accessibility      → follow the prompt"
      echo "   (If no prompt appears: System Settings → Privacy & Security,"
      echo "    look for 'BridgeSessions' in both lists, toggle ON, then:"
      echo "    launchctl kickstart -k gui/\$(id -u)/com.bridgesessions.cua-helper)"
      echo ""
    fi
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

  # Strip duplicate --start flag (invite output already includes it)
  JOIN_ARGS=""
  HAS_START=0
  for arg in "$@"; do
    if [ "$arg" = "--start" ]; then
      HAS_START=1
    else
      JOIN_ARGS="${JOIN_ARGS} ${arg}"
    fi
  done
  [ "$HAS_START" = "1" ] && JOIN_ARGS="${JOIN_ARGS} --start"

  # Retry join up to 3 times — TLS unexpected-eof happens when the
  # join window on the host is momentarily closed or the token was
  # just regenerated. The host re-opens the window on each invite.
  JOIN_OK=0
  for attempt in 1 2 3; do
    echo "→ Joining mesh (attempt ${attempt}/3)..."
    if "${INSTALL_DIR}/${BIN_NAME}" join ${JOIN_ARGS}; then
      JOIN_OK=1
      break
    fi
    echo "→ Join attempt ${attempt} failed, retrying in 2s..."
    sleep 2
  done

  if [ "${JOIN_OK}" = "0" ]; then
    echo "ERROR: All join attempts failed." >&2
    echo "  The invite token may be expired or the host daemon may need a fresh 'bs invite'." >&2
    exit 1
  fi
  echo "→ Join successful."
  exit 0
fi

echo "→ Ready."
echo "   To join a mesh, run:"
echo "   ${INSTALL_DIR}/${BIN_NAME} join <host-addr> <invite-code> --start"
