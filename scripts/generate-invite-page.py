#!/usr/bin/env python3
"""Generate a private, self-contained BridgeSessions join page.

The generator runs ``bridgesessions invite`` itself so the single-use token never
appears in this script's command line or terminal output. The resulting HTML file
contains the token and is therefore written with mode 0600.
"""

from __future__ import annotations

import argparse
import html
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import NamedTuple


INSTALL_URL = (
    "https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/"
    "main/scripts/install.ps1"
)


class InviteInfo(NamedTuple):
    token: str
    seed_address: str
    window_seconds: int = 300


class Commands(NamedTuple):
    new_pc: str
    rejoin: str
    confirm: str


_TOKEN_RE = re.compile(
    r"(?im)^Invite\s*\(valid\s+(\d+)([smh])\)\s*:\s*([0-9a-f]{32,128})\s*$"
)
_JOIN_RE = re.compile(
    r"(?im)^\s*(?:bs|bridgesessions)\s+join\s+([A-Za-z0-9._:\-\[\]]+:\d{1,5})(?:\s|$)"
)
_NODE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_VERSION_RE = re.compile(r"^[0-9][A-Za-z0-9._-]{0,63}$")


def parse_invite_output(output: str) -> InviteInfo:
    """Extract validated invite material from ``bridgesessions invite`` output."""
    token_match = _TOKEN_RE.search(output)
    join_match = _JOIN_RE.search(output)
    if not token_match or not join_match:
        raise ValueError("invite output did not contain a safe token and seed address")

    seed_address = join_match.group(1)
    try:
        port = int(seed_address.rsplit(":", 1)[1])
    except (IndexError, ValueError) as exc:
        raise ValueError("invite output contained an invalid seed address") from exc
    if not 1 <= port <= 65535:
        raise ValueError("invite output contained an invalid seed port")

    unit_seconds = {"s": 1, "m": 60, "h": 3600}
    window_seconds = int(token_match.group(1)) * unit_seconds[token_match.group(2).lower()]
    if not 30 <= window_seconds <= 7200:
        raise ValueError("invite output contained an invalid validity window")

    return InviteInfo(
        token=token_match.group(3),
        seed_address=seed_address,
        window_seconds=window_seconds,
    )


def validate_node_name(node_name: str) -> str:
    if not _NODE_RE.fullmatch(node_name):
        raise ValueError(
            "node name must be 1-64 characters using only letters, numbers, '.', '_', or '-'"
        )
    return node_name


def validate_version(version: str) -> str:
    normalized = version.strip().removeprefix("v")
    if not _VERSION_RE.fullmatch(normalized):
        raise ValueError("bridgesessions returned an unsafe version string")
    return normalized


def build_commands(invite: InviteInfo, node_name: str, version: str) -> Commands:
    """Build paste-ready PowerShell commands from validated values."""
    node_name = validate_node_name(node_name)
    version = validate_version(version)
    exe = r"$env:LOCALAPPDATA\bridgesessions\bridgesessions.exe"

    join_steps = (
        f"$exe='{exe}'; "
        "Stop-ScheduledTask -TaskName 'BridgeSessions' -ErrorAction SilentlyContinue; "
        "Start-Sleep -Seconds 1; "
        f"$token='{invite.token}'; "
        f"$token | & $exe join '{invite.seed_address}' - --node-name '{node_name}'; "
        "if ($LASTEXITCODE -ne 0) { throw 'BridgeSessions join failed. Ask for a fresh invite page.' }; "
        "Start-ScheduledTask -TaskName 'BridgeSessions' -ErrorAction SilentlyContinue; "
        "Start-ScheduledTask -TaskName 'BridgeSessions-CuaHelper' -ErrorAction SilentlyContinue; "
        "Start-Sleep -Seconds 3; "
        "& $exe health host"
    )
    new_pc = (
        "$ErrorActionPreference='Stop'; "
        f"$env:BRIDGESESSIONS_TAG='{version}'; "
        f"irm '{INSTALL_URL}' | iex; "
        + join_steps
    )
    rejoin = (
        "$ErrorActionPreference='Stop'; "
        f"$exe='{exe}'; "
        "if (-not (Test-Path $exe)) { throw 'BridgeSessions is not installed. Use the New PC option.' }; "
        + join_steps
    )
    confirm = (
        "$ErrorActionPreference='Stop'; "
        f"$exe='{exe}'; "
        "& $exe --version; "
        "Get-ScheduledTask -TaskName 'BridgeSessions' | Select-Object TaskName,State; "
        "& $exe health host"
    )
    return Commands(new_pc=new_pc, rejoin=rejoin, confirm=confirm)


