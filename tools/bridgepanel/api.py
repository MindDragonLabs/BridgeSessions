"""BridgePanel — BridgeSessions IPC queries and session tree builder."""
from __future__ import annotations

import base64 as _b64
import json as _json
import os
import re
import socket
import time
from pathlib import Path
from urllib.parse import unquote

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
    from .cache import tree_get, tree_put

    hit = tree_get()
    if hit is not None:
        return hit
    tree = _query_mesh_tree_live()
    tree_put(tree)
    return tree


def _query_mesh_tree_live() -> dict:
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
            self_entry = fleet_obj.get(tree.get("node"), {})
            if isinstance(self_entry, dict):
                tree["os"] = self_entry.get("os", "")
                tree["version"] = self_entry.get("version", "")
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
                        "os": entry.get("os", ""),
                        "sessions": [],
                    })
                cua = bool(entry.get("cua", False))
                if status == "self":
                    tree["cua"] = cua
                    tree["os"] = entry.get("os", tree.get("os", ""))
                for p in tree["peers"]:
                    if p.get("name") == name:
                        p["cua"] = cua
                        p["os"] = entry.get("os", p.get("os", ""))
                        p["version"] = entry.get("version", p.get("version", ""))
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

    from .cache import listing_invalidate
    listing_invalidate(machine, "inbox")
    return {
        "ok": True,
        "dest": remote_path,
        "machine": machine,
        "size": len(data),
        "progress": progress,
    }


def is_self_node(machine: str, tree: dict | None = None) -> bool:
    name = (machine or "").strip()
    if not name or name in ("(local)", "local"):
        return True
    mesh = tree if tree is not None else query_mesh_tree()
    node = (mesh.get("node") or "").strip()
    return bool(node) and name == node


def _extract_json_object(text: str) -> dict | None:
    if not text:
        return None
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        obj = _json.loads(text[start:end + 1])
    except _json.JSONDecodeError:
        return None
    return obj if isinstance(obj, dict) else None


_LIST_PY = '''\
import json, os
rel = __REL__
root = os.path.expanduser("~/.bridgesessions/received")
base = os.path.realpath(root)
path = os.path.realpath(os.path.join(base, rel)) if rel else base
if not (path == base or path.startswith(base + os.sep)):
    print(json.dumps({"ok": False, "error": "path escape"})); raise SystemExit(0)
if not os.path.isdir(path):
    print(json.dumps({"ok": True, "path": rel, "count": 0, "items": []} if not os.path.isdir(base)
                     else {"ok": False, "error": "not a directory", "path": rel, "items": []}))
    raise SystemExit(0)
MD = {".md", ".markdown", ".txt"}
IMG = {".png", ".jpg", ".jpeg", ".gif", ".webp"}
VID = {".mp4", ".webm", ".mov", ".mkv"}
items = []
for name in os.listdir(path):
    if name.startswith("."):
        continue
    fp = os.path.join(path, name)
    try:
        if os.path.islink(fp):
            continue
        is_dir = os.path.isdir(fp)
        st = os.lstat(fp)
    except OSError:
        continue
    ext = os.path.splitext(name)[1].lower()
    kind = "dir" if is_dir else ("md" if ext in MD else "image" if ext in IMG else "video" if ext in VID else "pdf" if ext==".pdf" else "code" if ext in {".py",".pyw",".ps1",".psm1",".js",".ts",".json",".html",".htm",".css",".xml",".yaml",".yml",".toml",".ini",".sh",".c",".h",".cpp",".go",".rs",".rb",".php",".sql",".java",".bat"} or name.lower() in {"dockerfile","makefile","gnumakefile","cmakelists.txt"} else "file")
    items.append({"name": name, "dir": is_dir, "size": 0 if is_dir else int(st.st_size),
                  "mtime": int(st.st_mtime), "kind": kind})
items.sort(key=lambda x: (not x["dir"], x["name"].lower()))
print(json.dumps({"ok": True, "path": rel, "count": len(items), "items": items}))
'''

