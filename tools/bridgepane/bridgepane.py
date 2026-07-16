#!/usr/bin/env python3
"""BridgePane: a tiny tailnet-only artifact and file bridge for BridgeSpace.

No third-party dependencies. The browser UI is self-contained and auto-refreshes.
"""
from __future__ import annotations

import argparse
import html
import json
import mimetypes
import os
import re
import secrets
import shutil
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from email.parser import BytesParser
from email.policy import default as email_policy
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

APP = "BridgePane"
DEFAULT_BIND = os.environ.get("BRIDGEPANE_BIND", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("BRIDGEPANE_PORT", "9770"))
MAX_UPLOAD = 100 * 1024 * 1024
TEXT_SUFFIXES = {
    ".md", ".markdown", ".txt", ".log", ".json", ".yaml", ".yml", ".toml",
    ".py", ".js", ".ts", ".tsx", ".jsx", ".css", ".html", ".htm", ".sh",
    ".bash", ".zsh", ".c", ".cc", ".cpp", ".h", ".hpp", ".rs", ".go",
    ".java", ".sql", ".xml", ".csv", ".diff", ".patch",
}
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp"}


def data_home() -> Path:
    return Path(os.environ.get("BRIDGEPANE_HOME", Path.home() / ".local/share/bridgepane"))


def config_home() -> Path:
    return Path(os.environ.get("BRIDGEPANE_CONFIG", Path.home() / ".config/bridgepane"))


def artifacts_dir() -> Path:
    return data_home() / "artifacts"


def inbox_dir() -> Path:
    return data_home() / "inbox"


def token_path() -> Path:
    return config_home() / "token"


def state_path() -> Path:
    return data_home() / "state.json"


def safe_name(value: str) -> str:
    value = unquote(value or "").replace("\\", "/").split("/")[-1]
    value = re.sub(r"[^A-Za-z0-9._()\- ]+", "_", value).strip(" .")
    return value[:180] or "untitled"


def ensure_dirs() -> str:
    config_home().mkdir(parents=True, exist_ok=True, mode=0o700)
    artifacts_dir().mkdir(parents=True, exist_ok=True, mode=0o700)
    inbox_dir().mkdir(parents=True, exist_ok=True, mode=0o700)
    tp = token_path()
    if not tp.exists():
        tp.write_text(secrets.token_urlsafe(24), encoding="utf-8")
    tp.chmod(0o600)
    token = tp.read_text(encoding="utf-8").strip()
    if len(token) < 24:
        raise RuntimeError("BridgePane token is too short")
    welcome = artifacts_dir() / "WELCOME.md"
    if not welcome.exists():
        welcome.write_text(
            "# BridgePane is live\n\n"
            "This right-hand pane is now the durable output surface for long agent work.\n\n"
            "- Long reports appear here instead of flooding the terminal.\n"
            "- The page refreshes automatically when an artifact changes.\n"
            "- Drop files into this page to upload them to the remote host.\n"
            "- Download generated artifacts directly from the browser.\n\n"
            "## Operating rule\n\n"
            "Chat stays terse. Detailed work is published here.\n",
            encoding="utf-8",
        )
    if not state_path().exists():
        set_current("WELCOME.md")
    return token


def read_state() -> dict:
    try:
        return json.loads(state_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"current": "WELCOME.md", "updated": 0}


def set_current(name: str) -> None:
    data_home().mkdir(parents=True, exist_ok=True)
    tmp = state_path().with_suffix(".tmp")
    tmp.write_text(json.dumps({"current": safe_name(name), "updated": time.time()}), encoding="utf-8")
    os.replace(tmp, state_path())


def unique_destination(directory: Path, name: str) -> Path:
    name = safe_name(name)
    target = directory / name
    if not target.exists():
        return target
    stem, suffix = target.stem, target.suffix
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return directory / f"{stem}-{stamp}{suffix}"


