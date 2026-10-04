"""Frozen BridgePanel HTTP API v1 adapter.

This module contains protocol validation and response shapes.  The native
worker remains the source of truth for sessions; the panel never simulates
terminal input as an agent response.
"""
from __future__ import annotations

import base64
import binascii
import json
import hashlib
import os
import re
import selectors
import signal
import subprocess
import tempfile
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

from . import api
from . import auth
from .consts import max_file_upload
from .files import safe_relpath
from .volumes import normalize_rel, root_writable

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
REQUEST_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")
MAX_JSON = 256 * 1024
MAX_OUTPUT = 64 * 1024
MAX_INPUT = 64 * 1024
_chat_lock = threading.RLock()
_chat: dict[tuple[str, str], dict] = {}
_chat_pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="bridgepanel-agent")
CHAT_CAPACITY = 10  # two running, at most eight waiting
CHAT_STORE_LIMIT = 256
CHAT_RESULT_TTL = 3600
CHAT_TIMEOUT = 120
_chat_slots = threading.BoundedSemaphore(CHAT_CAPACITY)
TERMINAL = {"completed", "failed", "cancelled"}
AGENT_NAMES = frozenset({"claude", "codex", "grok", "hermes", "kimi", "mcode"})


class V1Error(Exception):
    def __init__(self, code: str, message: str, status: int = 400):
        super().__init__(message)
        self.code, self.message, self.status = code, message, status


def error(code: str, message: str, status: int = 400) -> tuple[int, dict]:
    return status, {"ok": False, "error": {"code": code, "message": message}}


def success(payload: dict | None = None) -> tuple[int, dict]:
    return 200, {"ok": True, **(payload or {})}


def _name(value: object, field: str) -> str:
    if not isinstance(value, str) or not NAME_RE.fullmatch(value):
        raise V1Error("invalid_name", f"{field} must be a stable safe name")
    return value


def _machine(value: object) -> str:
    if value == ".":
        return "."
    return _name(value, "machine")


