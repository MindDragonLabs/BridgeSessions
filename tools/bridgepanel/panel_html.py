"""BridgePanel — HTML/CSS/JS template (host inbox file manager)."""
from __future__ import annotations

# ── SVG Favicon ────────────────────────────────────────────────

FAVICON_SVG = b"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <rect x="3" y="3" width="26" height="26" rx="7" fill="#1B1F26" stroke="rgba(0,0,0,0.2)" stroke-width="1"/>
  <rect x="8" y="9" width="11" height="2.5" rx="1.25" fill="#E9EDF2"/>
  <rect x="8" y="14.75" width="16" height="2.5" rx="1.25" fill="#9BA4B0"/>
  <rect x="8" y="20.5" width="9" height="2.5" rx="1.25" fill="#3FA9E0"/>
</svg>"""

# ── HTML ───────────────────────────────────────────────────────

INDEX_HTML = r'''<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bridge Panel</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='7' fill='%233FA9E0'/%3E%3Ctext x='16' y='22' font-size='17' font-family='sans-serif' text-anchor='middle' fill='white'%3EB%3C/text%3E%3C/svg%3E">
<style>
  :root {
    --bg: #14171C; --surface: #1B1F26; --surface2: #232833;
    --border: #2A303B;
    --text: #E9EDF2; --muted: #9BA4B0; --faint: #66707E;
    --accent: #3FA9E0; --accent-soft: rgba(63,169,224,0.13);
    --ok: #35C77D; --warn: #E2A33D; --danger: #E26D6D;
    --shadow: 0 1px 2px rgba(0,0,0,0.25), 0 4px 14px rgba(0,0,0,0.18);
    --sans: 'Inter', -apple-system, system-ui, 'Segoe UI', Roboto, sans-serif;
    --mono: 'JetBrains Mono', ui-monospace, 'SF Mono', Menlo, Consolas, monospace;
  }
  [data-theme="light"] {
    --bg: #F6F7F9; --surface: #FFFFFF; --surface2: #F0F2F5;
    --border: #E4E8EE;
    --text: #1B2129; --muted: #5A6472; --faint: #98A1AD;
    --accent: #2A8BC0; --accent-soft: rgba(42,139,192,0.12);
    --ok: #1FA864; --warn: #B9831F; --danger: #C94747;
    --shadow: 0 1px 2px rgba(15,23,42,0.06), 0 4px 14px rgba(15,23,42,0.08);
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body { height: 100%; }
  body {
    background: var(--bg); color: var(--text);
    font-family: var(--sans); font-size: 14px; line-height: 1.5;
    display: flex; flex-direction: column; overflow: hidden;
    -webkit-font-smoothing: antialiased; transition: background .2s ease, color .2s ease;
  }
  button, input, textarea, select { font: inherit; color: inherit; }
  ::selection { background: var(--accent-soft); }

  header { display: flex; align-items: center; gap: 14px; padding: 0 16px; height: 52px; flex-shrink: 0; background: var(--surface); border-bottom: 1px solid var(--border); }
  .brand { display: flex; align-items: center; gap: 9px; }
  .brand .mark { width: 20px; height: 20px; border-radius: 5px; background: var(--accent); color: #fff; display: grid; place-items: center; font-family: var(--mono); font-weight: 700; font-size: 12px; }
  .brand .name { font-weight: 650; font-size: 14.5px; letter-spacing: -0.01em; }
  .brand .sub { font-size: 11.5px; color: var(--faint); }
  .search { flex: 1; max-width: 320px; position: relative; }
  .search input { width: 100%; height: 32px; background: var(--surface2); border: 1px solid var(--border); border-radius: 8px; padding: 0 10px 0 30px; font-size: 13px; color: var(--text); outline: none; }
  .search input:focus { border-color: var(--accent); }
  .search .icon { position: absolute; left: 10px; top: 50%; transform: translateY(-50%); color: var(--faint); }
  .file-search { flex: none; margin: 0 10px 8px; }
  .file-search input { width: 100%; height: 32px; background: var(--surface2); border: 1px solid var(--border); border-radius: 8px; padding: 0 10px; font-size: 13px; color: var(--text); outline: none; }
  .file-search input:focus { border-color: var(--accent); }
  .path-jump { flex: none; margin: 0 10px 8px; }
  .path-jump input { width: 100%; height: 32px; background: var(--surface2); border: 1px dashed var(--border); border-radius: 8px; padding: 0 10px; font-size: 13px; font-family: var(--mono); color: var(--text); outline: none; }
  .path-jump input:focus { border-color: var(--accent); border-style: solid; }
  .path-jump input::placeholder { color: var(--faint); font-family: var(--sans); }
  .hdr-right { margin-left: auto; display: flex; align-items: center; gap: 12px; }
  .summary { font-size: 12.5px; color: var(--muted); }
  .summary b { color: var(--text); font-weight: 600; }
  .theme-toggle { width: 32px; height: 32px; border-radius: 8px; background: transparent; border: 1px solid var(--border); color: var(--muted); cursor: pointer; font-size: 15px; }
  .theme-toggle:hover { background: var(--surface2); color: var(--text); }
  .avatar { width: 28px; height: 28px; border-radius: 50%; background: var(--accent-soft); color: var(--accent); display: grid; place-items: center; font-weight: 600; font-size: 12px; }

  .shell { flex: 1; display: grid; grid-template-columns: var(--w-hosts, 240px) 6px var(--w-files, 280px) 6px minmax(240px, 1fr); min-height: 0; }
  .splitter { width: 6px; cursor: col-resize; background: var(--border); }
  .splitter:hover, .splitter.dragging { background: var(--accent); }
  body.resizing, body.resizing * { cursor: col-resize !important; user-select: none !important; }
  .col { min-height: 0; overflow: hidden; background: var(--surface); display: flex; flex-direction: column; }
  .col.machines { border-right: 1px solid var(--border); }
  .col.files { border-right: 1px solid var(--border); }
  .col-scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .col-head { padding: 10px 14px 8px; font-size: 11px; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; color: var(--faint); display: flex; align-items: center; justify-content: space-between; gap: 8px; }
  .col-head .count { font-family: var(--mono); font-weight: 400; letter-spacing: 0; }
  .icon-btn { width: 22px; height: 22px; border-radius: 6px; background: var(--surface2); color: var(--muted); border: 1px solid var(--border); cursor: pointer; font-size: 13px; line-height: 1; }
  .icon-btn:hover { border-color: var(--muted); color: var(--text); }

  .mrow { display: flex; align-items: center; gap: 10px; padding: 9px 13px; cursor: pointer; border-left: 2px solid transparent; }
  .mrow:hover { background: var(--surface2); }
  .mrow.active { background: var(--accent-soft); border-left-color: var(--accent); }
  .mrow.offline { opacity: 0.62; }
  .mrow .dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; background: var(--faint); }
  .mrow .dot.up { background: var(--ok); }
  .mrow .dot.you { background: var(--accent); }
  .mrow .dot.offline { background: var(--faint); }
  .mrow .dot.stale { background: var(--warn); }
  .mrow .mi { flex: 1; min-width: 0; }
  .mrow .mn { font-size: 13.5px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mrow .ma { font-family: var(--mono); font-size: 10px; color: var(--faint); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mrow .you { font-size: 10px; color: var(--accent); text-transform: uppercase; letter-spacing: .06em; }

  .filters { display: flex; gap: 4px; padding: 0 10px 8px; flex-wrap: wrap; }
  .chip { height: 22px; padding: 0 8px; border-radius: 999px; border: 1px solid var(--border); background: transparent; color: var(--muted); font-size: 11px; cursor: pointer; }
  .chip:hover { color: var(--text); }
  .chip.active { background: var(--accent-soft); color: var(--accent); border-color: var(--accent); }
  .pathrow { display: flex; align-items: center; gap: 6px; padding: 0 12px 8px; font-family: var(--mono); font-size: 11px; color: var(--faint); }
  .pathrow button { border: none; background: none; color: var(--accent); cursor: pointer; font: inherit; }
  .pathrow button:hover { text-decoration: underline; }
  .volrow { display: flex; gap: 4px; padding: 0 10px 8px; flex-wrap: wrap; align-items: center; }
  .volrow .chip .free { color: var(--faint); margin-left: 4px; font-size: 10px; }
  .volrow .chip.other { border-style: dashed; }
  .viewbar { display: flex; gap: 4px; padding: 0 10px 6px; align-items: center; }
  .viewbar .icon-btn.active { background: var(--accent-soft); color: var(--accent); border-color: var(--accent); }
  .tree { max-height: 36%; overflow: auto; border-bottom: 1px solid var(--border); padding: 2px 6px 8px; font-size: 12px; }
  .tnode { display: flex; align-items: center; gap: 4px; padding: 3px 4px; cursor: pointer; border-radius: 4px; color: var(--muted); }
  .tnode:hover { background: var(--surface2); color: var(--text); }
  .tnode.open > .tn { color: var(--text); }
  .tnode.here { background: var(--accent-soft); color: var(--text); }
  .tnode .tw { width: 14px; text-align: center; color: var(--faint); flex-shrink: 0; }
  .tnode .tn { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(104px, 1fr)); gap: 8px; padding: 8px 10px 12px; }
  .gitem { display: flex; flex-direction: column; gap: 4px; padding: 6px; border: 1px solid var(--border); border-radius: 8px; cursor: pointer; background: var(--surface2); min-width: 0; }
  .gitem:hover, .gitem.active { border-color: var(--accent); }
  .gitem img { width: 100%; height: 72px; object-fit: cover; border-radius: 5px; background: var(--surface); }
  .gitem .fn { font-size: 11px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .gitem.kind-dir .ph { height: 72px; border-radius: 5px; background: var(--accent-soft); }
  .dest-hint { padding: 0 12px 8px; font-size: 11px; color: var(--faint); }
  .dest-hint b { color: var(--muted); font-weight: 600; }
  .drop.hidden { display: none; }

  .fitem { display: flex; align-items: center; gap: 8px; padding: 7px 12px; cursor: pointer; font-size: 13px; color: var(--muted); border-left: 2px solid transparent; }
  .fitem:hover { background: var(--surface2); color: var(--text); }
  .fitem.active { background: var(--accent-soft); color: var(--text); border-left-color: var(--accent); }
  .fitem .ic { width: 16px; height: 16px; border-radius: 3px; flex-shrink: 0; background: var(--surface2); border: 1px solid var(--border); }
  .fitem.kind-dir .ic { background: var(--accent-soft); border-color: transparent; }
  .fitem.kind-md .ic { background: #3FA9E033; }
  .fitem.kind-image .ic { background: #35C77D33; }
  .fitem.kind-video .ic { background: #E2A33D33; }
  .fitem.kind-code .ic { background: #A78BFA33; }
  .fitem.kind-pdf .ic { background: #F8717133; }
  .fitem .fn { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .fitem .sz { font-family: var(--mono); font-size: 10px; color: var(--faint); }
  .fitem .more { opacity: 0; width: 22px; height: 22px; border: 0; background: transparent; color: var(--muted); cursor: pointer; border-radius: 6px; flex-shrink: 0; }
  .fitem:hover .more, .fitem.active .more { opacity: 1; }
  .fitem .more:hover { background: var(--surface); color: var(--text); }

  .drop { margin: 8px 10px 12px; border: 1px dashed var(--border); border-radius: 10px; padding: 10px; text-align: center; color: var(--faint); font-size: 12px; }
  .drop.over { border-color: var(--accent); background: var(--accent-soft); color: var(--accent); }
  .drop input { display: none; }
  .drop label { color: var(--accent); cursor: pointer; }

  .work { min-width: 0; min-height: 0; display: flex; flex-direction: column; background: var(--bg); }
  .work-top { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 20px; border-bottom: 1px solid var(--border); }
  .breadcrumb { font-family: var(--mono); font-size: 12px; color: var(--faint); min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .breadcrumb .cur { color: var(--accent); }
  .breadcrumb .sep { margin: 0 6px; }
  .breadcrumb button { border: none; background: none; color: var(--faint); cursor: pointer; font: inherit; padding: 0; }
  .breadcrumb button:hover { color: var(--accent); text-decoration: underline; }
  .toolbar { display: flex; gap: 8px; flex-shrink: 0; }
  .btn { padding: 6px 13px; border-radius: 8px; font-size: 12.5px; font-weight: 500; background: var(--surface2); color: var(--text); border: 1px solid var(--border); cursor: pointer; }
  .btn:hover { border-color: var(--muted); }
  .btn.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
  .btn.ghost { background: transparent; }
  .btn:disabled { opacity: .5; cursor: default; }

  .content-wrap { flex: 1; min-height: 0; overflow-y: auto; padding: 22px 26px; }
  .content { max-width: 860px; }
  .content h1 { font-size: 21px; font-weight: 700; letter-spacing: -0.01em; margin: 4px 0 14px; }
  .content h2 { font-size: 15px; font-weight: 650; margin: 22px 0 8px; }
  .content h3 { font-size: 13.5px; font-weight: 650; margin: 16px 0 6px; }
  .content p { color: var(--muted); margin: 8px 0; }
  .content a { color: var(--accent); text-decoration: none; }
  .content a:hover { text-decoration: underline; }
  .content ul, .content ol { margin: 8px 0 8px 22px; color: var(--muted); }
  .content li { margin: 4px 0; }
  .content code { font-family: var(--mono); font-size: 12.5px; background: var(--surface2); color: var(--accent); padding: 1px 5px; border-radius: 5px; }
  .content pre { background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 13px 15px; overflow-x: auto; margin: 12px 0; }
  .content pre code { background: none; color: var(--text); padding: 0; }
  .content blockquote { border-left: 3px solid var(--accent); padding: 2px 0 2px 14px; margin: 12px 0; color: var(--muted); font-style: italic; }
  .content table { border-collapse: collapse; width: 100%; margin: 12px 0; font-size: 13px; }
  .content th { text-align: left; font-family: var(--mono); font-size: 11px; font-weight: 600; color: var(--faint); padding: 8px 12px; border-bottom: 1px solid var(--border); }
  .content td { padding: 8px 12px; border-bottom: 1px solid var(--border); color: var(--muted); }
  .content strong { color: var(--text); }
  .editor { width: 100%; min-height: 100%; background: var(--surface); border: 1px solid var(--border); border-radius: 8px; color: var(--text); font-family: var(--mono); font-size: 13px; line-height: 1.7; padding: 16px 18px; resize: none; outline: none; }
  .editor:focus { border-color: var(--accent); }
  .empty { color: var(--faint); font-size: 13px; padding: 24px 20px; text-align: center; }
  .list-banner { font-size: 11px; color: var(--faint); padding: 6px 12px; border-bottom: 1px solid var(--border); }
  .skel { padding: 8px 12px; }
  .skel i { display: block; height: 11px; margin: 8px 0; border-radius: 4px; background: var(--surface2); }
  .preview-media { max-width: 100%; max-height: calc(100vh - 180px); border-radius: 10px; border: 1px solid var(--border); background: var(--surface); }
  video.preview-media { width: 100%; background: #000; }
  .md-host { display: none; flex: 1; min-height: 0; }
  .md-host.on { display: block; }
  .cm-host { display: none; flex: 1; min-height: 0; }
  .cm-host.on { display: flex; flex-direction: column; min-height: 0; }
  .cm-host.on .cm-editor { flex: 1; min-height: 0; }
  .preview-pdf { width: 100%; height: calc(100vh - 160px); border: 0; border-radius: 10px; background: var(--surface); }
  .lang-pick { font: inherit; font-size: 12px; background: var(--surface2); color: var(--text); border: 1px solid var(--border); border-radius: 6px; padding: 3px 6px; max-width: 160px; }
  .new-box { display: none; gap: 6px; padding: 0 10px 8px; align-items: center; }
  .new-box.on { display: flex; }
  .new-box input { flex: 1; min-width: 0; background: var(--surface2); border: 1px solid var(--border); color: var(--text); border-radius: 6px; padding: 5px 8px; font: inherit; font-size: 12px; }
  .ctx { position: fixed; z-index: 50; background: var(--surface); border: 1px solid var(--border); border-radius: 8px; min-width: 148px; padding: 4px; box-shadow: var(--shadow); }
  .ctx button { display: block; width: 100%; text-align: left; background: transparent; border: 0; color: var(--text); padding: 7px 10px; border-radius: 6px; cursor: pointer; font: inherit; font-size: 13px; }
  .ctx button:hover { background: var(--surface2); }
  .ctx button.danger { color: var(--danger); }
  .content-wrap { display: flex; flex-direction: column; }
  .toastui-editor-defaultUI { border-color: var(--border) !important; }
  .toastui-editor-contents { font-family: var(--sans); }
  .drop.pond-on { border: none; padding: 0; margin: 0; background: transparent; }
  .filepond--root { margin: 8px 10px 12px; font-family: var(--sans); font-size: 13px; }
  .filepond--panel-root { background-color: var(--surface2); }
  .filepond--drop-label { color: var(--muted); }
  .filepond--label-action { color: var(--accent); text-decoration: none; }
  [data-theme="dark"] .filepond--panel-root { background-color: var(--surface2); }
  [data-theme="dark"] .filepond--drip { background: var(--accent-soft); }

  .toast { position: fixed; bottom: 22px; left: 50%; transform: translate(-50%, 12px); background: var(--text); color: var(--bg); padding: 9px 16px; border-radius: 9px; font-size: 13px; font-weight: 500; box-shadow: var(--shadow); opacity: 0; pointer-events: none; transition: opacity .2s, transform .2s; z-index: 60; }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  .toast.err { background: var(--danger); color: #fff; }

  @media (max-width: 800px) {
    .shell { grid-template-columns: 160px minmax(0,1fr); }
    .col.files, .splitter { display: none; }
    .search { display: none; }
  }
</style>
<link rel="stylesheet" href="__ASSET_BASE__static/toastui-editor.min.css">
<link rel="stylesheet" href="__ASSET_BASE__static/toastui-editor-dark.min.css">
<link rel="stylesheet" href="__ASSET_BASE__static/filepond.min.css">
</head>
<body>

<header>
  <div class="brand"><span class="mark">B</span><span class="name">Bridge Panel</span><span class="sub">__BUILD_TAG__</span></div>
  <div class="search"><span class="icon">&#8962;</span><input id="search" placeholder="Filter hosts…" aria-label="Filter hosts"></div>
  <div class="hdr-right">
    <span class="summary" id="summary"></span>
    <button class="theme-toggle" id="themeToggle" title="Toggle theme" aria-label="Toggle theme">&#9680;</button>
    <div class="avatar" id="nodeAvatar">·</div>
  </div>
</header>

<div class="shell" id="shell">
  <aside class="col machines" id="colHosts">
    <div class="col-head">Hosts <span class="count" id="machineCount"></span></div>
    <div class="col-scroll" id="machines"></div>
  </aside>
  <div class="splitter" id="splitHosts" role="separator" aria-orientation="vertical" aria-label="Resize hosts pane"></div>
  <aside class="col files" id="colFiles">
    <div class="col-head"><span id="filesHead">Files</span><button class="icon-btn" id="refreshBtn" title="Refresh" aria-label="Refresh files">↻</button></div>
    <div class="filters" id="filters">
      <button class="chip active" data-filter="all">All</button>
      <button class="chip" data-filter="md">Markdown</button>
      <button class="chip" data-filter="image">Images</button>
      <button class="chip" data-filter="video">Video</button>
    </div>
    <div class="file-search">
      <input id="fileSearch" placeholder="Filter files…" aria-label="Filter files">
    </div>
    <div class="path-jump">
      <input id="pathJump" placeholder="Paste path to open (Enter)…" aria-label="Paste path to open" spellcheck="false">
    </div>
    <div class="volrow" id="volrow"></div>
    <div class="pathrow" id="pathrow"></div>
    <div class="viewbar" id="viewbar">
      <button type="button" class="icon-btn" id="treeToggle" title="Folder tree (B)" aria-label="Toggle folder tree">&#127794;</button>
      <button type="button" class="icon-btn" id="gridToggle" title="Grid" aria-label="Toggle grid" style="display:none">&#9638;</button>
      <button type="button" class="icon-btn" id="newFileBtn" title="New file" aria-label="New file" style="display:none">+</button>
      <button type="button" class="icon-btn" id="newDirBtn" title="New folder" aria-label="New folder" style="display:none">&#128193;</button>
    </div>
    <div class="new-box" id="newBox">
      <input id="newName" placeholder="Name" aria-label="New name">
      <button type="button" class="btn primary" id="newOk">Create</button>
      <button type="button" class="btn ghost" id="newCancel">Cancel</button>
    </div>
    <div class="tree" id="tree" hidden></div>
    <div class="list-banner" id="listBanner" hidden></div>
    <div class="col-scroll" id="filelist"></div>
    <div class="dest-hint" id="destHint">Uploads go to: inbox</div>
    <div class="drop" id="drop">
      <input type="file" id="fileInput" multiple>
    </div>
  </aside>
  <div class="splitter" id="splitFiles" role="separator" aria-orientation="vertical" aria-label="Resize files pane"></div>
  <main class="work">
    <div class="work-top">
      <div class="breadcrumb" id="breadcrumb"></div>
      <div class="toolbar">
        <button class="btn" id="editBtn" style="display:none">Edit</button>
        <button class="btn primary" id="saveBtn" style="display:none">Save</button>
        <button class="btn ghost" id="cancelBtn" style="display:none">Cancel</button>
        <button class="btn ghost" id="copyBtn" style="display:none">Copy</button>
        <button class="btn ghost" id="downloadBtn" style="display:none">Download</button>
        <select class="lang-pick" id="langPick" style="display:none" aria-label="Language"></select>
      </div>
    </div>
    <div class="content-wrap">
      <div class="content" id="content"><div class="empty">Select a host, then a file.</div></div>
      <div class="md-host" id="mdHost"></div>
      <div class="cm-host" id="cmHost"></div>
      <textarea class="editor" id="editor" style="display:none" spellcheck="false" aria-label="Markdown editor"></textarea>
    </div>
  </main>
</div>

<div class="toast" id="toast"></div>
<div class="ctx" id="ctx" hidden>
  <button type="button" data-act="rename">Rename</button>
  <button type="button" data-act="download">Download</button>
  <button type="button" class="danger" data-act="trash">Delete</button>
</div>

<script src="__ASSET_BASE__static/filepond-plugin-file-validate-size.min.js"></script>
<script src="__ASSET_BASE__static/filepond.min.js"></script>
<script src="__ASSET_BASE__static/toastui-editor-all.min.js"></script>
<script src="__ASSET_BASE__static/codemirror-bundle.min.js"></script>
<script>
(function(){
  "use strict";
  const base = location.pathname.replace(/\/$/, "");
  let mesh = {node:"", peers:[], sessions:[], offline:true, uptime_s:0};
  let selMachine = null;
  let selRoot = "inbox";
  let volumes = [];
  let showOther = false;
  let cwd = "";
  let listing = {items:[], ok:true, error:"", count:0};
  let filter = "all";
  let fileQuery = "";
  let selName = null;
  let selKind = null;
  let curRaw = "";
  let editing = false;
  let query = "";
  let theme = localStorage.getItem("bp-theme") === "light" ? "light" : "dark";
  let listingReq = 0;
  let showTree = false;
  let viewMode = "list";
  let treeKids = {};
  let treeOpen = [];
  let hist = [];
  let histI = -1;
  const listMem = {};

  const $ = s => document.querySelector(s);
  const $$ = s => Array.from(document.querySelectorAll(s));
  const esc = s => String(s).replace(/[&<>\"']/g, c =>
    ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
  const LANG_TABLE = __LANG_TABLE__;
  function detectLanguage(name, first, override) {
    if (override) return override;
    const base = String(name || "").split("/").pop() || "";
    const low = base.toLowerCase();
    if (LANG_TABLE.special && LANG_TABLE.special[low]) return LANG_TABLE.special[low];
    const dot = low.lastIndexOf(".");
    const ext = dot >= 0 ? low.slice(dot) : "";
    if (ext && LANG_TABLE.ext && LANG_TABLE.ext[ext]) return LANG_TABLE.ext[ext];
    const line = String(first || "").split("\n")[0] || "";
    const rules = LANG_TABLE.first || [];
    for (let i = 0; i < rules.length; i++) {
      try {
        if (new RegExp(rules[i].pat, rules[i].flags || "").test(line)) return rules[i].lang;
      } catch (e) {}
    }
    return "plaintext";
  }
  function langKey() {
    return "bp-lang:" + (selMachine || "") + "|" + (selRoot || "inbox") + "|" + joinPath(cwd, selName || "");
  }
  function savedLang() {
    try { return localStorage.getItem(langKey()) || ""; } catch (e) { return ""; }
  }
  function saveLang(id) {
    try { if (id) localStorage.setItem(langKey(), id); } catch (e) {}
  }
  function currentLang(first) {
    return detectLanguage(selName || "", first || (curRaw || "").split("\n")[0] || "", savedLang() || null);
  }
  function fillLangPick(sel) {
    const el = $("#langPick");
    if (!el) return;
    const rows = (LANG_TABLE.languages || []).slice();
    if (!rows.length) return;
    el.innerHTML = rows.map(r => "<option value=\"" + esc(r.id) + "\">" + esc(r.label) + "</option>").join("");
    el.value = sel || "plaintext";
  }
  function hasCM() { return !!(window.BridgeCM && BridgeCM.mount); }
  function destroyCm() {
    if (window.BridgeCM && BridgeCM.destroy) { try { BridgeCM.destroy(); } catch (e) {} }
    const host = $("#cmHost");
    if (host) { host.classList.remove("on"); host.innerHTML = ""; }
  }
  function showCode(raw, readOnly) {
    destroyMd();
    destroyCm();
    if (!hasCM()) return false;
    $("#content").style.display = "none";
    $("#editor").style.display = "none";
    const host = $("#cmHost");
    host.classList.add("on");
    const lang = currentLang(raw);
    BridgeCM.mount(host, {doc: raw || "", language: lang, theme: theme, readOnly: !!readOnly});
    fillLangPick(lang);
    return true;
  }
  function codeText() {
    if (hasCM() && BridgeCM.getValue) return BridgeCM.getValue();
    const ed = $("#editor");
    return ed ? ed.value : curRaw;
  }

  async function api(path, opts) {
    const r = await fetch(base + path, opts);
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.json();
  }

  let toastTimer;
  function toast(msg, isErr) {
    const t = $("#toast");
    t.textContent = msg; t.className = "toast show" + (isErr ? " err" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.className = "toast"; }, 1800);
  }
  function applyTheme(t) {
    theme = (t === "light") ? "light" : "dark";
    document.documentElement.setAttribute("data-theme", theme);
    try { localStorage.setItem("bp-theme", theme); } catch (e) {}
    $("#themeToggle").textContent = theme === "dark" ? "\u25D0" : "\u25D1";
  }

  let mdInst = null;
  function hasToastUi() {
    return !!(window.toastui && toastui.Editor);
  }
  function destroyMd() {
    if (mdInst) {
      try { mdInst.destroy(); } catch (e) {}
      mdInst = null;
    }
    const host = $("#mdHost");
    if (host) { host.classList.remove("on"); host.innerHTML = ""; }
    destroyCm();
  }
  function showMdViewer(raw) {
    destroyMd();
    if (!hasToastUi() || !toastui.Editor.factory) return false;
    $("#content").style.display = "none";
    $("#editor").style.display = "none";
    const host = $("#mdHost");
    host.classList.add("on");
    mdInst = toastui.Editor.factory({
      el: host,
      viewer: true,
      initialValue: raw || "",
      theme: theme === "light" ? "light" : "dark",
      usageStatistics: false,
      height: "auto"
    });
    return true;
  }
  function showMdEditor(raw) {
    destroyMd();
    if (!hasToastUi()) return false;
    $("#content").style.display = "none";
    $("#editor").style.display = "none";
    const host = $("#mdHost");
    host.classList.add("on");
    mdInst = new toastui.Editor({
      el: host,
      initialEditType: "markdown",
      previewStyle: "vertical",
      height: "calc(100vh - 150px)",
      initialValue: raw || "",
      theme: theme === "light" ? "light" : "dark",
      usageStatistics: false,
      autofocus: true
    });
    return true;
  }
  function mdText() {
    if (mdInst && typeof mdInst.getMarkdown === "function") return mdInst.getMarkdown();
    const ed = $("#editor");
    return ed ? ed.value : curRaw;
  }
  function remountMd() {
    if (!selName) return;
    const raw = editing ? (selKind === "md" ? mdText() : codeText()) : curRaw;
    if (selKind === "md") {
      if (editing) {
        if (!showMdEditor(raw)) { $("#editor").value = raw; }
      } else if (!showMdViewer(raw)) {
        $("#content").style.display = "block";
      }
      return;
    }
    if (selKind === "code" || selKind === "file") {
      if (!showCode(raw, !editing)) {
        $("#content").style.display = "block";
        $("#content").innerHTML = "<pre>" + esc(raw) + "</pre>";
      }
    }
  }

  function currentVol() {
    return (volumes || []).find(v => (v.token || "inbox") === (selRoot || "inbox")) || {token:"inbox", writable:true, media_hint:"mixed"};
  }
  function rootIsWritable() {
    if ((selRoot || "inbox") === "inbox") return true;
    const v = (volumes || []).find(x => x.token === selRoot);
    return !!(v && v.writable);
  }
  function updateDestHint() {
    const el = $("#destHint");
    const drop = $("#drop");
    const writable = rootIsWritable();
    if (el) {
      el.innerHTML = writable
        ? "Uploads go to: <b>" + esc(rootLabel(selRoot)) + "</b>"
        : "Read-only. Uploads stay in inbox.";
    }
    if (drop) drop.classList.toggle("hidden", !writable);
    const nf = $("#newFileBtn");
    const nd = $("#newDirBtn");
    if (nf) nf.style.display = writable ? "" : "none";
    if (nd) nd.style.display = writable ? "" : "none";
    const g = $("#gridToggle");
    if (g) {
      const media = (listing.media_hint || currentVol().media_hint) === "media";
      g.style.display = media ? "" : "none";
      if (!media && viewMode === "grid") viewMode = "list";
    }
    const t = $("#treeToggle");
    if (t) t.classList.toggle("active", showTree);
    if (g) g.classList.toggle("active", viewMode === "grid");
    const tree = $("#tree");
    if (tree) tree.hidden = !showTree;
  }
  function treeStoreKey() { return "bp-tree-open:" + (selMachine || "") + ":" + (selRoot || "inbox"); }
  function viewStoreKey() { return "bp-view:" + (selMachine || "") + ":" + (selRoot || "inbox"); }
  function loadTreeOpen() {
    try { treeOpen = JSON.parse(localStorage.getItem(treeStoreKey()) || "[]"); } catch (e) { treeOpen = []; }
    if (!Array.isArray(treeOpen)) treeOpen = [];
  }
  function saveTreeOpen() {
    try { localStorage.setItem(treeStoreKey(), JSON.stringify(treeOpen)); } catch (e) {}
  }
  function loadViewPref() {
    try {
      const saved = localStorage.getItem(viewStoreKey());
      if (saved === "grid" || saved === "list") viewMode = saved;
    } catch (e) {}
  }
  function saveViewPref() {
    try { localStorage.setItem(viewStoreKey(), viewMode); } catch (e) {}
  }
  function pushHist() {
    const rec = {root: selRoot || "inbox", cwd: cwd || ""};
    const last = hist[histI];
    if (last && last.root === rec.root && last.cwd === rec.cwd) return;
    hist = hist.slice(0, histI + 1);
    hist.push(rec);
    histI = hist.length - 1;
  }
  function goHist(delta) {
    const n = histI + delta;
    if (n < 0 || n >= hist.length) return;
    histI = n;
    const rec = hist[n];
    selRoot = rec.root || "inbox";
    cwd = rec.cwd || "";
    selName = null; curRaw = ""; editing = false;
    updateDestHint();
    renderVolrow();
    loadListing();
    if (showTree) renderTree();
  }
  function goParent() {
    if (!cwd) return;
    const parts = cwd.split("/").filter(Boolean);
    parts.pop();
    goDir(parts.join("/"));
  }
  async function fetchDirs(path) {
    if (treeKids[path] !== undefined) return treeKids[path];
    try {
      const d = await api("/api/files?machine=" + encodeURIComponent(selMachine) +
        "&root=" + encodeURIComponent(selRoot || "inbox") + "&path=" + encodeURIComponent(path));
      const dirs = ((d && d.items) || []).filter(it => it.dir).map(it => it.name);
      treeKids[path] = dirs;
      return dirs;
    } catch (e) {
      treeKids[path] = [];
      return [];
    }
  }
  async function renderTree() {
    const host = $("#tree");
    if (!host || !showTree || !selMachine) { if (host) host.innerHTML = ""; return; }
    loadTreeOpen();
    const lines = [];
    async function walk(path, depth) {
      const dirs = await fetchDirs(path);
      for (const name of dirs) {
        const child = joinPath(path, name);
        const open = treeOpen.indexOf(child) >= 0;
        const here = cwd === child;
        lines.push("<div class=\\\"tnode" + (open ? " open" : "") + (here ? " here" : "") +
          "\\\" data-path=\\\"" + esc(child) + "\\\" style=\\\"padding-left:" + (8 + depth * 12) + "px\\\">" +
          "<span class=\\\"tw\\\">" + (open ? "▾" : "▸") + "</span><span class=\\\"tn\\\">" + esc(name) + "</span></div>");
        if (open) await walk(child, depth + 1);
      }
    }
    lines.push("<div class=\\\"tnode" + (!cwd ? " here" : "") + "\\\" data-path=\\\"\\\" style=\\\"padding-left:8px\\\">" +
      "<span class=\\\"tw\\\">▾</span><span class=\\\"tn\\\">" + esc(rootLabel(selRoot)) + "</span></div>");
    if (treeOpen.indexOf("") < 0) treeOpen.push("");
    await walk("", 1);
    host.innerHTML = lines.join("");
  }
  function toggleTree() {
    showTree = !showTree;
    updateDestHint();
    if (showTree) renderTree();
  }
  function toggleGrid() {
    if ((listing.media_hint || currentVol().media_hint) !== "media") return;
    viewMode = viewMode === "grid" ? "list" : "grid";
    saveViewPref();
    updateDestHint();
    renderFiles();
  }
  function fmtBytes(n) {
    if (n == null || n === "" || !isFinite(Number(n))) return "";
    n = Number(n);
    if (n < 1024) return n + " B";
    if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
    return (n / 1048576).toFixed(1) + " MB";
  }
  function fmtDisk(n) {
    n = Number(n);
    if (!isFinite(n) || n <= 0) return "";
    if (n >= 1e12) return (n / 1e12).toFixed(1) + " TB free";
    if (n >= 1e9) return (n / 1e9).toFixed(0) + " GB free";
    return fmtBytes(n) + " free";
  }
  function rootLabel(token) {
    if (!token || token === "inbox") return "inbox";
    if (token.length === 1) return token.toUpperCase() + ":";
    if (token === "/") return "/";
    if (token.charAt(0) === "_") return token.split("_").join("/");
    return token;
  }
  function joinPath(dir, name) {
    if (!dir) return name || "";
    if (!name) return dir;
    return dir.replace(/\/+$/, "") + "/" + name;
  }
  function fileUrl(rel, extra) {
    let u = base + "/api/remote-file?machine=" + encodeURIComponent(selMachine || "") +
      "&root=" + encodeURIComponent(selRoot || "inbox") +
      "&path=" + encodeURIComponent(rel);
    if (extra) u += extra;
    return u;
  }

  function machineStatus(m) {
    if (m.you) return "you";
    if (m.status === "stale") return "stale";
    if (m.healthy === false || m.status === "offline" || m.status === "no-pong") return "offline";
    return "healthy";
  }
  function machineList() {
    const out = [];
    if (mesh.node) out.push({name: mesh.node, addr: "", you: true, healthy: !mesh.offline, os: mesh.os || "", status: mesh.offline ? "offline" : "self"});
    for (const p of (mesh.peers || [])) {
      out.push({name: p.name, addr: p.addr || "", you: false, healthy: !!p.healthy, status: p.status || "", os: p.os || ""});
    }
    out.sort((a, b) => a.name.localeCompare(b.name, undefined, {sensitivity:"base"})); // Alphabetical by machine name
    return out;
  }
  function selectedHost() {
    return machineList().find(m => m.name === selMachine) || null;
  }
  function renderMachines() {
    const list = machineList().filter(m => !query || m.name.toLowerCase().includes(query));
    $("#machines").innerHTML = !list.length
      ? "<div class=\"empty\">No hosts match \"" + esc(query) + "\".</div>"
      : list.map(m => {
        const st = machineStatus(m);
        const dotCls = st === "you" ? "you" : (st === "healthy" ? "up" : st);
        return "<div class=\"mrow" + (selMachine === m.name ? " active" : "") + (st === "offline" ? " offline" : "") + "\" data-machine=\"" + esc(m.name) + "\">" +
          "<span class=\"dot " + dotCls + "\"></span>" +
          "<div class=\"mi\"><div class=\"mn\">" + esc(m.name) + "</div></div>" +
          (m.you ? "<span class=\"you\">you</span>" : "") + "</div>";
      }).join("");
    $("#machineCount").textContent = list.length + " shown";
    const online = machineList().filter(m => machineStatus(m) !== "offline").length;
    const total = machineList().length;
    if (mesh.offline && !mesh.node) $("#summary").innerHTML = "<b>mesh offline</b>";
    else $("#summary").innerHTML = "<b>" + online + "</b> online · " + (total - online) + " offline";
    $("#nodeAvatar").textContent = (mesh.node || "?").charAt(0).toUpperCase();
  }

  function visibleItems() {
    const items = listing.items || [];
    const q = fileQuery;
    return items.filter(it => {
      if (filter !== "all" && !it.dir && it.kind !== filter) return false;
      if (q && !(it.name || "").toLowerCase().includes(q)) return false;
      return true;
    });
  }
  function renderPath() {
    const bits = [rootLabel(selRoot)].concat(cwd ? cwd.split("/") : []);
    let acc = "";
    const parts = bits.map((b, i) => {
      if (i === 0) return "<button type=\"button\" data-path=\"\">" + esc(rootLabel(selRoot)) + "</button>";
      acc = acc ? acc + "/" + b : b;
      return "<button type=\"button\" data-path=\"" + esc(acc) + "\">" + esc(b) + "</button>";
    });
    $("#pathrow").innerHTML = parts.join("<span> / </span>");
  }
  function renderVolrow() {
    const row = $("#volrow");
    if (!row) return;
    if (!selMachine) { row.innerHTML = ""; return; }
    const primary = (volumes || []).filter(v => v.group !== "other");
    const other = (volumes || []).filter(v => v.group === "other");
    const chips = (showOther ? (volumes || []) : primary).map(v => {
      const on = (v.token || "") === (selRoot || "inbox");
      const free = v.token === "inbox" ? "" : fmtDisk(v.bytes_free);
      return "<button type=\"button\" class=\"chip" + (on ? " active" : "") + (v.group === "other" ? " other" : "") +
        "\" data-root=\"" + esc(v.token) + "\">" + esc(v.label || v.token) +
        (free ? "<span class=\"free\">" + esc(free) + "</span>" : "") + "</button>";
    });
    if (other.length && !showOther) {
      chips.push("<button type=\"button\" class=\"chip other\" data-root=\"__other__\">Other disks ▸</button>");
    }
    row.innerHTML = chips.join("");
  }
  function renderFiles() {
    $("#filesHead").textContent = selMachine ? ("Files · " + selMachine) : "Files";
    renderPath();
    renderVolrow();
    if (!selMachine) {
      $("#filelist").innerHTML = "<div class=\"empty\">Select a host.</div>";
      return;
    }
    if (listing.ok === false) {
      $("#filelist").innerHTML = "<div class=\"empty\">" + esc(listing.error || "Could not list files.") + "</div>";
      return;
    }
    const items = visibleItems();
    if (!items.length) {
      $("#filelist").innerHTML = "<div class=\"empty\">" + (cwd || filter !== "all" || fileQuery ? "Nothing in this view." : "Inbox is empty. Upload a file.") + "</div>";
      return;
    }
    const cap = 400;
    const shown = items.slice(0, cap);
    const more = items.length > cap ? "<div class=\"empty\">Showing " + cap + " of " + items.length + ". Filter to narrow.</div>" : "";
    const hidden = listing.hidden_by_policy ? "<div class=\\\"empty\\\">" + listing.hidden_by_policy + " items hidden by policy</div>" : "";
    if (viewMode === "grid") {
      $("#filelist").innerHTML = "<div class=\\\"grid\\\">" + shown.map(it => {
        const kind = it.dir ? "dir" : (it.kind || "file");
        const active = (!it.dir && selName === it.name) ? " active" : "";
        const rel = joinPath(cwd, it.name);
        const thumb = (!it.dir && kind === "image")
          ? "<img alt=\\\"\\\" src=\\\"" + esc(fileUrl(rel, "&inline=1")) + "\\\">"
          : "<div class=\\\"ph\\\"></div>";
        return "<div class=\\\"gitem kind-" + kind + active + "\\\" data-name=\\\"" + esc(it.name) + "\\\" data-dir=\\\"" + (it.dir ? "1" : "0") + "\\\" data-kind=\\\"" + esc(kind) + "\\\">" +
          thumb + "<span class=\\\"fn\\\">" + esc(it.name) + "</span></div>";
      }).join("") + "</div>" + more + hidden;
      updateDestHint();
      return;
    }
    $("#filelist").innerHTML = shown.map(it => {
      const kind = it.dir ? "dir" : (it.kind || "file");
      const active = (!it.dir && selName === it.name) ? " active" : "";
      const sz = it.dir ? "" : fmtBytes(it.size);
      return "<div class=\"fitem kind-" + kind + active + "\" data-name=\"" + esc(it.name) + "\" data-dir=\"" + (it.dir ? "1" : "0") + "\" data-kind=\"" + esc(kind) + "\">" +
        "<span class=\"ic\"></span><span class=\"fn\">" + esc(it.name) + "</span><span class=\"sz\">" + esc(sz) + "</span>" +
        "<button type=\"button\" class=\"more\" data-more=\"1\" title=\"Actions\" aria-label=\"Actions\">&#8942;</button></div>";
    }).join("") + more + hidden;
  }

  function renderBreadcrumb() {
    if (!selMachine) { $("#breadcrumb").innerHTML = ""; return; }
    const crumbs = [{label: selMachine, kind: "host", path: ""}];
    crumbs.push({label: rootLabel(selRoot), kind: "root", path: ""});
    if (cwd) {
      let acc = "";
      cwd.split("/").forEach(seg => {
        acc = acc ? acc + "/" + seg : seg;
        crumbs.push({label: seg, kind: "dir", path: acc});
      });
    }
    if (selName) crumbs.push({label: selName, kind: "file", path: joinPath(cwd, selName)});
    $("#breadcrumb").innerHTML = crumbs.map((c, i) => {
      if (i === crumbs.length - 1) return "<span class=\"cur\">" + esc(c.label) + "</span>";
      return "<button type=\"button\" data-kind=\"" + esc(c.kind) + "\" data-path=\"" + esc(c.path) + "\">" + esc(c.label) + "</button><span class=\"sep\">/</span>";
    }).join("");
  }
  function updateTools() {
    const hasFile = !!selName && selKind !== "dir";
    const texty = selKind === "md" || selKind === "code" || selKind === "file";
    const canEdit = hasFile && texty && rootIsWritable();
    const viewingMd = !!(mdInst && $("#mdHost").classList.contains("on") && !editing);
    const viewingCm = !!( $("#cmHost") && $("#cmHost").classList.contains("on"));
    $("#editBtn").style.display = canEdit && !editing ? "" : "none";
    $("#saveBtn").style.display = editing ? "" : "none";
    $("#cancelBtn").style.display = editing ? "" : "none";
    $("#copyBtn").style.display = hasFile && !editing && curRaw ? "" : "none";
    $("#downloadBtn").style.display = hasFile && !editing ? "" : "none";
    const lp = $("#langPick");
    if (lp) lp.style.display = (selKind === "code" || selKind === "file") && viewingCm ? "" : "none";
    $("#content").style.display = (editing || viewingMd || viewingCm) ? "none" : "block";
    $("#editor").style.display = (editing && selKind === "md" && !hasToastUi()) ? "block" : "none";
  }

  function listCacheKey() {
    return (selMachine || "") + "|" + (selRoot || "inbox") + "|" + (cwd || "");
  }
  function setListBanner(text) {
    const el = $("#listBanner");
    if (!el) return;
    if (!text) { el.hidden = true; el.textContent = ""; return; }
    el.hidden = false;
    el.textContent = text;
  }
  function rememberListing(key, data) {
    if (!data || !data.ok) return;
    const rec = {items: data.items || [], count: data.count || 0, media_hint: data.media_hint || "", ts: Date.now(), ok: true, root: data.root, path: data.path};
    listMem[key] = rec;
    try {
      const store = JSON.parse(sessionStorage.getItem("bp-list-cache") || "{}");
      store[key] = rec;
      const keys = Object.keys(store);
      if (keys.length > 16) delete store[keys[0]];
      sessionStorage.setItem("bp-list-cache", JSON.stringify(store));
    } catch (e) {}
  }
  function readCachedListing(key) {
    if (listMem[key]) return listMem[key];
    try {
      const store = JSON.parse(sessionStorage.getItem("bp-list-cache") || "{}");
      if (store[key]) { listMem[key] = store[key]; return store[key]; }
    } catch (e) {}
    return null;
  }
  function forgetListing(key) {
    delete listMem[key];
    try {
      const store = JSON.parse(sessionStorage.getItem("bp-list-cache") || "{}");
      delete store[key];
      sessionStorage.setItem("bp-list-cache", JSON.stringify(store));
    } catch (e) {}
  }
  async function loadListing(refresh) {
    if (!selMachine) {
      listing = {items:[], ok:true, error:"", count:0};
      setListBanner("");
      renderFiles();
      return;
    }
    const key = listCacheKey();
    const painted = readCachedListing(key);
    const req = ++listingReq;
    if (painted && painted.items) {
      listing = painted;
      renderFiles();
      setListBanner(refresh ? "Refreshing…" : "Showing last listing…");
    } else {
      $("#filelist").innerHTML = "<div class=\"skel\"><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></div>";
      setListBanner("Listing " + rootLabel(selRoot) + " on " + selMachine + "…");
    }
    try {
      const d = await api("/api/files?machine=" + encodeURIComponent(selMachine) + "&root=" + encodeURIComponent(selRoot || "inbox") + "&path=" + encodeURIComponent(cwd) + (refresh ? "&refresh=1" : ""));
      if (req !== listingReq) return;
      listing = d;
      if (d && d.ok) rememberListing(key, d);
    } catch (err) {
      if (req !== listingReq) return;
      if (!painted) listing = {ok:false, error: String(err.message || err), items:[]};
    }
    if (listing && listing.offline) setListBanner("Host unreachable — showing last listing");
    else if (listing && listing.stale) setListBanner("Showing last listing — refresh in background");
    else setListBanner("");
    pushHist();
    updateDestHint();
    renderFiles();
    if (showTree) renderTree();
  }

  function showEmptyPreview(msg) {
    selName = null; selKind = null; curRaw = ""; editing = false;
    destroyMd();
    $("#content").style.display = "block";
    $("#content").innerHTML = "<div class=\"empty\">" + esc(msg) + "</div>";
    updateTools();
    renderBreadcrumb();
  }

  async function openFile(name, kind) {
    selName = name;
    selKind = kind;
    curRaw = "";
    editing = false;
    destroyMd();
    renderFiles();
    renderBreadcrumb();
    updateTools();
    const rel = joinPath(cwd, name);
    if (kind === "image") {
      $("#content").style.display = "block";
      $("#content").innerHTML = "<img class=\"preview-media\" alt=\"\" src=\"" + esc(fileUrl(rel, "&inline=1")) + "\">";
      return;
    }
    if (kind === "video") {
      $("#content").style.display = "block";
      $("#content").innerHTML = "<video class=\"preview-media\" controls src=\"" + esc(fileUrl(rel, "&inline=1")) + "\"></video>";
      return;
    }
    if (kind === "pdf") {
      $("#content").style.display = "block";
      $("#content").innerHTML = "<embed class=\"preview-pdf\" type=\"application/pdf\" src=\"" + esc(fileUrl(rel, "&inline=1")) + "\">";
      updateTools();
      return;
    }
    if (kind !== "md" && kind !== "file" && kind !== "code") {
      $("#content").style.display = "block";
      $("#content").innerHTML = "<div class=\"empty\">No preview for this type. Use Download.</div>";
      updateTools();
      return;
    }
    $("#content").style.display = "block";
    $("#content").innerHTML = "<div class=\"empty\">Opening…</div>";
    try {
      const r = await fetch(fileUrl(rel));
      const ct = r.headers.get("Content-Type") || "";
      if (ct.indexOf("application/json") !== -1) {
        const d = await r.json();
        if (!d.ok) { $("#content").innerHTML = "<div class=\"empty\">" + esc(d.error || "Could not open") + "</div>"; return; }
        curRaw = d.raw || "";
        if (kind === "md") {
          if (!showMdViewer(curRaw)) {
            $("#content").style.display = "block";
            $("#content").innerHTML = d.html || "<div class=\"empty\">(empty)</div>";
          }
        } else if (!showCode(curRaw, true)) {
          $("#content").style.display = "block";
          $("#content").innerHTML = d.html || "<pre>" + esc(curRaw) + "</pre>";
        }
      } else {
        $("#content").innerHTML = "<div class=\"empty\">Binary file. Use Download.</div>";
      }
    } catch (e) {
      $("#content").innerHTML = "<div class=\"empty\">Could not open file.</div>";
    }
    updateTools();
  }

  async function loadVolumes() {
    volumes = [{token:"inbox", label:"Inbox", group:"primary", bytes_free:0}];
    if (!selMachine) { renderVolrow(); return; }
    try {
      const d = await api("/api/volumes?machine=" + encodeURIComponent(selMachine));
      if (d && d.volumes && d.volumes.length) volumes = d.volumes;
    } catch (e) {}
    if (!volumes.some(v => v.token === (selRoot || "inbox"))) selRoot = "inbox";
    renderVolrow();
  }

  async function selectHost(name) {
    selMachine = name;
    selRoot = "inbox";
    showOther = false;
    cwd = "";
    treeKids = {};
    loadTreeOpen();
    loadViewPref();
    selName = null;
    selKind = null;
    curRaw = "";
    editing = false;
    destroyMd();
    renderMachines();
    renderBreadcrumb();
    updateTools();
    $("#content").style.display = "block";
    $("#content").innerHTML = "<div class=\"empty\">Select a file in the inbox.</div>";
    $("#filesHead").textContent = "Files · " + name;
    updateDestHint();
    await loadVolumes();
    await loadListing();
  }

  function bufToB64(buf) {
    const bytes = new Uint8Array(buf);
    let bin = "";
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    return btoa(bin);
  }
  async function uploadFiles(fileList) {
    if (!selMachine) { toast("Select a host first", true); return; }
    if (!rootIsWritable()) { toast("Read-only root — switch to inbox or Outbox", true); return; }
    const files = Array.from(fileList || []);
    if (!files.length) return;
    for (const file of files) {
      const dest = joinPath(cwd, file.name);
      try {
        const b64 = bufToB64(await file.arrayBuffer());
        const d = await api("/api/upload", {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: dest, content_b64: b64})
        });
        if (d.ok) toast("Uploaded " + file.name);
        else toast(d.error || "Upload failed", true);
      } catch (e) {
        toast("Upload failed", true);
      }
    }
    forgetListing(listCacheKey());
    await loadListing(true);
  }

  $("#search").addEventListener("input", e => { query = e.target.value.trim().toLowerCase(); renderMachines(); });
  $("#fileSearch").addEventListener("input", e => { fileQuery = e.target.value.trim().toLowerCase(); renderFiles(); });
  async function jumpToPath(raw) {
    if (!selMachine || !raw) return;
    const text = String(raw).trim();
    if (!text) return;
    try {
      const d = await api("/api/open-path?machine=" + encodeURIComponent(selMachine) +
        "&root=" + encodeURIComponent(selRoot || "inbox") +
        "&cwd=" + encodeURIComponent(cwd || "") +
        "&path=" + encodeURIComponent(text));
      if (!d.ok) { toast(d.error || "Could not open path", true); return; }
      selRoot = d.root || "inbox";
      cwd = d.dir || "";
      selName = null; curRaw = ""; editing = false;
      renderVolrow(); updateDestHint(); renderBreadcrumb();
      await loadListing(true);
      if (d.name) {
        openFile(d.name, d.kind || "file");
      } else {
        showEmptyPreview(selRoot === "inbox" ? "Select a file in the inbox." : "Select a file.");
        renderFiles();
      }
      $("#pathJump").value = "";
    } catch (err) {
      toast("Could not open path: " + String(err.message || err), true);
    }
  }
  $("#pathJump").addEventListener("keydown", e => {
    if (e.key === "Enter") { e.preventDefault(); jumpToPath(e.target.value); }
  });
  $("#themeToggle").addEventListener("click", () => {
    applyTheme(theme === "dark" ? "light" : "dark");
    remountMd();
  });
  $("#refreshBtn").addEventListener("click", () => loadListing(true));
  const treeBtn = $("#treeToggle");
  if (treeBtn) treeBtn.addEventListener("click", () => toggleTree());
  const gridBtn = $("#gridToggle");
  if (gridBtn) gridBtn.addEventListener("click", () => toggleGrid());
  const treeEl = $("#tree");
  if (treeEl) treeEl.addEventListener("click", e => {
    const node = e.target.closest(".tnode"); if (!node) return;
    const path = node.dataset.path || "";
    const tw = e.target.closest(".tw");
    if (tw) {
      const i = treeOpen.indexOf(path);
      if (i >= 0) treeOpen.splice(i, 1);
      else treeOpen.push(path);
      saveTreeOpen();
      renderTree();
      return;
    }
    if (treeOpen.indexOf(path) < 0) { treeOpen.push(path); saveTreeOpen(); }
    goDir(path);
  });
  $("#filters").addEventListener("click", e => {
    const b = e.target.closest(".chip"); if (!b) return;
    filter = b.dataset.filter || "all";
    $$("#filters .chip").forEach(c => c.classList.toggle("active", c === b));
    renderFiles();
  });
  function goDir(path) {
    cwd = path || "";
    selName = null; curRaw = ""; editing = false;
    showEmptyPreview("Select a file in the inbox.");
    loadListing();
  }
  $("#pathrow").addEventListener("click", e => {
    const b = e.target.closest("button"); if (!b) return;
    goDir(b.dataset.path || "");
  });
  $("#volrow").addEventListener("click", e => {
    const b = e.target.closest("button"); if (!b) return;
    const root = b.dataset.root || "";
    if (root === "__other__") { showOther = true; renderVolrow(); return; }
    selRoot = root || "inbox";
    cwd = "";
    treeKids = {};
    loadTreeOpen();
    loadViewPref();
    selName = null; curRaw = ""; editing = false;
    renderVolrow();
    updateDestHint();
    showEmptyPreview(selRoot === "inbox" ? "Select a file in the inbox." : "Select a file.");
    loadListing();
  });
  $("#breadcrumb").addEventListener("click", e => {
    const b = e.target.closest("button"); if (!b) return;
    if (b.dataset.kind === "host") { selRoot = "inbox"; goDir(""); return; }
    goDir(b.dataset.path || "");
  });
  $("#machines").addEventListener("click", e => {
    const row = e.target.closest(".mrow"); if (!row || !row.dataset.machine) return;
    selectHost(row.dataset.machine);
  });
  $("#filelist").addEventListener("click", e => {
    const more = e.target.closest("[data-more]");
    if (more) {
      e.preventDefault();
      e.stopPropagation();
      const item = more.closest(".fitem, .gitem");
      if (item) openCtx(e.clientX, e.clientY, item);
      return;
    }
    const item = e.target.closest(".fitem, .gitem"); if (!item) return;
    if (item.dataset.dir === "1") {
      cwd = joinPath(cwd, item.dataset.name);
      selName = null; curRaw = ""; editing = false;
      showEmptyPreview("Select a file in the inbox.");
      loadListing();
      return;
    }
    openFile(item.dataset.name, item.dataset.kind || "file");
  });
  function initPond() {
    if (!window.FilePond) {
      $("#fileInput").addEventListener("change", e => {
        uploadFiles(e.target.files);
        e.target.value = "";
      });
      const drop = $("#drop");
      ["dragenter","dragover"].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.add("over"); }));
      ["dragleave","drop"].forEach(ev => drop.addEventListener(ev, e => { e.preventDefault(); drop.classList.remove("over"); }));
      drop.addEventListener("drop", e => uploadFiles(e.dataTransfer && e.dataTransfer.files));
      return;
    }
    if (window.FilePondPluginFileValidateSize) {
      FilePond.registerPlugin(FilePondPluginFileValidateSize);
    }
    FilePond.setOptions({ credits: false });
    FilePond.create($("#fileInput"), {
      allowMultiple: true,
      instantUpload: true,
      maxFileSize: "256MB",
      credits: false,
      labelIdle: 'Drop files here or <span class="filepond--label-action">choose to upload</span>',
      server: {
        process: (fieldName, file, metadata, load, error, progress, abort) => {
          if (!selMachine) { error("Select a host first"); return; }
          if (!rootIsWritable()) { error("Read-only root"); return; }
          let cancelled = false;
          (async () => {
            try {
              const dest = joinPath(cwd, file.name);
              const b64 = bufToB64(await file.arrayBuffer());
              if (cancelled) return;
              const d = await api("/api/upload", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: dest, content_b64: b64})
              });
              if (!d.ok) throw new Error(d.error || "Upload failed");
              load(file.name);
              toast("Uploaded " + file.name);
              forgetListing(listCacheKey());
              loadListing(true);
            } catch (err) {
              error(String(err.message || err));
              toast("Upload failed", true);
            }
          })();
          return { abort: () => { cancelled = true; abort(); } };
        }
      }
    });
    $("#drop").classList.add("pond-on");
  }

  function closeCtx() {
    const m = $("#ctx");
    if (m) m.hidden = true;
  }
  function openCtx(x, y, item) {
    const m = $("#ctx");
    if (!m || !item) return;
    m.hidden = false;
    m.style.left = Math.min(x, window.innerWidth - 170) + "px";
    m.style.top = Math.min(y, window.innerHeight - 130) + "px";
    m.dataset.name = item.dataset.name || "";
    m.dataset.kind = item.dataset.kind || "file";
    m.dataset.dir = item.dataset.dir || "0";
    const writable = rootIsWritable();
    m.querySelectorAll("[data-act=rename], [data-act=trash]").forEach(b => {
      b.style.display = writable ? "" : "none";
    });
  }
  function hideNewBox() {
    const box = $("#newBox");
    if (box) box.classList.remove("on");
  }
  function showNewBox(kind, preset) {
    const box = $("#newBox");
    const inp = $("#newName");
    if (!box || !inp) return;
    box.dataset.kind = kind;
    box.classList.add("on");
    inp.value = preset || (kind === "dir" ? "untitled-folder" : "untitled.md");
    inp.focus();
    inp.select();
  }
  async function createNamed() {
    const box = $("#newBox");
    const inp = $("#newName");
    if (!box || !inp || !selMachine) return;
    const kind = box.dataset.kind || "file";
    const name = (inp.value || "").trim();
    if (!name) { toast("Name required", true); return; }
    if (name.indexOf("/") >= 0 || name.indexOf("\\") >= 0) { toast("Name cannot contain /", true); return; }
    const rel = joinPath(cwd, name);
    try {
      if (kind === "dir") {
        const d = await api("/api/mkdir", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: rel})});
        if (!d.ok) throw new Error(d.error || "mkdir failed");
        toast("Folder created");
      } else if (kind === "rename") {
        const src = joinPath(cwd, box.dataset.src || "");
        const d = await api("/api/rename", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: src, name: name})});
        if (!d.ok) throw new Error(d.error || "rename failed");
        toast("Renamed");
        if (selName === box.dataset.src) selName = name;
      } else {
        const d = await api("/api/upload", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: rel, content: ""})});
        if (!d.ok) throw new Error(d.error || "create failed");
        toast("File created");
      }
      hideNewBox();
      forgetListing(listCacheKey());
      await loadListing(true);
      if (kind === "file") {
        const ext = (name.split(".").pop() || "").toLowerCase();
        openFile(name, (ext === "md" || ext === "markdown" || ext === "txt") ? "md" : "code");
      }
    } catch (e) { toast(String(e.message || e), true); }
  }
  async function trashNamed(name) {
    if (!selMachine || !name) return;
    if (!confirm("Move “" + name + "” to trash?")) return;
    const rel = joinPath(cwd, name);
    try {
      const d = await api("/api/trash", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify({machine: selMachine, root: selRoot || "inbox", path: rel})});
      if (!d.ok) throw new Error(d.error || "delete failed");
      if (selName === name) { selName = null; selKind = null; curRaw = ""; destroyMd(); showEmptyPreview("Select a file."); }
      forgetListing(listCacheKey());
      await loadListing(true);
      toast("Moved to trash");
    } catch (e) { toast(String(e.message || e), true); }
  }
  const newFileBtn = $("#newFileBtn");
  if (newFileBtn) newFileBtn.addEventListener("click", () => showNewBox("file", "untitled.md"));
  const newDirBtn = $("#newDirBtn");
  if (newDirBtn) newDirBtn.addEventListener("click", () => showNewBox("dir", "untitled-folder"));
  const newOk = $("#newOk");
  if (newOk) newOk.addEventListener("click", () => createNamed());
  const newCancel = $("#newCancel");
  if (newCancel) newCancel.addEventListener("click", () => hideNewBox());
  const newName = $("#newName");
  if (newName) newName.addEventListener("keydown", e => {
    if (e.key === "Enter") { e.preventDefault(); createNamed(); }
    if (e.key === "Escape") { e.preventDefault(); hideNewBox(); }
  });
  const langPick = $("#langPick");
  if (langPick) langPick.addEventListener("change", () => {
    const id = langPick.value || "plaintext";
    saveLang(id);
    if (hasCM() && BridgeCM.setLanguage) BridgeCM.setLanguage(id);
  });
  const ctx = $("#ctx");
  if (ctx) ctx.addEventListener("click", e => {
    const b = e.target.closest("[data-act]"); if (!b) return;
    const name = ctx.dataset.name || "";
    const act = b.dataset.act;
    closeCtx();
    if (act === "download") {
      const a = document.createElement("a");
      a.href = fileUrl(joinPath(cwd, name), "&download=1");
      a.download = name;
      a.click();
    } else if (act === "rename") {
      const box = $("#newBox");
      if (box) box.dataset.src = name;
      showNewBox("rename", name);
    } else if (act === "trash") {
      trashNamed(name);
    }
  });
  document.addEventListener("click", e => {
    if (!e.target.closest("#ctx") && !e.target.closest("[data-more]")) closeCtx();
  });

  $("#editBtn").addEventListener("click", () => {
    editing = true;
    if (selKind === "md") {
      if (!showMdEditor(curRaw)) {
        $("#editor").value = curRaw;
        $("#editor").focus();
      }
    } else if (!showCode(curRaw, false)) {
      $("#editor").value = curRaw;
      $("#editor").focus();
    }
    updateTools();
  });
  $("#cancelBtn").addEventListener("click", () => {
    editing = false;
    if (selKind === "md") {
      if (!showMdViewer(curRaw)) $("#content").style.display = "block";
    } else if (selKind === "code" || selKind === "file") {
      if (!showCode(curRaw, true)) $("#content").style.display = "block";
    }
    updateTools();
  });
  $("#copyBtn").addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(curRaw); toast("Copied"); } catch (_) { toast("Copy failed", true); }
  });
  $("#downloadBtn").addEventListener("click", () => {
    if (!selName) return;
    const a = document.createElement("a");
    a.href = fileUrl(joinPath(cwd, selName), "&download=1");
    a.download = selName;
    a.click();
  });
  $("#saveBtn").addEventListener("click", async () => {
    if (!selName) return;
    if (!rootIsWritable()) { toast("Read-only root", true); return; }
    const btn = $("#saveBtn"); const old = btn.textContent;
    btn.disabled = true; btn.textContent = "Saving…";
    try {
      const text = (selKind === "md") ? mdText() : codeText();
      const body = {machine: selMachine, root: selRoot || "inbox", path: joinPath(cwd, selName), content: text};
      const d = await api("/api/upload", {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(body)});
      if (!d.ok) throw new Error(d.error || "save failed");
      curRaw = text;
      editing = false;
      await openFile(selName, selKind);
      toast("Saved");
    } catch (e) { toast("Save failed", true); }
    finally { btn.disabled = false; btn.textContent = old; }
  });
  document.addEventListener("keydown", e => {
    const tag = ((e.target && e.target.tagName) || "").toLowerCase();
    const inCm = !!(e.target && e.target.closest && e.target.closest(".cm-editor"));
    const typing = tag === "input" || tag === "textarea" || (e.target && e.target.isContentEditable && !inCm) || (inCm && editing);
    if (e.key === "Escape" && editing) {
      editing = false;
      if (selKind === "md") {
        if (!showMdViewer(curRaw)) $("#content").style.display = "block";
      } else if (selKind === "code" || selKind === "file") {
        if (!showCode(curRaw, true)) $("#content").style.display = "block";
      }
      updateTools();
      return;
    }
    if (typing || editing) return;
    if (e.key === "Backspace" || (e.altKey && e.key === "ArrowUp")) {
      e.preventDefault();
      goParent();
    } else if (e.key === "[") {
      e.preventDefault();
      goHist(-1);
    } else if (e.key === "]") {
      e.preventDefault();
      goHist(1);
    } else if (e.key === "b" || e.key === "B") {
      e.preventDefault();
      toggleTree();
    }
  });

  async function refreshHosts() {
    try { mesh = await api("/api/machines"); }
    catch (err) { mesh = {node:"", peers:[], sessions:[], offline:true, uptime_s:0}; }
    if (selMachine === null) {
      const list = machineList();
      const self = list.find(m => m.you && machineStatus(m) !== "offline");
      const first = self || list.find(m => machineStatus(m) !== "offline") || list[0];
      if (first) { await selectHost(first.name); return; }
    }
    renderMachines();
  }

  function initSplitters() {
    const shell = $("#shell");
    if (!shell) return;
    const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
    let hosts = 240, files = 280;
    try {
      const saved = JSON.parse(localStorage.getItem("bp-split") || "null");
      if (saved && saved.hosts) hosts = saved.hosts;
      if (saved && saved.files) files = saved.files;
    } catch (e) {}
    const apply = () => {
      const maxHosts = Math.max(160, shell.clientWidth - 6 - 200 - 6 - 240);
      hosts = clamp(hosts, 160, maxHosts);
      const maxFiles = Math.max(200, shell.clientWidth - hosts - 6 - 6 - 240);
      files = clamp(files, 200, maxFiles);
      shell.style.setProperty("--w-hosts", hosts + "px");
      shell.style.setProperty("--w-files", files + "px");
    };
    const persist = () => { try { localStorage.setItem("bp-split", JSON.stringify({hosts, files})); } catch (e) {} };
    apply();
    const bind = (el, which) => {
      if (!el) return;
      el.addEventListener("mousedown", ev => {
        ev.preventDefault();
        el.classList.add("dragging");
        document.body.classList.add("resizing");
        const startX = ev.clientX;
        const start = which === "hosts" ? hosts : files;
        const move = e => {
          const dx = e.clientX - startX;
          if (which === "hosts") hosts = start + dx;
          else files = start + dx;
          apply();
        };
        const up = () => {
          el.classList.remove("dragging");
          document.body.classList.remove("resizing");
          document.removeEventListener("mousemove", move);
          document.removeEventListener("mouseup", up);
          persist();
        };
        document.addEventListener("mousemove", move);
        document.addEventListener("mouseup", up);
      });
    };
    bind($("#splitHosts"), "hosts");
    bind($("#splitFiles"), "files");
    window.addEventListener("resize", apply);
  }
  applyTheme(theme);
  initSplitters();
  initPond();
  refreshHosts();
  setInterval(refreshHosts, 8000);
})();
</script>
</body>
</html>
'''
