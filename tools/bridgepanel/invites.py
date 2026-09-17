"""BridgePanel — invite minting, listing, and join-page rendering.

The daemon owns the actual invite token: ``INVITE`` on the control socket
returns a fresh single-use token and opens the TLS join window. This module
wraps that verb, records the tokens the panel itself mints so the operator
can copy them later, and renders a private self-contained join page.

Nothing here invents a token. If the daemon is down, minting fails cleanly.
"""
from __future__ import annotations

import html as _html
import json
import os
import re
import subprocess
import threading
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .api import bs_ipc
from .consts import data_home, state_path  # noqa: F401  (state_path for parity)

INVITE_TTL_SEC = 2 * 60 * 60          # matches kInviteTtl in bs-mesh-transfer.h
JOIN_WINDOW_SEC_DEFAULT = 300         # matches mesh.join_window_max_secs default
MAX_STORED = 50                       # cap the persisted invite ledger
CONFIG_DIR = Path.home() / ".bridgesessions"
INSTALL_SH = (
    "https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/"
    "main/scripts/install.sh"
)
INSTALL_PS1 = (
    "https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/"
    "main/scripts/install.ps1"
)

_lock = threading.Lock()


def _config_value(key: str) -> str:
    """Read a ``key value`` line from ~/.bridgesessions/config, best-effort."""
    try:
        for line in (CONFIG_DIR / "config").read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[0] == key:
                return parts[1].strip()
    except OSError:
        pass
    return ""


def _tailscale_ip4() -> str:
    try:
        out = subprocess.run(
            ["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=5,
        )
        for line in (out.stdout or "").splitlines():
            line = line.strip()
            if line:
                return line
    except (OSError, subprocess.SubprocessError):
        pass
    return ""


def join_window_seconds() -> int:
    raw = _config_value("mesh.join_window_max_secs")
    try:
        val = int(raw)
        return val if val > 0 else JOIN_WINDOW_SEC_DEFAULT
    except (TypeError, ValueError):
        return JOIN_WINDOW_SEC_DEFAULT


def seed_info() -> dict:
    """Return the reachable seed endpoint for a joiner as {addr, host, port}.

    Mirrors the address-selection logic in ``main.cpp`` for ``bs invite``:
    prefer ``node.listen``, fall back to the Tailscale IPv4, then loopback.
    """
    host = ""
    port = 19949
    raw = _config_value("node.listen")
    match = re.match(r"^(.+?):(\d{1,5})$", raw or "")
    if match:
        host = match.group(1).strip("[]")
        port = int(match.group(2))
    if not host or host in ("0.0.0.0", "::"):
        host = _tailscale_ip4() or "127.0.0.1"
    return {"addr": f"{host}:{port}", "host": host, "port": port}


def _ledger_path() -> Path:
    return data_home() / "invites.json"


def _load_ledger() -> list[dict]:
    path = _ledger_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, list):
            return [d for d in data if isinstance(d, dict)]
    except (OSError, ValueError):
        pass
    return []


def _save_ledger(items: list[dict]) -> None:
    path = _ledger_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(items, indent=2), encoding="utf-8")
    try:
        os.chmod(tmp, 0o600)
    except OSError:
        pass
    tmp.replace(path)


def _is_expired(record: dict) -> bool:
    try:
        expires = datetime.fromisoformat(record.get("expires_at", ""))
    except (TypeError, ValueError):
        return True
    return datetime.now(timezone.utc) > expires


def mint_invite() -> dict:
    """Mint one invite token via the daemon and record it in the ledger."""
    token = (bs_ipc("INVITE") or "").strip()
    if not token or token.startswith("ERROR"):
        return {"ok": False, "error": "daemon invite failed (daemon not running?)"}
    if not re.fullmatch(r"[0-9a-f]{32,128}", token):
        return {"ok": False, "error": "daemon returned a malformed invite token"}

    seed = seed_info()
    now = datetime.now(timezone.utc)
    record = {
        "token": token,
        "seed": seed["addr"],
        "host": seed["host"],
        "port": seed["port"],
        "window_seconds": join_window_seconds(),
        "ttl_seconds": INVITE_TTL_SEC,
        "created_at": now.isoformat(),
        "expires_at": (now + timedelta(seconds=INVITE_TTL_SEC)).isoformat(),
    }
    with _lock:
        ledger = _load_ledger()
        ledger.insert(0, record)
        ledger = [r for r in ledger if not _is_expired(r)][:MAX_STORED]
        _save_ledger(ledger)
    return {"ok": True, "invite": record}


def list_invites() -> dict:
    """Return the current ledger (expired entries pruned) plus seed metadata."""
    with _lock:
        ledger = _load_ledger()
        kept = [r for r in ledger if not _is_expired(r)]
        if len(kept) != len(ledger):
            _save_ledger(kept)
    return {
        "ok": True,
        "seed": seed_info(),
        "window_seconds": join_window_seconds(),
        "ttl_seconds": INVITE_TTL_SEC,
        "invites": kept,
    }