def _b64(value: object, field: str, cap: int) -> bytes:
    if not isinstance(value, str) or len(value) > ((cap + 2) // 3) * 4 + 4:
        raise V1Error("invalid_base64", f"{field} is not valid base64")
    try:
        raw = base64.b64decode(value.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError, binascii.Error) as exc:
        raise V1Error("invalid_base64", f"{field} is not valid base64") from exc
    if len(raw) > cap:
        raise V1Error("body_too_large", f"{field} exceeds its size limit", 413)
    return raw


def _offset(value: object, field: str = "offset") -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise V1Error("invalid_integer", f"{field} must be a non-negative integer")
    return value


def _required(body: dict, fields: tuple[str, ...], optional: tuple[str, ...] = ()) -> None:
    unknown = set(body) - set(fields) - set(optional)
    if unknown:
        raise V1Error("unknown_field", f"unknown field: {sorted(unknown)[0]}")
    for field in fields:
        if field not in body:
            raise V1Error("missing_field", f"{field} is required")


def capabilities() -> tuple[int, dict]:
    sessions = api.bs_ipc("SESSIONS", timeout=0.5)
    connected = bool(sessions and not sessions.lstrip().startswith("ERROR"))
    probe = api.bs_ipc("SESSION_SCROLLBACK . __bridgepanel_capability_probe__ 0 1", timeout=0.5) if connected else ""
    control = bool(probe and not probe.lstrip().upper().startswith("ERROR UNKNOWN"))
    return success({
        "version": 1,
        "backend": {"connected": connected},
        "routes": {
            "sessions": connected,
            "session_input": control,
            "session_scrollback": control,
            "session_kill": control,
            "files": True,
            "chat": any(row["available"] for row in agents()[1]["agents"]),
        },
    })


def peers() -> tuple[int, dict]:
    tree = api.query_mesh_tree()
    return success({"peers": tree.get("peers", []), "local": tree.get("node", "")})


def sessions(machine: str) -> tuple[int, dict]:
    if machine == "." or api.is_self_node(machine):
        rows = api.query_bs_sessions()
        return success({"machine": machine, "sessions": rows})
    tree = api.query_mesh_tree()
    peer = next((p for p in tree.get("peers", []) if p.get("name") == machine), None)
    if peer is None:
        raise V1Error("unknown_machine", "machine is not in the mesh", 404)
    return success({"machine": machine, "sessions": peer.get("sessions", [])})


def create_session(body: dict) -> tuple[int, dict]:
    _required(body, ("machine", "name", "command"), ("cols", "rows"))
    machine = _machine(body["machine"])
    name = _name(body["name"], "name")
    command = body["command"]
    if not isinstance(command, str) or not command or len(command) > 4096:
        raise V1Error("invalid_command", "command must be a non-empty string")
    cols, rows = body.get("cols", 80), body.get("rows", 24)
    if isinstance(cols, bool) or not isinstance(cols, int) or not 1 <= cols <= 500:
        raise V1Error("invalid_size", "cols must be between 1 and 500")
    if isinstance(rows, bool) or not isinstance(rows, int) or not 1 <= rows <= 300:
        raise V1Error("invalid_size", "rows must be between 1 and 300")
    result = api.daemon_create_session(machine, name, command, cols, rows)
    if not result.get("ok"):
        raise V1Error("backend_unavailable", str(result.get("error", "session creation failed")), 503)
    return success({"machine": machine, "session": name})


def session_input(body: dict) -> tuple[int, dict]:
    _required(body, ("machine", "session", "data_b64"))
    machine, session = _machine(body["machine"]), _name(body["session"], "session")
    data = _b64(body["data_b64"], "data_b64", MAX_INPUT)
    result = api.daemon_session_input_v1(machine, session, data)
    if not result.get("ok"):
        raise V1Error("backend_error", str(result.get("error", "session input failed")), 503)
    return success({"machine": machine, "session": session})


def session_scrollback(params: dict[str, list[str]]) -> tuple[int, dict]:
    machine, session = _machine(params.get("machine", [""])[0]), _name(params.get("session", [""])[0], "session")
    try:
        offset = _offset(int(params.get("offset", ["0"])[0]))
        limit = _offset(int(params.get("limit", [str(MAX_OUTPUT)])[0]), "limit")
    except ValueError as exc:
        raise V1Error("invalid_integer", "offset and limit must be integers") from exc
    if not 1 <= limit <= MAX_OUTPUT:
        raise V1Error("invalid_limit", f"limit must be between 1 and {MAX_OUTPUT}")
    result = api.daemon_session_scrollback_v1(machine, session, offset, limit)
    if not result.get("ok"):
        raise V1Error("backend_error", str(result.get("error", "scrollback failed")), 503)
    return success({"offset": result["offset"], "text_b64": result["text_b64"], "reset": bool(result.get("reset"))})


def kill_session(params: dict[str, list[str]]) -> tuple[int, dict]:
    machine, session = _machine(params.get("machine", [""])[0]), _name(params.get("session", [""])[0], "session")
    result = api.daemon_session_kill_v1(machine, session)
    if not result.get("ok"):
        raise V1Error("backend_error", str(result.get("error", "session kill failed")), 503)
    return success({"machine": machine, "session": session})


def _file_read(machine: str, root: str, path: str) -> dict:
    if root in ("", "inbox"):
        rel = safe_relpath(path)
        if not rel:
            raise V1Error("invalid_path", "path must name a file")
        if api.is_self_node(machine):
            return api.read_local_inbox_file(rel)
        return api.remote_file_recv(machine, rel)
    rel = normalize_rel(root, path)
    if not rel:
        raise V1Error("invalid_path", "path is not allowed")
    return api.read_volume_file(machine, root, rel)


def file_list(params: dict[str, list[str]]) -> tuple[int, dict]:
    machine = _machine(params.get("machine", [""])[0])
    root, path = params.get("root", ["inbox"])[0], params.get("path", [""])[0]
    if not isinstance(root, str) or not root or len(root) > 80:
        raise V1Error("invalid_root", "root is required")
    rel = normalize_rel(root, path)
    if rel is None:
        raise V1Error("invalid_path", "path is not allowed")
    result = api.list_host_files(machine, rel, root=root)
    return success({k: v for k, v in result.items() if k != "ok"} | {"items": result.get("items", [])}) if result.get("ok") else error("file_error", str(result.get("error", "file listing failed")), 503)


def file_content_get(params: dict[str, list[str]]) -> tuple[int, dict]:
    machine = _machine(params.get("machine", [""])[0])
    root, path = params.get("root", ["inbox"])[0], params.get("path", [""])[0]
    result = _file_read(machine, root, path)
    if not result.get("ok"):
        raise V1Error("file_error", str(result.get("error", "file read failed")), 404)
    data = result.get("data", b"")
    return success({"machine": machine, "root": root, "path": path, "content_b64": base64.b64encode(data).decode("ascii"), "sha256": __import__("hashlib").sha256(data).hexdigest()})


def file_content_put(body: dict) -> tuple[int, dict]:
    _required(body, ("machine", "root", "path", "content_b64"))
    machine, root, path = _machine(body["machine"]), body["root"], body["path"]
    if not isinstance(root, str) or not isinstance(path, str):
        raise V1Error("invalid_path", "root and path must be strings")
    data = _b64(body["content_b64"], "content_b64", max_file_upload())
    if root == "inbox":
        rel = safe_relpath(path)
    else:
        rel = normalize_rel(root, path)
    if not rel or not root_writable(root, machine):
        raise V1Error("write_not_allowed", "the configured root is not writable", 403)
    result = api.write_volume_file(machine, root, rel, data)
    if not result.get("ok"):
        raise V1Error("file_error", str(result.get("error", "file write failed")), 403)
    return success({"machine": machine, "root": root, "path": rel, "size": len(data), "sha256": __import__("hashlib").sha256(data).hexdigest()})


def _fixture_enabled() -> bool:
    import os
    return os.environ.get("BRIDGEPANEL_AGENT_FIXTURES", "").lower() in {"1", "true", "yes"}


def _configured_agents() -> list[str]:
    raw = os.environ.get("BRIDGEPANEL_AGENT_ADAPTERS", "")
    return list(dict.fromkeys(x for x in (v.strip() for v in raw.split(",")) if NAME_RE.fullmatch(x)))[:32]


def _adapter(name: str) -> tuple[str, str] | None:
    """Explicit executable and dedicated credential home; never discover host credentials."""
    if os.name != "posix" or name not in AGENT_NAMES:
        return None
    prefix = "BRIDGEPANEL_AGENT_" + name.upper()
    executable = os.environ.get(prefix + "_EXECUTABLE", "")
    home = os.environ.get(prefix + "_HOME", "")
    if (not os.path.isabs(executable) or not os.path.isfile(executable)
            or not os.access(executable, os.X_OK) or not os.path.isabs(home)
            or not os.path.isdir(home)):
        return None
    # Inheriting the panel's own home would expose its credentials and settings.
    from .consts import config_home
    if os.path.realpath(home) in {os.path.realpath(os.path.expanduser("~")), os.path.realpath(config_home())}:
        return None
    return executable, home


def agents() -> tuple[int, dict]:
    rows = [{"name": n, "available": _adapter(n) is not None, "mode": "headless"}
            for n in _configured_agents()]
    if _fixture_enabled():
        rows.append({"name": "fixture", "available": True, "mode": "fixture"})
    return success({"agents": rows})


def _argv(executable: str, agent: str, prompt: str) -> list[str]:
    # Match the existing cli-sub run_lane contracts; never build a shell command.
    args = {
        "claude": ["-p", prompt, "--max-turns", "20"],
        "codex": ["exec", "--skip-git-repo-check", prompt],
        "grok": ["-m", "grok-4.6", "-p", prompt],
        "hermes": ["-z", prompt],
        "kimi": ["-p", prompt],
        "mcode": ["exec", "--permission", "off", "--prompt-mode", "coding", "--timeout", "10m", prompt],
    }
    return [executable, *args[agent]]


def _stop_process(process: subprocess.Popen) -> None:
    # Kill the entire session even if the leader already exited with children alive.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def _run_adapter(key: tuple[str, str], agent: str, prompt: str, adapter: tuple[str, str]) -> dict:
    executable, home = adapter
    # Only the selected adapter home is exposed; HTTP/provider env secrets are not inherited.
    env = {"HOME": home, "PATH": os.path.dirname(executable) + ":/usr/bin:/bin",
           "LANG": "C.UTF-8", "XDG_CONFIG_HOME": os.path.join(home, ".config"),
           "XDG_CACHE_HOME": os.path.join(home, ".cache")}
    with tempfile.TemporaryDirectory(prefix="bridgepanel-chat-") as cwd:
        with _chat_lock:
            row = _chat[key]
            if row["status"] == "cancelled":
                return {"ok": False}
            process = subprocess.Popen(_argv(executable, agent, prompt), cwd=cwd, env=env,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.DEVNULL, start_new_session=True)
            row["_process"] = process
        output = bytearray()
        deadline = time.monotonic() + CHAT_TIMEOUT
        try:
            with selectors.DefaultSelector() as selector:
                os.set_blocking(process.stdout.fileno(), False)
                selector.register(process.stdout, selectors.EVENT_READ)
                while selector.get_map():
                    with _chat_lock:
                        if _chat[key]["status"] == "cancelled":
                            return {"ok": False}
                    if time.monotonic() >= deadline:
                        return {"ok": False, "error": {"code": "agent_timeout", "message": "agent timed out"}}
                    for event, _ in selector.select(min(0.05, max(0, deadline - time.monotonic()))):
                        chunk = os.read(event.fd, 8192)
                        if not chunk:
                            selector.unregister(event.fileobj)
                        else:
                            output.extend(chunk)
                            if len(output) > MAX_OUTPUT:
                                return {"ok": False, "error": {"code": "output_too_large", "message": "agent output exceeds limit"}}
                while process.poll() is None:
                    with _chat_lock:
                        if _chat[key]["status"] == "cancelled":
                            return {"ok": False}
                    if time.monotonic() >= deadline:
                        return {"ok": False, "error": {"code": "agent_timeout", "message": "agent timed out"}}
                    time.sleep(0.02)
            if process.returncode != 0:
                return {"ok": False, "error": {"code": "agent_failed", "message": "agent executable failed"}}
            return {"ok": True, "text": output.decode("utf-8", errors="replace")}
        finally:
            _stop_process(process)
            process.stdout.close()
            with _chat_lock:
                _chat[key].pop("_process", None)


def _complete_chat(key: tuple[str, str], prompt: str, agent: str, adapter: tuple[str, str] | None) -> None:
    with _chat_lock:
        row = _chat.get(key)
        if not row or row["status"] == "cancelled":
            return
        row["status"] = "running"
    try:
        if agent == "fixture":
            result = {"ok": True, "text": f"fixture response: {prompt}"}
        else:
            result = _run_adapter(key, agent, prompt, adapter)
    except (OSError, ValueError, subprocess.SubprocessError):
        result = {"ok": False, "error": {"code": "agent_unavailable", "message": "agent could not run"}}
    with _chat_lock:
        row = _chat[key]
        if row["status"] != "cancelled":
            row.update({"status": "completed" if result.get("ok") else "failed", "result": result,
                        "finished_at": time.time()})


def _prune_chat() -> None:
    now = time.time()
    for key, row in list(_chat.items()):
        if row["status"] in TERMINAL and not row.get("_process") and row.get("_future", None) is not None and row["_future"].done():
            if now - row.get("finished_at", row["created_at"]) >= CHAT_RESULT_TTL:
                del _chat[key]


def _owner(identity: str | None) -> str:
    if not isinstance(identity, str) or not identity:
        raise V1Error("unauthorized", "credential identity required", 401)
    return identity


def chat_post(body: dict, identity: str | None = None) -> tuple[int, dict]:
    owner = _owner(identity)
    _required(body, ("machine", "agent", "prompt", "request_id"))
    machine, agent, request_id = _machine(body["machine"]), _name(body["agent"], "agent"), body["request_id"]
    if machine != ".":
        raise V1Error("unsupported_machine", "headless chat supports only the local machine (.)", 400)
    if not isinstance(request_id, str) or not REQUEST_RE.fullmatch(request_id):
        raise V1Error("invalid_request_id", "request_id must be a stable safe identifier")
    if not isinstance(body["prompt"], str) or not body["prompt"] or len(body["prompt"]) > 32 * 1024:
        raise V1Error("invalid_prompt", "prompt must be 1..32768 characters")
    fingerprint = hashlib.sha256(json.dumps(body, sort_keys=True, ensure_ascii=True).encode()).hexdigest()
    key = (owner, request_id)
    with _chat_lock:
        _prune_chat()
        if key in _chat:
            row = _chat[key]
            if row["_fingerprint"] != fingerprint:
                raise V1Error("request_id_conflict", "request_id already has a different payload", 409)
            return success({"request_id": request_id, "status": row["status"]})
        adapter = _adapter(agent) if agent in _configured_agents() else None
        if not adapter and not (agent == "fixture" and _fixture_enabled()):
            raise V1Error("unknown_agent", "agent adapter is unavailable", 404)
        if len(_chat) >= CHAT_STORE_LIMIT or not _chat_slots.acquire(blocking=False):
            raise V1Error("chat_busy", "chat capacity is exhausted; retry later", 429)
        _chat[key] = {"request_id": request_id, "machine": machine, "agent": agent,
                      "status": "pending", "created_at": time.time(), "_fingerprint": fingerprint}
        try:
            future = _chat_pool.submit(_complete_chat, key, body["prompt"], agent, adapter)
        except RuntimeError:
            del _chat[key]
            _chat_slots.release()
            raise V1Error("chat_busy", "chat executor unavailable", 503)
        _chat[key]["_future"] = future
        future.add_done_callback(lambda _: _chat_slots.release())
    return 202, {"ok": True, "request_id": request_id, "status": "pending"}


def _chat_row(request_id: str, identity: str | None) -> dict:
    owner = _owner(identity)
    if not isinstance(request_id, str) or not REQUEST_RE.fullmatch(request_id):
        raise V1Error("invalid_request_id", "request_id is not valid")
    _prune_chat()
    row = _chat.get((owner, request_id))
    if row is None:
        raise V1Error("not_found", "chat request not found", 404)
    return row


def chat_get(request_id: str, identity: str | None = None) -> tuple[int, dict]:
    with _chat_lock:
        row = _chat_row(request_id, identity)
        return success({k: v for k, v in row.items() if not k.startswith("_")})


def chat_delete(request_id: str, identity: str | None = None) -> tuple[int, dict]:
    with _chat_lock:
        row = _chat_row(request_id, identity)
        if row["status"] in {"pending", "running"}:
            row.update(status="cancelled", finished_at=time.time())
            row["_future"].cancel()
            process = row.get("_process")
            if process is not None:
                _stop_process(process)
        return success({"request_id": request_id, "status": row["status"]})


def token_list() -> tuple[int, dict]:
    return success({"tokens": auth.list_api_tokens()})


def token_create(body: dict) -> tuple[int, dict]:
    _required(body, ("label", "scopes"), ("ttl",))
    ttl = body.get("ttl", auth.API_TOKEN_TTL)
    if isinstance(ttl, bool) or not isinstance(ttl, int):
        raise V1Error("invalid_ttl", "ttl must be an integer")
    try:
        row = auth.issue_api_token(body["label"], body["scopes"], ttl)
    except (ValueError, RuntimeError) as exc:
        raise V1Error("invalid_token_request", str(exc)) from exc
    return 201, {"ok": True, "token": row}


def token_revoke(token_id: str) -> tuple[int, dict]:
    if not re.fullmatch(r"[0-9a-f]{16}", token_id):
        raise V1Error("invalid_token_id", "token id is not valid")
    if not auth.revoke_api_token(token_id):
        raise V1Error("not_found", "token not found", 404)
    return success({"revoked": True, "id": token_id})


def dispatch(method: str, path: str, params: dict[str, list[str]], body: dict | None = None, *, identity: str | None = None) -> tuple[int, dict]:
    try:
        if method == "GET" and path == "/api/v1/capabilities": return capabilities()
        if method == "GET" and path == "/api/v1/peers": return peers()
        if method == "GET" and path == "/api/v1/sessions": return sessions(_machine(params.get("machine", ["."])[0]))
        if method == "POST" and path == "/api/v1/sessions": return create_session(body or {})
        if method == "POST" and path == "/api/v1/sessions/input": return session_input(body or {})
        if method == "GET" and path == "/api/v1/sessions/output": return session_scrollback(params)
        if method == "DELETE" and path == "/api/v1/sessions": return kill_session(params)
        if method == "GET" and path == "/api/v1/files": return file_list(params)
        if method == "GET" and path == "/api/v1/files/content": return file_content_get(params)
        if method == "POST" and path == "/api/v1/files/content": return file_content_put(body or {})
        if method == "GET" and path == "/api/v1/agents": return agents()
        if method == "GET" and path == "/api/v1/auth/tokens": return token_list()
        if method == "POST" and path == "/api/v1/auth/tokens": return token_create(body or {})
        if method == "DELETE" and path.startswith("/api/v1/auth/tokens/"):
            return token_revoke(path.rsplit("/", 1)[1])
        if method == "POST" and path == "/api/v1/chat": return chat_post(body or {}, identity)
        if path.startswith("/api/v1/chat/"):
            request_id = path.rsplit("/", 1)[1]
            if method == "GET": return chat_get(request_id, identity)
            if method == "DELETE": return chat_delete(request_id, identity)
        raise V1Error("not_found", "v1 route not found", 404)
    except V1Error as exc:
        return error(exc.code, exc.message, exc.status)
