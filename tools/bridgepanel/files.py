"""BridgePanel filesystem operations — path resolution, safety, publish, markdown."""
from __future__ import annotations

import html as _html
import os
import re
from pathlib import Path
from urllib.parse import unquote

from .consts import sessions_dir as _sessions_dir_root

MD_EXTS = {".md", ".markdown", ".txt"}
IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".webp"}
VIDEO_EXTS = {".mp4", ".webm", ".mov", ".mkv"}
PDF_EXTS = {".pdf"}
_REL_OK = re.compile(r"^[A-Za-z0-9._/-]*$")


def data_home_path() -> Path:
    from .consts import data_home
    return data_home()


def sessions_dir() -> Path:
    return _sessions_dir_root()


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


# ── Markdown renderer ──────────────────────────────────────────

def inline_markup(text: str) -> str:
    escaped = _html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"(?<!\*)\*([^*]+)\*", r"<em>\1</em>", escaped)
    url_pattern = r'(?<!["\'=])(https?://[^\s<]+)'
    escaped = re.sub(
        url_pattern,
        lambda m: f'<a href="{_html.escape(m.group(1), quote=True)}" target="_blank" rel="noopener">{m.group(1)}</a>',
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
                    f'<pre class="code"><code data-lang="{_html.escape(code_lang)}">'
                    f"{_html.escape(chr(10).join(code_lines))}</code></pre>"
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
        if re.match(r"^-+$", line.strip()):
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
            f'<pre class="code"><code>{_html.escape(chr(10).join(code_lines))}</code></pre>'
        )
    return "\n".join(out)


# ── Host inbox (receive_dir) ───────────────────────────────────

def receive_dir() -> Path:
    raw = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
    if raw:
        return Path(raw)
    return Path.home() / ".bridgesessions" / "received"


def safe_relpath(value: str) -> str:
    """Relative inbox path. Empty = inbox root. Rejects abs/~ / .. / odd chars."""
    value = unquote(value or "").replace("\\", "/").strip()
    if not value or value in (".", "./"):
        return ""
    if value.startswith("/") or value.startswith("~") or ":" in value:
        return ""
    parts: list[str] = []
    for part in value.split("/"):
        if part in ("", "."):
            continue
        if part == ".." or not _REL_OK.match(part):
            return ""
        parts.append(part[:180])
    return "/".join(parts)[:400]


def resolve_receive_path(rel: str, *, must_exist: bool = False) -> Path | None:
    """Resolve rel under receive_dir. None if it would escape."""
    rel = safe_relpath(rel)
    base = receive_dir()
    try:
        base_r = base.resolve()
        target = (base_r / rel).resolve() if rel else base_r
        target.relative_to(base_r)
    except (ValueError, OSError):
        return None
    if must_exist and not target.exists():
        return None
    return target


def file_kind(name: str, is_dir: bool = False) -> str:
    if is_dir:
        return "dir"
    ext = Path(name).suffix.lower()
    if ext in MD_EXTS:
        return "md"
    if ext in IMAGE_EXTS:
        return "image"
    if ext in VIDEO_EXTS:
        return "video"
    if ext in PDF_EXTS:
        return "pdf"
    from .lang import is_code_name
    if is_code_name(name):
        return "code"
    return "file"


def list_receive_dir(rel: str = "") -> dict:
    """List one directory of the local inbox. Never walks outside receive_dir."""
    rel = safe_relpath(rel)
    target = resolve_receive_path(rel)
    if target is None:
        return {"ok": False, "error": "path escape", "path": rel, "items": []}
    if not target.exists():
        return {"ok": True, "path": rel, "count": 0, "items": []}
    if not target.is_dir():
        return {"ok": False, "error": "not a directory", "path": rel, "items": []}
    items: list[dict] = []
    try:
        for entry in os.scandir(target):
            if entry.name.startswith("."):
                continue
            try:
                is_dir = entry.is_dir(follow_symlinks=False)
                is_lnk = entry.is_symlink()
                if is_lnk:
                    continue
                st = entry.stat(follow_symlinks=False)
            except OSError:
                continue
            items.append({
                "name": entry.name,
                "dir": is_dir,
                "size": 0 if is_dir else int(st.st_size),
                "mtime": int(st.st_mtime),
                "kind": file_kind(entry.name, is_dir),
            })
    except OSError as exc:
        return {"ok": False, "error": str(exc), "path": rel, "items": []}
    items.sort(key=lambda x: (not x["dir"], x["name"].lower()))
    return {"ok": True, "path": rel, "count": len(items), "items": items}