def join_commands(record: dict) -> list[dict]:
    """Human-ready join command set for a minted invite.

    Every command embeds the single-use token inline so the operator can
    copy/paste one command to the new host with no separate token-file step.
    The token is strict hex (``[0-9a-f]{32,128}``), so it is shell-safe to
    interpolate inside single quotes on both sh and PowerShell.
    """
    seed = record.get("seed", "")
    token = record.get("token", "")
    return [
        {
            "label": "Linux / macOS (join only)",
            "shell": "sh",
            "cmd": f"printf '%s\\n' '{token}' | bridgesessions join {seed} - --start",
        },
        {
            "label": "Linux / macOS (install + join)",
            "shell": "sh",
            "cmd": f"curl -fsSL {INSTALL_SH} | bash -s -- join {seed} {token} --start",
        },
        {
            "label": "Windows PowerShell (join only)",
            "shell": "powershell",
            "cmd": f"'{token}' | bridgesessions join {seed} - --start",
        },
        {
            "label": "Windows PowerShell (install + join)",
            "shell": "powershell",
            "cmd": (
                f"irm {INSTALL_PS1} | iex; "
                f"'{token}' | & \"$env:LOCALAPPDATA\\bridgesessions\\bridgesessions.exe\" "
                f"join {seed} - --start"
            ),
        },
    ]


def render_invite_page(record: dict) -> str:
    """Render a self-contained, shareable join page for one invite.

    The output embeds the single-use token and is therefore only ever served
    to an authenticated panel request (POST /api/invites/page), never cached.
    """
    seed = _html.escape(record.get("seed", ""))
    token = _html.escape(record.get("token", ""))
    window = int(record.get("window_seconds", JOIN_WINDOW_SEC_DEFAULT) or 0)
    expires = _html.escape(record.get("expires_at", ""))
    cmds = join_commands(record)

    blocks = []
    for c in cmds:
        cmd_html = _html.escape(c["cmd"]).replace("\n", "&#10;")
        blocks.append(
            f'<div class="cmdblock"><div class="cmdlabel">'
            f'{_html.escape(c["label"])}</div>'
            f'<pre class="cmd">{cmd_html}</pre>'
            f'<button class="copy" data-cmd="{cmd_html}">Copy</button></div>'
        )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Join the mesh</title>
<style>
  :root {{
    --bg:#14171C; --surface:#1B1F26; --surface2:#232833; --border:#2A303B;
    --text:#E9EDF2; --muted:#9BA4B0; --faint:#66707E; --accent:#3FA9E0;
    --danger:#E26D6D; --warn:#E2A33D;
    --mono:'JetBrains Mono',ui-monospace,'SF Mono',Menlo,Consolas,monospace;
    --sans:-apple-system,system-ui,'Segoe UI',Roboto,sans-serif;
  }}
  * {{ box-sizing:border-box; margin:0; padding:0; }}
  body {{ background:var(--bg); color:var(--text); font-family:var(--sans);
    line-height:1.5; padding:40px 20px; display:flex; justify-content:center; }}
  .card {{ width:100%; max-width:680px; background:var(--surface);
    border:1px solid var(--border); border-radius:14px; padding:32px; }}
  h1 {{ font-size:22px; letter-spacing:-0.01em; margin-bottom:6px; }}
  .sub {{ color:var(--muted); font-size:13px; margin-bottom:24px; }}
  .warn {{ background:rgba(226,163,61,0.12); border:1px solid var(--warn);
    color:var(--warn); border-radius:8px; padding:10px 12px; font-size:12.5px;
    margin-bottom:20px; }}
  .field {{ margin-bottom:18px; }}
  .field label {{ display:block; font-size:11px; text-transform:uppercase;
    letter-spacing:.08em; color:var(--muted); margin-bottom:6px; }}
  .token {{ font-family:var(--mono); font-size:13px; background:var(--surface2);
    border:1px solid var(--border); border-radius:8px; padding:12px;
    word-break:break-all; }}
  .cmdblock {{ margin-bottom:16px; }}
  .cmdlabel {{ font-size:12px; color:var(--muted); margin-bottom:6px; }}
  .cmd {{ font-family:var(--mono); font-size:12.5px; background:var(--surface2);
    border:1px solid var(--border); border-radius:8px; padding:12px;
    white-space:pre-wrap; word-break:break-all; position:relative; }}
  .copy {{ margin-top:6px; background:var(--accent); color:#fff; border:none;
    border-radius:6px; padding:6px 12px; font-size:12px; cursor:pointer; }}
  .copy:active {{ opacity:.8; }}
  .note {{ color:var(--faint); font-size:12px; margin-top:20px; }}
</style>
</head>
<body>
  <div class="card">
    <h1>Join the BridgeSessions mesh</h1>
    <div class="sub">Single-use invite &middot; seed <b>{seed}</b></div>
    <div class="warn">This token is single-use and expires in {window // 60} minutes
      (invite ledger TTL 2h). Do not share it in a public channel.</div>
    <div class="field">
      <label>Invite token</label>
      <div class="token">{token}</div>
    </div>
    {''.join(blocks)}
    <div class="note">Expires {expires} (UTC). Run one command set on the new
      machine, then confirm the node appears in the mesh before discarding this page.</div>
  </div>
  <script>
    document.querySelectorAll(".copy").forEach(function(b){{
      b.addEventListener("click", function(){{
        var txt = b.getAttribute("data-cmd");
        navigator.clipboard.writeText(txt).then(function(){{
          b.textContent = "Copied";
          setTimeout(function(){{ b.textContent = "Copy"; }}, 1500);
        }});
      }});
    }});
  </script>
</body>
</html>"""
