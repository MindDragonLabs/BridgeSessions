#!/usr/bin/env bash
set -eu
# BridgeSessions one-line install (Linux / macOS)
#
#   curl -fsSL https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.9-alpha5/scripts/install.sh | bash
#
# Or join a mesh in one command:
#
#   curl ... | bash -s -- join <host-addr> <invite-code>
#
# On Windows (PowerShell):
#   Invoke-WebRequest -Uri "https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.9-alpha5/dist/bridgesessions-windows-x86_64.exe" -OutFile "$env:LOCALAPPDATA\bridgesessions.exe"

TAG="${BRIDGESESSIONS_TAG:-v2.0.9-alpha5}"
BASE="https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/${TAG}/dist"
INSTALL_DIR="${HOME}/.local/bin"
VERSION_FILE="${INSTALL_DIR}/.bridgesessions-version"

os=$(uname -s)
arch=$(uname -m)
case "${os}" in
  Linux)
    BIN="bridgesessions-linux-x86_64"
    BIN_NAME="bridgesessions"
    ;;
  Darwin)
    BIN="bridgesessions-macos-arm64"
    BIN_NAME="bridgesessions"
    ;;
  *)
    echo "Unsupported OS: ${os}" >&2
    echo "On Windows, download manually:" >&2
    echo "  ${BASE}/bridgesessions-windows-x86_64.exe" >&2
    exit 1
    ;;
esac

mkdir -p "${INSTALL_DIR}"

CURRENT=""
[ -f "${VERSION_FILE}" ] && CURRENT="$(cat "${VERSION_FILE}")" || true
if [ "${CURRENT}" = "${TAG}" ] && [ -x "${INSTALL_DIR}/${BIN_NAME}" ]; then
  echo "→ bridgesessions ${TAG} already installed."
else
  echo "→ Downloading bridgesessions ${TAG} for ${os}..."
  curl -fsSL --progress-bar "${BASE}/${BIN}" -o "${INSTALL_DIR}/${BIN_NAME}"
  chmod +x "${INSTALL_DIR}/${BIN_NAME}"
  echo "${TAG}" > "${VERSION_FILE}"
fi

"${INSTALL_DIR}/${BIN_NAME}" --version

if ! echo "${PATH}" | grep -q "${INSTALL_DIR}"; then
  echo "→ Add to PATH: export PATH=\"${INSTALL_DIR}:\$PATH\""
  echo "   (or restart your shell)"
fi

# ── Join mode ─────────────────────────────────────────────────────
if [ $# -ge 2 ] && [ "$1" = "join" ]; then
  shift
  echo "→ Joining mesh: bridgesessions join $@ --start"
  exec "${INSTALL_DIR}/${BIN_NAME}" join "$@" --start
fi

echo "→ Ready."
echo "   To join a mesh, run:"
echo "   ${INSTALL_DIR}/${BIN_NAME} join <host-addr> <invite-code> --start"
