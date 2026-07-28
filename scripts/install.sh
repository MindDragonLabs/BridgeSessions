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

CURRENT=""
[ -f "${VERSION_FILE}" ] && CURRENT="$(cat "${VERSION_FILE}")" || true
if [ "${CURRENT}" = "${TAG}" ] && [ -x "${INSTALL_DIR}/${BIN_NAME}" ]; then
  validate_binary "${INSTALL_DIR}/${BIN_NAME}"
  echo "→ bridgesessions ${TAG} already installed."
else
  echo "→ Downloading bridgesessions ${TAG} for ${os}-${arch}..."
  TMP_BIN="${INSTALL_DIR}/.${BIN_NAME}.download.$$"
  trap 'rm -f "${TMP_BIN:-}"' EXIT INT TERM
  curl -fsSL --progress-bar "${BASE}/${BIN}" -o "${TMP_BIN}"
  chmod +x "${TMP_BIN}"
  validate_binary "${TMP_BIN}"
  REPORTED_VERSION=$("${TMP_BIN}" --version)
  EXPECTED_VERSION=${TAG#v}
  if [ "${REPORTED_VERSION}" != "${EXPECTED_VERSION}" ]; then
    echo "ERROR: downloaded binary reports ${REPORTED_VERSION}; expected ${EXPECTED_VERSION}" >&2
    exit 1
  fi
  mv -f "${TMP_BIN}" "${INSTALL_DIR}/${BIN_NAME}"
  echo "${TAG}" > "${VERSION_FILE}"
  trap - EXIT INT TERM
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
