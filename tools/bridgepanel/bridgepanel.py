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
VERSION = "2.0.0"
BUILDTAG = "2026.07.16"  # updated per release; see commit/build number in titlebar
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

def query_bs_sessions() -> list[dict]:
    """Query BridgeSessions daemon for active sessions via IPC.

    Returns a list of {name, state, command} dicts. Returns [] if BS
    is not running or unreachable — panel works without BS.
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(BS_IPC_TIMEOUT)
        s.connect(("127.0.0.1", BS_IPC_PORT))
        s.sendall(b"SESSIONS\n")
        raw = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            raw += chunk
        s.close()
    except (OSError, socket.timeout):
        return []

    text = raw.decode("utf-8", errors="replace").strip()
    if not text or text == "No sessions.":
        return []

    sessions = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(":", 1)
        name = parts[0].strip()
        rest = parts[1].strip() if len(parts) > 1 else ""
        state = "unknown"
        command = ""
        for token in rest.split():
            if token.startswith("state="):
                state = token.split("=", 1)[1]
            elif token.startswith("command="):
                command = token.split("=", 1)[1]
        sessions.append({"name": name, "state": state, "command": command})
    return sessions


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

    # Merge BS live sessions
    for bs_sess in query_bs_sessions():
        sname = safe_session_name(bs_sess["name"])
        if sname not in tree:
            tree[sname] = {"comms": [], "documents": [], "live": True}
        else:
            tree[sname]["live"] = True

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

/* ── Layout ──────────────────────────────────── */
.shell {
  height: 100vh;
  display: grid;
  grid-template-columns: 300px minmax(0, 1fr);
}

/* ── Sidebar ─────────────────────────────────── */
aside {
  min-height: 0;
  overflow-y: auto;
  border-right: 1px solid rgba(0,0,0,0.18);
  padding: 20px 0 40px;
  background: var(--bg);
}
.sidebar-header {
  padding: 0 20px 16px;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-4);
  text-transform: uppercase;
  letter-spacing: 0.65px;
}

/* ── Titlebar ──────────────────────────────── */
.titlebar {
  display: flex;
  align-items: baseline;
  gap: 8px;
  padding: 14px 20px 12px;
  border-bottom: 1px solid rgba(0,0,0,0.18);
  margin-bottom: 8px;
}
.titlebar .brand {
  font-size: 15px;
  font-weight: 700;
  letter-spacing: 0.4px;
  color: var(--text);
}
.titlebar .build {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-3);
}

/* ── Tree ────────────────────────────────────── */
.tree-session {
  margin-bottom: 2px;
}
.session-row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 5px 20px;
  cursor: pointer;
  user-select: none;
  font-size: 14px;
  font-weight: 500;
  color: var(--text);
}
.session-row:hover { background: var(--hover); }
.session-row .chevron {
  width: 12px; height: 12px;
  flex-shrink: 0;
  opacity: 0.4;
  transition: transform 0.15s ease;
}
.session-row.expanded .chevron { transform: rotate(90deg); }
.session-row .live-dot {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: var(--accent);
  flex-shrink: 0;
  margin-left: auto;
}
.session-children {
  overflow: hidden;
  max-height: 0;
  transition: max-height 0.2s ease;
}
.session-children.open { max-height: 2000px; }

.type-group { padding: 2px 0; }
.type-row {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 3px 20px 3px 42px;
  cursor: pointer;
  user-select: none;
  font-size: 12px;
  font-family: var(--mono);
  text-transform: uppercase;
  letter-spacing: 0.6px;
  color: var(--text-4);
}
.type-row:hover { color: var(--text-3); }
.type-row .chevron {
  width: 10px; height: 10px;
  opacity: 0.4;
  transition: transform 0.15s ease;
}
.type-row.expanded .chevron { transform: rotate(90deg); }
.type-children { overflow: hidden; max-height: 0; }
.type-children.open { max-height: 500px; }

.file-item {
  display: flex;
  align-items: center;
  padding: 4px 20px 4px 64px;
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
.file-item .file-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  flex: 1;
}
.file-item .file-time {
  font-size: 10px;
  font-family: var(--mono);
  color: var(--text-4);
  margin-left: 6px;
  flex-shrink: 0;
}
.file-item.active .file-time { color: var(--accent-deep); }

/* ── Main content ────────────────────────────── */
main {
  min-width: 0;
  min-height: 0;
  overflow-y: auto;
  position: relative;
}
/* ── Breadcrumb (above document, aligned to sidebar 'Sessions' title) ── */
.breadcrumb {
  padding: 0 20px 6px;
  font-size: 12px;
  font-family: var(--mono);
  color: var(--text-4);
}
.breadcrumb span.sep { opacity: 0.4; margin: 0 4px; }
.breadcrumb span.current { color: var(--text-2); }

/* ── Tools bar (action buttons, under breadcrumb) ── */
.toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 20px 16px;
  background: var(--bg);
  border-bottom: 1px solid var(--border);
}
.btn-group {
  display: inline-flex;
  gap: 8px;
}
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  min-height: 34px;
  padding: 0 14px;
  border-radius: 8px;
  border: 1px solid var(--border-2);
  background: var(--bg);
  color: var(--text-2);
  font-size: 13px;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.12s ease, border-color 0.12s ease;
}
.btn:hover { background: var(--hover); border-color: var(--text-4); }
.btn:active { background: rgba(0,0,0,0.06); }
.btn:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.btn svg { width: 15px; height: 15px; }
.btn-primary {
  background: var(--text);
  color: #fff;
  border-color: var(--text);
}
.btn-primary:hover { background: #000; border-color: #000; }
.btn[hidden] { display: none; }

/* ── Document ────────────────────────────────── */
.doc-container {
  max-width: 860px;
  margin: 0;
  padding: 24px 20px 120px;
}
.doc-container h1 {
  font-size: 32px;
  font-weight: 600;
  letter-spacing: -0.64px;
  line-height: 1.2;
  color: var(--text);
  margin-bottom: 8px;
}
.doc-container h2 {
  font-size: 22px;
  font-weight: 600;
  letter-spacing: -0.44px;
  color: var(--text);
  margin-top: 36px;
  margin-bottom: 12px;
}
.doc-container h3 {
  font-size: 18px;
  font-weight: 600;
  letter-spacing: -0.36px;
  color: var(--text);
  margin-top: 28px;
  margin-bottom: 8px;
}
.doc-container p {
  font-size: 15px;
  line-height: 1.65;
  margin-bottom: 16px;
  color: var(--text-2);
}
.doc-container ul, .doc-container ol {
  padding-left: 24px;
  margin-bottom: 16px;
}
.doc-container li {
  font-size: 15px;
  line-height: 1.65;
  color: var(--text-2);
  margin-bottom: 4px;
}
.doc-container code {
  font-family: var(--mono);
  font-size: 13px;
  background: rgba(0,0,0,0.04);
  padding: 1px 5px;
  border-radius: 4px;
  color: var(--text);
}
.doc-container pre.code {
  background: #0d0d0d;
  color: #e0e0e0;
  padding: 18px 20px;
  border-radius: 12px;
  overflow-x: auto;
  margin: 20px 0;
  font-size: 13px;
  line-height: 1.5;
}
.doc-container pre.code code {
  background: none;
  padding: 0;
  color: inherit;
  font-size: 13px;
}
.doc-container blockquote {
  border-left: 3px solid var(--accent);
  padding: 4px 0 4px 16px;
  margin: 20px 0;
  color: var(--text-3);
  font-style: italic;
}
.doc-container a {
  color: var(--text);
  text-decoration: underline;
  text-decoration-color: var(--accent);
  text-underline-offset: 2px;
}
.doc-container a:hover { color: var(--accent-deep); }
.doc-container hr {
  border: none;
  border-top: 1px solid var(--border);
  margin: 32px 0;
}
.doc-container .table-wrap {
  overflow-x: auto;
  margin: 20px 0;
}
.doc-container table {
  width: 100%;
  border-collapse: collapse;
  font-size: 14px;
}
.doc-container th {
  text-align: left;
  font-weight: 600;
  color: var(--text);
  padding: 10px 12px;
  border-bottom: 2px solid var(--border-2);
}
.doc-container td {
  padding: 8px 12px;
  border-bottom: 1px solid var(--border);
  color: var(--text-2);
}

/* ── Edit mode ───────────────────────────────── */
.edit-area {
  display: none;
}
.edit-area.active { display: block; }
.edit-area textarea {
  width: 100%;
  min-height: 500px;
  font-family: var(--mono);
  font-size: 13px;
  line-height: 1.6;
  color: var(--text);
  background: #fafafa;
  border: 1px solid var(--border-2);
  border-radius: 12px;
  padding: 20px;
  resize: vertical;
  outline: none;
  transition: border-color 0.15s;
}
.edit-area textarea:focus { border-color: var(--accent); }
.read-area.hidden { display: none; }

/* ── Empty state ─────────────────────────────── */
.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  color: var(--text-4);
  gap: 8px;
}
.empty-state svg { opacity: 0.15; }
.empty-state p { font-size: 14px; }

/* ── Toast ───────────────────────────────────── */
.toast {
  position: fixed;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%) translateY(20px);
  background: var(--text);
  color: #fff;
  padding: 10px 20px;
  border-radius: 9999px;
  font-size: 13px;
  font-weight: 500;
  opacity: 0;
  transition: all 0.25s ease;
  pointer-events: none;
  z-index: 100;
  box-shadow: 0 4px 12px rgba(0,0,0,0.15);
}
.toast.show {
  opacity: 1;
  transform: translateX(-50%) translateY(0);
}

/* ── Scrollbar ───────────────────────────────── */
aside::-webkit-scrollbar, main::-webkit-scrollbar,
.doc-container pre::-webkit-scrollbar {
  width: 6px;
  height: 6px;
}
aside::-webkit-scrollbar-thumb, main::-webkit-scrollbar-thumb,
.doc-container pre::-webkit-scrollbar-thumb {
  background: rgba(0,0,0,0.12);
  border-radius: 3px;
}
aside::-webkit-scrollbar-thumb:hover, main::-webkit-scrollbar-thumb:hover {
  background: rgba(0,0,0,0.2);
}
</style>
</head>
<body>
<div class="shell">
  <aside id="sidebar">
    <div class="titlebar">
      <span class="brand">BRIDGE PANEL</span>
      <span class="build" id="buildTag">__BUILD_TAG__</span>
    </div>
    <div class="sidebar-header">Sessions</div>
    <div id="tree"></div>
  </aside>
  <main>
    <div class="breadcrumb" id="breadcrumb" style="display:none"></div>
    <div class="toolbar" id="toolbar" style="display:none">
      <div class="btn-group">
        <button class="btn" id="editBtn" hidden>Edit</button>
        <button class="btn btn-primary" id="saveBtn" hidden>Save</button>
        <button class="btn" id="cancelBtn" hidden>Cancel</button>
        <button class="btn" id="copyBtn" hidden>Copy</button>
      </div>
    </div>
    <div id="content">
      <div class="empty-state">
        <svg width="48" height="48" viewBox="0 0 32 32" fill="none">
          <rect x="3" y="3" width="26" height="26" rx="7" stroke="currentColor" stroke-width="1.5"/>
          <rect x="8" y="9" width="11" height="2.5" rx="1.25" fill="currentColor"/>
          <rect x="8" y="14.75" width="16" height="2.5" rx="1.25" fill="currentColor"/>
          <rect x="8" y="20.5" width="9" height="2.5" rx="1.25" fill="currentColor"/>
        </svg>
        <p>Select a file from the tree</p>
      </div>
    </div>
  </main>
</div>
<div class="toast" id="toast"></div>

<script>
(() => {
  'use strict';

  const base = location.pathname.replace(/\/$/, '');
  const treeEl = document.getElementById('tree');
  const contentEl = document.getElementById('content');
  const toolbarEl = document.getElementById('toolbar');
  const breadcrumbEl = document.getElementById('breadcrumb');
  const editBtn = document.getElementById('editBtn');
  const saveBtn = document.getElementById('saveBtn');
  const cancelBtn = document.getElementById('cancelBtn');
  const copyBtn = document.getElementById('copyBtn');
  const toastEl = document.getElementById('toast');

  const ICONS = {
    edit:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4z"/></svg>',
    save:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><path d="M17 21v-8H7v8M7 3v5h8"/></svg>',
    cancel: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18M6 6l12 12"/></svg>',
    copy:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>'
  };
  editBtn.innerHTML = ICONS.edit + ' Edit';
  saveBtn.innerHTML = ICONS.save + ' Save';
  cancelBtn.innerHTML = ICONS.cancel + ' Cancel';
  copyBtn.innerHTML = ICONS.copy + ' Copy';

  let currentSession = null;
  let currentType = null;
  let currentFile = null;
  let isEditing = false;
  let canEdit = false;
  let rawContent = '';
  let renderedHtml = '';

  const esc = s => String(s).replace(/[&<>"']/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  function toast(msg) {
    toastEl.textContent = msg;
    toastEl.classList.add('show');
    clearTimeout(toast._t);
    toast._t = setTimeout(() => toastEl.classList.remove('show'), 1800);
  }

  async function api(path) {
    const r = await fetch(base + path);
    if (!r.ok) throw new Error(await r.text() || r.statusText);
    return r;
  }

  // ── Tree rendering ──────────────────────────
  function chevron(w) {
    return '<svg class="chevron" width="' + w + '" height="' + w + '" viewBox="0 0 16 16" fill="none">'
      + '<path d="M6 4l4 4-4 4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  }

  function renderTree(data) {
    const sessions = data.sessions || [];
    if (sessions.length === 0) {
      treeEl.innerHTML = '<div style="padding:0 20px;color:var(--text-4);font-size:13px">No sessions yet.</div>';
      return;
    }

    let html = '';
    for (const sess of sessions) {
      const expanded = sess.live || (sessions.length <= 2);
      html += '<div class="tree-session" data-session="' + esc(sess.name) + '">'
        + '<div class="session-row ' + (expanded ? 'expanded' : '') + '">'
        + chevron(12)
        + '<span>' + esc(sess.name) + '</span>'
        + (sess.live ? '<span class="live-dot" title="live session"></span>' : '')
        + '</div>'
        + '<div class="session-children ' + (expanded ? 'open' : '') + '">'
        + renderTypeGroup('comms', sess.comms, expanded)
        + renderTypeGroup('documents', sess.documents, expanded)
        + '</div></div>';
    }
    treeEl.innerHTML = html;
  }

  function renderTypeGroup(dtype, files, parentExpanded) {
    const expanded = parentExpanded && files.length > 0 && files.length <= 4;
    const muted = files.length === 0 ? 'opacity:0.4' : '';
    let html = '<div class="type-group" data-type="' + dtype + '">'
      + '<div class="type-row ' + (expanded ? 'expanded' : '') + '" style="' + muted + '">'
      + chevron(10)
      + '<span>' + dtype + '</span>'
      + '<span style="margin-left:auto;padding-right:20px">' + files.length + '</span>'
      + '</div>'
      + '<div class="type-children ' + (expanded ? 'open' : '') + '">';

    for (const f of files) {
      const isActive = currentSession && currentType === dtype && currentFile === f.name;
      html += '<div class="file-item' + (isActive ? ' active' : '') + '" data-filename="' + esc(f.name) + '">'
        + '<span class="file-name">' + esc(f.name) + '</span>'
        + '<span class="file-time">' + esc(f.modified_human) + '</span>'
        + '</div>';
    }
    html += '</div></div>';
    return html;
  }

  // ── Event delegation for tree clicks ────────
  treeEl.addEventListener('click', function(e) {
    // Toggle session row
    const sessionRow = e.target.closest('.session-row');
    if (sessionRow && treeEl.contains(sessionRow)) {
      sessionRow.classList.toggle('expanded');
      const children = sessionRow.nextElementSibling;
      if (children) children.classList.toggle('open');
      return;
    }
    // Toggle type row
    const typeRow = e.target.closest('.type-row');
    if (typeRow) {
      typeRow.classList.toggle('expanded');
      const children = typeRow.nextElementSibling;
      if (children) children.classList.toggle('open');
      return;
    }
    // Open file
    const fileItem = e.target.closest('.file-item');
    if (fileItem) {
      const sessionEl = fileItem.closest('.tree-session');
      const typeGroup = fileItem.closest('.type-group');
      if (sessionEl && typeGroup) {
        const session = sessionEl.dataset.session;
        const dtype = typeGroup.dataset.type;
        const filename = fileItem.dataset.filename;
        openFile(session, dtype, filename, fileItem);
      }
    }
  });

  // ── File open ───────────────────────────────
  async function openFile(session, dtype, filename, el) {
    if (isEditing) return;
    currentSession = session;
    currentType = dtype;
    currentFile = filename;

    document.querySelectorAll('.file-item').forEach(function(item) {
      item.classList.remove('active');
    });
    if (el) el.classList.add('active');

    try {
      var url = '/api/content?session=' + encodeURIComponent(session)
        + '&type=' + encodeURIComponent(dtype)
        + '&name=' + encodeURIComponent(filename);
      const r = await api(url);
      const d = await r.json();
      rawContent = d.raw || '';
      renderedHtml = d.html || '';
      canEdit = !!d.editable;
      renderContent(false);
      updateBreadcrumb();
    } catch (e) {
      toast('Error: ' + e.message);
    }
  }

  function renderContent(editMode) {
    isEditing = editMode;
    if (editMode) {
      contentEl.innerHTML = '<div class="doc-container edit-area active">'
        + '<textarea id="editTextarea" spellcheck="false">' + esc(rawContent) + '</textarea>'
        + '</div>';
      updateTools();
      setTimeout(function() {
        var ta = document.getElementById('editTextarea');
        if (ta) ta.focus();
      }, 50);
    } else {
      contentEl.innerHTML = '<div class="doc-container read-area">' + renderedHtml + '</div>';
      updateTools();
    }
  }

  // Show/hide the action tools based on mode + permissions.
  function updateTools() {
    if (!currentFile) {
      toolbarEl.style.display = 'none';
      return;
    }
    toolbarEl.style.display = 'flex';
    var editing = isEditing;
    editBtn.hidden   = !editing && (!canEdit || !rawContent);
    saveBtn.hidden   = !editing;
    cancelBtn.hidden = !editing;
    copyBtn.hidden   = editing;
  }

  function updateBreadcrumb() {
    if (!currentFile) {
      breadcrumbEl.style.display = 'none';
      toolbarEl.style.display = 'none';
      return;
    }
    breadcrumbEl.style.display = 'block';
    breadcrumbEl.innerHTML =
      '<span>' + esc(currentSession || '') + '</span>'
      + '<span class="sep">/</span>'
      + '<span>' + esc(currentType || '') + '</span>'
      + '<span class="sep">/</span>'
      + '<span class="current">' + esc(currentFile || '') + '</span>';
    // Pin the breadcrumb's top to the sidebar 'Sessions' header so the
    // two are on the same horizontal line (measured, not guessed).
    const sHdr = document.querySelector('.sidebar-header');
    const mEl = document.querySelector('main');
    if (sHdr && mEl) {
      const top = sHdr.getBoundingClientRect().top
                - mEl.getBoundingClientRect().top + 4;
      breadcrumbEl.style.paddingTop = top + 'px';
    }
    updateTools();
  }

  // ── Edit / Save ─────────────────────────────
  function toggleEdit() {
    if (!currentFile) return;
    renderContent(true);
  }

  function cancelEdit() {
    renderContent(false);
    toast('Edit cancelled');
  }

  async function copyToClipboard() {
    if (!rawContent) { toast('Nothing to copy'); return; }
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(rawContent);
      } else {
        var ta = document.createElement('textarea');
        ta.value = rawContent;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
      }
      toast('Copied to clipboard');
    } catch (e) {
      toast('Copy failed: ' + e.message);
    }
  }

  async function saveEdit() {
    const ta = document.getElementById('editTextarea');
    if (!ta) return;
    const newContent = ta.value;

    try {
      const r = await fetch(base + '/api/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          session: currentSession,
          type: currentType,
          name: currentFile,
          content: newContent
        })
      });
      if (!r.ok) throw new Error(await r.text() || r.statusText);
      rawContent = newContent;
      const d = await r.json();
      renderedHtml = d.html || '';
      renderContent(false);
      toast('Saved');
      await loadTree();
    } catch (e) {
      toast('Save failed: ' + e.message);
    }
  }

  // Wire toolbar buttons
  editBtn.addEventListener('click', toggleEdit);
  saveBtn.addEventListener('click', saveEdit);
  cancelBtn.addEventListener('click', cancelEdit);
  copyBtn.addEventListener('click', copyToClipboard);

  // ── Keyboard ────────────────────────────────
  document.addEventListener('keydown', function(e) {
    var inField = document.activeElement && (
      document.activeElement.tagName === 'TEXTAREA' ||
      document.activeElement.tagName === 'INPUT');
    if (e.key === 'e' && !isEditing && currentFile && !inField) {
      e.preventDefault();
      toggleEdit();
    }
    if (e.key === 'Escape' && isEditing) {
      cancelEdit();
    }
    if ((e.metaKey || e.ctrlKey) && e.key === 's' && isEditing) {
      e.preventDefault();
      saveEdit();
    }
  });

  // ── Tree loading + auto-refresh ─────────────
  async function loadTree() {
    try {
      const r = await api('/api/tree');
      const d = await r.json();

      // If nothing selected, auto-open first file
      var autoOpen = null;
      if (!currentFile) {
        for (var i = 0; i < d.sessions.length; i++) {
          var sess = d.sessions[i];
          if (sess.documents.length > 0) {
            autoOpen = { session: sess.name, type: 'documents', file: sess.documents[0].name };
            break;
          }
          if (sess.comms.length > 0) {
            autoOpen = { session: sess.name, type: 'comms', file: sess.comms[0].name };
            break;
          }
        }
      }

      renderTree(d);

      if (autoOpen) {
        currentSession = autoOpen.session;
        currentType = autoOpen.type;
        currentFile = autoOpen.file;
        // Find the rendered element and open
        var el = document.querySelector(
          '.tree-session[data-session="' + esc(autoOpen.session) + '"] '
          + '.type-group[data-type="' + autoOpen.type + '"] '
          + '.file-item[data-filename="' + esc(autoOpen.file) + '"]'
        );
        await openFile(autoOpen.session, autoOpen.type, autoOpen.file, el);
      } else if (currentFile && !isEditing) {
        // Refresh current file content if changed externally
        var url = '/api/content?session=' + encodeURIComponent(currentSession)
          + '&type=' + encodeURIComponent(currentType)
          + '&name=' + encodeURIComponent(currentFile);
        const r2 = await api(url);
        const d2 = await r2.json();
        if (d2.raw !== rawContent) {
          rawContent = d2.raw || '';
          renderedHtml = d2.html || '';
          renderContent(false);
        }
      }
    } catch (e) {
      // Silent — don't spam console
    }
  }

  // ── Init ────────────────────────────────────
  loadTree();
  setInterval(loadTree, 5000);
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

    def authorized_path(self) -> tuple[str, str] | None:
        parsed = urlparse(self.path)
        parts = parsed.path.split("/")
        trusted_ips = getattr(self.server, "trusted_ips", set())
        if self.client_address[0] in trusted_ips:
            if len(parts) >= 2 and secrets.compare_digest(parts[1], self.token):
                parts = [""] + parts[2:]
            return "/" + "/".join(parts[1:]), parsed.query
        if len(parts) < 2 or not secrets.compare_digest(parts[1], self.token):
            return None
        return "/" + "/".join(parts[2:]), parsed.query

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        # Health check (no auth)
        if parsed.path == "/healthz":
            self.send_json({"ok": True, "service": APP, "version": VERSION})
            return

        auth = self.authorized_path()
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
        auth = self.authorized_path()
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, _ = auth

        if path == "/api/save":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
                raw_body = self.rfile.read(length)
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError):
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