_LIST_PS = r'''
$ErrorActionPreference = 'Stop'
$rel = __REL__
function Get-InboxRoot {
  $cands = New-Object System.Collections.Generic.List[string]
  $up = [Environment]::GetFolderPath('UserProfile')
  if (-not $up) { $up = $env:USERPROFILE }
  if ($up) { [void]$cands.Add((Join-Path $up '.bridgesessions\received')) }
  $users = Join-Path $env:SystemDrive 'Users'
  if (Test-Path -LiteralPath $users) {
    Get-ChildItem -LiteralPath $users -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object {
      if ($_.Name -in @('Public','Default','Default User','All Users')) { return }
      [void]$cands.Add((Join-Path $_.FullName '.bridgesessions\received'))
    }
  }
  $best = $null
  $bestN = -1
  foreach ($c in $cands) {
    if (-not (Test-Path -LiteralPath $c -PathType Container)) { continue }
    if ($c -match 'systemprofile') { continue }
    $n = @(Get-ChildItem -LiteralPath $c -Force -ErrorAction SilentlyContinue).Count
    if ($n -gt $bestN) { $best = $c; $bestN = $n }
  }
  if ($best) { return $best }
  if ($up) { return (Join-Path $up '.bridgesessions\received') }
  return $null
}
$root = Get-InboxRoot
if (-not $root -or -not (Test-Path -LiteralPath $root)) {
  Write-Output '{"ok":true,"path":"","count":0,"items":[]}'
  exit 0
}
$base = (Resolve-Path -LiteralPath $root).Path
$path = $base
if ($rel) { $path = [IO.Path]::GetFullPath((Join-Path $base ($rel -replace '/','\'))) }
$prefix = $base.TrimEnd('\') + '\'
if ($path -ne $base -and -not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
  Write-Output '{"ok":false,"error":"path escape"}'
  exit 0
}
if (-not (Test-Path -LiteralPath $path -PathType Container)) {
  Write-Output '{"ok":false,"error":"not a directory"}'
  exit 0
}
$items = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $path | ForEach-Object {
  $n = $_.Name.Replace('\','\\').Replace('"','\"')
  $dir = if ($_.PSIsContainer) { 'true' } else { 'false' }
  $sz = if ($_.PSIsContainer) { 0 } else { $_.Length }
  $mt = [int]([DateTimeOffset]$_.LastWriteTimeUtc).ToUnixTimeSeconds()
  $ext = $_.Extension.ToLower()
  $kind = 'file'
  if ($_.PSIsContainer) { $kind = 'dir' }
  elseif ($ext -match '\.(md|markdown|txt)$') { $kind = 'md' }
  elseif ($ext -match '\.(png|jpe?g|gif|webp)$') { $kind = 'image' }
  elseif ($ext -match '\.(mp4|webm|mov|mkv)$') { $kind = 'video' }
  elseif ($ext -match '\.pdf$') { $kind = 'pdf' }
  elseif ($_.Name -match '^(Dockerfile|Makefile|GNUMakefile|CMakeLists\.txt)$') { $kind = 'code' }
  elseif ($ext -match '\.(py|pyw|ps1|psm1|js|ts|json|html?|css|xml|ya?ml|toml|ini|sh|c|h|cpp|go|rs|rb|php|sql|java|bat)$') { $kind = 'code' }
  [void]$items.Add(('{{"name":"{0}","dir":{1},"size":{2},"mtime":{3},"kind":"{4}"}}' -f $n,$dir,$sz,$mt,$kind))
}
$pathJson = ($rel | ConvertTo-Json -Compress)
Write-Output ('{{"ok":true,"path":{0},"count":{1},"items":[{2}]}}' -f $pathJson, $items.Count, ($items -join ','))
'''


_VOL_CACHE: dict[str, tuple[float, dict]] = {}

_VOL_PS = r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$rows = New-Object System.Collections.Generic.List[object]
Get-CimInstance Win32_LogicalDisk | ForEach-Object {
  [void]$rows.Add([ordered]@{
    device = $_.DeviceID
    type = [int]$_.DriveType
    fs = [string]$_.FileSystem
    size = [int64]($_.Size)
    free = [int64]($_.FreeSpace)
    label = [string]$_.VolumeName
  })
}
$rows | ConvertTo-Json -Compress -Depth 4
'''

_VOL_PY = r'''
import json, os
items = []
seen = set()
try:
    lines = open("/proc/self/mounts", encoding="utf-8", errors="replace")
except OSError:
    print("[]")
    raise SystemExit(0)
for line in lines:
    parts = line.split()
    if len(parts) < 3:
        continue
    src, tgt, fs = parts[0], parts[1].replace("\\040", " "), parts[2]
    if src in seen:
        continue
    try:
        st = os.statvfs(tgt)
    except OSError:
        continue
    seen.add(src)
    items.append({"target": tgt, "fs": fs,
                  "size": int(st.f_frsize * st.f_blocks),
                  "free": int(st.f_frsize * st.f_bavail)})
print(json.dumps(items))
'''

_LIST_VOL_PY = r'''
import json, os, stat
root = __ROOT__
rel = __REL__
base = os.path.realpath(root)
path = os.path.realpath(os.path.join(base, rel.replace("/", os.sep))) if rel else base
prefix = base if base.endswith(os.sep) else base + os.sep
if path != base and not path.startswith(prefix):
    print(json.dumps({"ok": False, "error": "path_rejected"}))
    raise SystemExit(0)
if not os.path.isdir(path):
    print(json.dumps({"ok": False, "error": "not a directory", "path": rel}))
    raise SystemExit(0)
items = []
for name in os.listdir(path):
    fp = os.path.join(path, name)
    try:
        if os.path.islink(fp):
            continue
        st = os.lstat(fp)
        is_dir = stat.S_ISDIR(st.st_mode)
    except OSError:
        continue
    ext = os.path.splitext(name)[1].lower()
    kind = "dir" if is_dir else ("md" if ext in (".md",".markdown",".txt") else "image" if ext in (".png",".jpg",".jpeg",".gif",".webp") else "video" if ext in (".mp4",".webm",".mov",".mkv") else "pdf" if ext==".pdf" else "code" if ext in (".py",".pyw",".ps1",".psm1",".js",".ts",".json",".html",".htm",".css",".xml",".yaml",".yml",".toml",".ini",".sh",".c",".h",".cpp",".go",".rs",".rb",".php",".sql",".java",".bat") or name.lower() in ("dockerfile","makefile","gnumakefile","cmakelists.txt") else "file")
    items.append({"name": name, "dir": is_dir, "size": 0 if is_dir else int(st.st_size),
                  "mtime": int(st.st_mtime), "kind": kind})
