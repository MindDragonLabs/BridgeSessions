#!/usr/bin/env python3
"""BridgePanel: a reading and writing surface for human-agent communication.

Organized by session (from BridgeSessions or filesystem), with two document
types: comms (short messages) and documents (long-form reports). Markdown
is rendered in a clean reading pane with an edit marker for in-browser editing.

No third-party dependencies. Works over any private network (VPN, LAN, loopback).
Never binds to a public address.
"""
from __future__ import annotations

import argparse
import html
import json
import os
import re
import secrets
import socket
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

APP = "BridgePanel"


def release_version() -> str:
    """Read the repository/package VERSION without maintaining a second copy."""
    override = os.environ.get("BRIDGEPANEL_VERSION")
    if override:
        return override
    here = Path(__file__).resolve()
    for parent in (here.parent, *list(here.parents)[:3]):
        candidate = parent / "VERSION"
        try:
            version = candidate.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if version:
            return version
    return "dev"


VERSION = release_version()
BUILDTAG = VERSION
DEFAULT_BIND = os.environ.get("BRIDGEPANEL_BIND", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("BRIDGEPANEL_PORT", "9770"))
MAX_UPLOAD = 10 * 1024 * 1024  # 10 MB — markdown text is small
BS_IPC_PORT = 19980
BS_IPC_TIMEOUT = 2  # seconds — don't block the panel on BS

# ── Paths ──────────────────────────────────────────────────────

def data_home() -> Path:
    return Path(os.environ.get("BRIDGEPANEL_HOME", Path.home() / ".local/share/bridgepanel"))


def config_home() -> Path:
    return Path(os.environ.get("BRIDGEPANEL_CONFIG", Path.home() / ".config/bridgepanel"))


def sessions_dir() -> Path:
    return data_home() / "sessions"


def token_path() -> Path:
    return config_home() / "token"


def state_path() -> Path:
    return data_home() / "state.json"


# ── Filename safety ────────────────────────────────────────────

def safe_name(value: str) -> str:
    value = unquote(value or "").replace("\\", "/").split("/")[-1]
    value = re.sub(r"[^A-Za-z0-9._()\- ]+", "_", value).strip(" .")
    return value[:180] or "untitled"


def safe_session_name(value: str) -> str:
    """Session names are used as directory names — stricter."""
    value = unquote(value or "").strip()
    value = re.sub(r"[^A-Za-z0-9._\-]+", "-", value).strip("-")
    return value[:80] or "default"


def safe_type(value: str) -> str:
    if value in ("comms", "documents"):
        return value
    return "documents"


def resolve_file(session: str, dtype: str, filename: str) -> Path | None:
    """Resolve a file path within sessions/, preventing traversal."""
    base = sessions_dir()
    target = base / safe_session_name(session) / safe_type(dtype) / safe_name(filename)
    try:
        target.resolve().relative_to(base.resolve())
    except (ValueError, OSError):
        return None
    return target if target.is_file() else None


# ── Token / state ──────────────────────────────────────────────

def read_state() -> dict:
    try:
        return json.loads(state_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"current": None, "updated": 0}


