#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIND="127.0.0.1"
PORT="9770"
TRUSTED_IPS=""
ENABLE_SERVE=0
LAUNCHER="$SCRIPT_DIR/panel.py"

usage() {
  printf '%s\n' "Usage: $0 [--bind 127.0.0.1] [--port PORT] [--trusted-ip IP[,IP...]] [--tailscale-serve]"
  printf '%s\n' "The HTTP server binds loopback only. --tailscale-serve proxies to that loopback port."
}

is_loopback() {
  case "$1" in
    127.*|localhost|::1) return 0 ;;
    *) return 1 ;;
  esac
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

if ! is_loopback "$BIND"; then
  printf '%s\n' "Refusing non-loopback bind '$BIND'. BridgePanel listens on 127.0.0.1 only." >&2
  exit 1
fi
if [[ ! -f "$LAUNCHER" ]]; then
  printf '%s\n' "Missing launcher: $LAUNCHER" >&2
  exit 1
fi

mkdir -p "$HOME/.config/bridgepanel" "$HOME/.config/systemd/user" "$HOME/.local/bin"
chmod 0755 "$LAUNCHER"
cat > "$HOME/.local/bin/bridgepanel" << EOF
#!/usr/bin/env bash
exec python3 $(printf '%q' "$LAUNCHER") "\$@"
EOF
chmod 0755 "$HOME/.local/bin/bridgepanel"
install -m 0644 "$SCRIPT_DIR/bridgepanel.service" "$HOME/.config/systemd/user/bridgepanel.service"
printf 'BRIDGEPANEL_SCRIPT=%s\nBRIDGEPANEL_BIND=%s\nBRIDGEPANEL_PORT=%s\nBRIDGEPANEL_TRUSTED_IPS=%s\n' \
  "$LAUNCHER" "$BIND" "$PORT" "$TRUSTED_IPS" \
  > "$HOME/.config/bridgepanel/environment"
chmod 0600 "$HOME/.config/bridgepanel/environment"

# ensure_dirs() (token + dirs + sample data) runs on first serve/publish,
# so no separate init step is needed for v2.
systemctl --user daemon-reload
systemctl --user enable bridgepanel.service
systemctl --user restart bridgepanel.service

if (( ENABLE_SERVE )); then
  sudo tailscale serve --bg --yes "http://127.0.0.1:${PORT}"
fi

printf 'BridgePanel active: http://%s:%s/<token>/\n' "$BIND" "$PORT"
if (( ENABLE_SERVE )); then
  tailscale serve status
fi