def publish(source: Path, title: str | None = None) -> Path:
    ensure_dirs()
    source = source.expanduser().resolve(strict=True)
    if not source.is_file():
        raise ValueError(f"Not a file: {source}")
    name = safe_name(title or source.name)
    if title and not Path(name).suffix and source.suffix:
        name += source.suffix
    target = unique_destination(artifacts_dir(), name)
    shutil.copy2(source, target)
    set_current(target.name)
    return target


def publish_text(text: str, title: str) -> Path:
    ensure_dirs()
    name = safe_name(title)
    if not Path(name).suffix:
        name += ".md"
    target = unique_destination(artifacts_dir(), name)
    target.write_text(text, encoding="utf-8")
    set_current(target.name)
    return target


def resolve_item(kind: str, name: str) -> Path | None:
    base = inbox_dir() if kind == "inbox" else artifacts_dir()
    candidate = base / safe_name(name)
    try:
        candidate.resolve().relative_to(base.resolve())
    except (ValueError, OSError):
        return None
    return candidate if candidate.is_file() else None


def inline_markup(text: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"(?<!\*)\*([^*]+)\*", r"<em>\1</em>", escaped)
    url_pattern = r"(?<![\"'=])(https?://[^\s<]+)"
    escaped = re.sub(url_pattern, lambda m: f'<a href="{html.escape(m.group(1), quote=True)}" target="_blank" rel="noopener">{m.group(1)}</a>', escaped)
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
            flush_paragraph(); close_list()
            if in_code:
                out.append(f'<pre class="code"><code data-lang="{html.escape(code_lang)}">{html.escape(chr(10).join(code_lines))}</code></pre>')
                code_lines = []; code_lang = ""; in_code = False
            else:
                in_code = True; code_lang = line[3:].strip()[:30]
            i += 1; continue
        if in_code:
            code_lines.append(line); i += 1; continue
        if not line.strip():
            flush_paragraph(); close_list(); i += 1; continue
        heading = re.match(r"^(#{1,6})\s+(.+)$", line)
        if heading:
            flush_paragraph(); close_list()
            level = len(heading.group(1)); body = inline_markup(heading.group(2))
            out.append(f"<h{level}>{body}</h{level}>"); i += 1; continue
        if line.startswith("> "):
            flush_paragraph(); close_list(); out.append(f"<blockquote>{inline_markup(line[2:])}</blockquote>"); i += 1; continue
        unordered = re.match(r"^\s*[-*+]\s+(.+)$", line)
        ordered = re.match(r"^\s*\d+[.)]\s+(.+)$", line)
        if unordered or ordered:
            flush_paragraph()
            needed = "ul" if unordered else "ol"
            if list_kind != needed:
                close_list(); out.append(f"<{needed}>"); list_kind = needed
            match = unordered or ordered
            assert match is not None
            out.append(f"<li>{inline_markup(match.group(1))}</li>"); i += 1; continue
        if "|" in line and i + 1 < len(lines) and re.match(r"^\s*\|?\s*:?-+", lines[i + 1]):
            flush_paragraph(); close_list()
            headers = [x.strip() for x in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < len(lines) and "|" in lines[i] and lines[i].strip():
                rows.append([x.strip() for x in lines[i].strip().strip("|").split("|")]); i += 1
            table = ["<div class=\"table-wrap\"><table><thead><tr>"]
            table.extend(f"<th>{inline_markup(x)}</th>" for x in headers)
            table.append("</tr></thead><tbody>")
            for row in rows:
                table.append("<tr>"); table.extend(f"<td>{inline_markup(x)}</td>" for x in row); table.append("</tr>")
            table.append("</tbody></table></div>"); out.append("".join(table)); continue
        if re.match(r"^---+$", line.strip()):
            flush_paragraph(); close_list(); out.append("<hr>"); i += 1; continue
        paragraph.append(line); i += 1
    flush_paragraph(); close_list()
    if in_code:
        out.append(f'<pre class="code"><code>{html.escape(chr(10).join(code_lines))}</code></pre>')
    return "\n".join(out)


INDEX_HTML = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BridgePane</title>
<style>
:root{--ivory:#FAF9F5;--white:#FFF;--slate:#141413;--clay:#D97757;--olive:#788C5D;--rust:#B04A3F;--oat:#E3DACC;--g150:#F0EEE6;--g300:#D1CFC5;--g500:#87867F;--g700:#3D3D3A;--serif:ui-serif,Georgia,"Times New Roman",serif;--sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;--mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace}
*{box-sizing:border-box}html,body{margin:0;height:100%;background:var(--ivory);color:var(--g700);font-family:var(--sans)}button,input,textarea{font:inherit}.shell{height:100%;display:grid;grid-template-rows:auto 1fr}.top{height:62px;background:var(--slate);color:#f7f4ec;display:flex;align-items:center;gap:16px;padding:0 22px;border-bottom:3px solid var(--clay)}.brand{font-family:var(--serif);font-size:23px}.live{font:10px var(--mono);letter-spacing:.08em;color:#dce5d2;border:1px solid #596c44;border-radius:999px;padding:3px 8px}.spacer{flex:1}.top button{background:transparent;color:#eee;border:1px solid #555;border-radius:7px;padding:7px 10px;cursor:pointer}.layout{min-height:0;display:grid;grid-template-columns:280px minmax(0,1fr)}aside{min-height:0;background:var(--white);border-right:1px solid var(--g300);display:flex;flex-direction:column}.tabs{display:flex;border-bottom:1px solid var(--g300)}.tabs button{flex:1;border:0;background:transparent;padding:13px 6px;font:11px var(--mono);text-transform:uppercase;color:var(--g500);cursor:pointer}.tabs button.on{color:var(--clay);box-shadow:inset 0 -2px var(--clay)}.list{overflow:auto;padding:10px}.item{width:100%;text-align:left;border:0;border-radius:8px;background:transparent;padding:10px;cursor:pointer;margin-bottom:3px}.item:hover,.item.on{background:var(--g150)}.item strong{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:13px}.meta{display:block;margin-top:3px;color:var(--g500);font:10px var(--mono)}.upload{margin:auto 12px 12px;border:1.5px dashed var(--g300);border-radius:10px;padding:14px;text-align:center;color:var(--g500);font-size:12px;cursor:pointer}.upload.hot{border-color:var(--clay);background:#fff8f4}.upload input{display:none}main{min-width:0;min-height:0;overflow:auto}.doc{max-width:920px;margin:0 auto;padding:46px 42px 100px}.doc-head{display:flex;align-items:flex-start;gap:16px;padding-bottom:20px;border-bottom:1px solid var(--g300);margin-bottom:28px}.doc-title{font:500 34px var(--serif);letter-spacing:-.01em;color:var(--slate);word-break:break-word}.doc-actions{margin-left:auto;display:flex;gap:8px}.doc-actions a,.doc-actions button{white-space:nowrap;text-decoration:none;color:var(--g700);background:var(--white);border:1px solid var(--g300);border-radius:8px;padding:7px 10px;cursor:pointer;font:11px var(--mono)}.render h1,.render h2,.render h3,.render h4{font-family:var(--serif);font-weight:500;color:var(--slate);letter-spacing:-.01em;scroll-margin-top:20px}.render h1{font-size:32px;margin:0 0 18px}.render h2{font-size:24px;margin:42px 0 12px}.render h3{font-size:19px;margin:30px 0 10px}.render p{line-height:1.68;margin:0 0 16px}.render ul,.render ol{line-height:1.65;padding-left:25px;margin:0 0 18px}.render li{margin:4px 0}.render code{font-family:var(--mono);font-size:.88em;background:var(--g150);padding:2px 5px;border-radius:4px}.render pre.code{background:var(--slate);color:#e8e6df;border-radius:12px;padding:18px;overflow:auto;line-height:1.5}.render pre.code code{background:none;padding:0}.render blockquote{border-left:3px solid var(--clay);background:#fff7f2;margin:18px 0;padding:13px 16px}.render a{color:var(--clay)}.table-wrap{overflow:auto;border:1px solid var(--g300);border-radius:10px;margin:18px 0}.render table{border-collapse:collapse;width:100%;background:var(--white)}.render th{font:11px var(--mono);text-transform:uppercase;text-align:left;background:var(--g150);color:var(--g500);padding:10px 12px}.render td{padding:10px 12px;border-top:1px solid var(--g150);font-size:14px}.raw{white-space:pre-wrap;word-break:break-word;background:var(--white);border:1px solid var(--g300);border-radius:10px;padding:18px;font:13px/1.55 var(--mono)}.preview{max-width:100%;max-height:75vh;border-radius:10px;border:1px solid var(--g300)}.empty{margin:80px auto;text-align:center;color:var(--g500)}.toast{position:fixed;right:20px;bottom:20px;background:var(--slate);color:white;padding:11px 14px;border-radius:8px;font-size:12px;opacity:0;transform:translateY(8px);transition:.2s;pointer-events:none}.toast.show{opacity:1;transform:none}.paste{position:fixed;inset:0;background:#0008;display:none;align-items:center;justify-content:center}.paste.on{display:flex}.paste-card{width:min(620px,90vw);background:var(--white);padding:22px;border-radius:12px}.paste textarea{width:100%;height:260px;margin:12px 0;border:1px solid var(--g300);border-radius:8px;padding:10px;font-family:var(--mono)}.paste input{width:100%;border:1px solid var(--g300);border-radius:8px;padding:9px}.paste-row{display:flex;justify-content:flex-end;gap:8px}.paste-row button{border:1px solid var(--g300);background:var(--white);border-radius:7px;padding:8px 12px}.paste-row .primary{background:var(--clay);border-color:var(--clay);color:white}@media(max-width:760px){.layout{grid-template-columns:1fr}.layout aside{display:none}.doc{padding:30px 20px 80px}.doc-head{display:block}.doc-actions{margin-top:14px}.doc-title{font-size:28px}}
</style></head><body><div class="shell">
<header class="top"><span class="brand">BridgePane</span><span class="live">TAILNET LIVE</span><span class="spacer"></span><button id="pasteBtn">Paste text</button><button id="refreshBtn">Refresh</button></header>
<div class="layout"><aside><div class="tabs"><button class="on" data-kind="artifact">Artifacts</button><button data-kind="inbox">Inbox</button></div><div class="list" id="list"></div><label class="upload" id="drop">Drop or choose files<input id="file" type="file" multiple></label></aside>
<main><article class="doc"><div id="view" class="empty">Loading BridgePane…</div></article></main></div></div>
<div class="paste" id="paste"><div class="paste-card"><h2 style="font-family:var(--serif);font-weight:500;margin:0">Send text to remote inbox</h2><input id="pasteName" value="note.md" aria-label="File name"><textarea id="pasteText" placeholder="Paste prompt, logs, notes, or content here…"></textarea><div class="paste-row"><button id="pasteCancel">Cancel</button><button class="primary" id="pasteSend">Upload</button></div></div></div><div class="toast" id="toast"></div>
<script>
(()=>{const base=location.pathname.replace(/\/$/,''),list=document.getElementById('list'),view=document.getElementById('view'),toast=document.getElementById('toast');let kind='artifact',selected='',lastVersion='',currentName='';const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));function say(s){toast.textContent=s;toast.classList.add('show');setTimeout(()=>toast.classList.remove('show'),1800)}async function api(path,opt){const r=await fetch(base+path,opt);if(!r.ok)throw new Error(await r.text()||r.statusText);return r}async function loadList(force=false){try{const d=await (await api('/api/list')).json();const arr=kind==='artifact'?d.artifacts:d.inbox;list.innerHTML=arr.map(x=>`<button class="item ${selected===x.name?'on':''}" data-name="${esc(x.name)}"><strong>${esc(x.name)}</strong><span class="meta">${esc(x.size_human)} · ${esc(x.modified_human)}</span></button>`).join('')||'<div class="empty">Nothing here yet.</div>';list.querySelectorAll('.item').forEach(b=>b.onclick=()=>open(b.dataset.name,true));if(kind==='artifact'&&d.current&&(!selected||(d.current!==currentName&&selected===currentName))){currentName=d.current;selected=d.current;await open(selected,true);return}currentName=d.current||currentName;if(force&&selected)await open(selected,true);const chosen=arr.find(x=>x.name===selected);if(chosen){const v=kind+':'+chosen.name+':'+chosen.modified;if(lastVersion&&v!==lastVersion)await open(selected,true);lastVersion=v}}catch(e){view.innerHTML='<div class="empty">'+esc(e.message)+'</div>'}}async function open(name,force){selected=name;const d=await (await api('/api/content?kind='+kind+'&name='+encodeURIComponent(name))).json();const raw=base+'/raw?kind='+kind+'&name='+encodeURIComponent(name),down=base+'/download?kind='+kind+'&name='+encodeURIComponent(name);let body=d.html?`<div class="render">${d.html}</div>`:d.image?`<img class="preview" src="${raw}" alt="${esc(name)}">`:d.pdf?`<iframe sandbox src="${raw}" style="width:100%;height:75vh;border:1px solid #ddd;border-radius:10px"></iframe>`:`<div class="raw">Preview unavailable. Download the file to inspect it.</div>`;view.className='';view.innerHTML=`<div class="doc-head"><div><div style="font:11px var(--mono);color:var(--g500);text-transform:uppercase">${kind}</div><div class="doc-title">${esc(name)}</div><div class="meta">${esc(d.remote_path)}</div></div><div class="doc-actions"><button id="copyPath">Copy path</button><a href="${down}">Download</a></div></div>${body}`;document.getElementById('copyPath').onclick=()=>navigator.clipboard.writeText(d.remote_path).then(()=>say('Remote path copied'));await loadList(false)}async function upload(files){for(const f of files){const fd=new FormData();fd.append('file',f,f.name);await api('/upload',{method:'POST',body:fd})}say(files.length+' file(s) uploaded');kind='inbox';selected='';document.querySelectorAll('.tabs button').forEach(b=>b.classList.toggle('on',b.dataset.kind===kind));await loadList()}document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{kind=b.dataset.kind;selected='';document.querySelectorAll('.tabs button').forEach(x=>x.classList.toggle('on',x===b));loadList()});document.getElementById('refreshBtn').onclick=()=>loadList(true);const input=document.getElementById('file'),drop=document.getElementById('drop');input.onchange=()=>upload(input.files);['dragenter','dragover'].forEach(e=>drop.addEventListener(e,x=>{x.preventDefault();drop.classList.add('hot')}));['dragleave','drop'].forEach(e=>drop.addEventListener(e,x=>{x.preventDefault();drop.classList.remove('hot')}));drop.ondrop=e=>upload(e.dataTransfer.files);const modal=document.getElementById('paste');document.getElementById('pasteBtn').onclick=()=>modal.classList.add('on');document.getElementById('pasteCancel').onclick=()=>modal.classList.remove('on');document.getElementById('pasteSend').onclick=async()=>{const fd=new FormData();fd.append('name',document.getElementById('pasteName').value);fd.append('text',document.getElementById('pasteText').value);await api('/upload-text',{method:'POST',body:fd});modal.classList.remove('on');kind='inbox';selected='';say('Text uploaded');loadList()};loadList();setInterval(()=>loadList(false),2500)})();
</script></body></html>'''


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{size} B"


def list_items(directory: Path) -> list[dict]:
    result = []
    for path in directory.iterdir():
        if not path.is_file() or path.name.startswith("."):
            continue
        stat = path.stat()
        result.append({
            "name": path.name,
            "size": stat.st_size,
            "size_human": human_size(stat.st_size),
            "modified": stat.st_mtime,
            "modified_human": time.strftime("%b %d, %H:%M", time.localtime(stat.st_mtime)),
        })
    return sorted(result, key=lambda x: x["modified"], reverse=True)


class BridgePaneHandler(BaseHTTPRequestHandler):
    server_version = "BridgePane/1.0"

    @property
    def token(self) -> str:
        return self.server.bridgepane_token  # type: ignore[attr-defined]

    def log_message(self, format: str, *args) -> None:
        safe_args = tuple(str(value).replace(self.token, "<token>") for value in args)
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), format % safe_args))

    def security_headers(self, content_type: str, length: int | None = None) -> None:
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; img-src 'self' data:; frame-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'")
        if length is not None:
            self.send_header("Content-Length", str(length))

    def send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.security_headers(content_type, len(body))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, payload: dict, status: int = 200) -> None:
        self.send_bytes(json.dumps(payload).encode("utf-8"), "application/json; charset=utf-8", status)

    def reject(self, status: int, message: str) -> None:
        self.send_bytes(message.encode("utf-8"), "text/plain; charset=utf-8", status)

    def authorized_path(self) -> tuple[str, str] | None:
        parsed = urlparse(self.path)
        parts = parsed.path.split("/")
        trusted_ips = getattr(self.server, "trusted_ips", set())
        if self.client_address[0] in trusted_ips:
            # Tailscale authenticates this source address with WireGuard.
            # A trusted desktop gets a short root URL; tokenized URLs still work.
            if len(parts) >= 2 and secrets.compare_digest(parts[1], self.token):
                parts = [""] + parts[2:]
            return "/" + "/".join(parts[1:]), parsed.query
        if len(parts) < 2 or not secrets.compare_digest(parts[1], self.token):
            return None
        rest = "/" + "/".join(parts[2:])
        return rest, parsed.query

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/healthz":
            self.send_json({"ok": True, "service": APP})
            return
        auth = self.authorized_path()
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, query = auth
        params = parse_qs(query)
        if path in ("/", ""):
            self.send_bytes(INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/api/list":
            state = read_state()
            self.send_json({"current": state.get("current"), "artifacts": list_items(artifacts_dir()), "inbox": list_items(inbox_dir())})
        elif path == "/api/content":
            kind = params.get("kind", ["artifact"])[0]
            name = params.get("name", [""])[0]
            item = resolve_item(kind, name)
            if not item:
                self.reject(HTTPStatus.NOT_FOUND, "File not found")
                return
            suffix = item.suffix.lower()
            payload = {"name": item.name, "remote_path": str(item), "html": "", "image": suffix in IMAGE_SUFFIXES, "pdf": suffix == ".pdf"}
            if suffix in TEXT_SUFFIXES or (not suffix and item.stat().st_size < 2 * 1024 * 1024):
                raw = item.read_text(encoding="utf-8", errors="replace")
                if suffix in (".md", ".markdown", ""):
                    payload["html"] = markdown_to_html(raw)
                else:
                    payload["html"] = f'<pre class="raw">{html.escape(raw)}</pre>'
            self.send_json(payload)
        elif path in ("/raw", "/download"):
            kind = params.get("kind", ["artifact"])[0]
            name = params.get("name", [""])[0]
            item = resolve_item(kind, name)
            if not item:
                self.reject(HTTPStatus.NOT_FOUND, "File not found")
                return
            suffix = item.suffix.lower()
            if path == "/raw" and suffix not in IMAGE_SUFFIXES and suffix != ".pdf":
                mime = "text/plain; charset=utf-8"
            else:
                mime = mimetypes.guess_type(item.name)[0] or "application/octet-stream"
            body = item.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.security_headers(mime, len(body))
            disposition = "attachment" if path == "/download" else "inline"
            self.send_header("Content-Disposition", f"{disposition}; filename*=UTF-8''{quote(item.name)}")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")

    def parse_form(self) -> dict[str, tuple[str | None, bytes]] | None:
        try:
            length = int(self.headers.get("Content-Length", "0") or 0)
        except ValueError:
            self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
            return None
        if length <= 0 or length > MAX_UPLOAD:
            self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Upload is empty or too large")
            return None
        content_type = self.headers.get("Content-Type", "")
        if not content_type.startswith("multipart/form-data"):
            self.reject(HTTPStatus.BAD_REQUEST, "Expected multipart/form-data")
            return None
        raw = self.rfile.read(length)
        envelope = (
            f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode("utf-8")
            + raw
        )
        message = BytesParser(policy=email_policy).parsebytes(envelope)
        result: dict[str, tuple[str | None, bytes]] = {}
        for part in message.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if not name:
                continue
            payload = part.get_payload(decode=True)
            if not isinstance(payload, bytes):
                payload = str(payload or "").encode("utf-8")
            result[str(name)] = (part.get_filename(), payload)
        return result

    def do_POST(self) -> None:
        auth = self.authorized_path()
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, _ = auth
        if path not in ("/upload", "/upload-text"):
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        form = self.parse_form()
        if form is None:
            return
        if path == "/upload":
            field = form.get("file")
            if field is None:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing file")
                return
            filename, payload = field
            target = unique_destination(inbox_dir(), filename or "upload.bin")
            target.write_bytes(payload)
        else:
            name = form.get("name", (None, b"note.md"))[1].decode("utf-8", errors="replace")
            text = form.get("text", (None, b""))[1].decode("utf-8", errors="replace")
            if len(text.encode("utf-8")) > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Text too large")
                return
            target = unique_destination(inbox_dir(), name)
            target.write_text(text, encoding="utf-8")
        self.send_json({"ok": True, "name": target.name, "remote_path": str(target)}, HTTPStatus.CREATED)


def serve(bind: str, port: int) -> None:
    token = ensure_dirs()
    server = ThreadingHTTPServer((bind, port), BridgePaneHandler)
    server.bridgepane_token = token  # type: ignore[attr-defined]
    trusted_ips = {
        value.strip()
        for value in os.environ.get("BRIDGEPANE_TRUSTED_IPS", "").split(",")
        if value.strip()
    }
    setattr(server, "trusted_ips", trusted_ips)
    print(f"BridgePane listening on http://{bind}:{port}/{token}/", flush=True)
    if trusted_ips:
        print(f"Trusted Tailscale clients: {', '.join(sorted(trusted_ips))}", flush=True)
    server.serve_forever()


def main() -> int:
    parser = argparse.ArgumentParser(description="BridgeSpace-friendly remote artifact and file pane")
    sub = parser.add_subparsers(dest="command", required=True)
    init_p = sub.add_parser("init", help="create storage and token")
    init_p.add_argument("--bind", default=DEFAULT_BIND)
    init_p.add_argument("--port", type=int, default=DEFAULT_PORT)
    serve_p = sub.add_parser("serve", help="start the web service")
    serve_p.add_argument("--bind", default=DEFAULT_BIND)
    serve_p.add_argument("--port", type=int, default=DEFAULT_PORT)
    pub_p = sub.add_parser("publish", help="copy a file into the artifact pane")
    pub_p.add_argument("source", type=Path)
    pub_p.add_argument("--title")
    note_p = sub.add_parser("note", help="publish stdin as a Markdown artifact")
    note_p.add_argument("--title", default="Agent report.md")
    url_p = sub.add_parser("url", help="print the private pane URL")
    url_p.add_argument("--bind", default=DEFAULT_BIND)
    url_p.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()
    if args.command == "serve":
        serve(args.bind, args.port)
    elif args.command == "publish":
        target = publish(args.source, args.title); print(target)
    elif args.command == "note":
        target = publish_text(sys.stdin.read(), args.title); print(target)
    else:
        token = ensure_dirs()
        print(f"http://{args.bind}:{args.port}/{token}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
