"""BridgePanel — BridgeSessions IPC queries and session tree builder."""
from __future__ import annotations

import base64 as _b64
import json as _json
import socket
import time
from pathlib import Path

from .consts import BS_IPC_PORT, BS_IPC_TIMEOUT
from .files import safe_session_name, sessions_dir


# ── BridgeSessions integration ─────────────────────────────────

def bs_ipc_token() -> str:
    """Read the BridgeSessions daemon IPC token (best-effort)."""
    for cand in (Path.home() / ".bridgesessions" / "ipc-token",):
        try:
            tok = cand.read_text(encoding="utf-8").strip()
            if tok:
                return tok
        except OSError:
            continue
    return ""


def bs_ipc(verb: str, timeout: float = BS_IPC_TIMEOUT) -> str:
    """Send a token-prefixed IPC verb to the local BS daemon; return raw body.

    Returns "" on any failure (daemon down, no token, timeout) — callers
    degrade to their offline/empty state.
    """
    token = bs_ipc_token()
    if not token:
        return ""
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(("127.0.0.1", BS_IPC_PORT))
        s.sendall(f"{token} {verb}\n".encode("utf-8"))
        chunks = []
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", errors="replace")
    except (OSError, socket.timeout):
        return ""
    finally:
        if s is not None:
            s.close()


def query_bs_sessions() -> list[dict]:
    """Query BridgeSessions daemon for active sessions via IPC.

    Returns a list of {name, state, command} dicts. Returns [] if BS
    is not running or unreachable — panel works without BS.
    """
    text = bs_ipc("SESSIONS").strip()
    if not text or text == "No sessions." or text.startswith("ERROR"):
        return []

    sessions = []
    # Daemon emits pipe-separated records: "live <name> k=v k=v | recent ..."
    for record in text.split(" | "):
        record = record.strip()
        if not record:
            continue
        parts = record.split(None, 1)
        kind = parts[0]
        rest = parts[1] if len(parts) > 1 else ""
        fields = rest.split(None, 1)
        name = fields[0] if fields else ""
        kv = fields[1] if len(fields) > 1 else ""
        state = "unknown"
        command = ""
        for token_ in kv.split():
            if token_.startswith("state="):
                state = token_.split("=", 1)[1]
            elif token_.startswith("command="):
                command = token_.split("=", 1)[1]
        if kind == "live" and name:
            sessions.append({"name": name, "state": state, "command": command})
    return sessions


def query_mesh_tree() -> dict:
    """Query the daemon MESH_TREE verb. Returns parsed JSON or a safe empty shape."""
    raw = bs_ipc("MESH_TREE", timeout=3.0).strip()
    if not raw or raw.startswith("ERROR"):
        return {"node": "", "uptime_s": 0, "peers": [], "sessions": [], "offline": True}
    try:
        tree = _json.loads(raw.split("\n", 1)[0])
        if not isinstance(tree, dict):
            raise ValueError("not a dict")
        tree.setdefault("peers", [])
        tree.setdefault("sessions", [])
        return tree
    except (ValueError, _json.JSONDecodeError):
        return {"node": "", "uptime_s": 0, "peers": [], "sessions": [], "offline": True}


def query_scrollback(session: str, since: int) -> dict:
    """Query SCROLLBACK <session> <since>. Returns {offset, text, reset, error}."""
    raw = bs_ipc(f"SCROLLBACK {session} {int(since)}", timeout=3.0).strip()
    if not raw or raw.startswith("ERROR"):
        return {"offset": since, "text": "", "reset": False, "error": raw or "unreachable"}
    parts = raw.split(" ", 2)
    if len(parts) < 2 or parts[0] != "OK":
        return {"offset": since, "text": "", "reset": False, "error": raw}
    try:
        new_offset = int(parts[1])
    except ValueError:
        return {"offset": since, "text": "", "reset": False, "error": "bad offset"}
    payload = parts[2] if len(parts) > 2 else ""
    reset = payload.endswith(" RESET")
    if reset:
        payload = payload[: -len(" RESET")]
    if payload == "RESET":
        payload = ""
        reset = True
    pad = (-len(payload)) % 4
    try:
        text = _b64.b64decode(payload + "=" * pad).decode("utf-8", errors="replace")
    except Exception:
        return {"offset": since, "text": "", "reset": False, "error": "bad payload"}
    return {"offset": new_offset, "text": text, "reset": reset, "error": ""}


# ── Tree builder ───────────────────────────────────────────────

def build_tree() -> dict:
    """Build the session tree from filesystem + BS live sessions."""
    base = sessions_dir()
    tree = {}

    # Filesystem sessions
    if base.exists():
        for session_path in sorted(base.iterdir()):
            if not session_path.is_dir() or session_path.name.startswith("."):
                continue
            sname = session_path.name
            tree[sname] = {"comms": [], "documents": [], "live": False}
            for dtype in ("comms", "documents"):
                dtype_dir = session_path / dtype
                if dtype_dir.is_dir():
                    for f in sorted(dtype_dir.iterdir()):
                        if f.is_file() and not f.name.startswith("."):
                            stat = f.stat()
                            tree[sname][dtype].append({
                                "name": f.name,
                                "size": stat.st_size,
                                "modified": stat.st_mtime,
                                "modified_human": time.strftime(
                                    "%b %d, %H:%M", time.localtime(stat.st_mtime)
                                ),
                            })

    # Merge BS live sessions (local daemon)
    for bs_sess in query_bs_sessions():
        sname = safe_session_name(bs_sess["name"])
        if sname not in tree:
            tree[sname] = {"comms": [], "documents": [], "live": True}
        else:
            tree[sname]["live"] = True

    # Merge remote peer sessions from MESH_TREE gossip
    mesh = query_mesh_tree()
    for peer in mesh.get("peers", []):
        peer_name = peer.get("name", "?")
        for sess in peer.get("sessions", []):
            sname = safe_session_name(sess.get("name", ""))
            if not sname:
                continue
            if sname not in tree:
                tree[sname] = {"comms": [], "documents": [], "live": False}
            tree[sname].setdefault("peer", peer_name)
            tree[sname]["live"] = tree[sname].get("live", False) or (
                sess.get("state", "") == "live"
            )

    # Sort files: comms by time (newest first), documents by name
    result = []
    for sname, data in tree.items():
        data["comms"].sort(key=lambda x: x["modified"], reverse=True)
        data["documents"].sort(key=lambda x: x["name"])
        result.append({"name": sname, **data})
    result.sort(key=lambda x: (not x["live"], x["name"]))

    return {"sessions": result}
