"""BridgePanel — BridgeSessions IPC queries and session tree builder."""
from __future__ import annotations

import base64 as _b64
import json as _json
import os
import socket
import time
from pathlib import Path

from .consts import BS_IPC_PORT, BS_IPC_TIMEOUT, file_timeout_sec, max_file_upload
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
    """Query the daemon MESH_TREE verb, merged with FLEET for the full directory.

    MESH_TREE carries live peers + their sessions; FLEET additionally lists
    configured seeds that are currently offline. Merge so the panel can render
    durable fleet members that are temporarily away (e.g. a weekend-off Mac)
    instead of silently dropping them.
    """
    raw = bs_ipc("MESH_TREE", timeout=3.0).strip()
    if not raw or raw.startswith("ERROR"):
        return {"node": "", "uptime_s": 0, "peers": [], "sessions": [], "offline": True}
    try:
        tree = _json.loads(raw.split("\n", 1)[0])
        if not isinstance(tree, dict):
            raise ValueError("not a dict")
    except (ValueError, _json.JSONDecodeError):
        return {"node": "", "uptime_s": 0, "peers": [], "sessions": [], "offline": True}

    tree.setdefault("peers", [])
    tree.setdefault("sessions", [])

    # Merge offline/seed entries from FLEET (best-effort; daemon may be mid-restart).
    fleet = bs_ipc("FLEET", timeout=3.0).strip()
    if fleet and not fleet.startswith("ERROR"):
        try:
            fleet_obj = _json.loads(fleet.split("\n", 1)[0])
        except (ValueError, _json.JSONDecodeError):
            fleet_obj = {}
        if isinstance(fleet_obj, dict):
            live_names = {p.get("name") for p in tree["peers"]}
            for name, entry in fleet_obj.items():
                if not isinstance(entry, dict):
                    continue
                status = entry.get("status", "")
                # Offline/stale seeds get appended; live peers are already in MESH_TREE.
                if status in ("offline", "stale", "no-pong") and name not in live_names:
                    tree["peers"].append({
                        "name": name,
                        "addr": entry.get("addr", ""),
                        "healthy": False,
                        "last_pong_s": -1,
                        "status": status,
                        "source": entry.get("source", "seed"),
                        "sessions": [],
                    })
                # Enrich live peers + self with the CUA helper capability flag so
                # the panel can surface computer-use sessions in the bot tree.
                cua = bool(entry.get("cua", False))
                if status == "self":
                    tree["cua"] = cua
                for p in tree["peers"]:
                    if p.get("name") == name:
                        p["cua"] = cua
                        break

    return tree


def daemon_create_session(machine: str, name: str, command: str,
                          cols: int = 80, rows: int = 24,
                          detach: bool = True) -> dict:
    """Spawn a session on <machine> via `bs shell --detach` subprocess.

    The daemon connects to the peer via direct TLS, sends an AttachMsg
    with the session name + command, then detaches. Returns {"ok": True}
    on success or {"ok": False, "error": ...} on failure.
    """
    import subprocess

    bs_bin = os.path.expanduser("~/bridgesessions/build/bridgesessions")
    if not os.path.isfile(bs_bin):
        bs_bin = os.path.expanduser("~/bridgesessions/bridgesessions")

    args = [bs_bin, "shell", machine, "-n", name, "-x", command]
    if detach:
        args.append("--detach")

    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=30.0,
            env={**os.environ, "HOME": os.path.expanduser("~")},
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "command timed out"}
    except FileNotFoundError:
        return {"ok": False, "error": f"bs binary not found: {bs_bin}"}

    if result.returncode != 0:
        err = result.stderr.strip() or result.stdout.strip() or "exit=%d" % result.returncode
        return {"ok": False, "error": err}
    return {"ok": True, "output": result.stdout.strip()}


def daemon_connect_session(machine: str, session: str) -> str:
    """Return the CLI command to connect to a session."""
    return f"bs shell {machine} -n {session}"


def daemon_session_input(session: str, data: str) -> dict:
    """Send keystrokes to a local attached session.

    The daemon IPC surface exposes SESSIONS / MESH_TREE / SCROLLBACK but has
    no PTY-write / SESSION_INPUT verb — keystrokes travel on an attached
    TLS shell, not the control socket. Stub until a daemon change owns that
    verb. Do not invent an IPC command here.
    """
    return {
        "ok": False,
        "wired": False,
        "error": (
            "not wired: daemon IPC has no keystroke verb "
            "(no SESSION_INPUT / PTY_WRITE); attach with `bs shell` to type"
        ),
        "session": session,
        "bytes": len(data.encode("utf-8")) if isinstance(data, str) else 0,
    }


