#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIND=""
PORT="9770"
TRUSTED_IPS=""
ENABLE_SERVE=0

usage() {
  printf '%s\n' "Usage: $0 [--bind TAILSCALE_IP] [--port PORT] [--trusted-ip IP[,IP...]] [--tailscale-serve]"
}

while (( $# )); do
  case "$1" in
    --bind) BIND="${2:?missing --bind value}"; shift 2 ;;
    --port) PORT="${2:?missing --port value}"; shift 2 ;;
    --trusted-ip) TRUSTED_IPS="${2:?missing --trusted-ip value}"; shift 2 ;;
    --tailscale-serve) ENABLE_SERVE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$BIND" ]]; then
  BIND="$(tailscale ip -4 | python3 -c 'import sys; print(next((line.strip() for line in sys.stdin if line.strip()), ""))')"
fi
if [[ -z "$BIND" ]]; then
  printf '%s\n' "No Tailscale IPv4 address found; pass --bind explicitly." >&2
  exit 1
fi

mkdir -p "$HOME/.config/bridgepanel" "$HOME/.config/systemd/user" "$HOME/.local/bin"
install -m 0755 "$SCRIPT_DIR/bridgepanel.py" "$HOME/.local/bin/bridgepanel"
install -m 0644 "$SCRIPT_DIR/bridgepanel.service" "$HOME/.config/systemd/user/bridgepanel.service"
printf 'BRIDGEPANEL_SCRIPT=%s\nBRIDGEPANEL_BIND=%s\nBRIDGEPANEL_PORT=%s\nBRIDGEPANEL_TRUSTED_IPS=%s\n' \
  "$SCRIPT_DIR/bridgepanel.py" "$BIND" "$PORT" "$TRUSTED_IPS" \
  > "$HOME/.config/bridgepanel/environment"
chmod 0600 "$HOME/.config/bridgepanel/environment"

# ensure_dirs() (token + dirs + sample data) runs on first serve/publish,
# so no separate init step is needed for v2.
systemctl --user daemon-reload
systemctl --user enable bridgepanel.service
systemctl --user restart bridgepanel.service

if (( ENABLE_SERVE )); then
  sudo tailscale serve --bg --yes "http://${BIND}:${PORT}"
fi

printf 'BridgePanel active: http://%s:%s/<token>/\n' "$BIND" "$PORT"
if (( ENABLE_SERVE )); then
  tailscale serve status
fi
