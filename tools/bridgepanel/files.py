"""BridgePanel filesystem operations — path resolution, safety, publish, markdown."""
from __future__ import annotations

import html as _html
import re
from pathlib import Path
from urllib.parse import unquote

from .consts import sessions_dir as _sessions_dir_root


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