def _bs_binary() -> str:
    """Find the bs binary."""
    for cand in (
        os.path.expanduser("~/.local/bin/bridgesessions"),
        os.path.expanduser("~/bridgesessions/build/bridgesessions"),
        os.path.expanduser("~/bridgesessions/bridgesessions"),
    ):
        if os.path.isfile(cand):
            return cand
    return "bs"


def query_remote_session_info(machine: str, session: str) -> dict:
    """Get session state/bytes/command from MESH_TREE gossip for a remote peer.

    Returns {state, bytes, command} or empty dict if not found.
    """
    mesh = query_mesh_tree()
    for peer in mesh.get("peers", []):
        if peer.get("name") != machine:
            continue
        for sess in peer.get("sessions", []):
            if sess.get("name") == session:
                return {
                    "state": sess.get("state", ""),
                    "bytes": sess.get("bytes", 0),
                    "command": sess.get("command", ""),
                }
    return {}


def query_remote_scrollback(machine: str, session: str) -> dict:
    """Peek at a remote session's scrollback via `bs shell --wait`.

    This re-attaches to the named session briefly, captures available output,
    and returns. Returns {offset, text, reset, error}.
    """

    # Use --detach to create a peek that doesn't disrupt the session,
    # then --wait to capture output. Actually, simplest: run a no-op command
    # in the session to get a snapshot of scrollback.
    # But that modifies the session. Instead, use the MESH_TREE bytes field
    # to report activity, and give the user the connect command for full output.
    info = query_remote_session_info(machine, session)
    if not info:
        return {"offset": 0, "text": "", "reset": False,
                "error": f"session not found on {machine}"}

    state = info.get("state", "")
    nbytes = info.get("bytes", 0)
    cmd = info.get("command", "")

    # Build an informational snapshot
    lines = [
        f"Session: {session}",
        f"Machine: {machine}",
        f"State:   {state}",
        f"Output:  {nbytes:,} bytes",
    ]
    if cmd:
        # Truncate long commands (run-script base64 blobs)
        display_cmd = cmd if len(cmd) <= 120 else cmd[:117] + "..."
        lines.append(f"Command: {display_cmd}")

    if state in ("died", "exited"):
        lines.append("")
        lines.append("Session has ended. Output buffer retained on remote daemon.")
        lines.append(f"Connect to view full scrollback: bs shell {machine} -n {session}")
    elif state in ("live", "attached"):
        lines.append("")
        lines.append("Session is active. Connect to interact:")
        lines.append(f"  bs shell {machine} -n {session}")
    else:
        lines.append("")
        lines.append(f"Connect: bs shell {machine} -n {session}")

    return {
        "offset": nbytes,
        "text": "\n".join(lines),
        "reset": False,
        "error": "",
        "remote": True,
    }


_TEXT_EXTS = {
    ".md", ".markdown", ".txt", ".text", ".json", ".py", ".sh", ".ps1",
    ".js", ".ts", ".css", ".html", ".htm", ".xml", ".yaml", ".yml",
    ".toml", ".ini", ".cfg", ".log", ".csv", ".rst", ".c", ".h", ".cpp",
    ".hpp", ".go", ".rs", ".rb", ".php", ".env",
}
_BINARY_EXTS = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".pdf", ".zip",
    ".gz", ".bz2", ".xz", ".7z", ".mp4", ".mp3", ".wav", ".woff",
    ".woff2", ".exe", ".dll", ".bin", ".dmg", ".wasm",
}


