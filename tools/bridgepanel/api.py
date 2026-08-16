"""BridgePanel — BridgeSessions IPC queries and session tree builder."""
from __future__ import annotations

import base64 as _b64
import json as _json
import os
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
    import subprocess, tempfile

    bs_bin = _bs_binary()
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


def remote_file_recv(machine: str, remote_path: str) -> dict:
    """Fetch a file from a remote peer via `bs file recv`.

    Returns {ok, raw, html, name, error}.
    """
    import subprocess, tempfile

    bs_bin = _bs_binary()
    tmpdir = tempfile.mkdtemp(prefix="bridgepanel-recv-")
    local_name = os.path.basename(remote_path.rstrip("/")) or "remote-file"
    local_path = os.path.join(tmpdir, local_name)

    args = [bs_bin, "file", "recv", machine, remote_path,
            "--to", local_path, "--wait"]
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=30.0,
            env={**os.environ, "HOME": os.path.expanduser("~")},
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "transfer timed out (30s)"}
    except FileNotFoundError:
        return {"ok": False, "error": f"bs binary not found: {bs_bin}"}

    if result.returncode != 0:
        err = result.stderr.strip() or result.stdout.strip()
        # Extract just the error line, skip PROGRESS lines
        for line in err.split("\n"):
            if line.startswith("ERROR") or "error" in line.lower():
                err = line
                break
        return {"ok": False, "error": err or "transfer failed"}

    try:
        raw = open(local_path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        return {"ok": False, "error": f"cannot read fetched file: {e}"}

    # Render markdown
    suffix = os.path.splitext(local_name)[1].lower()
    is_md = suffix in (".md", ".markdown", "")
    # Use the local markdown renderer
    from .files import markdown_to_html
    import html as _html
    html_body = markdown_to_html(raw) if is_md else (
        f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;word-break:break-all">{_html.escape(raw)}</pre>'
    )

    # Cleanup
    try:
        os.unlink(local_path)
        os.rmdir(tmpdir)
    except OSError:
        pass

    return {
        "ok": True,
        "raw": raw,
        "html": html_body,
        "name": local_name,
        "size": len(raw.encode("utf-8")),
    }


def remote_file_send(machine: str, remote_path: str, content: str) -> dict:
    """Send a file to a remote peer via `bs file send`.

    Returns {ok, error}.
    """
    import subprocess, tempfile

    bs_bin = _bs_binary()
    tmpdir = tempfile.mkdtemp(prefix="bridgepanel-send-")
    local_name = os.path.basename(remote_path.rstrip("/")) or "upload.txt"
    local_path = os.path.join(tmpdir, local_name)

    try:
        open(local_path, "w", encoding="utf-8").write(content)
    except OSError as e:
        return {"ok": False, "error": f"cannot write temp file: {e}"}

    args = [bs_bin, "file", "send", machine, local_path,
            "--dest", remote_path, "--wait"]
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=30.0,
            env={**os.environ, "HOME": os.path.expanduser("~")},
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "transfer timed out (30s)"}
    except FileNotFoundError:
        return {"ok": False, "error": f"bs binary not found: {bs_bin}"}

    # Cleanup
    try:
        os.unlink(local_path)
        os.rmdir(tmpdir)
    except OSError:
        pass

    if result.returncode != 0:
        err = result.stderr.strip() or result.stdout.strip()
        for line in err.split("\n"):
            if line.startswith("ERROR") or "error" in line.lower():
                err = line
                break
        return {"ok": False, "error": err or "transfer failed"}

    return {"ok": True, "dest": remote_path, "machine": machine}


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


# ── 9warp provider integration ────────────────────────────────

def _sidecar_secret() -> str:
    """Read the 9warp sidecar shared secret."""
    for path in (
        Path.home() / ".hermes" / "oauth-sidecar.secret",
    ):
        try:
            tok = path.read_text().strip()
            if tok:
                return tok
        except OSError:
            continue
    return ""


_SIDECAR_URL = os.environ.get("SIDECAR_URL", "http://<sidecar-host>:9753")


