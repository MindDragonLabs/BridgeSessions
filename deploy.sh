#!/usr/bin/env bash
# R6.3 — scp binary + config to Linux mesh nodes and restart systemd unit
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$ROOT/bridgesessions}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_shadow_to_linux}"
USER="${BS_USER:-agent}"
FECV4="${FECV4:-203.0.113.12}"
FECV3="${FECV3:-203.0.113.11}"
REMOTE_DIR="${REMOTE_DIR:-/home/agent/bridgesessions}"

deploy_one() {
  local host="$1"
  echo "=== deploy $host ==="
  scp -i "$SSH_KEY" -o StrictHostKeyChecking=accept-new "$BIN" "${USER}@${host}:${REMOTE_DIR}/bsmesh"
  ssh -i "$SSH_KEY" "${USER}@${host}" "chmod +x ${REMOTE_DIR}/bsmesh && sudo systemctl restart bsmesh && systemctl is-active bsmesh && ${REMOTE_DIR}/bsmesh --version"
}

deploy_one "$FECV4"
deploy_one "$FECV3"
echo "DEPLOY OK"