items.sort(key=lambda x: (not x["dir"], x["name"].lower()))
print(json.dumps({"ok": True, "path": rel, "count": len(items), "items": items}))
'''

_LIST_VOL_PS = r'''
$ErrorActionPreference = 'Stop'
$root = __ROOT__
$rel = __REL__
if (-not (Test-Path -LiteralPath $root)) {
  Write-Output '{"ok":false,"error":"not a directory"}'
  exit 0
}
$base = (Resolve-Path -LiteralPath $root).Path
$path = $base
if ($rel) { $path = [IO.Path]::GetFullPath((Join-Path $base ($rel -replace '/','\'))) }
$prefix = $base.TrimEnd('\') + '\'
if ($path -ne $base -and -not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
  Write-Output '{"ok":false,"error":"path_rejected"}'
  exit 0
}
if (-not (Test-Path -LiteralPath $path -PathType Container)) {
  Write-Output '{"ok":false,"error":"not a directory"}'
  exit 0
}
$items = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $path -Force | ForEach-Object {
  if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { return }
  $n = $_.Name.Replace('\','\\').Replace('"','\"')
  $dir = if ($_.PSIsContainer) { 'true' } else { 'false' }
  $sz = if ($_.PSIsContainer) { 0 } else { $_.Length }
  $mt = [int]([DateTimeOffset]$_.LastWriteTimeUtc).ToUnixTimeSeconds()
  $ext = $_.Extension.ToLower()
  $kind = 'file'
  if ($_.PSIsContainer) { $kind = 'dir' }
  elseif ($ext -match '\.(md|markdown|txt)$') { $kind = 'md' }
  elseif ($ext -match '\.(png|jpe?g|gif|webp)$') { $kind = 'image' }
  elseif ($ext -match '\.(mp4|webm|mov|mkv)$') { $kind = 'video' }
  elseif ($ext -match '\.pdf$') { $kind = 'pdf' }
  elseif ($_.Name -match '^(Dockerfile|Makefile|GNUMakefile|CMakeLists\.txt)$') { $kind = 'code' }
  elseif ($ext -match '\.(py|pyw|ps1|psm1|js|ts|json|html?|css|xml|ya?ml|toml|ini|sh|c|h|cpp|go|rs|rb|php|sql|java|bat)$') { $kind = 'code' }
  [void]$items.Add(('{{"name":"{0}","dir":{1},"size":{2},"mtime":{3},"kind":"{4}"}}' -f $n,$dir,$sz,$mt,$kind))
}
$pathJson = ($rel | ConvertTo-Json -Compress)
Write-Output ('{{"ok":true,"path":{0},"count":{1},"items":[{2}]}}' -f $pathJson, $items.Count, ($items -join ','))
'''


def _peer_for(machine: str, tree: dict | None = None) -> dict | None:
    tree = tree if tree is not None else query_mesh_tree()
    for p in tree.get("peers") or []:
        if p.get("name") == machine:
            return p
    return None


def _peer_windows(peer: dict | None) -> bool:
    os_name = ((peer or {}).get("os") or "").lower()
    return os_name.startswith("win")


def list_host_volumes(machine: str) -> dict:
    """Named roots for a host. Inbox is always first."""
    from .volumes import (
        apply_acl, assemble_volumes, classify_linux_mount, classify_windows_volume,
        inbox_volume, list_local_linux_volumes, volume_browse_enabled,
    )

    tree = query_mesh_tree()
    host = machine or tree.get("node") or "(local)"
    if is_self_node(machine, tree):
        host = tree.get("node") or host
        if not volume_browse_enabled():
            vols = apply_acl(host, [inbox_volume(host)])
        else:
            vols = apply_acl(host, list_local_linux_volumes(host))
        if host not in ("(local)", "local"):
            vols = apply_acl("(local)", vols)
        return {"machine": host, "volumes": vols}

    if not machine:
        return {"machine": "", "volumes": [inbox_volume("")], "error": "machine required"}
    peer = _peer_for(machine, tree)
    if peer is None:
        return {"machine": machine, "volumes": apply_acl(machine, [inbox_volume(machine)]), "error": "unknown peer"}
    if peer.get("healthy") is False or peer.get("status") in ("offline", "stale", "no-pong"):
        return {"machine": machine, "volumes": apply_acl(machine, [inbox_volume(machine)]), "error": "host_unreachable"}
    if not volume_browse_enabled():
        return {"machine": machine, "volumes": apply_acl(machine, [inbox_volume(machine)])}

    now = time.time()
    hit = _VOL_CACHE.get(machine)
    if hit and now - hit[0] < 30:
        return hit[1]

    windows = _peer_windows(peer)
    raw = _run_remote_script(machine, _VOL_PS if windows else _VOL_PY,
                             ".ps1" if windows else ".py",
                             "powershell" if windows else "python3")
    disks: list = []
    payload = raw if isinstance(raw, list) else (raw.get("_rows") if isinstance(raw, dict) else None)
    if isinstance(raw, dict) and "device" in raw:
        payload = [raw]
    if isinstance(raw, dict) and "target" in raw:
        payload = [raw]
    if not isinstance(payload, list):
        payload = []
    for row in payload:
        if not isinstance(row, dict):
            continue
        if windows:
            classified = classify_windows_volume(
                machine,
                str(row.get("device") or ""),
                int(row.get("type") or 0),
                str(row.get("fs") or ""),
                int(row.get("size") or 0),
                int(row.get("free") or 0),
                str(row.get("label") or ""),
            )
        else:
            classified = classify_linux_mount(
                machine,
                str(row.get("target") or ""),
                str(row.get("fs") or ""),
                int(row.get("size") or 0),
                int(row.get("free") or 0),
            )
        if classified:
            disks.append(classified)
    out = {"machine": machine, "volumes": apply_acl(machine, assemble_volumes(machine, disks))}
    _VOL_CACHE[machine] = (now, out)
    return out


def _run_remote_script(machine: str, script: str, suffix: str, interp: str) -> dict | list:
    import subprocess
    import tempfile

    fd, path = tempfile.mkstemp(prefix="bridgepanel-vol-", suffix=suffix)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(script)
        args = [_bs_binary(), "run-script", machine, path, "--interpreter", interp]
        try:
            result = subprocess.run(
                args, capture_output=True, text=True, timeout=45.0,
                env={**os.environ, "HOME": os.path.expanduser("~")},
            )
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return {}
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    combined = (result.stdout or "") + "\n" + (result.stderr or "")
    text = (result.stdout or "").strip()
    try:
        parsed = _json.loads(text)
        if isinstance(parsed, (list, dict)):
            return parsed
    except ValueError:
        pass
    start = text.find("[")
    end = text.rfind("]")
    if start >= 0 and end > start:
        try:
            parsed = _json.loads(text[start:end + 1])
            if isinstance(parsed, list):
                return parsed
        except ValueError:
            pass
    obj = _extract_json_object(result.stdout or "") or _extract_json_object(combined)
    if obj is not None:
        return obj
    return {}


def list_host_files(machine: str, rel: str = "", root: str = "inbox",
                    refresh: bool = False) -> dict:
    """List inbox (default) or an allowlisted volume root. Cached 20s/120s stale."""
    from .cache import (
        cache_key, kick_listing_refresh, listing_get, listing_put,
    )

    root = root or "inbox"
    key = cache_key(machine, root, rel or "")
    cached, state = listing_get(key)
    if not refresh:
        if state == "fresh" and cached is not None:
            out = dict(cached)
            out["cached"] = True
            out["stale"] = False
            return out
        if state == "stale" and cached is not None:
            kick_listing_refresh(key, lambda: _list_host_files_live(machine, rel, root))
            out = dict(cached)
            out["cached"] = True
            out["stale"] = True
            return out
    live = _list_host_files_live(machine, rel, root)
    if live.get("ok"):
        listing_put(key, live)
        out = dict(live)
        out["cached"] = False
        out["stale"] = False
        return out
    if cached is not None:
        kept = dict(cached)
        kept["cached"] = True
        kept["stale"] = True
        if live.get("offline"):
            kept["offline"] = True
        return kept
    return live


def _list_host_files_live(machine: str, rel: str = "", root: str = "inbox") -> dict:
    """List inbox (default) or an allowlisted volume root."""
    from .files import list_receive_dir, safe_relpath
    from .volumes import (
        decorate_inbox_listing, filter_hidden, list_dir_os, media_hint_from_items,
        normalize_rel, resolve_os_path, volume_browse_enabled,
    )

    root = root or "inbox"
    tree = query_mesh_tree()
    if root in ("", "inbox"):
        rel = safe_relpath(rel)
        if is_self_node(machine, tree):
            out = list_receive_dir(rel)
            out["machine"] = tree.get("node") or machine or "(local)"
            out["local"] = True
            return decorate_inbox_listing(out, rel)
        if not machine:
            return decorate_inbox_listing(
                {"ok": False, "error": "machine required", "path": rel, "items": []}, rel)
        peer = _peer_for(machine, tree)
        if peer is None:
            return decorate_inbox_listing(
                {"ok": False, "error": f"unknown peer {machine}", "path": rel, "items": []}, rel)
        if peer.get("healthy") is False or peer.get("status") in ("offline", "stale", "no-pong"):
            return decorate_inbox_listing(
                {"ok": False, "error": "host offline", "path": rel, "items": [], "offline": True}, rel)
        windows = _peer_windows(peer)
        attempts = [True] if windows else [False, True]
        last = {"ok": False, "error": "list failed", "path": rel, "items": []}
        for use_win in attempts:
            last = _run_remote_list(machine, rel, use_win)
            if last.get("ok"):
                return decorate_inbox_listing(last, rel)
            err = (last.get("error") or "")
            if use_win or ("python3" not in err and "mkdir" not in err and "could not parse" not in err):
                break
        return decorate_inbox_listing(last, rel)

    cleaned = normalize_rel(root, rel)
    if cleaned is None:
        return {"ok": False, "error": "path_rejected", "root": root, "path": rel, "items": []}
    if not volume_browse_enabled():
        return {"ok": False, "error": "root_not_allowed", "root": root, "path": cleaned, "items": []}

    vols = list_host_volumes(machine).get("volumes") or []
    match = next((v for v in vols if v.get("token") == root), None)
    if match is None or match.get("kind") == "inbox" or not match.get("os_path"):
        return {"ok": False, "error": "root_not_allowed", "root": root, "path": cleaned, "items": []}

    windows = False
    if is_self_node(machine, tree):
        windows = os.name == "nt"
        abs_path, err = resolve_os_path(match["os_path"], cleaned, windows=windows)
        if err:
            return {"ok": False, "error": err, "root": root, "path": cleaned, "items": []}
        if not os.path.isdir(abs_path) and match.get("writable"):
            try:
                os.makedirs(abs_path, exist_ok=True)
            except OSError:
                pass
        out = list_dir_os(abs_path, cleaned)
        out["root"] = root
        out["machine"] = tree.get("node") or machine
        out["local"] = True
        return out

    peer = _peer_for(machine, tree)
    if peer is None:
        return {"ok": False, "error": f"unknown peer {machine}", "root": root, "path": cleaned, "items": []}
    if peer.get("healthy") is False or peer.get("status") in ("offline", "stale", "no-pong"):
        return {"ok": False, "error": "host offline", "root": root, "path": cleaned, "items": [], "offline": True}
    windows = _peer_windows(peer)
    abs_path, err = resolve_os_path(match["os_path"], cleaned, windows=windows)
    if err:
        return {"ok": False, "error": err, "root": root, "path": cleaned, "items": []}
    script = _LIST_VOL_PS if windows else _LIST_VOL_PY
    script = script.replace("__ROOT__", _json.dumps(match["os_path"]))
    script = script.replace("__REL__", _json.dumps(cleaned))
    raw = _run_remote_script(
        machine, script,
        ".ps1" if windows else ".py",
        "powershell" if windows else "python3",
    )
    if not isinstance(raw, dict) or not raw.get("ok"):
        return {
            "ok": False,
            "error": (raw.get("error") if isinstance(raw, dict) else "list failed") or "list failed",
            "root": root, "path": cleaned, "items": [],
        }
    items = []
    for item in raw.get("items") or []:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "")
        if not name or "/" in name or "\\" in name or name in (".", ".."):
            continue
        is_dir = bool(item.get("dir"))
        from .files import file_kind
        items.append({
            "name": name[:180],
            "dir": is_dir,
            "size": int(item.get("size") or 0),
            "mtime": int(item.get("mtime") or 0),
            "kind": item.get("kind") or file_kind(name, is_dir),
        })
    kept, hidden = filter_hidden(items)
    kept.sort(key=lambda x: (not x["dir"], x["name"].lower()))
    cap = 5000
    return {
        "ok": True,
        "root": root,
        "path": cleaned,
        "machine": machine,
        "local": False,
        "items": kept[:cap],
        "count": min(len(kept), cap),
        "hidden_by_policy": hidden,
        "truncated": len(kept) > cap,
        "media_hint": media_hint_from_items(kept),
    }


def _run_remote_list(machine: str, rel: str, windows: bool) -> dict:
    """Run the POSIX or PowerShell inbox listing script on a peer."""
    import subprocess
    import tempfile

    rel_lit = _json.dumps(rel)
    if windows:
        script = _LIST_PS.replace("__REL__", rel_lit)
        suffix, interp = ".ps1", "powershell"
    else:
        script = _LIST_PY.replace("__REL__", rel_lit)
        suffix, interp = ".py", "python3"

    fd, path = tempfile.mkstemp(prefix="bridgepanel-ls-", suffix=suffix)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(script)
        bs_bin = _bs_binary()
        args = [bs_bin, "run-script", machine, path, "--interpreter", interp]
        try:
            result = subprocess.run(
                args, capture_output=True, text=True, timeout=45.0,
                env={**os.environ, "HOME": os.path.expanduser("~")},
            )
        except subprocess.TimeoutExpired:
            return {"ok": False, "error": "list timed out", "path": rel, "items": []}
        except FileNotFoundError:
            return {"ok": False, "error": f"bs binary not found: {bs_bin}", "path": rel, "items": []}
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

    combined = (result.stdout or "") + "\n" + (result.stderr or "")
    obj = _extract_json_object(result.stdout or "") or _extract_json_object(combined)
    if obj is None:
        err = (result.stderr or result.stdout or "no list output").strip()
        if result.returncode != 0:
            return {"ok": False, "error": err[:300], "path": rel, "items": []}
        return {"ok": False, "error": "could not parse listing", "path": rel, "items": []}
    obj.setdefault("path", rel)
    obj.setdefault("items", [])
    obj["machine"] = machine
    obj["local"] = False
    if not obj.get("ok", True):
        return obj
    cleaned = []
    for item in obj.get("items") or []:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "")
        if not name or "/" in name or "\\" in name or name in (".", ".."):
            continue
        is_dir = bool(item.get("dir"))
        from .files import file_kind
        cleaned.append({
            "name": name[:180],
            "dir": is_dir,
            "size": int(item.get("size") or 0),
            "mtime": int(item.get("mtime") or 0),
            "kind": item.get("kind") or file_kind(name, is_dir),
        })
    cleaned.sort(key=lambda x: (not x["dir"], x["name"].lower()))
    obj["items"] = cleaned
    obj["count"] = len(cleaned)
    obj["ok"] = True
    return obj


def read_local_inbox_file(rel: str) -> dict:
    """Read one local inbox file for preview/download."""
    from .files import file_kind, markdown_to_html, resolve_receive_path
    import html as _html
    import mimetypes

    target = resolve_receive_path(rel, must_exist=True)
    if target is None or not target.is_file():
        return {"ok": False, "error": "file not found"}
    try:
        size = target.stat().st_size
        if size > max_file_upload():
            return {"ok": False, "error": f"file too large ({size} bytes)"}
        data = target.read_bytes()
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    kind = file_kind(target.name, False)
    guessed, _ = mimetypes.guess_type(target.name)
    content_type, is_text = guess_file_type(target.name, data)
    out = {
        "ok": True,
        "name": target.name,
        "size": len(data),
        "kind": kind,
        "content_type": content_type or guessed or "application/octet-stream",
        "is_text": is_text,
        "data": data,
        "local": True,
    }
    if is_text:
        raw = data.decode("utf-8", errors="replace")
        out["raw"] = raw
        out["html"] = markdown_to_html(raw) if kind == "md" else (
            f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;'
            f'word-break:break-all">{_html.escape(raw)}</pre>'
        )
    return out


def read_volume_file(machine: str, root: str, rel: str) -> dict:
    """Read a file from an allowlisted non-inbox volume (read-only)."""
    from .files import file_kind, markdown_to_html
    from .volumes import is_hidden_name, normalize_rel, resolve_os_path, volume_browse_enabled
    import html as _html
    import mimetypes

    root = root or "inbox"
    if root in ("", "inbox"):
        return read_local_inbox_file(rel)
    cleaned = normalize_rel(root, rel)
    if cleaned is None or not cleaned:
        return {"ok": False, "error": "path_rejected"}
    if is_hidden_name(cleaned.split("/")[-1]):
        return {"ok": False, "error": "path_rejected"}
    if not volume_browse_enabled():
        return {"ok": False, "error": "root_not_allowed"}
    vols = list_host_volumes(machine).get("volumes") or []
    match = next((v for v in vols if v.get("token") == root), None)
    if match is None or not match.get("os_path"):
        return {"ok": False, "error": "root_not_allowed"}
    tree = query_mesh_tree()
    windows = os.name == "nt" if is_self_node(machine, tree) else _peer_windows(_peer_for(machine, tree))
    abs_path, err = resolve_os_path(match["os_path"], cleaned, windows=windows)
    if err:
        return {"ok": False, "error": err}
    if is_self_node(machine, tree):
        target = Path(abs_path)
        if not target.is_file():
            return {"ok": False, "error": "file not found"}
        try:
            size = target.stat().st_size
            if size > max_file_upload():
                return {"ok": False, "error": f"file too large ({size} bytes)"}
            data = target.read_bytes()
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        kind = file_kind(target.name, False)
        guessed, _ = mimetypes.guess_type(target.name)
        content_type, is_text = guess_file_type(target.name, data)
        out = {
            "ok": True, "name": target.name, "size": len(data), "kind": kind,
            "content_type": content_type or guessed or "application/octet-stream",
            "is_text": is_text, "data": data, "local": True,
        }
        if is_text:
            raw = data.decode("utf-8", errors="replace")
            out["raw"] = raw
            out["html"] = markdown_to_html(raw) if kind == "md" else (
                f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;'
                f'word-break:break-all">{_html.escape(raw)}</pre>'
            )
        return out
    return _read_remote_volume_file(machine, abs_path, cleaned.split("/")[-1], windows)


def _read_remote_volume_file(machine: str, abs_path: str, name: str, windows: bool) -> dict:
    """Bounded base64 read of a remote volume file (stdout-safe)."""
    cap = min(1_500_000, max_file_upload())
    if windows:
        script = (
            "$p = %s; $cap = %d\n"
            "if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Write-Output '{\"ok\":false,\"error\":\"file not found\"}'; exit 0 }\n"
            "$len = (Get-Item -LiteralPath $p).Length\n"
            "if ($len -gt $cap) { Write-Output ('{\"ok\":false,\"error\":\"file too large\"}'); exit 0 }\n"
            "$b = [Convert]::ToBase64String([IO.File]::ReadAllBytes($p))\n"
            "Write-Output (('{\"ok\":true,\"size\":' + $len + ',\"b64\":\"' + $b + '\"}'))\n"
        ) % (_json.dumps(abs_path), cap)
        raw = _run_remote_script(machine, script, ".ps1", "powershell")
    else:
        script = (
            "import json,os,base64\n"
            f"p={abs_path!r}; cap={cap}\n"
            "if not os.path.isfile(p):\n"
            "    print(json.dumps({'ok':False,'error':'file not found'})); raise SystemExit(0)\n"
            "n=os.path.getsize(p)\n"
            "if n>cap:\n"
            "    print(json.dumps({'ok':False,'error':'file too large'})); raise SystemExit(0)\n"
            "print(json.dumps({'ok':True,'size':n,'b64':base64.b64encode(open(p,'rb').read()).decode('ascii')}))\n"
        )
        raw = _run_remote_script(machine, script, ".py", "python3")
    if not isinstance(raw, dict) or not raw.get("ok"):
        return {"ok": False, "error": (raw.get("error") if isinstance(raw, dict) else "read failed") or "read failed"}
    try:
        data = _b64.b64decode(raw.get("b64") or "")
    except Exception:
        return {"ok": False, "error": "read failed"}
    from .files import file_kind, markdown_to_html
    import html as _html
    kind = file_kind(name, False)
    content_type, is_text = guess_file_type(name, data)
    out = {
        "ok": True, "name": name, "size": len(data), "kind": kind,
        "content_type": content_type, "is_text": is_text, "data": data, "local": False,
    }
    if is_text:
        raw_txt = data.decode("utf-8", errors="replace")
        out["raw"] = raw_txt
        out["html"] = markdown_to_html(raw_txt) if kind == "md" else (
            f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;'
            f'word-break:break-all">{_html.escape(raw_txt)}</pre>'
        )
    return out


def write_local_inbox_file(rel: str, data: bytes) -> dict:
    from .files import resolve_receive_path, safe_relpath

    rel = safe_relpath(rel)
    if not rel:
        return {"ok": False, "error": "path required"}
    target = resolve_receive_path(rel)
    if target is None:
        return {"ok": False, "error": "path escape"}
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    from .cache import listing_invalidate_self_aliases
    node = ""
    try:
        node = (query_mesh_tree().get("node") or "")
    except Exception:
        node = ""
    listing_invalidate_self_aliases(node, "inbox")
    return {"ok": True, "dest": rel, "machine": "(local)", "size": len(data), "local": True}


def write_volume_file(machine: str, root: str, rel: str, data: bytes) -> dict:
    """Write to inbox or an ACL-writable volume. Path still prefix-checked."""
    from .volumes import (
        is_hidden_name, normalize_rel, resolve_os_path, root_writable,
        volume_browse_enabled,
    )

    root = root or "inbox"
    if root in ("", "inbox"):
        if is_self_node(machine):
            return write_local_inbox_file(rel, data)
        return remote_file_send(machine, rel, data)
    if not root_writable(root, machine) and not root_writable(root, "(local)"):
        return {"ok": False, "error": "write_not_allowed"}
    if not volume_browse_enabled():
        return {"ok": False, "error": "root_not_allowed"}
    cleaned = normalize_rel(root, rel)
    if cleaned is None or not cleaned:
        return {"ok": False, "error": "path_rejected"}
    if is_hidden_name(cleaned.split("/")[-1]):
        return {"ok": False, "error": "path_rejected"}
    vols = list_host_volumes(machine).get("volumes") or []
    match = next((v for v in vols if v.get("token") == root), None)
    if match is None or not match.get("os_path") or not match.get("writable"):
        return {"ok": False, "error": "write_not_allowed"}
    tree = query_mesh_tree()
    windows = os.name == "nt" if is_self_node(machine, tree) else _peer_windows(_peer_for(machine, tree))
    abs_path, err = resolve_os_path(match["os_path"], cleaned, windows=windows)
    if err:
        return {"ok": False, "error": err}
    if is_self_node(machine, tree):
        target = Path(abs_path)
        try:
            if target.exists() and (target.is_dir() or target.is_symlink()):
                return {"ok": False, "error": "path_rejected"}
            target.parent.mkdir(parents=True, exist_ok=True)
            root_real = os.path.realpath(match["os_path"])
            real_parent = os.path.realpath(target.parent)
            if real_parent != root_real and not real_parent.startswith(root_real.rstrip(os.sep) + os.sep):
                return {"ok": False, "error": "path_rejected"}
            target.write_bytes(data)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        from .cache import listing_invalidate, listing_invalidate_self_aliases
        listing_invalidate(machine, root)
        listing_invalidate_self_aliases(tree.get("node") or "", root)
        return {"ok": True, "dest": cleaned, "machine": machine, "size": len(data),
                "root": root, "local": True}
    out = _write_remote_volume_file(machine, abs_path, data, windows, cleaned, match["os_path"])
    if out.get("ok"):
        from .cache import listing_invalidate
        listing_invalidate(machine, root)
    return out


def _write_remote_volume_file(
    machine: str, abs_path: str, data: bytes, windows: bool, rel: str, root_os: str,
) -> dict:
    """Bounded base64 write of a remote volume file. Path already prefix-checked."""
    b64 = _b64.b64encode(data).decode("ascii")
    if windows:
        script = (
            "$p = %s; $root = %s; $b64 = %s\n"
            "$prefix = $root.TrimEnd('\\') + '\\'\n"
            "if (-not ($p.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $p -eq $root)) {\n"
            "  Write-Output '{\"ok\":false,\"error\":\"path_rejected\"}'; exit 0\n"
            "}\n"
            "$dir = Split-Path -Parent $p\n"
            "if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }\n"
            "if (Test-Path -LiteralPath $p -PathType Container) { Write-Output '{\"ok\":false,\"error\":\"path_rejected\"}'; exit 0 }\n"
            "[IO.File]::WriteAllBytes($p, [Convert]::FromBase64String($b64))\n"
            "Write-Output ('{\"ok\":true,\"size\":' + ([IO.File]::ReadAllBytes($p).Length) + '}')\n"
        ) % (_json.dumps(abs_path), _json.dumps(root_os), _json.dumps(b64))
        raw = _run_remote_script(machine, script, ".ps1", "powershell")
    else:
        script = (
            "import json,os,base64\n"
            f"p={abs_path!r}; root={root_os!r}; b64={b64!r}\n"
            "root=os.path.realpath(root)\n"
            "prefix=root if root.endswith(os.sep) else root+os.sep\n"
            "parent=os.path.dirname(p)\n"
            "os.makedirs(parent, exist_ok=True)\n"
            "real=os.path.realpath(parent)\n"
            "if real!=root and not real.startswith(prefix):\n"
            "    print(json.dumps({'ok':False,'error':'path_rejected'})); raise SystemExit(0)\n"
            "if os.path.isdir(p) or os.path.islink(p):\n"
            "    print(json.dumps({'ok':False,'error':'path_rejected'})); raise SystemExit(0)\n"
            "open(p,'wb').write(base64.b64decode(b64))\n"
            "print(json.dumps({'ok':True,'size':os.path.getsize(p)}))\n"
        )
        raw = _run_remote_script(machine, script, ".py", "python3")
    if not isinstance(raw, dict) or not raw.get("ok"):
        return {"ok": False, "error": (raw.get("error") if isinstance(raw, dict) else "write failed") or "write failed"}
    return {"ok": True, "dest": rel, "machine": machine, "size": len(data), "local": False}


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


# ── Paste-path resolution ───────────────────────────────────────

def resolve_open_path(machine: str, root: str, raw: str, cwd: str = "") -> dict:
    """Resolve a pasted path (absolute or relative) to a navigable file/dir.

    Accepts:
      - an absolute OS path (e.g. `/home/u/projects/notes.md`,
        `C:\\Users\\u\\file.txt`), matched against the machine's browsable
        roots (inbox receive_dir for the local node, allowlisted volumes
        otherwise), or
      - a path relative to the current root + cwd.

    Returns {ok, root, dir, name, kind} so the client can navigate to `dir`
    and open `name` (or navigate to `dir` alone when `name` is empty — the
    pasted path was a directory). Paths are verified to exist inside the
    browsable root; traversal outside is rejected.
    """
    from .files import receive_dir as _receive_dir
    from .volumes import normalize_rel

    tree = query_mesh_tree()
    root = root or "inbox"

    # Normalize: strip quotes, unquote, unify separators.
    text = unquote(raw or "").replace("\\", "/").strip().strip("\"'")
    if not text:
        return {"ok": False, "error": "empty path"}

    is_abs = text.startswith("/") or bool(re.match(r"^[A-Za-z]:", text))

    if is_abs:
        # 1) Match against the local inbox receive_dir (self node only).
        if is_self_node(machine, tree):
            base = str(_receive_dir()).replace("\\", "/").rstrip("/")
            base_real = str(_receive_dir().resolve()).replace("\\", "/").rstrip("/")
            for cand in {base, base_real}:
                if not cand:
                    continue
                if text == cand or text.startswith(cand + "/"):
                    rel = "" if text == cand else text[len(cand) + 1:]
                    return _resolve_open_rel(machine, "inbox", rel, tree)
        # 2) Match against allowlisted volume roots (local or remote).
        vols = list_host_volumes(machine).get("volumes") or []
        best = None
        for v in vols:
            osp = (v.get("os_path") or "").replace("\\", "/").rstrip("/")
            if not osp:
                continue
            if text == osp or text.startswith(osp + "/"):
                rel = "" if text == osp else text[len(osp) + 1:]
                if best is None or len(osp) > len(best[1]):
                    best = (v.get("token") or "inbox", osp, rel)
        if best is not None:
            return _resolve_open_rel(machine, best[0], best[2], tree)
        return {"ok": False, "error": "path not under a browsable root"}
    # Relative: resolve against the current root + cwd.
    joined = "/".join([cwd, text]) if cwd else text
    cleaned = normalize_rel(root, joined)
    if cleaned is None:
        return {"ok": False, "error": "path_rejected"}
    return _resolve_open_rel(machine, root, cleaned, tree)


def _resolve_open_rel(machine: str, root: str, rel: str, tree: dict) -> dict:
    """Split a root-relative path into (dir, name) and verify existence."""
    from .files import file_kind
    from .volumes import normalize_rel

    cleaned = normalize_rel(root, rel)
    if cleaned is None:
        return {"ok": False, "error": "path_rejected"}
    if not cleaned:
        return {"ok": True, "root": root, "dir": "", "name": "", "kind": "dir"}

    parts = cleaned.split("/")
    name = parts[-1]
    parent = "/".join(parts[:-1])

    # Verify by listing the parent dir (reuses cache + remote listing).
    listing = list_host_files(machine, parent, root=root, refresh=True)
    if listing.get("ok"):
        for item in listing.get("items") or []:
            if item.get("name") == name:
                if item.get("dir"):
                    return {"ok": True, "root": root, "dir": cleaned,
                            "name": "", "kind": "dir"}
                return {"ok": True, "root": root, "dir": parent,
                        "name": name, "kind": item.get("kind") or file_kind(name)}
        # Name not in the parent listing: for self-node, stat the path
        # directly (list_receive_dir cannot distinguish a missing path from
        # an empty dir). For remote peers, only accept a real dir.
        if is_self_node(machine, tree):
            abs_path = _open_abs_self(machine, root, cleaned)
            if abs_path is not None and os.path.isdir(abs_path):
                return {"ok": True, "root": root, "dir": cleaned,
                        "name": "", "kind": "dir"}
        return {"ok": False, "error": "not found", "root": root,
                "dir": parent, "name": name}
    # Listing failed (offline / policy): fall back to lexical result so the
    # client can at least attempt navigation and surface the real error.
    return {"ok": True, "root": root, "dir": parent, "name": name,
            "kind": file_kind(name), "unverified": True}


def _open_abs_self(machine: str, root: str, rel: str) -> str | None:
    """Absolute path for a root-relative path on the self node, or None."""
    from .files import receive_dir as _receive_dir
    from .volumes import resolve_os_path

    if root in ("", "inbox"):
        base = _receive_dir().resolve()
        candidate = (base / rel).resolve()
        try:
            candidate.relative_to(base)
        except ValueError:
            return None
        return str(candidate)
    vols = list_host_volumes(machine).get("volumes") or []
    match = next((v for v in vols if v.get("token") == root), None)
    if match is None or not match.get("os_path"):
        return None
    abs_path, err = resolve_os_path(match["os_path"], rel, windows=False)
    return abs_path if not err else None