def parse_progress_lines(text: str) -> list[dict]:
    """Parse `PROGRESS k=v …` lines from `bs file send|recv --wait`."""
    out: list[dict] = []
    if not text:
        return out
    for line in text.splitlines():
        if not line.startswith("PROGRESS "):
            continue
        rec: dict = {"raw": line}
        for tok in line.split()[1:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                rec[k] = v
        out.append(rec)
    return out


def guess_file_type(name: str, data: bytes) -> tuple[str, bool]:
    """Return (content_type, is_text) from name + a byte sniff."""
    import mimetypes

    ext = os.path.splitext(name)[1].lower()
    guessed, _ = mimetypes.guess_type(name)
    sniff = data[:8192]
    if ext in _BINARY_EXTS:
        return guessed or "application/octet-stream", False
    if ext in _TEXT_EXTS or ext == "":
        if b"\x00" in sniff:
            return "application/octet-stream", False
        return guessed or "text/plain; charset=utf-8", True
    if b"\x00" in sniff:
        return guessed or "application/octet-stream", False
    try:
        sniff.decode("utf-8")
    except UnicodeDecodeError:
        return guessed or "application/octet-stream", False
    return guessed or "application/octet-stream", True


def _transfer_error(stdout: str, stderr: str) -> str:
    err = (stderr or "").strip() or (stdout or "").strip()
    for line in err.split("\n"):
        if line.startswith("ERROR") or "error" in line.lower():
            return line
    return err or "transfer failed"


def remote_file_recv(machine: str, remote_path: str) -> dict:
    """Fetch a file from a remote peer via `bs file recv`.

    Returns bytes in `data` plus `content_type` / `is_text`. Text files also
    carry `raw` + `html` for the JSON preview path.
    """
    import subprocess
    import tempfile

    bs_bin = _bs_binary()
    tmpdir = tempfile.mkdtemp(prefix="bridgepanel-recv-")
    local_name = os.path.basename(remote_path.rstrip("/")) or "remote-file"
    local_path = os.path.join(tmpdir, local_name)
    timeout = file_timeout_sec()

    args = [bs_bin, "file", "recv", machine, remote_path,
            "--to", local_path, "--wait"]
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "HOME": os.path.expanduser("~")},
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"transfer timed out ({int(timeout)}s)"}
    except FileNotFoundError:
        return {"ok": False, "error": f"bs binary not found: {bs_bin}"}

    combined = (result.stdout or "") + "\n" + (result.stderr or "")
    progress = parse_progress_lines(combined)
    if result.returncode != 0:
        return {"ok": False, "error": _transfer_error(result.stdout, result.stderr),
                "progress": progress}

    try:
        size = os.path.getsize(local_path)
        if size > max_file_upload():
            return {"ok": False, "error": f"file too large ({size} bytes)",
                    "progress": progress}
        with open(local_path, "rb") as fh:
            data = fh.read()
    except OSError as e:
        return {"ok": False, "error": f"cannot read fetched file: {e}",
                "progress": progress}
    finally:
        try:
            os.unlink(local_path)
            os.rmdir(tmpdir)
        except OSError:
            pass

    content_type, is_text = guess_file_type(local_name, data)
    out = {
        "ok": True,
        "name": local_name,
        "size": len(data),
        "content_type": content_type,
        "is_text": is_text,
        "data": data,
        "progress": progress,
    }
    if is_text:
        raw = data.decode("utf-8", errors="replace")
        suffix = os.path.splitext(local_name)[1].lower()
        is_md = suffix in (".md", ".markdown", "")
        from .files import markdown_to_html
        import html as _html
        out["raw"] = raw
        out["html"] = markdown_to_html(raw) if is_md else (
            f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;'
            f'word-break:break-all">{_html.escape(raw)}</pre>'
        )
    return out


def remote_file_send(machine: str, remote_path: str, content: str | bytes) -> dict:
    """Send a file to a remote peer via `bs file send`.

    Returns {ok, dest, machine, progress, size} or {ok: False, error, progress}.
    """
    import subprocess
    import tempfile

    bs_bin = _bs_binary()
    tmpdir = tempfile.mkdtemp(prefix="bridgepanel-send-")
    local_name = os.path.basename(remote_path.rstrip("/")) or "upload.bin"
    local_path = os.path.join(tmpdir, local_name)
    data = content.encode("utf-8") if isinstance(content, str) else content
    timeout = file_timeout_sec()

    try:
        with open(local_path, "wb") as fh:
            fh.write(data)
    except OSError as e:
        return {"ok": False, "error": f"cannot write temp file: {e}"}

    args = [bs_bin, "file", "send", machine, local_path,
            "--dest", remote_path, "--wait"]
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=timeout,
            env={**os.environ, "HOME": os.path.expanduser("~")},
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": f"transfer timed out ({int(timeout)}s)"}
    except FileNotFoundError:
        return {"ok": False, "error": f"bs binary not found: {bs_bin}"}
    finally:
        try:
            os.unlink(local_path)
            os.rmdir(tmpdir)
        except OSError:
            pass

    combined = (result.stdout or "") + "\n" + (result.stderr or "")
    progress = parse_progress_lines(combined)
    if result.returncode != 0:
        return {"ok": False, "error": _transfer_error(result.stdout, result.stderr),
                "progress": progress}

    return {
        "ok": True,
        "dest": remote_path,
        "machine": machine,
        "size": len(data),
        "progress": progress,
    }


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
            # Skip ephemeral one-shot sessions (health probes, auto-generated)
            # that have no filesystem artifacts. Only show sessions that are
            # genuinely operator-created or have persisted documents/comms.
            if sname.startswith("health-bs-health-"):
                continue
            state = sess.get("state", "")
            if state in ("died", "recoverable") and sname not in tree:
                continue
            if sname not in tree:
                tree[sname] = {"comms": [], "documents": [], "live": False}
            tree[sname].setdefault("peer", peer_name)
            tree[sname]["live"] = tree[sname].get("live", False) or (
                state == "live"
            )

    # Sort files: comms by time (newest first), documents by name
    result = []
    for sname, data in tree.items():
        data["comms"].sort(key=lambda x: x["modified"], reverse=True)
        data["documents"].sort(key=lambda x: x["name"])
        result.append({"name": sname, **data})
    result.sort(key=lambda x: (not x["live"], x["name"]))

    return {"sessions": result}
