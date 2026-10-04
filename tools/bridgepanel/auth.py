"""BridgePanel authentication primitives.

The panel has three deliberately different credentials:

* the administrator session cookie, issued after a local/verified-HTTPS
  password login;
* scoped API tokens, stored as hashes and revocable independently; and
* BridgeSessions invite tokens, which are never accepted by this HTTP API.

All persistent reads are fail-closed.  Tests should patch ``config_home`` or
use a temporary BRIDGEPANEL_CONFIG; this module never reads a credential file
from a test fixture implicitly.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import sys
import threading
import time
import os
import math
import tempfile
from contextlib import contextmanager
from pathlib import Path

COOKIE = "bp_session"
PBKDF2_SCHEME = "pbkdf2-sha256"
PBKDF2_ITERATIONS = 600_000
SESSION_TTL = 7 * 24 * 3600
API_TOKEN_TTL = 90 * 24 * 3600
DEVICE_SCOPES = frozenset({"read", "write", "sessions", "chat"})
MAX_BODY = 256 * 1024

_lock = threading.RLock()
_verify_slots = threading.BoundedSemaphore(4)
_attempts: dict[str, list[float]] = {}
SESSIONS: dict[str, tuple[float, int]] = {}
_MAX_ATTEMPT_CLIENTS = 4096
_MAX_SESSIONS = 10000


@contextmanager
def _file_transaction(path: Path):
    """Serialize read/modify/write transactions across threads and processes."""
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    lock_path = path.with_suffix(path.suffix + ".lock")
    with _lock:
        with lock_path.open("a+b") as lock_file:
            try:
                import fcntl
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            except ImportError as exc:  # pragma: no cover - fail closed outside POSIX
                raise RuntimeError("process-safe auth storage requires POSIX flock") from exc
            try:
                yield
            finally:
                try:
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
                except (NameError, ImportError):
                    pass


def _atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(value, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp_name, 0o600)
        os.replace(temp_name, path)
    finally:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def auth_path() -> Path:
    from .consts import config_home
    return config_home() / "auth.json"


def token_store_path() -> Path:
    from .consts import config_home
    return config_home() / "api-tokens.json"


def audit_log_path() -> Path:
    """Where credential lifecycle events are recorded.

    Separate from the token store: this must survive token-store rewrites and
    stay readable by an operator without decrypting anything.
    """
    from .consts import config_home
    return config_home() / "auth-audit.log"


def audit(event: str, **fields: object) -> None:
    """Append one structured auth event.

    Deliberately never records a secret: callers pass identifiers, labels and
    scopes, never the token value. A write failure must not take down the
    request being audited, so it is swallowed after being reported to stderr.
    """
    record = {"ts": round(time.time(), 3), "event": event}
    record.update(fields)
    line = json.dumps(record, sort_keys=True, default=str)
    try:
        path = audit_log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")
        os.chmod(path, 0o600)
    except OSError as exc:
        print(f"bridgepanel: auth audit write failed: {exc}", file=sys.stderr)


def read_audit(limit: int = 200) -> list[dict]:
    """Most recent audit events, oldest first. Malformed lines are skipped."""
    try:
        raw = audit_log_path().read_text(encoding="utf-8")
    except OSError:
        return []
    out: list[dict] = []
    for line in raw.splitlines()[-limit:]:
        line = line.strip()
        if not line:
            continue
        try:
            parsed = json.loads(line)
        except ValueError:
            continue
        if isinstance(parsed, dict):
            out.append(parsed)
    return out


def _kdf(password: str, salt: bytes, iterations: int = PBKDF2_ITERATIONS) -> str:
    return hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt, iterations, dklen=32
    ).hex()


def _load_auth() -> dict | None:
    try:
        value = json.loads(auth_path().read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            return None
        user = value.get("user")
        salt = value.get("salt")
        digest = value.get("hash")
        secret = value.get("session_secret")
        generation = value.get("generation")
        iterations = value.get("iterations")
        if not isinstance(user, str) or not user or len(user) > 128:
            return None
        if not isinstance(salt, str) or len(salt) != 32:
            return None
        if not isinstance(digest, str) or len(digest) != 64:
            return None
        if not isinstance(secret, str) or len(secret) < 32:
            return None
        if not isinstance(generation, int) or generation < 1:
            return None
        if iterations != PBKDF2_ITERATIONS:
            return None
        bytes.fromhex(salt)
        bytes.fromhex(digest)
        bytes.fromhex(secret)
        return value
    except (OSError, ValueError, TypeError):
        return None


def _load_tokens() -> list[dict] | None:
    try:
        value = json.loads(token_store_path().read_text(encoding="utf-8"))
        if not isinstance(value, list):
            return None
        result = []
        for row in value:
            if not isinstance(row, dict):
                return None
            if not isinstance(row.get("id"), str) or not isinstance(row.get("hash"), str):
                return None
            if len(row["hash"]) != 64:
                return None
            scopes = row.get("scopes")
            if not isinstance(scopes, list) or not scopes or not all(isinstance(s, str) and s in DEVICE_SCOPES for s in scopes):
                return None
            exp = row.get("expires_at")
            if isinstance(exp, bool) or not isinstance(exp, (int, float)) or not math.isfinite(exp):
                return None
            if not isinstance(row.get("revoked", False), bool):
                return None
            result.append(row)
        return result
    except (OSError, ValueError, TypeError):
        return None


def login_enabled() -> bool:
    return _load_auth() is not None


def set_password(user: str, password: str) -> Path:
    if not isinstance(user, str) or not user or len(user) > 128:
        raise ValueError("user must be non-empty and at most 128 characters")
    if not isinstance(password, str) or len(password) < 12:
        raise ValueError("password must contain at least 12 characters")
    path = auth_path()
    with _file_transaction(path):
        old = _load_auth()
        generation = int(old["generation"]) + 1 if old else 1
        data = {
            "user": user,
            "salt": secrets.token_bytes(16).hex(),
            "hash": "",
            "session_secret": secrets.token_bytes(32).hex(),
            "generation": generation,
            "iterations": PBKDF2_ITERATIONS,
            "scheme": f"{PBKDF2_SCHEME}-{PBKDF2_ITERATIONS}",
        }
        data["hash"] = _kdf(password, bytes.fromhex(data["salt"]))
        _atomic_json(path, data)
    with _lock:
        SESSIONS.clear()
    return path


def verify_password(user: str, password: str, client_id: str = "") -> bool:
    now = time.monotonic()
    with _lock:
        recent = [t for t in _attempts.get(client_id, []) if now - t < 60]
        if len(recent) >= 8:
            return False
        recent.append(now)
        if client_id not in _attempts and len(_attempts) >= _MAX_ATTEMPT_CLIENTS:
            for key, attempts in list(_attempts.items()):
                if not attempts or now - attempts[-1] >= 60:
                    _attempts.pop(key, None)
            if len(_attempts) >= _MAX_ATTEMPT_CLIENTS:
                return False
        _attempts[client_id] = recent
    acquired = _verify_slots.acquire(timeout=2)
    if not acquired:
        return False
    try:
        data = _load_auth()
        if not data or not isinstance(password, str):
            return False
        candidate = _kdf(password, bytes.fromhex(data["salt"]))
        return hmac.compare_digest(str(data["user"]).encode(), str(user).encode()) and hmac.compare_digest(candidate, data["hash"])
    finally:
        _verify_slots.release()


def _sign(sid: str, secret: str, generation: int) -> str:
    return hmac.new(secret.encode(), f"{generation}:{sid}".encode(), hashlib.sha256).hexdigest()


def _cookie_value(header: str) -> str:
    for part in (header or "").split(";"):
        key, _, value = part.strip().partition("=")
        if key == COOKIE:
            return value
    return ""


def new_session() -> str:
    data = _load_auth()
    if not data:
        raise RuntimeError("login not configured")
    sid = secrets.token_urlsafe(32)
    with _lock:
        now = time.time()
        for old_sid, (expires, _) in list(SESSIONS.items()):
            if expires <= now:
                SESSIONS.pop(old_sid, None)
        if len(SESSIONS) >= _MAX_SESSIONS:
            SESSIONS.pop(next(iter(SESSIONS)))
        SESSIONS[sid] = (time.time() + SESSION_TTL, data["generation"])
    return f"{sid}.{data['generation']}.{_sign(sid, data['session_secret'], data['generation'])}"


def session_cookie_valid(header: str) -> bool:
    data = _load_auth()
    value = _cookie_value(header)
    sid, generation, signature = value.split(".", 2) if value.count(".") == 2 else ("", "", "")
    if not data or not sid or generation != str(data["generation"]):
        return False
    if not hmac.compare_digest(signature, _sign(sid, data["session_secret"], data["generation"])):
        return False
    with _lock:
        item = SESSIONS.get(sid)
        if not item or item[1] != data["generation"] or item[0] <= time.time():
            SESSIONS.pop(sid, None)
            return False
    return True


def drop_session_cookie(header: str) -> None:
    value = _cookie_value(header)
    sid = value.split(".", 1)[0]
    with _lock:
        SESSIONS.pop(sid, None)


def _write_tokens(rows: list[dict]) -> None:
    path = token_store_path()
    _atomic_json(path, rows)


def issue_api_token(label: str, scopes: list[str], ttl: int = API_TOKEN_TTL) -> dict:
    if not isinstance(label, str) or not label or not isinstance(scopes, list) or not scopes:
        raise ValueError("label and scopes are required")
    if any(not isinstance(scope, str) or scope not in DEVICE_SCOPES for scope in scopes):
        raise ValueError("invalid device scope")
    if ttl <= 0 or ttl > 366 * 24 * 3600:
        raise ValueError("invalid token lifetime")
    path = token_store_path()
    with _file_transaction(path):
        rows = _load_tokens()
        if rows is None:
            if path.exists():
                raise RuntimeError("malformed API token store")
            rows = []
        token = secrets.token_urlsafe(32)
        row = {"id": secrets.token_hex(8), "label": label[:128], "hash": hashlib.sha256(token.encode()).hexdigest(), "scopes": sorted(set(scopes)), "expires_at": time.time() + ttl, "revoked": False}
        rows.append(row)
        _write_tokens(rows)
    # Never log the token itself: the id, label and scopes are what an
    # operator needs to identify and revoke the credential later.
    audit("token_issued", id=row["id"], label=row["label"],
          scopes=row["scopes"], expires_at=row["expires_at"])
    return {k: v for k, v in row.items() if k != "hash"} | {"token": token}


def revoke_api_token(token_id: str) -> bool:
    with _file_transaction(token_store_path()):
        rows = _load_tokens()
        if rows is None:
            return False
        changed = False
        for row in rows:
            if row.get("id") == token_id:
                row["revoked"] = True
                changed = True
        if changed:
            _write_tokens(rows)
        label = next((r.get("label") for r in rows if r.get("id") == token_id), "")
    if changed:
        audit("token_revoked", id=token_id, label=label)
    return changed


def list_api_tokens() -> list[dict]:
    rows = _load_tokens()
    if rows is None:
        return []
    now = time.time()
    return [{k: v for k, v in row.items() if k != "hash"} for row in rows if not row.get("revoked") and float(row.get("expires_at", 0)) > now]


def credential_scope(bearer: str, admin_token: str = "") -> set[str]:
    """Return scopes, or an empty set. Invite tokens never reach this path."""
    if not bearer:
        return set()
    if admin_token and hmac.compare_digest(bearer, admin_token):
        return {"admin", "read", "write", "sessions", "chat"}
    rows = _load_tokens()
    if rows is None:
        return set()
    digest = hashlib.sha256(bearer.encode()).hexdigest()
    for row in rows:
        if row.get("revoked") or float(row.get("expires_at", 0)) <= time.time():
            continue
        if hmac.compare_digest(digest, str(row.get("hash"))):
            return set(row.get("scopes", []))
    return set()


def credential_identity(bearer: str, admin_token: str = "") -> str | None:
    """Return a stable opaque identity only after validating a bearer credential."""
    if not bearer:
        return None
    if admin_token and hmac.compare_digest(bearer, admin_token):
        return "admin:" + hashlib.sha256(admin_token.encode()).hexdigest()
    rows = _load_tokens()
    if rows is None:
        return None
    digest = hashlib.sha256(bearer.encode()).hexdigest()
    for row in rows:
        if row.get("revoked") or float(row.get("expires_at", 0)) <= time.time():
            continue
        if hmac.compare_digest(digest, str(row.get("hash"))):
            return "token:" + str(row["id"])
    return None


def cookie_identity(header: str) -> str | None:
    """Return an opaque session identity after full cookie validation."""
    if not session_cookie_valid(header):
        return None
    return "session:" + hashlib.sha256(_cookie_value(header).encode()).hexdigest()


def is_admin_cookie(header: str) -> bool:
    return session_cookie_valid(header)


LOGIN_HTML = """<!doctype html><meta charset=utf-8><title>BridgePanel sign in</title>
<form id=f><label>User <input id=u autocomplete=username required></label>
<label>Password <input id=p type=password autocomplete=current-password required></label>
<button>Sign in</button><p id=e hidden>Sign-in failed.</p></form>
<script>document.getElementById('f').onsubmit=async function(ev){ev.preventDefault();let r=await fetch('/api/login',
{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({user:document.getElementById('u').value,pass:document.getElementById('p').value})});
if(r.ok) location.replace('/'); else document.getElementById('e').hidden=false}</script>"""


if __name__ == "__main__":
    import argparse
    import getpass

    parser = argparse.ArgumentParser(description="Configure BridgePanel administrator login")
    parser.add_argument("user")
    args = parser.parse_args()
    set_password(args.user, getpass.getpass("BridgePanel password: "))
    print(f"saved credentials for {args.user!r}")