def render_page(
    *,
    invite: InviteInfo,
    node_name: str,
    mesh_label: str,
    version: str,
    issued_at: datetime,
    window_seconds: int,
) -> str:
    """Render one dependency-free, offline-capable invite page."""
    if issued_at.tzinfo is None:
        raise ValueError("issued_at must include a timezone")
    if not 30 <= window_seconds <= 7200:
        raise ValueError("window_seconds must be between 30 and 7200")

    commands = build_commands(invite, node_name, version)
    expires_at = issued_at + timedelta(seconds=window_seconds)
    expires_epoch = int(expires_at.timestamp())
    safe_mesh = html.escape(mesh_label, quote=True)
    safe_node = html.escape(node_name, quote=True)
    safe_address = html.escape(invite.seed_address, quote=True)
    safe_version = html.escape(validate_version(version), quote=True)
    safe_new = html.escape(commands.new_pc)
    safe_rejoin = html.escape(commands.rejoin)
    safe_confirm = html.escape(commands.confirm)
    safe_issued = html.escape(issued_at.astimezone(timezone.utc).isoformat())
    safe_expires = html.escape(expires_at.astimezone(timezone.utc).isoformat())

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="robots" content="noindex,nofollow,noarchive">
  <meta name="referrer" content="no-referrer">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'none'; img-src 'none'; font-src 'none'; form-action 'none'; base-uri 'none'">
  <title>Join {safe_mesh}</title>
  <style>
    :root {{
      --ivory:#FAF9F5; --white:#FFFFFF; --slate:#141413; --clay:#D97757;
      --olive:#788C5D; --rust:#B04A3F; --oat:#E3DACC; --gray-150:#F0EEE6;
      --gray-300:#D1CFC5; --gray-500:#5F5E59; --gray-700:#3D3D3A;
      --border:1.5px solid var(--gray-300); --panel:14px; --row:9px;
      --serif:ui-serif,Georgia,"Times New Roman",serif;
      --sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
      --mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
    }}
    * {{ box-sizing:border-box; }}
    html {{ scroll-behavior:smooth; }}
    body {{ margin:0; background:var(--ivory); color:var(--gray-700); font-family:var(--sans); line-height:1.58; -webkit-font-smoothing:antialiased; }}
    button {{ font:inherit; }}
    button:focus-visible {{ outline:3px solid rgba(217,119,87,.45); outline-offset:3px; }}
    .page {{ width:min(840px,calc(100% - 32px)); margin:0 auto; padding:42px 0 80px; }}
    header {{ margin-bottom:28px; }}
    .brand {{ display:flex; align-items:center; gap:10px; font-family:var(--mono); font-size:12px; letter-spacing:.08em; text-transform:uppercase; color:var(--gray-500); }}
    .mark {{ display:grid; place-items:center; width:30px; height:30px; border-radius:9px; background:var(--slate); color:var(--ivory); font-weight:700; letter-spacing:0; }}
    h1,h2,h3 {{ color:var(--slate); font-family:var(--serif); font-weight:500; letter-spacing:-.01em; }}
    h1 {{ margin:18px 0 10px; font-size:clamp(34px,6vw,52px); line-height:1.06; }}
    .lede {{ margin:0; max-width:720px; color:var(--gray-500); font-size:17px; }}
    .status {{ display:flex; flex-wrap:wrap; align-items:center; justify-content:space-between; gap:12px; margin-top:24px; padding:14px 16px; background:rgba(120,140,93,.10); border:1px solid rgba(120,140,93,.35); border-radius:var(--row); }}
    .status-main {{ display:flex; align-items:center; gap:10px; color:#4F623C; font-weight:650; }}
    .dot {{ width:10px; height:10px; border-radius:50%; background:var(--olive); box-shadow:0 0 0 4px rgba(120,140,93,.14); }}
    .countdown {{ font-family:var(--mono); font-size:13px; color:#4F623C; }}
    body.expired .status {{ background:rgba(176,74,63,.08); border-color:rgba(176,74,63,.35); }}
    body.expired .status-main,body.expired .countdown {{ color:var(--rust); }}
    body.expired .dot {{ background:var(--rust); box-shadow:0 0 0 4px rgba(176,74,63,.12); }}
    .privacy {{ margin:12px 0 0; font-size:13px; color:var(--gray-500); }}
    .journey {{ display:grid; grid-template-columns:1fr 24px 1fr 24px 1fr; align-items:center; gap:8px; margin:18px 0 0; padding:12px 14px; background:var(--white); border:var(--border); border-radius:var(--row); }}
    .journey-step {{ display:flex; align-items:center; gap:8px; min-width:0; color:var(--gray-700); font-size:13px; font-weight:650; }}
    .journey-step b {{ display:grid; place-items:center; flex:0 0 auto; width:24px; height:24px; border-radius:7px; background:var(--oat); color:var(--slate); font-family:var(--mono); font-size:11px; }}
    .journey-arrow {{ color:var(--gray-500); text-align:center; }}
    .steps {{ display:grid; gap:14px; }}
    .step {{ display:grid; grid-template-columns:52px minmax(0,1fr); gap:18px; background:var(--white); border:var(--border); border-radius:var(--panel); padding:22px; }}
    .step > div:last-child {{ min-width:0; }}
    .step-num {{ display:grid; place-items:center; align-self:start; width:44px; height:44px; border-radius:12px; background:var(--oat); color:var(--slate); font-family:var(--mono); font-weight:700; }}
    .step h2 {{ margin:0 0 6px; font-size:24px; line-height:1.2; }}
    .step p {{ margin:0 0 14px; }}
    .mini {{ color:var(--gray-500); font-size:14px; }}
    .admin-path {{ display:inline-flex; align-items:center; gap:8px; padding:9px 12px; background:var(--gray-150); border-radius:8px; font-family:var(--mono); font-size:13px; color:var(--slate); }}
    .choices {{ display:grid; grid-template-columns:1fr 1fr; gap:10px; margin:18px 0; }}
    .choice {{ position:relative; min-height:76px; padding:13px 82px 13px 14px; text-align:left; cursor:pointer; border:var(--border); border-radius:10px; background:var(--white); color:var(--gray-700); transition:border-color .18s ease,background .18s ease,transform .18s ease; }}
    .choice:hover {{ border-color:var(--clay); transform:translateY(-1px); }}
    .choice[aria-pressed="true"] {{ border-color:var(--clay); background:rgba(217,119,87,.07); box-shadow:inset 0 0 0 1px var(--clay); }}
    .choice[aria-pressed="true"]::after {{ content:"Selected"; position:absolute; top:12px; right:12px; padding:3px 7px; border-radius:999px; background:var(--slate); color:var(--white); font-family:var(--mono); font-size:10px; letter-spacing:.03em; }}
    .choice strong {{ display:block; color:var(--slate); font-size:15px; }}
    .choice span {{ display:block; margin-top:2px; font-size:13px; color:var(--gray-500); }}
    .mode-panel {{ margin-top:12px; }}
    .js .mode-panel[hidden] {{ display:none; }}
    .panel-label {{ margin-bottom:8px; font-family:var(--mono); font-size:11px; letter-spacing:.07em; text-transform:uppercase; color:var(--gray-500); }}
    .command-wrap {{ position:relative; min-width:0; max-width:100%; overflow:hidden; border-radius:11px; background:var(--slate); }}
    pre {{ box-sizing:border-box; width:100%; margin:0; padding:18px; max-height:250px; overflow:auto; color:#ECEAE3; font-family:var(--mono); font-size:12px; line-height:1.62; white-space:pre; overflow-wrap:normal; }}
    .copy {{ display:block; min-height:46px; min-width:176px; margin:0 12px 12px auto; padding:0 15px; cursor:pointer; border:0; border-radius:8px; background:var(--clay); color:var(--white); font-weight:700; transition:filter .18s ease,transform .18s ease; }}
    .copy:hover {{ filter:brightness(.94); transform:translateY(-1px); }}
    .copy:disabled {{ cursor:not-allowed; opacity:.45; transform:none; }}
    .copy.secondary {{ background:var(--olive); }}
    .result-list {{ display:grid; gap:8px; margin:16px 0 0; padding:0; list-style:none; }}
    .result-list li {{ position:relative; padding-left:26px; font-size:14px; }}
    .result-list li::before {{ content:""; position:absolute; left:2px; top:7px; width:10px; height:6px; border-left:2px solid var(--olive); border-bottom:2px solid var(--olive); transform:rotate(-45deg); }}
    .details {{ margin-top:20px; background:var(--white); border:var(--border); border-radius:var(--panel); }}
    details summary {{ cursor:pointer; padding:16px 18px; color:var(--slate); font-weight:650; }}
    .details-body {{ padding:0 18px 18px; color:var(--gray-500); font-size:14px; }}
    .facts {{ display:grid; grid-template-columns:repeat(3,1fr); gap:10px; margin:0 0 14px; }}
    .fact {{ padding:11px 12px; background:var(--gray-150); border-radius:8px; }}
    .fact b {{ display:block; color:var(--slate); font-family:var(--mono); font-size:12px; overflow-wrap:anywhere; }}
    .fact span {{ font-size:11px; text-transform:uppercase; letter-spacing:.05em; }}
    .toast {{ position:fixed; left:50%; bottom:24px; transform:translate(-50%,20px); opacity:0; pointer-events:none; z-index:20; padding:10px 15px; border-radius:999px; background:var(--slate); color:var(--white); font-size:13px; box-shadow:0 12px 30px rgba(20,20,19,.2); transition:.2s ease; }}
    .toast.show {{ opacity:1; transform:translate(-50%,0); }}
    @media (max-width:640px) {{
      .page {{ width:min(100% - 20px,840px); padding-top:24px; }}
      .step {{ grid-template-columns:1fr; gap:12px; padding:18px; }}
      .step-num {{ width:38px; height:38px; }}
      .choices,.facts {{ grid-template-columns:1fr; }}
      .journey {{ grid-template-columns:1fr; gap:6px; }}
      .journey-arrow {{ display:none; }}
      .status {{ align-items:flex-start; }}
    }}
    @media (prefers-reduced-motion:reduce) {{ *,*::before,*::after {{ scroll-behavior:auto!important; transition:none!important; }} }}
  </style>
</head>
<body data-expires-at="{expires_epoch}">
  <main class="page">
    <header>
      <div class="brand"><span class="mark">BS</span> BridgeSessions private setup</div>
      <h1>Join {safe_mesh}</h1>
      <p class="lede">Install BridgeSessions, connect this PC, and check the link—all in one PowerShell window.</p>
      <div class="status">
        <div class="status-main"><span class="dot" aria-hidden="true"></span><span id="invite-state" role="status" aria-live="polite">Private invite is active</span></div>
        <div class="countdown" id="countdown" aria-hidden="true">Checking time…</div>
      </div>
      <p class="privacy">Private file. It contains a one-time invite. Do not post or forward it. If time runs out, ask the sender for a fresh page.</p>
      <nav class="journey" aria-label="Three setup steps">
        <div class="journey-step"><b>1</b><span>Open PowerShell</span></div>
        <div class="journey-arrow" aria-hidden="true">→</div>
        <div class="journey-step"><b>2</b><span>Run one command</span></div>
        <div class="journey-arrow" aria-hidden="true">→</div>
        <div class="journey-step"><b>3</b><span>Confirm the link</span></div>
      </nav>
    </header>

    <div class="steps">
      <section class="step" aria-labelledby="step-1">
        <div class="step-num" aria-hidden="true">1</div>
        <div>
          <h2 id="step-1">Open PowerShell as Administrator</h2>
          <p>Click Start, search for PowerShell, then choose <strong>Run as administrator</strong>.</p>
          <div class="admin-path">Start → PowerShell → Run as administrator</div>
        </div>
      </section>

      <section class="step" aria-labelledby="step-2">
        <div class="step-num" aria-hidden="true">2</div>
        <div>
          <h2 id="step-2">Copy one setup command</h2>
          <p>Choose the option that matches this PC. Copy the command, paste it into PowerShell, and press Enter.</p>
          <div class="choices" role="group" aria-label="Setup type">
            <button class="choice" type="button" data-mode="new" aria-pressed="true">
              <strong>New PC</strong><span>Install BridgeSessions, then join.</span>
            </button>
            <button class="choice" type="button" data-mode="rejoin" aria-pressed="false">
              <strong>Rejoin or repair</strong><span>Keep the install and reconnect it.</span>
            </button>
          </div>

          <div class="mode-panel" data-panel="new">
            <div class="panel-label">New PC command</div>
            <div class="command-wrap">
              <pre><code id="new-command">{safe_new}</code></pre>
              <button class="copy" type="button" data-copy-target="new-command" data-expiring>Copy setup command</button>
            </div>
          </div>

          <div class="mode-panel" data-panel="rejoin">
            <div class="panel-label">Rejoin or repair command</div>
            <div class="command-wrap">
              <pre><code id="rejoin-command">{safe_rejoin}</code></pre>
              <button class="copy" type="button" data-copy-target="rejoin-command" data-expiring>Copy rejoin command</button>
            </div>
          </div>
          <p class="mini" style="margin-top:12px">Windows may ask permission to make changes. Choose Yes. Setup can take one or two minutes.</p>
        </div>
      </section>

      <section class="step" aria-labelledby="step-3">
        <div class="step-num" aria-hidden="true">3</div>
        <div>
          <h2 id="step-3">Confirm the connection</h2>
          <p>After setup finishes, copy and run this check in the same PowerShell window.</p>
          <div class="command-wrap">
            <pre><code id="confirm-command">{safe_confirm}</code></pre>
            <button class="copy secondary" type="button" data-copy-target="confirm-command">Copy check command</button>
          </div>
          <ul class="result-list">
            <li>BridgeSessions reports version <strong>{safe_version}</strong>.</li>
            <li>The <strong>BridgeSessions</strong> task says <strong>Running</strong>.</li>
            <li>The health check reports a live data-plane connection to <strong>host</strong>.</li>
          </ul>
        </div>
      </section>
    </div>

    <div class="details">
      <details>
        <summary>Invite details and help</summary>
        <div class="details-body">
          <div class="facts">
            <div class="fact"><span>This PC</span><b>{safe_node}</b></div>
            <div class="fact"><span>Seed</span><b>{safe_address}</b></div>
            <div class="fact"><span>Version</span><b>{safe_version}</b></div>
          </div>
          <p>If setup says the invite expired, stop and ask for a fresh page. Do not disable seed-pin checks or copy identity files by hand.</p>
          <p>Created <time datetime="{safe_issued}">{safe_issued}</time>. Invite closes by <time datetime="{safe_expires}">{safe_expires}</time>.</p>
        </div>
      </details>
    </div>
  </main>
  <div class="toast" id="toast" role="status" aria-live="polite">Copied</div>

  <script>
    (function () {{
      document.documentElement.classList.add('js');
      var body = document.body;
      var expiresAt = Number(body.getAttribute('data-expires-at')) * 1000;
      var countdown = document.getElementById('countdown');
      var inviteState = document.getElementById('invite-state');
      var toast = document.getElementById('toast');
      var toastTimer;

      function setMode(mode) {{
        document.querySelectorAll('[data-mode]').forEach(function (button) {{
          button.setAttribute('aria-pressed', button.getAttribute('data-mode') === mode ? 'true' : 'false');
        }});
        document.querySelectorAll('[data-panel]').forEach(function (panel) {{
          panel.hidden = panel.getAttribute('data-panel') !== mode;
        }});
      }}

      document.querySelectorAll('[data-mode]').forEach(function (button) {{
        button.addEventListener('click', function () {{ setMode(button.getAttribute('data-mode')); }});
      }});
      setMode('new');

      function showToast(message) {{
        toast.textContent = message;
        toast.classList.add('show');
        clearTimeout(toastTimer);
        toastTimer = setTimeout(function () {{ toast.classList.remove('show'); }}, 1800);
      }}

      function fallbackCopy(text) {{
        var field = document.createElement('textarea');
        field.value = text;
        field.setAttribute('readonly', '');
        field.style.position = 'fixed';
        field.style.opacity = '0';
        document.body.appendChild(field);
        field.select();
        var copied = document.execCommand('copy');
        field.remove();
        return copied;
      }}

      document.querySelectorAll('[data-copy-target]').forEach(function (button) {{
        button.addEventListener('click', function () {{
          var source = document.getElementById(button.getAttribute('data-copy-target'));
          var text = source ? source.textContent : '';
          if (!text) return;
          if (navigator.clipboard && window.isSecureContext) {{
            navigator.clipboard.writeText(text).then(function () {{ showToast('Copied to clipboard'); }}, function () {{
              showToast(fallbackCopy(text) ? 'Copied to clipboard' : 'Select the command and copy it');
            }});
          }} else {{
            showToast(fallbackCopy(text) ? 'Copied to clipboard' : 'Select the command and copy it');
          }}
        }});
      }});

      function updateCountdown() {{
        var remaining = Math.max(0, Math.ceil((expiresAt - Date.now()) / 1000));
        if (remaining <= 0) {{
          body.classList.add('expired');
          countdown.textContent = 'Invite expired';
          inviteState.textContent = 'Ask the sender for a fresh invite page';
          document.querySelectorAll('[data-expiring]').forEach(function (button) {{ button.disabled = true; }});
          return;
        }}
        var minutes = Math.floor(remaining / 60);
        var seconds = String(remaining % 60).padStart(2, '0');
        countdown.textContent = 'Closes in ' + minutes + ':' + seconds;
      }}
      updateCountdown();
      setInterval(updateCountdown, 1000);
    }})();
  </script>
</body>
</html>
"""


def run_bridgesessions(executable: str) -> tuple[InviteInfo, str]:
    """Mint an invite and read the installed version without leaking the token."""
    try:
        version_result = subprocess.run(
            [executable, "--version"],
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"could not run {executable!r}") from exc
    if version_result.returncode != 0:
        raise RuntimeError("bridgesessions --version failed")
    version = validate_version(version_result.stdout.strip())

    try:
        invite_result = subprocess.run(
            [executable, "invite"],
            text=True,
            capture_output=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("could not create a BridgeSessions invite") from exc
    if invite_result.returncode != 0:
        raise RuntimeError(
            "bridgesessions invite failed; confirm the daemon is healthy and try again"
        )
    invite = parse_invite_output(invite_result.stdout + "\n" + invite_result.stderr)
    return invite, version


def write_private_page(path: Path, content: str, *, force: bool) -> None:
    """Atomically write a token-bearing page with owner-only permissions."""
    path = path.expanduser().resolve()
    if path.exists() and not force:
        raise FileExistsError(f"{path} already exists; pass --force to replace it")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        path.chmod(0o600)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def open_page(path: Path) -> None:
    if sys.platform == "darwin":
        command = ["open", str(path)]
    elif os.name == "nt":
        command = ["cmd", "/c", "start", "", str(path)]
    else:
        command = ["xdg-open", str(path)]
    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError("page was generated, but the browser could not be opened")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Mint a BridgeSessions invite and generate a private 1-2-3 Windows join page."
    )
    parser.add_argument("--node", required=True, help="Name to assign the joining PC")
    parser.add_argument(
        "--mesh-label", default="your BridgeSessions mesh", help="Friendly heading shown on the page"
    )
    parser.add_argument(
        "--output", type=Path, help="Output HTML path (default: bridge-invite-<node>.html)"
    )
    parser.add_argument(
        "--bridgesessions", default="bridgesessions", help=argparse.SUPPRESS
    )
    parser.add_argument("--force", action="store_true", help="Replace an existing output file")
    parser.add_argument("--open", action="store_true", help="Open the generated page locally")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        node_name = validate_node_name(args.node)
        output = (args.output or Path(f"bridge-invite-{node_name}.html")).expanduser().resolve()
        if output.exists() and not args.force:
            raise FileExistsError(f"{output} already exists; pass --force to replace it")
        invite, version = run_bridgesessions(args.bridgesessions)
        issued_at = datetime.now(timezone.utc)
        page = render_page(
            invite=invite,
            node_name=node_name,
            mesh_label=args.mesh_label,
            version=version,
            issued_at=issued_at,
            window_seconds=invite.window_seconds,
        )
        write_private_page(output, page, force=args.force)
        if args.open:
            open_page(output)
    except (FileExistsError, RuntimeError, ValueError) as exc:
        parser.exit(2, f"error: {exc}\n")

    expires_at = issued_at + timedelta(seconds=invite.window_seconds)
    print(f"Created PRIVATE FILE: {output}")
    print(f"Invite closes by: {expires_at.astimezone().strftime('%Y-%m-%d %H:%M:%S %Z')}")
    print("Send the HTML file privately. Generate a fresh page if it expires.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