def query_registry() -> dict:
    """Fetch models.dev registry from sidecar (170 providers)."""
    import urllib.request
    try:
        req = urllib.request.Request(f"{_SIDECAR_URL}/v1/registry/")
        secret = _sidecar_secret()
        if secret:
            req.add_header("Authorization", f"Bearer {secret}")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return {"ok": True, **_json.loads(resp.read())}
    except Exception as e:
        return {"ok": False, "error": str(e), "providers": []}

def query_discovered() -> dict:
    """Fetch newly discovered models from sidecar."""
    return _sidecar_fetch("/v1/discovered/")

def query_events(limit: int = 100) -> dict:
    """Fetch recent events from sidecar."""
    import urllib.request
    try:
        url = f"{_SIDECAR_URL}/v1/events/?limit={limit}"
        req = urllib.request.Request(url)
        secret = _sidecar_secret()
        if secret:
            req.add_header("Authorization", f"Bearer {secret}")
        with urllib.request.urlopen(req, timeout=5) as resp:
            return {"ok": True, **_json.loads(resp.read())}
    except Exception as e:
        return {"ok": False, "error": str(e), "events": []}

def _sidecar_fetch(path: str) -> dict:
    """Generic sidecar fetch helper."""
    import urllib.request
    try:
        req = urllib.request.Request(f"{_SIDECAR_URL}{path}")
        secret = _sidecar_secret()
        if secret:
            req.add_header("Authorization", f"Bearer {secret}")
        with urllib.request.urlopen(req, timeout=4) as resp:
            return {"ok": True, **_json.loads(resp.read())}
    except Exception as e:
        return {"ok": False, "error": str(e)}


def query_fleet() -> dict:
    """Aggregate fleet: spokes, harnesses, events, config per host."""
    import urllib.request
    import concurrent.futures
    secret = _sidecar_secret()
    headers = {}
    if secret:
        headers["Authorization"] = f"Bearer {secret}"

    def _fetch(path: str) -> dict:
        try:
            req = urllib.request.Request(f"{_SIDECAR_URL}{path}", headers=headers)
            with urllib.request.urlopen(req, timeout=3) as resp:
                return _json.loads(resp.read())
        except Exception:
            return {}

    # Fetch hub data in parallel
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
        f_spokes = ex.submit(_fetch, "/v1/spokes/")
        f_harnesses = ex.submit(_fetch, "/v1/harnesses/")
        f_events = ex.submit(_fetch, "/v1/events/")
        spokes_data = f_spokes.result(timeout=5)
        harnesses_data = f_harnesses.result(timeout=5)
        events_data = f_events.result(timeout=5)

    # Probe per-spoke config — also parallel
    spokes = spokes_data.get("spokes", [])
    spoke_configs = {}
    def _probe_spoke(ip: str) -> tuple:
        try:
            req = urllib.request.Request(f"http://{ip}:9753/config")
            with urllib.request.urlopen(req, timeout=2) as resp:
                return ip, _json.loads(resp.read())
        except Exception:
            return ip, {"error": "unreachable"}

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
        futures = {ex.submit(_probe_spoke, sp.get("ip", "")): sp for sp in spokes if sp.get("ip")}
        for fut in concurrent.futures.as_completed(futures, timeout=5):
            try:
                ip, cfg = fut.result()
                spoke_configs[ip] = cfg
            except Exception:
                pass

    return {
        "ok": True,
        "spokes": spokes,
        "spoke_configs": spoke_configs,
        "harnesses": harnesses_data.get("harnesses", []),
        "events": events_data.get("events", [])[:50],
    }

def query_providers() -> dict:
    """Fetch provider status from 9warp sidecar. Returns provider map or error."""
    import urllib.request
    secret = _sidecar_secret()
    url = f"{_SIDECAR_URL}/v1/providers/"
    try:
        req = urllib.request.Request(url)
        if secret:
            req.add_header("Authorization", f"Bearer {secret}")
        with urllib.request.urlopen(req, timeout=5) as resp:
            data = _json.loads(resp.read())
        # sidecar returns {providers: {...}}
        if isinstance(data, dict) and "providers" in data:
            return {"providers": data["providers"], "ok": True}
        return {"providers": data, "ok": True} if isinstance(data, dict) else {"providers": {}, "ok": False, "error": "unexpected response"}
    except Exception as e:
        return {"providers": {}, "ok": False, "error": str(e)}
