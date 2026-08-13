#!/usr/bin/env bash
# Install whatisit-nl2sh + Q4_K_M weights on a Linux mesh hub.
# NL → single shell command only. Run outputs via `bs job` / `bs run-script`.
#
# Usage (on hub):
#   bash scripts/nl2sh-hub-setup.sh [--dir ~/nl2sh] [--size 1.5b|3b]
# Then:
#   whatisit list large files under /var/log
#   curl -sS localhost:8741/v1/nl2sh -d '{"q":"disk free","os":"linux"}'
set -euo pipefail

DIR="${NL2SH_DIR:-$HOME/nl2sh}"
SIZE="1.5b"
PORT="${NL2SH_HTTP_PORT:-8741}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) DIR="$2"; shift 2 ;;
    --size) SIZE="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,20p' "$0"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$DIR"
cd "$DIR"

if ! command -v whatisit >/dev/null 2>&1; then
  python3 -m pip install --user -U whatisit huggingface_hub
  export PATH="$HOME/.local/bin:$PATH"
fi

echo "→ Installing model size=$SIZE into $DIR"
if [[ "$SIZE" == "3b" ]]; then
  whatisit setup --size 3b || {
    hf download ThorOdinson246/nl2sh-3b-Q4_K_M nl2sh-3b-Q4_K_M.gguf --local-dir "$DIR"
    whatisit setup --model "$DIR/nl2sh-3b-Q4_K_M.gguf"
  }
else
  whatisit setup --size 1.5b || {
    hf download ThorOdinson246/nl2sh-1.5b-Q4_K_M nl2sh-1.5b-Q4_K_M.gguf --local-dir "$DIR"
    whatisit setup --model "$DIR/nl2sh-1.5b-Q4_K_M.gguf"
  }
fi

whatisit doctor || true
echo "smoke: $(whatisit -q disk free space on root || true)"

# Tiny local HTTP wrapper (stdlib only) — agents call this instead of shelling whatisit.
cat > "$DIR/nl2sh_http.py" <<'PY'
#!/usr/bin/env python3
"""Minimal NL2SH HTTP for fleet agents. Bind 127.0.0.1 only."""
from __future__ import annotations
import json, os, subprocess, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("NL2SH_HTTP_PORT", "8741"))
WHATISIT = os.environ.get("WHATISIT_BIN", "whatisit")

class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quieter
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self):
        if self.path in ("/health", "/"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true,"service":"nl2sh"}\n')
            return
        self.send_error(404)

    def do_POST(self):
        if self.path not in ("/v1/nl2sh", "/nl2sh"):
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(body.decode("utf-8") or "{}")
        except json.JSONDecodeError as e:
            self.send_error(400, str(e))
            return
        q = (req.get("q") or req.get("query") or "").strip()
        os_hint = (req.get("os") or "linux").strip()
        if not q:
            self.send_error(400, "missing q")
            return
        # Prefix OS so the model sees the target dialect.
        prompt = f"[{os_hint}] {q}"
        try:
            out = subprocess.check_output(
                [WHATISIT, "-q", prompt],
                text=True,
                timeout=float(req.get("timeout_sec", 30)),
                stderr=subprocess.STDOUT,
            ).strip()
            danger = any(
                t in out
                for t in ("rm -rf /", "mkfs", "dd if=", ":(){", "shutdown", "reboot")
            )
            payload = {"ok": True, "command": out, "os": os_hint, "danger": danger}
            code = 200
        except subprocess.CalledProcessError as e:
            payload = {"ok": False, "error": e.output or str(e)}
            code = 500
        except Exception as e:
            payload = {"ok": False, "error": str(e)}
            code = 500
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

if __name__ == "__main__":
    # Loopback only — mesh/Tailscale agents should reach via SSH tunnel or bs shell curl.
    httpd = HTTPServer(("127.0.0.1", PORT), H)
    print(f"nl2sh http on 127.0.0.1:{PORT}", flush=True)
    httpd.serve_forever()
PY
chmod +x "$DIR/nl2sh_http.py"

# systemd user unit (optional)
mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/nl2sh-http.service" <<UNIT
[Unit]
Description=NL2SH local HTTP (whatisit)
After=default.target

[Service]
Type=simple
Environment=NL2SH_HTTP_PORT=$PORT
Environment=PATH=$HOME/.local/bin:/usr/local/bin:/usr/bin
WorkingDirectory=$DIR
ExecStart=/usr/bin/python3 $DIR/nl2sh_http.py
Restart=on-failure

[Install]
WantedBy=default.target
UNIT

if command -v systemctl >/dev/null 2>&1; then
  systemctl --user daemon-reload || true
  systemctl --user enable --now nl2sh-http.service || true
  systemctl --user status nl2sh-http.service --no-pager || true
else
  echo "No systemd user — start manually:"
  echo "  python3 $DIR/nl2sh_http.py"
fi

echo ""
echo "Done. Test from this host:"
echo "  curl -sS 127.0.0.1:$PORT/health"
echo "  curl -sS 127.0.0.1:$PORT/v1/nl2sh -H 'Content-Type: application/json' -d '{\"q\":\"list open ports\",\"os\":\"linux\"}'"
echo "From another mesh node:"
echo "  bs shell <peer> --cmd 'curl -sS 127.0.0.1:$PORT/v1/nl2sh -H Content-Type:application/json -d \"{\\\"q\\\":\\\"disk free\\\",\\\"os\\\":\\\"linux\\\"}\"'"
echo "Execute results only via: bs job run <peer> job.json   (never raw stacked &&)"
