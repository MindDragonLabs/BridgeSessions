"""BridgePanel user/password login.

scrypt-hashed credentials in ~/.config/bridgepanel/auth.json (0600), plus
HMAC-signed session cookies so a logged-in browser is authorized for every
endpoint (including require_token ones like /api/invites).

Enable by creating credentials:
    python3 -m bridgepanel.auth set admin <password>

While auth.json is absent the panel behaves exactly as before (token only).
"""
from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import threading
import time
from pathlib import Path

COOKIE = "bp_session"
SESSION_TTL = 7 * 24 * 3600  # seconds

# Process-wide session store: sid -> expiry. Single-user panel, one process.
SESSIONS: dict[str, float] = {}
_lock = threading.Lock()

LOGIN_HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BridgePanel — Sign in</title>
<style>
  :root {
    --bg: #0d1117; --card: #161b22; --border: #30363d;
    --text: #e6edf3; --muted: #8b949e; --accent: #4f8ff7;
    --accent-hover: #3b7ddb; --danger: #f85149; --mono: ui-monospace, SFMono-Regular, Menlo, monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    min-height: 100vh; display: grid; place-items: center;
    background: var(--bg); color: var(--text);
    font: 15px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  }
  .card {
    width: min(92vw, 360px); background: var(--card);
    border: 1px solid var(--border); border-radius: 12px; padding: 32px 28px;
  }
  .logo { display: flex; align-items: center; gap: 10px; margin-bottom: 6px; }
  .logo svg { width: 28px; height: 28px; }
  h1 { font-size: 18px; font-weight: 600; }
  .sub { color: var(--muted); font-size: 13px; margin-bottom: 24px; }
  label { display: block; font-size: 12px; color: var(--muted); margin: 14px 0 6px; }
  input {
    width: 100%; padding: 9px 12px; border-radius: 8px;
    border: 1px solid var(--border); background: var(--bg); color: var(--text);
    font-size: 14px; outline: none;
  }
  input:focus { border-color: var(--accent); }
  button {
    width: 100%; margin-top: 22px; padding: 10px 0; border: none; border-radius: 8px;
    background: var(--accent); color: #fff; font-size: 14px; font-weight: 600;
    cursor: pointer;
  }
  button:hover { background: var(--accent-hover); }
  button:disabled { opacity: 0.6; cursor: default; }
  .err { display: none; margin-top: 14px; padding: 8px 10px; border-radius: 8px;
         background: rgba(248,81,73,.12); border: 1px solid rgba(248,81,73,.4);
         color: var(--danger); font-size: 13px; }
</style>
</head>
<body>
  <form class="card" id="f" autocomplete="on">
    <div class="logo">
      <svg viewBox="0 0 24 24" fill="none" stroke="#4f8ff7" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 19c-5 1.5-5-2.5-7-3m14 6v-3.87a3.37 3.37 0 0 0-.94-2.61c3.14-.35 4.94-1.6 4.94-6a4.67 4.67 0 0 0-1.29-3.23 4.33 4.33 0 0 0-.08-3.27s-1.18-.35-3.91 1.48a13.38 13.38 0 0 0-7 0C5.99 3.65 4.81 4 4.81 4a4.33 4.33 0 0 0-.08 3.27A4.67 4.67 0 0 0 3.44 10.5c0 4.36 1.8 5.61 4.94 6-.55.56-.87 1.3-.94 2.13v3.87"/></svg>
      <h1>BridgePanel</h1>
    </div>
    <div class="sub">Sign in to manage sessions, files, and invites.</div>
    <label for="user">Username</label>
    <input id="user" name="user" type="text" autocomplete="username" required autofocus>
    <label for="pass">Password</label>
    <input id="pass" name="pass" type="password" autocomplete="current-password" required>
    <button type="submit" id="go">Sign in</button>
    <div class="err" id="err">Invalid username or password.</div>
  </form>