def write_state(data: dict) -> None:
    data_home().mkdir(parents=True, exist_ok=True)
    tmp = state_path().with_suffix(".tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, state_path())


def ensure_dirs() -> str:
    config_home().mkdir(parents=True, exist_ok=True, mode=0o700)
    sessions_dir().mkdir(parents=True, exist_ok=True, mode=0o700)

    tp = token_path()
    if not tp.exists():
        tp.write_text(secrets.token_urlsafe(24), encoding="utf-8")
    tp.chmod(0o600)
    token = tp.read_text(encoding="utf-8").strip()
    if len(token) < 24:
        raise RuntimeError("Token too short")

    if not state_path().exists():
        write_state({"current": None, "updated": time.time()})

    _seed_sample_data()
    return token


def _seed_sample_data() -> None:
    """Create sample documents so the panel isn't empty on first launch."""
    base = sessions_dir()

    samples = {
        "hermes/comms/sample-instructions.md": (
            "# Instructions\n\n"
            "This is the **comms** channel — short messages between you and the agent.\n\n"
            "- Instructions you write here are picked up by the agent.\n"
            "- Agent responses appear here too.\n"
            "- Press `e` to edit any file in the panel.\n"
        ),
        "hermes/documents/sample-report.md": (
            "# Sample Report\n\n"
            "This is the **documents** area — long-form output that's hard to read in a terminal.\n\n"
            "When the agent produces a multi-page audit, code review, or debug trace, it lands here.\n"
            "You read it comfortably in this pane, and optionally edit with the edit marker.\n\n"
            "## How it works\n\n"
            "1. Agent publishes: `bridgepanel.py publish --session hermes --type documents report.md`\n"
            "2. Panel auto-refreshes — no spinner, content just appears\n"
            "3. Click any file in the tree to read it\n"
            "4. Press `e` or click Edit to modify — saves back to disk\n\n"
            "## Markdown support\n\n"
            "Tables, code blocks, lists, blockquotes — all rendered.\n\n"
            "| Feature | Status |\n|---------|--------|\n"
            "| Sessions | From BridgeSessions or filesystem |\n"
            "| Edit | Press `e` for raw markdown |\n"
            "| Networking | VPN, LAN, loopback — never public |\n\n"
            "> The terminal is for commands. This panel is for reading.\n"
        ),
    }

    for relpath, content in samples.items():
        fp = base / relpath
        if not fp.exists():
            fp.parent.mkdir(parents=True, exist_ok=True)
            fp.write_text(content, encoding="utf-8")


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
    import json as _json
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
    import base64 as _b64
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
    if payload == "RESET":  # 'OK <off> RESET' with empty tail
        payload = ""
        reset = True
    # daemon b64enc strips '=' padding — restore before decoding
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
    result.sort(key=lambda x: (not x["live"], x["name"]))  # live sessions first

    return {"sessions": result}


# ── Markdown renderer ──────────────────────────────────────────

def inline_markup(text: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"(?<!\*)\*([^*]+)\*", r"<em>\1</em>", escaped)
    url_pattern = r"(?<![\"'=])(https?://[^\s<]+)"
    escaped = re.sub(
        url_pattern,
        lambda m: f'<a href="{html.escape(m.group(1), quote=True)}" target="_blank" rel="noopener">{m.group(1)}</a>',
        escaped,
    )
    return escaped


def markdown_to_html(text: str) -> str:
    """Small safe Markdown renderer; raw HTML is always escaped."""
    lines = text.replace("\r\n", "\n").split("\n")
    out: list[str] = []
    paragraph: list[str] = []
    in_code = False
    code_lines: list[str] = []
    code_lang = ""
    list_kind: str | None = None

    def flush_paragraph() -> None:
        if paragraph:
            out.append("<p>" + inline_markup(" ".join(x.strip() for x in paragraph)) + "</p>")
            paragraph.clear()

    def close_list() -> None:
        nonlocal list_kind
        if list_kind:
            out.append(f"</{list_kind}>")
            list_kind = None

    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("```"):
            flush_paragraph()
            close_list()
            if in_code:
                out.append(
                    f'<pre class="code"><code data-lang="{html.escape(code_lang)}">'
                    f"{html.escape(chr(10).join(code_lines))}</code></pre>"
                )
                code_lines = []
                code_lang = ""
                in_code = False
            else:
                in_code = True
                code_lang = line[3:].strip()[:30]
            i += 1
            continue
        if in_code:
            code_lines.append(line)
            i += 1
            continue
        if not line.strip():
            flush_paragraph()
            close_list()
            i += 1
            continue
        heading = re.match(r"^(#{1,6})\s+(.+)$", line)
        if heading:
            flush_paragraph()
            close_list()
            level = len(heading.group(1))
            body = inline_markup(heading.group(2))
            out.append(f"<h{level}>{body}</h{level}>")
            i += 1
            continue
        if line.startswith("> "):
            flush_paragraph()
            close_list()
            out.append(f"<blockquote>{inline_markup(line[2:])}</blockquote>")
            i += 1
            continue
        unordered = re.match(r"^\s*[-*+]\s+(.+)$", line)
        ordered = re.match(r"^\s*\d+[.)]\s+(.+)$", line)
        if unordered or ordered:
            flush_paragraph()
            needed = "ul" if unordered else "ol"
            if list_kind != needed:
                close_list()
                out.append(f"<{needed}>")
                list_kind = needed
            match = unordered or ordered
            if match is None:
                continue
            out.append(f"<li>{inline_markup(match.group(1))}</li>")
            i += 1
            continue
        if "|" in line and i + 1 < len(lines) and re.match(r"^\s*\|?\s*:?-+", lines[i + 1]):
            flush_paragraph()
            close_list()
            headers = [x.strip() for x in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < len(lines) and "|" in lines[i] and lines[i].strip():
                rows.append([x.strip() for x in lines[i].strip().strip("|").split("|")])
                i += 1
            table = ['<div class="table-wrap"><table><thead><tr>']
            table.extend(f"<th>{inline_markup(x)}</th>" for x in headers)
            table.append("</tr></thead><tbody>")
            for row in rows:
                table.append("<tr>")
                table.extend(f"<td>{inline_markup(x)}</td>" for x in row)
                table.append("</tr>")
            table.append("</tbody></table></div>")
            out.append("".join(table))
            continue
        if re.match(r"^---+$", line.strip()):
            flush_paragraph()
            close_list()
            out.append("<hr>")
            i += 1
            continue
        paragraph.append(line)
        i += 1
    flush_paragraph()
    close_list()
    if in_code:
        out.append(
            f'<pre class="code"><code>{html.escape(chr(10).join(code_lines))}</code></pre>'
        )
    return "\n".join(out)


# ── SVG Favicon ────────────────────────────────────────────────

FAVICON_SVG = b"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <rect x="3" y="3" width="26" height="26" rx="7" fill="#ffffff" stroke="rgba(0,0,0,0.08)" stroke-width="1"/>
  <rect x="8" y="9" width="11" height="2.5" rx="1.25" fill="#0d0d0d"/>
  <rect x="8" y="14.75" width="16" height="2.5" rx="1.25" fill="#888888"/>
  <rect x="8" y="20.5" width="9" height="2.5" rx="1.25" fill="#18E299"/>
</svg>"""


# ── HTML ───────────────────────────────────────────────────────

INDEX_HTML = r'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BridgePanel</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAzMiAzMiIgd2lkdGg9IjMyIiBoZWlnaHQ9IjMyIj48cmVjdCB4PSIzIiB5PSIzIiB3aWR0aD0iMjYiIGhlaWdodD0iMjYiIHJ4PSI3IiBmaWxsPSIjZmZmZmZmIiBzdHJva2U9InJnYmEoMCwwLDAsMC4wOCkiIHN0cm9rZS13aWR0aD0iMSIvPjxyZWN0IHg9IjgiIHk9IjkiIHdpZHRoPSIxMSIgaGVpZ2h0PSIyLjUiIHJ4PSIxLjI1IiBmaWxsPSIjMGQwZDBkIi8+PHJlY3QgeD0iOCIgeT0iMTQuNzUiIHdpZHRoPSIxNiIgaGVpZ2h0PSIyLjUiIHJ4PSIxLjI1IiBmaWxsPSIjODg4ODg4Ii8+PHJlY3QgeD0iOCIgeT0iMjAuNSIgd2lkdGg9IjkiIGhlaWdodD0iMi41IiByeD0iMS4yNSIgZmlsbD0iIzE4RTI5OSIvPjwvc3ZnPg==">
<style>
:root {
  --bg: #ffffff;
  --text: #0d0d0d;
  --text-2: #333333;
  --text-3: #666666;
  --text-4: #888888;
  --border: rgba(0,0,0,0.05);
  --border-2: rgba(0,0,0,0.08);
  --divider: rgba(0,0,0,0.18);
  --hover: rgba(0,0,0,0.02);
  --accent: #18E299;
  --accent-soft: #d4fae8;
  --accent-deep: #0fa76e;
  --sans: 'Inter', system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif;
  --mono: 'JetBrains Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
* { box-sizing: border-box; margin: 0; padding: 0; }
html, body {
  height: 100%;
  font-family: var(--sans);
  font-size: 15px;
  color: var(--text-2);
  background: var(--bg);
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
}
button, input, textarea { font: inherit; color: inherit; }

/* ── Layout: machines | sessions | work area ── */
.shell {
  height: 100vh;
  display: grid;
  grid-template-columns: 200px 260px minmax(0, 1fr);
}
aside {
  min-height: 0;
  overflow-y: auto;
  background: var(--bg);
}
aside.machines { border-right: 1px solid var(--divider); }
aside.sessions { border-right: 1px solid var(--divider); }

/* ── Titlebar ── */
.titlebar {
  display: flex;
  align-items: baseline;
  gap: 8px;
  padding: 14px 16px 12px;
  border-bottom: 1px solid var(--divider);
  margin-bottom: 6px;
}
.titlebar .brand { font-size: 15px; font-weight: 700; letter-spacing: 0.4px; color: var(--text); }
.titlebar .build { font-family: var(--mono); font-size: 11px; color: var(--text-3); }

.col-header {
  padding: 10px 16px 8px;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-4);
  text-transform: uppercase;
  letter-spacing: 0.65px;
}

/* ── Machine rows ── */
.machine-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 16px;
  cursor: pointer;
  user-select: none;
  font-size: 14px;
  font-weight: 500;
  color: var(--text);
}
.machine-row:hover { background: var(--hover); }
.machine-row.active { background: var(--accent-soft); }
.machine-row .dot {
  width: 6px; height: 6px; border-radius: 50%;
  background: #c8c8c8; flex-shrink: 0;
}
.machine-row .dot.up { background: var(--accent); }
.machine-row .count {
  margin-left: auto;
  font-family: var(--mono); font-size: 11px; color: var(--text-4);
}
.machine-row .you {
  font-family: var(--mono); font-size: 10px; color: var(--text-4);
}

/* ── Session rows ── */
.session-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 16px;
  cursor: pointer;
  user-select: none;
  font-size: 14px;
  color: var(--text);
}
.session-row:hover { background: var(--hover); }
.session-row.active { background: var(--accent-soft); }
.session-row .hicon { font-size: 12px; opacity: 0.6; width: 16px; text-align: center; }
.session-row .sname { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }
.session-row .sstate { font-family: var(--mono); font-size: 10px; color: var(--text-4); }
.session-row .dot {
  width: 6px; height: 6px; border-radius: 50%;
  background: #c8c8c8; flex-shrink: 0;
}
.session-row .dot.live { background: var(--accent); }

/* ── Main ── */
main { min-width: 0; min-height: 0; display: flex; flex-direction: column; overflow: hidden; }
.breadcrumb {
  padding: 14px 20px 4px;
  font-size: 12px;
  font-family: var(--mono);
  color: var(--text-4);
}
.breadcrumb span.sep { opacity: 0.4; margin: 0 4px; }
.breadcrumb span.current { color: var(--text-2); }

/* ── Tab bar ── */
.tabbar {
  display: flex;
  gap: 2px;
  padding: 6px 20px 0;
  border-bottom: 1px solid var(--border);
}
.tabbar button {
  border: none;
  background: none;
  padding: 6px 12px;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-4);
  cursor: pointer;
  border-bottom: 2px solid transparent;
}
.tabbar button:hover { color: var(--text-2); }
.tabbar button.active {
  color: var(--text);
  border-bottom-color: var(--accent);
}

/* ── Tools bar ── */
.toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 20px;
  background: var(--bg);
  border-bottom: 1px solid var(--border);
}
.btn-group { display: flex; gap: 6px; }
.btn-group button {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 4px 10px;
  font-size: 12px;
  font-weight: 500;
  border: 1px solid var(--border-2);
  border-radius: 6px;
  background: var(--bg);
  color: var(--text-2);
  cursor: pointer;
}
.btn-group button:hover { background: var(--hover); }
.btn-group button.primary {
  background: var(--text);
  color: var(--bg);
  border-color: var(--text);
}
.btn-group button svg { width: 12px; height: 12px; }
.toolbar .spacer { flex: 1; }
.follow-chip {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-4);
  cursor: pointer;
  user-select: none;
  padding: 3px 8px;
  border: 1px solid var(--border-2);
  border-radius: 10px;
}
.follow-chip.on { color: var(--accent-deep); border-color: var(--accent); }

/* ── Work area ── */
#workarea { flex: 1; min-height: 0; display: flex; overflow: hidden; }
#outputPane {
  flex: 1;
  overflow-y: auto;
  padding: 12px 20px 40px;
  font-family: var(--mono);
  font-size: 13px;
  line-height: 1.45;
  white-space: pre-wrap;
  word-break: break-all;
  color: var(--text-2);
  background: var(--bg);
}
#filePane { flex: 1; min-height: 0; display: flex; overflow: hidden; }
#filelist {
  width: 220px;
  border-right: 1px solid var(--border);
  overflow-y: auto;
  padding: 8px 0;
}
.file-item {
  display: flex;
  align-items: center;
  padding: 5px 16px;
  cursor: pointer;
  font-size: 13px;
  color: var(--text-2);
  border-left: 2px solid transparent;
  overflow: hidden;
}
.file-item:hover { background: var(--hover); }
.file-item.active {
  background: var(--accent-soft);
  border-left-color: var(--accent);
  color: var(--text);
  font-weight: 500;
}
.file-item .file-name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }
.file-item .file-time {
  font-size: 10px; font-family: var(--mono); color: var(--text-4);
  margin-left: 6px; flex-shrink: 0;
}
#content {
  flex: 1;
  overflow-y: auto;
  padding: 12px 20px 60px;
}
.doc-container { max-width: 860px; }
.doc-container h1, .doc-container h2, .doc-container h3 { color: var(--text); margin: 18px 0 8px; }
.doc-container h1 { font-size: 22px; }
.doc-container h2 { font-size: 18px; }
.doc-container h3 { font-size: 16px; }
.doc-container p { margin: 8px 0; line-height: 1.65; }
.doc-container code {
  font-family: var(--mono); font-size: 13px;
  background: rgba(0,0,0,0.04); padding: 1px 5px; border-radius: 4px;
}
.doc-container pre {
  font-family: var(--mono); font-size: 13px;
  background: rgba(0,0,0,0.03);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px 14px;
  overflow-x: auto;
  margin: 10px 0;
}
.doc-container pre code { background: none; padding: 0; }
.doc-container ul, .doc-container ol { margin: 8px 0 8px 22px; line-height: 1.65; }
.doc-container table { border-collapse: collapse; margin: 10px 0; }
.doc-container th, .doc-container td {
  border: 1px solid var(--border-2); padding: 5px 10px; font-size: 13px; text-align: left;
}
.doc-container th { background: rgba(0,0,0,0.02); font-weight: 600; }
.doc-container a { color: var(--accent-deep); text-decoration: none; }
.doc-container a:hover { text-decoration: underline; }
.doc-container blockquote {
  border-left: 3px solid var(--border-2);
  margin: 10px 0; padding: 2px 14px; color: var(--text-3);
}
#editTextarea {
  width: 100%;
  min-height: 60vh;
  font-family: var(--mono);
  font-size: 13px;
  line-height: 1.5;
  padding: 14px;
  border: 1px solid var(--border-2);
  border-radius: 8px;
  resize: vertical;
  outline: none;
}
#editTextarea:focus { border-color: var(--accent); }

.empty-state {
  padding: 60px 20px;
  text-align: center;
  color: var(--text-4);
  font-size: 14px;
}
.empty-state .hint { font-size: 12px; margin-top: 6px; font-family: var(--mono); }

/* ── Toast ── */
#toast {
  position: fixed;
  bottom: 20px;
  left: 50%;
  transform: translateX(-50%);
  background: var(--text);
  color: var(--bg);
  padding: 8px 16px;
  border-radius: 8px;
  font-size: 13px;
  opacity: 0;
  transition: opacity 0.2s ease;
  pointer-events: none;
  z-index: 50;
}
#toast.show { opacity: 1; }
#toast.err { background: #b3261e; }
</style>
</head>
<body>
<div class="shell">
  <aside class="machines">
    <div class="titlebar"><span class="brand">BRIDGE PANEL</span><span class="build">__BUILD_TAG__</span></div>
    <div class="col-header">Machines</div>
    <div id="machines"></div>
  </aside>
  <aside class="sessions">
    <div class="col-header" id="sessionsHeader">Sessions</div>
    <div id="sessions"></div>
  </aside>
  <main>
    <div class="breadcrumb" id="breadcrumb"></div>
    <div class="tabbar" id="tabbar" style="display:none">
      <button data-tab="output" class="active">Output</button>
      <button data-tab="comms">Comms</button>
      <button data-tab="docs">Docs</button>
    </div>
    <div class="toolbar" id="toolbar" style="display:none">
      <div class="btn-group">
        <button id="editBtn"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M11.3 2.7a1.4 1.4 0 0 1 2 2L5 13H3v-2l8.3-8.3z"/></svg>Edit</button>
        <button id="saveBtn" class="primary" style="display:none"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M13 3 6 10 3 7"/></svg>Save</button>
        <button id="cancelBtn" style="display:none"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="m4 4 8 8M12 4l-8 8"/></svg>Cancel</button>
        <button id="copyBtn"><svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="5" y="5" width="8" height="9" rx="1"/><path d="M11 5V3a1 1 0 0 0-1-1H4a1 1 0 0 0-1 1v7a1 1 0 0 0 1 1h1"/></svg>Copy</button>
      </div>
      <div class="spacer"></div>
      <span class="follow-chip on" id="followChip" style="display:none">follow ●</span>
    </div>
    <div id="workarea">
      <pre id="outputPane" style="display:none"></pre>
      <div id="filePane" style="display:none">
        <div id="filelist"></div>
        <div id="content"><div class="empty-state">Select a file</div></div>
      </div>
      <div class="empty-state" id="welcomePane">
        <div>Select a machine, then a session.</div>
        <div class="hint">machines → sessions → output / comms / docs</div>
      </div>
    </div>
  </main>
</div>
<div id="toast"></div>
<script>
(function(){
  'use strict';
  const base = location.pathname.replace(/\/$/, '');
  const machinesEl = document.getElementById('machines');
  const sessionsEl = document.getElementById('sessions');
  const sessionsHeaderEl = document.getElementById('sessionsHeader');
  const breadcrumbEl = document.getElementById('breadcrumb');
  const tabbarEl = document.getElementById('tabbar');
  const toolbarEl = document.getElementById('toolbar');
  const outputPane = document.getElementById('outputPane');
  const filePane = document.getElementById('filePane');
  const welcomePane = document.getElementById('welcomePane');
  const filelistEl = document.getElementById('filelist');
  const contentEl = document.getElementById('content');
  const followChip = document.getElementById('followChip');
  const editBtn = document.getElementById('editBtn');
  const saveBtn = document.getElementById('saveBtn');
  const cancelBtn = document.getElementById('cancelBtn');
  const copyBtn = document.getElementById('copyBtn');
  const toastEl = document.getElementById('toast');

  const HARNESS_RE = /hermes|codex|claude|kimi|grok|opencode/i;

  // ── State ──
  let mesh = {node:'', peers:[], sessions:[], offline:true};
  let fsTree = {sessions:[]};
  let selMachine = null;       // '' = local node
  let selSession = null;
  let selSessionLive = false;
  let selSessionCommand = '';
  let tab = 'output';
  let outputOffset = 0;
  let outputFollow = true;
  let outputTimer = null;
  let curFile = null, curType = null, curRaw = '', editing = false;

  const esc = s => String(s).replace(/[&<>"']/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  function toast(msg, isErr) {
    toastEl.textContent = msg;
    toastEl.className = 'show' + (isErr ? ' err' : '');
    setTimeout(() => { toastEl.className = ''; }, 1800);
  }

  async function api(path) {
    const r = await fetch(base + path);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }

  // Strip ANSI CSI sequences (colors/cursor); OSC52 already stripped server-side.
  function stripAnsi(s) {
    return s
      .replace(/\x1b\[[0-9;?]*[A-Za-z@-~]/g, '')
      .replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '')
      .replace(/\x1b[@-Z\\-_]/g, '');
  }

  // ── Machines column ──
  function machineList() {
    // Local node first (marked), then peers sorted by name
    const out = [];
    out.push({name: mesh.node || '(local)', healthy: !mesh.offline, you: true,
              sessions: mesh.sessions || []});
    for (const p of (mesh.peers || [])) {
      out.push({name: p.name, healthy: !!p.healthy, you: false,
                sessions: p.sessions || []});
    }
    return out;
  }

  function renderMachines() {
    const list = machineList();
    if (!list.length || (list.length === 1 && mesh.offline)) {
      machinesEl.innerHTML = '<div class="machine-row"><span class="dot"></span>' +
        '<span>(mesh offline)</span></div>';
      return;
    }
    machinesEl.innerHTML = list.map(m =>
      '<div class="machine-row' + (selMachine === m.name ? ' active' : '') +
      '" data-machine="' + esc(m.name) + '">' +
      '<span class="dot' + (m.healthy ? ' up' : '') + '"></span>' +
      '<span>' + esc(m.name) + '</span>' +
      (m.you ? '<span class="you">you</span>' : '') +
      '<span class="count">' + (m.sessions ? m.sessions.length : 0) + '</span>' +
      '</div>'
    ).join('');
  }

  machinesEl.addEventListener('click', e => {
    const row = e.target.closest('.machine-row');
    if (!row || !row.dataset.machine) return;
    selMachine = row.dataset.machine;
    selSession = null;
    renderMachines();
    renderSessions();
    renderWork();
  });

  // ── Sessions column ──
  function sessionsFor(machineName) {
    const m = machineList().find(x => x.name === machineName);
    const live = (m && m.sessions) || [];
    // Merge filesystem sessions for the local node only
    const isLocal = !machineName || machineName === mesh.node ||
                    machineName === '(local)' || machineName === '';
    const out = live.map(s => ({
      name: s.name, state: s.state || '', command: s.command || '',
      bytes: s.bytes || 0, live: true,
      harness: HARNESS_RE.test(s.command || s.name || '')
    }));
    if (isLocal) {
      const liveNames = new Set(out.map(s => s.name));
      for (const fs of (fsTree.sessions || [])) {
        if (!liveNames.has(fs.name)) {
          out.push({name: fs.name, state: 'stored', command: '', bytes: 0,
                    live: !!fs.live, harness: HARNESS_RE.test(fs.name)});
        }
      }
    }
    out.sort((a, b) => (b.live - a.live) || a.name.localeCompare(b.name));
    return out;
  }

  function renderSessions() {
    const list = selMachine !== null ? sessionsFor(selMachine) : [];
    sessionsHeaderEl.textContent = selMachine ? ('Sessions  ' + selMachine) : 'Sessions';
    if (!selMachine) {
      sessionsEl.innerHTML = '';
      return;
    }
    if (!list.length) {
      sessionsEl.innerHTML = '<div class="session-row"><span class="sname" ' +
        'style="color:var(--text-4)">no sessions</span></div>';
      return;
    }
    sessionsEl.innerHTML = list.map(s =>
      '<div class="session-row' + (selSession === s.name ? ' active' : '') +
      '" data-session="' + esc(s.name) + '" data-live="' + (s.live ? '1' : '') +
      '" data-command="' + esc(s.command || '') + '">' +
      '<span class="hicon">' + (s.harness ? '⚙' : '⬚') + '</span>' +
      '<span class="sname">' + esc(s.name) + '</span>' +
      '<span class="sstate">' + esc(s.state || '') + '</span>' +
      '<span class="dot' + (s.live ? ' live' : '') + '"></span>' +
      '</div>'
    ).join('');
  }

  sessionsEl.addEventListener('click', e => {
    const row = e.target.closest('.session-row');
    if (!row || !row.dataset.session) return;
    selSession = row.dataset.session;
    selSessionLive = row.dataset.live === '1';
    selSessionCommand = row.dataset.command || '';
    outputOffset = 0;
    outputPane.textContent = '';
    curFile = null; editing = false;
    renderSessions();
    renderWork();
  });

  // ── Tabs ──
  tabbarEl.addEventListener('click', e => {
    const btn = e.target.closest('button[data-tab]');
    if (!btn) return;
    tab = btn.dataset.tab;
    for (const b of tabbarEl.querySelectorAll('button'))
      b.classList.toggle('active', b === btn);
    curFile = null; editing = false;
    renderWork();
  });

  // ── Work area ──
  function renderBreadcrumb() {
    if (!selSession) { breadcrumbEl.innerHTML = ''; return; }
    breadcrumbEl.innerHTML =
      '<span>' + esc(selMachine || '') + '</span><span class="sep">/</span>' +
      '<span>' + esc(selSession) + '</span><span class="sep">/</span>' +
      '<span class="current">' + esc(tab) + '</span>';
  }

  function updateTools() {
    const showEditorBtns = (tab === 'comms' || tab === 'docs') && curFile;
    editBtn.style.display = showEditorBtns && !editing ? '' : 'none';
    saveBtn.style.display = editing ? '' : 'none';
    cancelBtn.style.display = editing ? '' : 'none';
    copyBtn.style.display = showEditorBtns && !editing ? '' : 'none';
    followChip.style.display = (tab === 'output' && selSessionLive) ? '' : 'none';
  }

  function renderWork() {
    renderBreadcrumb();
    updateTools();
    const has = !!selSession;
    tabbarEl.style.display = has ? '' : 'none';
    toolbarEl.style.display = has ? '' : 'none';
    welcomePane.style.display = has ? 'none' : '';
    outputPane.style.display = (has && tab === 'output') ? '' : 'none';
    filePane.style.display = (has && (tab === 'comms' || tab === 'docs')) ? 'flex' : 'none';

    if (outputTimer) { clearInterval(outputTimer); outputTimer = null; }
    if (has && tab === 'output') {
      if (selSessionLive) {
        pollOutput();
        outputTimer = setInterval(pollOutput, 1000);
      } else {
        outputPane.textContent = 'No live output — session is not running on the mesh.\n' +
          'Stored files are under the Comms / Docs tabs.';
      }
    }
    if (has && (tab === 'comms' || tab === 'docs')) {
      renderFileList();
    }
  }

  // ── Output plane ──
  async function pollOutput() {
    if (!selSession || tab !== 'output') return;
    try {
      const d = await api('/api/output?session=' + encodeURIComponent(selSession) +
                          '&since=' + outputOffset);
      if (d.error) {
        if (!outputPane.dataset.errShown) {
          outputPane.textContent = '(output unavailable: ' + d.error + ')';
          outputPane.dataset.errShown = '1';
        }
        return;
      }
      delete outputPane.dataset.errShown;
      if (d.reset) outputPane.textContent = '';
      if (d.text) {
        outputPane.textContent += stripAnsi(d.text);
        if (outputFollow) outputPane.scrollTop = outputPane.scrollHeight;
      }
      if (typeof d.offset === 'number') outputOffset = d.offset;
    } catch (_) { /* keep last good frame */ }
  }

  followChip.addEventListener('click', () => {
    outputFollow = !outputFollow;
    followChip.classList.toggle('on', outputFollow);
    followChip.textContent = outputFollow ? 'follow ●' : 'paused ○';
  });
  outputPane.addEventListener('scroll', () => {
    const atBottom = outputPane.scrollTop + outputPane.clientHeight >=
                     outputPane.scrollHeight - 24;
    if (!atBottom && outputFollow) followChip.click();
  });

  // ── File list (comms/docs) ──
  function filesFor(session, dtype) {
    const fs = (fsTree.sessions || []).find(x => x.name === session);
    if (!fs) return [];
    return fs[dtype] || [];
  }

  function renderFileList() {
    const files = filesFor(selSession, tab);
    if (!files.length) {
      filelistEl.innerHTML = '<div class="file-item"><span class="file-name" ' +
        'style="color:var(--text-4)">no ' + esc(tab) + ' yet</span></div>';
      contentEl.innerHTML = '<div class="empty-state">No files in this session yet.<div class="hint">publish with: bs pane publish --session ' +
        esc(selSession || '') + ' &lt;file&gt;</div></div>';
      return;
    }
    filelistEl.innerHTML = files.map(f =>
      '<div class="file-item' + (curFile === f.name ? ' active' : '') +
      '" data-file="' + esc(f.name) + '">' +
      '<span class="file-name">' + esc(f.name) + '</span>' +
      '<span class="file-time">' + esc(f.modified_human || '') + '</span>' +
      '</div>'
    ).join('');
    if (!curFile && files.length) openFile(files[0].name);
  }

  filelistEl.addEventListener('click', e => {
    const item = e.target.closest('.file-item');
    if (!item || !item.dataset.file) return;
    openFile(item.dataset.file);
  });

  async function openFile(name) {
    curFile = name;
    editing = false;
    renderFileListActiveOnly();
    try {
      const d = await api('/api/content?session=' + encodeURIComponent(selSession) +
                          '&type=' + encodeURIComponent(tab) +
                          '&name=' + encodeURIComponent(name));
      curRaw = d.raw || '';
      contentEl.innerHTML = '<div class="doc-container">' + (d.html || '') + '</div>';
    } catch (err) {
      contentEl.innerHTML = '<div class="empty-state">Failed to load file</div>';
      toast('Load failed', true);
    }
    updateTools();
  }
  function renderFileListActiveOnly() {
    for (const el of filelistEl.querySelectorAll('.file-item'))
      el.classList.toggle('active', el.dataset.file === curFile);
  }

  // ── Editor ──
  editBtn.addEventListener('click', () => {
    if (!curFile) return;
    editing = true;
    contentEl.innerHTML = '<textarea id="editTextarea"></textarea>';
    document.getElementById('editTextarea').value = curRaw;
    updateTools();
  });
  cancelBtn.addEventListener('click', () => { openFile(curFile); });
  copyBtn.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(curRaw); toast('Copied'); }
    catch (_) { toast('Copy failed', true); }
  });
  saveBtn.addEventListener('click', async () => {
    const ta = document.getElementById('editTextarea');
    if (!ta) return;
    const body = JSON.stringify({session: selSession, type: tab, name: curFile, content: ta.value});
    try {
      const r = await fetch(base + '/api/save', {method: 'POST',
        headers: {'Content-Type': 'application/json'}, body});
      if (!r.ok) throw new Error('HTTP ' + r.status);
      curRaw = ta.value;
      toast('Saved');
      await refreshFsTree();
      openFile(curFile);
    } catch (_) { toast('Save failed', true); }
  });

  // ── Polling ──
  async function refreshMesh() {
    try {
      mesh = await api('/api/machines');
      if (selMachine === null && mesh.node) selMachine = mesh.node;
      renderMachines();
      if (selMachine !== null) renderSessions();
    } catch (_) { /* keep last good frame */ }
  }
  async function refreshFsTree() {
    try { fsTree = await api('/api/tree'); } catch (_) {}
  }

  // ── Init ──
  (async function init() {
    await Promise.all([refreshMesh(), refreshFsTree()]);
    if (selMachine === null) {
      const list = machineList();
      if (list.length) selMachine = list[0].name;
    }
    renderMachines();
    renderSessions();
    renderWork();
    setInterval(refreshMesh, 5000);
    setInterval(refreshFsTree, 5000);
  })();
})();
</script>
</body>
</html>'''


# ── HTTP Handler ───────────────────────────────────────────────

class BridgePanelHandler(BaseHTTPRequestHandler):
    server_version = f"BridgePanel/{VERSION}"
    protocol_version = "HTTP/1.1"

    @property
    def token(self) -> str:
        return self.server.bridgepanel_token  # type: ignore[attr-defined]

    def log_message(self, format: str, *args) -> None:  # noqa: A002
        safe_args = tuple(str(v).replace(self.token, "<token>") for v in args)
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), format % safe_args))

    def security_headers(self, content_type: str, length: int | None = None) -> None:
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; "
            "style-src 'unsafe-inline'; "
            "script-src 'unsafe-inline'; "
            "img-src 'self' data:; "
            "connect-src 'self';"
        )
        if length is not None:
            self.send_header("Content-Length", str(length))

    def send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.security_headers(content_type, len(body))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, payload: dict, status: int = 200) -> None:
        self.send_bytes(
            json.dumps(payload).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
        )

    def reject(self, status: int, message: str) -> None:
        self.send_bytes(message.encode("utf-8"), "text/plain; charset=utf-8", status)

    def authorized_path(self, *, require_token: bool = False) -> tuple[str, str] | None:
        parsed = urlparse(self.path)
        parts = parsed.path.split("/")
        trusted_ips = getattr(self.server, "trusted_ips", set())
        has_token = len(parts) >= 2 and secrets.compare_digest(parts[1], self.token)
        if self.client_address[0] in trusted_ips and not require_token:
            # Trusted IP may read without token; optional token strip if present.
            if has_token:
                parts = [""] + parts[2:]
            return "/" + "/".join(parts[1:]), parsed.query
        # Untrusted IP, or any write: token required.
        if not has_token:
            return None
        return "/" + "/".join(parts[2:]), parsed.query

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        # Health check (no auth)
        if parsed.path == "/healthz":
            self.send_json({"ok": True, "service": APP, "version": VERSION})
            return

        auth = self.authorized_path(require_token=False)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return

        path, query = auth
        params = parse_qs(query)

        if path in ("/", ""):
            html = INDEX_HTML.replace("__BUILD_TAG__", f"{BUILDTAG}")
            self.send_bytes(html.encode("utf-8"), "text/html; charset=utf-8")
        elif path in ("/favicon.ico", "/favicon.svg"):
            self.send_response(200)
            self.security_headers("image/svg+xml; charset=utf-8", len(FAVICON_SVG))
            self.end_headers()
            self.wfile.write(FAVICON_SVG)
        elif path == "/api/tree":
            self.send_json(build_tree())
        elif path == "/api/machines":
            # Mesh plane: local node + peers + their sessions (MESH_TREE passthrough)
            self.send_json(query_mesh_tree())
        elif path == "/api/output":
            # Output plane: incremental scrollback for a live session
            session = params.get("session", [""])[0]
            try:
                since = int(params.get("since", ["0"])[0])
            except ValueError:
                since = 0
            if not session:
                self.reject(HTTPStatus.BAD_REQUEST, "session required")
                return
            self.send_json(query_scrollback(session, since))
        elif path == "/api/content":
            session = params.get("session", [""])[0]
            dtype = params.get("type", ["documents"])[0]
            name = params.get("name", [""])[0]
            item = resolve_file(session, dtype, name)
            if not item:
                self.reject(HTTPStatus.NOT_FOUND, "File not found")
                return
            raw = item.read_text(encoding="utf-8", errors="replace")
            suffix = item.suffix.lower()
            is_md = suffix in (".md", ".markdown", "")
            self.send_json({
                "name": item.name,
                "raw": raw,
                "html": markdown_to_html(raw) if is_md else f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;word-break:break-all">{html.escape(raw)}</pre>',
                "editable": True,
            })
        else:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        # Writes always require token — even from trusted IPs (audit P1-4c).
        auth = self.authorized_path(require_token=True)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, _ = auth

        if path == "/api/save":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
            except ValueError:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length < 0:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            # Reject oversized bodies BEFORE reading (audit P1-4a).
            if length > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return

            session = safe_session_name(body.get("session", ""))
            dtype = safe_type(body.get("type", ""))
            name = safe_name(body.get("name", ""))
            content = body.get("content", "")

            if not session or not name:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing session or name")
                return

            if len(content.encode("utf-8")) > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return

            target_dir = sessions_dir() / session / dtype
            target_dir.mkdir(parents=True, exist_ok=True)
            target = target_dir / name

            # Resolve and validate path stays under sessions/
            try:
                target.resolve().relative_to(sessions_dir().resolve())
            except (ValueError, OSError):
                self.reject(HTTPStatus.FORBIDDEN, "Path escape")
                return

            target.write_text(content, encoding="utf-8")
            self.send_json({"ok": True, "html": markdown_to_html(content)})
            return

        self.reject(HTTPStatus.NOT_FOUND, "Not found")


# ── CLI ────────────────────────────────────────────────────────

def serve(bind: str, port: int) -> None:
    token = ensure_dirs()
    server = ThreadingHTTPServer((bind, port), BridgePanelHandler)
    server.bridgepanel_token = token  # type: ignore[attr-defined]
    trusted_ips = {
        v.strip()
        for v in os.environ.get("BRIDGEPANEL_TRUSTED_IPS", "").split(",")
        if v.strip()
    }
    setattr(server, "trusted_ips", trusted_ips)
    print(f"{APP} {VERSION} listening on http://{bind}:{port}/{token}/", flush=True)
    server.serve_forever()


def publish(source: Path, session: str, dtype: str, title: str | None) -> Path:
    ensure_dirs()
    source = source.expanduser().resolve(strict=True)
    if not source.is_file():
        raise ValueError(f"Not a file: {source}")
    name = safe_name(title or source.name)
    if title and not Path(name).suffix and source.suffix:
        name += source.suffix
    target_dir = sessions_dir() / safe_session_name(session) / safe_type(dtype)
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / name
    target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description="Reading and writing surface for human-agent communication")
    sub = parser.add_subparsers(dest="command", required=True)

    serve_p = sub.add_parser("serve", help="start the web service")
    serve_p.add_argument("--bind", default=DEFAULT_BIND)
    serve_p.add_argument("--port", type=int, default=DEFAULT_PORT)

    pub_p = sub.add_parser("publish", help="copy a file into a session")
    pub_p.add_argument("source", type=Path)
    pub_p.add_argument("--session", default="default")
    pub_p.add_argument("--type", default="documents", choices=["comms", "documents"])
    pub_p.add_argument("--title")

    note_p = sub.add_parser("note", help="publish stdin as a markdown file")
    note_p.add_argument("--session", default="default")
    note_p.add_argument("--type", default="comms")
    note_p.add_argument("--title", default="Note.md")

    sub.add_parser("url", help="print the private panel URL").add_argument("--bind", default=DEFAULT_BIND)
    sub.add_parser("tree", help="print the session tree as JSON")

    args = parser.parse_args()

    if args.command == "serve":
        serve(args.bind, args.port)
    elif args.command == "publish":
        target = publish(args.source, args.session, args.type, args.title)
        print(target)
    elif args.command == "note":
        ensure_dirs()
        text = sys.stdin.read()
        name = safe_name(args.title)
        if not Path(name).suffix:
            name += ".md"
        target_dir = sessions_dir() / safe_session_name(args.session) / safe_type(args.type)
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / name
        target.write_text(text, encoding="utf-8")
        print(target)
    elif args.command == "tree":
        print(json.dumps(build_tree(), indent=2))
    else:
        token = ensure_dirs()
        print(f"http://{args.bind}:{9770}/{token}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