<script>
document.getElementById("f").addEventListener("submit", async (e) => {
  e.preventDefault();
  const btn = document.getElementById("go");
  const err = document.getElementById("err");
  btn.disabled = true; err.style.display = "none";
  try {
    const r = await fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        user: document.getElementById("user").value,
        pass: document.getElementById("pass").value,
      }),
    });
    if (r.ok) { location.replace("/"); return; }
    err.style.display = "block";
  } catch (_) {
    err.textContent = "Sign-in failed — check your connection.";
    err.style.display = "block";
  }
  btn.disabled = false;
});
</script>
</body>
</html>
"""


def auth_path() -> Path:
    from .consts import config_home
    return config_home() / "auth.json"


def _kdf(password: str, salt: bytes, iterations: int = 240_000) -> str:
    # pbkdf2_hmac over scrypt: available on every CPython (macOS CLT python
    # ships an OpenSSL without scrypt). 240k rounds ≈ 100ms — fine for a
    # single-user login, expensive for guessing.
    return hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt, iterations, dklen=32
    ).hex()


def _load() -> dict | None:
    try:
        d = json.loads(auth_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(d, dict):
        return None
    if not (d.get("user") and d.get("salt") and d.get("hash") and d.get("session_secret")):
        return None
    return d


def login_enabled() -> bool:
    return _load() is not None


def set_password(user: str, password: str) -> Path:
    """Write (or rewrite) credentials. Existing login sessions are revoked."""
    if not user or not password:
        raise ValueError("user and password must be non-empty")
    old = _load()
    secret = old["session_secret"] if old else secrets.token_hex(32)
    salt = secrets.token_bytes(16)
    data = {
        "user": user,
        "salt": salt.hex(),
        "hash": _kdf(password, salt),
        "session_secret": secret,
        "scheme": "pbkdf2-sha256-240k",
    }
    p = auth_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    p.chmod(0o600)
    with _lock:
        SESSIONS.clear()
    return p


def verify_password(user: str, password: str) -> bool:
    d = _load()
    if not d:
        return False
    ok = (
        hmac.compare_digest(user.encode("utf-8"), str(d["user"]).encode("utf-8"))
        and hmac.compare_digest(_kdf(password, bytes.fromhex(d["salt"])), str(d["hash"]))
    )
    if not ok:
        time.sleep(0.2)  # small constant brake on guessing
    return ok


def _sign(sid: str, secret: str) -> str:
    return hmac.new(secret.encode("utf-8"), sid.encode("utf-8"), hashlib.sha256).hexdigest()


def _cookie_value(cookie_header: str) -> str:
    for part in cookie_header.split(";"):
        k, _, v = part.strip().partition("=")
        if k == COOKIE:
            return v
    return ""


def new_session() -> str:
    """Create a session; returns the signed cookie value."""
    d = _load()
    if not d:
        raise RuntimeError("login not configured")
    sid = secrets.token_urlsafe(32)
    with _lock:
        now = time.time()
        for s in [s for s, exp in SESSIONS.items() if exp <= now]:
            del SESSIONS[s]
        SESSIONS[sid] = now + SESSION_TTL
    return f"{sid}.{_sign(sid, d['session_secret'])}"


def session_cookie_valid(cookie_header: str) -> bool:
    d = _load()
    if not d:
        return False
    value = _cookie_value(cookie_header or "")
    sid, _, sig = value.partition(".")
    if not sid or not sig:
        return False
    if not hmac.compare_digest(sig, _sign(sid, d["session_secret"])):
        return False
    with _lock:
        exp = SESSIONS.get(sid)
        if exp is None:
            return False
        if exp <= time.time():
            del SESSIONS[sid]
            return False
    return True


def drop_session_cookie(cookie_header: str) -> None:
    sid, _, _ = _cookie_value(cookie_header or "").partition(".")
    if not sid:
        return
    with _lock:
        SESSIONS.pop(sid, None)


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(prog="bridgepanel.auth",
                                 description="Manage BridgePanel user/password login")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("set", help="set (or replace) the username and password")
    sp.add_argument("user")
    sp.add_argument("password")
    args = ap.parse_args()
    if args.cmd == "set":
        out = set_password(args.user, args.password)
        print(f"saved credentials for {args.user!r} -> {out}")
