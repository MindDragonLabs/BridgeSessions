"""BridgePanel — HTML/CSS/JS template (elegant dark/light SPA, full features)."""
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
  .hdr-right { margin-left: auto; display: flex; align-items: center; gap: 12px; }
  .summary { font-size: 12.5px; color: var(--muted); }
  .summary b { color: var(--text); font-weight: 600; }
  .theme-toggle { width: 32px; height: 32px; border-radius: 8px; background: transparent; border: 1px solid var(--border); color: var(--muted); cursor: pointer; font-size: 15px; }
  .theme-toggle:hover { background: var(--surface2); color: var(--text); }
  .avatar { width: 28px; height: 28px; border-radius: 50%; background: var(--accent-soft); color: var(--accent); display: grid; place-items: center; font-weight: 600; font-size: 12px; }

  .shell { flex: 1; display: grid; grid-template-columns: 220px 240px minmax(0,1fr); min-height: 0; }
  .col { min-height: 0; overflow-y: auto; background: var(--surface); display: flex; flex-direction: column; }
  .col.machines { border-right: 1px solid var(--border); }
  .col.sessions { border-right: 1px solid var(--border); }
  .col-head { padding: 10px 14px 8px; font-size: 11px; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; color: var(--faint); display: flex; align-items: center; justify-content: space-between; }
  .col-head .count { font-family: var(--mono); font-weight: 400; letter-spacing: 0; }
  .add-btn { width: 22px; height: 22px; border-radius: 6px; background: var(--accent-soft); color: var(--accent); border: none; cursor: pointer; font-size: 15px; line-height: 1; }
  .add-btn:hover { background: var(--accent); color: #fff; }

  .mrow { display: flex; align-items: center; gap: 10px; padding: 9px 13px; cursor: pointer; border-left: 2px solid transparent; }
  .mrow:hover { background: var(--surface2); }
  .mrow.active { background: var(--accent-soft); border-left-color: var(--accent); }
  .mrow .dot { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; background: var(--faint); }
  .mrow .dot.up { background: var(--ok); }
  .mrow .dot.you { background: var(--accent); }
  .mrow .dot.offline { background: var(--faint); opacity: 0.55; }
  .mrow .dot.stale { background: var(--warn); }
  .mrow .mi { flex: 1; min-width: 0; }
  .mrow .mn { font-size: 13.5px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mrow .ma { font-family: var(--mono); font-size: 10px; color: var(--faint); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mrow .you { font-size: 10px; color: var(--accent); text-transform: uppercase; letter-spacing: .06em; }
  .mrow .n { font-family: var(--mono); font-size: 11px; color: var(--faint); }

  .srow { display: flex; align-items: center; gap: 9px; padding: 9px 13px; cursor: pointer; border-left: 2px solid transparent; }
  .srow:hover { background: var(--surface2); }
  .srow.active { background: var(--accent-soft); border-left-color: var(--accent); }
  .srow .sdot { width: 7px; height: 7px; border-radius: 50%; flex-shrink: 0; background: var(--faint); }
  .srow .sdot.live { background: var(--ok); }
  .srow .sname { flex: 1; min-width: 0; font-size: 13px; font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .srow .sstate { font-family: var(--mono); font-size: 10px; color: var(--faint); }
  .srow .skind { font-family: var(--mono); font-size: 9px; padding: 1px 6px; border-radius: 999px; flex-shrink: 0; letter-spacing: 0.02em; }
  .srow .skind.k-harness { background: var(--accent-soft); color: var(--accent); border: 1px solid var(--accent); }
  .srow .skind.k-probe { background: var(--surface2); color: var(--faint); border: 1px solid var(--border); }

  .sgroup-head { padding: 12px 14px 4px; font-size: 10.5px; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; color: var(--faint); display: flex; align-items: center; }
  .sgroup-head .cnt { margin-left: auto; font-family: var(--mono); font-weight: 400; }
  .ssub-head { padding: 8px 14px 2px 24px; font-size: 10.5px; font-weight: 600; color: var(--muted); display: flex; align-items: center; }
  .ssub-head .cnt { margin-left: auto; font-family: var(--mono); font-weight: 400; color: var(--faint); }
  .srow.child { padding-left: 28px; }
  .empty-sub { color: var(--faint); font-size: 11.5px; padding: 4px 14px 4px 24px; }
  .cua-row { display: flex; align-items: center; gap: 9px; padding: 8px 13px 8px 28px; font-size: 12.5px; color: var(--muted); }
  .probe-summary { color: var(--faint); font-size: 11px; padding: 10px 14px; border-top: 1px solid var(--border); margin-top: 6px; }

  .work { min-width: 0; min-height: 0; display: flex; flex-direction: column; background: var(--bg); }
  .work-top { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 20px; border-bottom: 1px solid var(--border); }
  .breadcrumb { font-family: var(--mono); font-size: 12px; color: var(--faint); min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .breadcrumb .cur { color: var(--accent); }
  .breadcrumb .sep { margin: 0 6px; }
  .toolbar { display: flex; gap: 8px; flex-shrink: 0; }
  .btn { padding: 6px 13px; border-radius: 8px; font-size: 12.5px; font-weight: 500; background: var(--surface2); color: var(--text); border: 1px solid var(--border); cursor: pointer; }
  .btn:hover { border-color: var(--muted); }
  .btn.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
  .btn.ghost { background: transparent; }
  .btn:disabled { opacity: .5; cursor: default; }

  .tabbar { display: flex; gap: 4px; padding: 0 20px; border-bottom: 1px solid var(--border); }
  .tab { padding: 9px 14px; font-size: 13px; font-weight: 500; color: var(--muted); background: none; border: none; border-bottom: 2px solid transparent; cursor: pointer; margin-bottom: -1px; }
  .tab:hover { color: var(--text); }
  .tab.active { color: var(--text); border-bottom-color: var(--accent); font-weight: 600; }

  .work-body { flex: 1; min-height: 0; position: relative; }
  .tab-pane { position: absolute; inset: 0; display: none; }
  .tab-pane.active { display: flex; }
  #pane-docs { display: flex; }
  #pane-docs.active { display: grid; grid-template-columns: 200px minmax(0,1fr); }
  .filelist { border-right: 1px solid var(--border); overflow-y: auto; padding: 10px 8px; }
  .filelist .group { font-size: 10.5px; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; color: var(--faint); padding: 10px 8px 6px; }
  .fitem { display: flex; align-items: center; gap: 8px; padding: 7px 9px; border-radius: 7px; cursor: pointer; font-size: 13px; color: var(--muted); }
  .fitem:hover { background: var(--surface2); color: var(--text); }
  .fitem.active { background: var(--accent-soft); color: var(--text); }
  .fitem .sz { margin-left: auto; font-family: var(--mono); font-size: 10px; color: var(--faint); }
  .content-wrap { min-width: 0; min-height: 0; overflow-y: auto; padding: 22px 26px; }
  .content { max-width: 760px; }
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

  .output { font-family: var(--mono); font-size: 12.5px; line-height: 1.65; color: var(--text); padding: 16px 20px; white-space: pre-wrap; word-break: break-word; overflow-y: auto; min-height: 0; }
  .output-head { display: flex; align-items: center; gap: 10px; padding: 10px 20px; border-bottom: 1px solid var(--border); }
  .follow { font-family: var(--mono); font-size: 11px; padding: 4px 10px; border-radius: 999px; border: 1px solid var(--border); background: var(--surface2); color: var(--muted); cursor: pointer; }
  .follow.on { color: var(--accent); border-color: var(--accent); }

  .files { padding: 20px; overflow-y: auto; }
  .files h2 { font-size: 16px; font-weight: 650; margin-bottom: 16px; }
  .rf-card { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 14px; margin-bottom: 14px; max-width: 640px; }
  .rf-card .tt { font-size: 12px; font-weight: 600; margin-bottom: 10px; }
  .rf-row { display: flex; gap: 8px; }
  .rf-row input { flex: 1; height: 34px; background: var(--surface2); border: 1px solid var(--border); border-radius: 7px; padding: 0 10px; font-family: var(--mono); font-size: 12.5px; outline: none; }
  .rf-row input:focus { border-color: var(--accent); }
  .rf-textarea { width: 100%; min-height: 90px; margin-top: 8px; background: var(--surface2); border: 1px solid var(--border); border-radius: 7px; padding: 10px; font-family: var(--mono); font-size: 12.5px; outline: none; resize: vertical; }
  .rf-hint { font-size: 11.5px; color: var(--faint); margin-top: 8px; }
  .rf-hint code { font-family: var(--mono); background: var(--surface2); padding: 1px 5px; border-radius: 4px; }
  .rf-result { max-width: 640px; }
  .rf-result .doc { margin-top: 4px; }

  .modal-backdrop { position: fixed; inset: 0; background: rgba(0,0,0,0.5); display: none; align-items: center; justify-content: center; z-index: 50; }
  .modal-backdrop.open { display: flex; }
  .modal { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 20px; width: 360px; box-shadow: var(--shadow); }
  .modal h3 { font-size: 15px; font-weight: 650; margin-bottom: 16px; }
  .modal label { display: block; font-size: 11px; font-weight: 600; letter-spacing: .06em; text-transform: uppercase; color: var(--faint); margin: 12px 0 5px; }
  .modal input, .modal select { width: 100%; height: 34px; background: var(--surface2); border: 1px solid var(--border); border-radius: 7px; padding: 0 10px; font-size: 13px; outline: none; }
  .modal input:focus, .modal select:focus { border-color: var(--accent); }
  .modal select { appearance: none; background-image: linear-gradient(45deg, transparent 50%, var(--muted) 50%), linear-gradient(135deg, var(--muted) 50%, transparent 50%); background-position: calc(100% - 15px) 14px, calc(100% - 10px) 14px; background-size: 5px 5px; background-repeat: no-repeat; padding-right: 30px; cursor: pointer; }
  .modal select option { background: var(--surface); color: var(--text); }
  .modal .field-hint { font-size: 11px; color: var(--faint); margin-top: 5px; line-height: 1.4; }
  .modal .check { display: flex; align-items: center; gap: 8px; margin: 14px 0 0; cursor: pointer; }
  .modal .check input[type=checkbox] { width: auto; height: auto; accent-color: var(--accent); }
  .modal .check span { font-size: 13px; color: var(--muted); }
  .modal .actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 18px; }

  .toast { position: fixed; bottom: 22px; left: 50%; transform: translate(-50%, 12px); background: var(--text); color: var(--bg); padding: 9px 16px; border-radius: 9px; font-size: 13px; font-weight: 500; box-shadow: var(--shadow); opacity: 0; pointer-events: none; transition: opacity .2s, transform .2s; z-index: 60; }
  .toast.show { opacity: 1; transform: translate(-50%, 0); }
  .toast.err { background: var(--danger); color: #fff; }

  @media (max-width: 800px) {
    .shell { grid-template-columns: 180px minmax(0,1fr); }
    .col.sessions { display: none; }
    #pane-docs.active { grid-template-columns: 1fr; }
    .filelist { display: none; }
    .search { display: none; }
  }
</style>
</head>
<body>

<header>
  <div class="brand"><span class="mark">B</span><span class="name">Bridge Panel</span><span class="sub">__BUILD_TAG__</span></div>
  <div class="search"><span class="icon">&#8962;</span><input id="search" placeholder="Filter machines…" aria-label="Filter machines"></div>
  <div class="hdr-right">
    <span class="summary" id="summary"></span>
    <button class="theme-toggle" id="themeToggle" title="Toggle theme" aria-label="Toggle theme">&#9680;</button>
    <div class="avatar" id="nodeAvatar">·</div>
  </div>
</header>

<div class="shell">
  <aside class="col machines">
    <div class="col-head">Machines <span class="count" id="machineCount"></span></div>
    <div id="machines"></div>
  </aside>
  <aside class="col sessions">
    <div class="col-head"><span id="sessionsHead">Sessions</span><button class="add-btn" id="newSessionBtn" title="New session">+</button></div>
    <div id="sessions"></div>
  </aside>
  <main class="work">
    <div class="work-top">
      <div class="breadcrumb" id="breadcrumb"></div>
      <div class="toolbar">
        <button class="btn" id="editBtn" style="display:none">Edit</button>
        <button class="btn primary" id="saveBtn" style="display:none">Save</button>
        <button class="btn ghost" id="cancelBtn" style="display:none">Cancel</button>
        <button class="btn ghost" id="copyBtn" style="display:none">Copy</button>
      </div>
    </div>
    <nav class="tabbar" id="tabbar">
      <button class="tab active" data-tab="docs">Documents</button>
      <button class="tab" data-tab="output">Output</button>
      <button class="tab" data-tab="files">Files</button>
    </nav>
    <div class="work-body">
      <div class="tab-pane active" id="pane-docs">
        <div class="filelist" id="filelist"></div>
        <div class="content-wrap">
          <div class="content" id="content"><div class="empty">Select a session to browse documents.</div></div>
          <textarea class="editor" id="editor" style="display:none" spellcheck="false" aria-label="Markdown editor"></textarea>
        </div>
      </div>
      <div class="tab-pane" id="pane-output">
        <div class="output-head">
          <span class="breadcrumb" id="outputLabel">No session selected</span>
          <span style="flex:1"></span>
          <button class="follow on" id="followBtn">follow ●</button>
        </div>
        <pre class="output" id="output"></pre>
      </div>
      <div class="tab-pane" id="pane-files">
        <div class="files" id="files"><div class="empty">Select a machine to browse its files.</div></div>
      </div>
    </div>
  </main>
</div>

<div class="modal-backdrop" id="createModal">
  <div class="modal">
    <h3>New session</h3>
    <label for="cmMachine">Machine</label>
    <input id="cmMachine" readonly>
    <label for="cmName">Session name</label>
    <input id="cmName" placeholder="e.g. build">
    <label for="cmHarness">Harness</label>
    <select id="cmHarness">
      <option value="bash">Bash</option>
      <option value="hermes">Hermes</option>
      <option value="claude">Claude Code</option>
      <option value="codex">Codex CLI</option>
      <option value="kimi">Kimi Code</option>
      <option value="commandcode">CommandCode</option>
      <option value="opencode">OpenCode</option>
      <option value="cursor">Cursor</option>
    </select>
    <label for="cmCommand">Command</label>
    <input id="cmCommand" value="/bin/bash -l">
    <div class="field-hint">Auto-filled from the harness. Terminal size is detected on connect (like SSH).</div>
    <label class="check"><input type="checkbox" id="cmYolo" checked><span>Auto-approve (--yolo / -p / --auto per harness)</span></label>
    <div class="actions">
      <button class="btn ghost" id="cmCancel">Cancel</button>
      <button class="btn primary" id="cmSubmit">Create</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
(function(){
  'use strict';
  const base = location.pathname.replace(/\/$/, '');
  let mesh = {node:'', peers:[], sessions:[], offline:true, uptime_s:0};
  let fsTree = {sessions:[]};
  let selMachine = null;
  let selSession = null;
  let curFile = null, curType = null, curRaw = '', editing = false;
  let tab = 'docs';
  let outputOffset = 0, outputFollow = true, outputTimer = null;
  let query = '';
  let theme = localStorage.getItem('bp-theme') === 'light' ? 'light' : 'dark';

  const $ = s => document.querySelector(s);
  const $$ = s => Array.from(document.querySelectorAll(s));
  const esc = s => String(s).replace(/[&<>"']/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

  const HARNESS_CATALOG = {
    bash:       { name: 'Bash',        cmd: '/bin/bash -l' },
    hermes:     { name: 'Hermes',      cmd: 'hermes --tui' },
    claude:     { name: 'Claude Code', cmd: 'claude' },
    codex:      { name: 'Codex CLI',   cmd: 'codex' },
    kimi:       { name: 'Kimi Code',   cmd: 'kimi' },
    commandcode:{ name: 'CommandCode', cmd: 'commandcode' },
    opencode:   { name: 'OpenCode',    cmd: 'opencode' },
    cursor:     { name: 'Cursor',      cmd: 'cursor' },
  };
  const YOLO_FLAGS = {
    hermes: ' --yolo', claude: ' -p', codex: ' --yolo',
    kimi: ' -p', commandcode: '', opencode: ' --auto',
    cursor: ' --yolo', bash: '',
  };
  function buildHarnessCmd(harness, yolo) {
    let cmd = HARNESS_CATALOG[harness] ? HARNESS_CATALOG[harness].cmd : '/bin/bash -l';
    if (yolo && YOLO_FLAGS[harness]) cmd += YOLO_FLAGS[harness];
    return cmd;
  }

  async function api(path, opts) {
    const r = await fetch(base + path, opts);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }
  function stripAnsi(s) {
    return s.replace(/\x1b\[[0-9;?]*[A-Za-z@-~]/g, '').replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '');
  }

  let toastTimer;
  function toast(msg, isErr) {
    const t = $('#toast');
    t.textContent = msg; t.className = 'toast show' + (isErr ? ' err' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.className = 'toast'; }, 1800);
  }
  function applyTheme(t) {
    theme = (t === 'light') ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem('bp-theme', theme); } catch (e) {}
    $('#themeToggle').textContent = theme === 'dark' ? '\u25D0' : '\u25D1';
  }

  // ── Machines ──
  // status: 'you' | 'healthy' | 'offline' | 'stale'
  function machineStatus(m) {
    if (m.you) return 'you';
    if (m.status === 'stale') return 'stale';
    if (m.healthy === false || m.status === 'offline' || m.status === 'no-pong') return 'offline';
    return 'healthy';
  }
  function machineList() {
    const out = [];
    if (mesh.node) out.push({name: mesh.node, addr: '', you: true, healthy: !mesh.offline, sessions: mesh.sessions || []});
    for (const p of (mesh.peers || [])) out.push({name: p.name, addr: p.addr || '', you: false, healthy: !!p.healthy, status: p.status || '', source: p.source || '', sessions: p.sessions || []});
    // Alphabetical by machine name (stable, predictable ordering).
    out.sort((a, b) => a.name.localeCompare(b.name, undefined, {sensitivity:'base'}));
    return out;
  }
  function renderMachines() {
    const list = machineList().filter(m => !query || m.name.toLowerCase().includes(query));
    $('#machines').innerHTML = !list.length
      ? '<div class="empty">No machines match "' + esc(query) + '".</div>'
      : list.map(m => {
        const st = machineStatus(m);
        const dotCls = st === 'you' ? 'you' : (st === 'healthy' ? 'up' : st);
        const sub = st === 'offline' ? 'offline · seed' : (st === 'stale' ? 'ephemeral · idle' : (m.addr ? m.addr : 'local node'));
        return '<div class="mrow' + (selMachine === m.name ? ' active' : '') + '" data-machine="' + esc(m.name) + '">' +
          '<span class="dot ' + dotCls + '"></span>' +
          '<div class="mi"><div class="mn">' + esc(m.name) + '</div>' +
          '<div class="ma">' + esc(sub) + '</div></div>' +
          (m.you ? '<span class="you">you</span>' : '') +
          '<span class="n">' + liveCount(m) + '</span></div>';
      }).join('');
    $('#machineCount').textContent = list.length + ' shown';
    if (mesh.offline && !mesh.node) $('#summary').innerHTML = '<b>mesh offline</b>';
    else $('#summary').innerHTML = '<b>' + (mesh.peers ? mesh.peers.length : 0) + '</b> peers connected';
    $('#nodeAvatar').textContent = (mesh.node || '?').charAt(0).toUpperCase();
  }

  // ── Sessions ──
  function isLiveState(st) {
    return st === 'running' || st === 'detached' || st === 'attached' || st === 'live';
  }
  function liveCount(m) {
    // Only sessions that are actually alive count as workload. Dead/probe
    // records must not make a host look busier than it is.
    return (m.sessions || []).filter(s => isLiveState(s.state)).length;
  }
  function sessionsFor(name) {
    const isLocal = !name || name === mesh.node || name === '(local)';
    const out = [];
    if (isLocal) {
      const fs = (fsTree.sessions || []).map(s => ({name: s.name, live: !!s.live, local: true, kind: s.kind || '', comms: s.comms || [], documents: s.documents || []}));
      out.push(...fs);
      const names = new Set(out.map(x => x.name));
      for (const s of (mesh.sessions || [])) {
        if (!names.has(s.name)) out.push({name: s.name, live: isLiveState(s.state), local: true, kind: s.kind || '', state: s.state || '', command: s.command || '', comms: [], documents: []});
      }
    } else {
      const peer = (mesh.peers || []).find(p => p.name === name);
      for (const s of (peer ? peer.sessions || [] : [])) {
        out.push({name: s.name, live: isLiveState(s.state), local: false, kind: s.kind || '', state: s.state || '', command: s.command || '', comms: [], documents: []});
      }
    }
    out.sort((a, b) => (b.live - a.live) || a.name.localeCompare(b.name));
    return out;
  }
  function renderSessions() {
    $('#sessionsHead').textContent = selMachine ? ('Sessions · ' + selMachine) : 'Sessions';
    if (!selMachine) { $('#sessions').innerHTML = ''; return; }
    const list = sessionsFor(selMachine);
    if (!list.length) { $('#sessions').innerHTML = '<div class="empty">No sessions. Create one with +.</div>'; return; }

    const EPHEMERAL = /^cmd-\d+-\d+$|^run-script-|^health-bs-health-/;
    const users = [], bots = [], probes = [];
    for (const s of list) {
      if (s.kind === 'probe' || EPHEMERAL.test(s.name)) { probes.push(s); continue; }
      const h = harnessOf(s.command);
      if (s.kind === 'harness' || h) { s.harness = h || 'other'; bots.push(s); }
      else { s.harness = shellOf(s.command); users.push(s); }
    }

    // CUA computer-use helper is a bot service, not a terminal session.
    const isLocal = !selMachine || selMachine === mesh.node || selMachine === '(local)';
    const cua = isLocal ? !!mesh.cua : !!((mesh.peers || []).find(p => p.name === selMachine) || {}).cua;

    const groupBy = arr => {
      const m = new Map();
      for (const s of arr) { const k = s.harness || 'other'; if (!m.has(k)) m.set(k, []); m.get(k).push(s); }
      return Array.from(m.entries()).sort((a, b) => a[0].localeCompare(b[0]));
    };
    const sessionRow = (s, child) =>
      '<div class="srow' + (child ? ' child' : '') + (selSession === s.name ? ' active' : '') + '" data-session="' + esc(s.name) + '">' +
      '<span class="sdot' + (s.live ? ' live' : '') + '"></span>' +
      '<span class="sname">' + esc(s.name) + '</span>' +
      '<span class="sstate">' + esc(s.state || (s.live ? 'live' : (s.local ? 'stored' : ''))) + '</span></div>';

    let html = '';
    const renderGroup = (title, arr) => {
      const groups = groupBy(arr);
      html += '<div class="sgroup-head">' + esc(title) + '<span class="cnt">' + arr.length + '</span></div>';
      if (!groups.length) { html += '<div class="empty-sub">none</div>'; return; }
      for (const [h, ss] of groups) {
        ss.sort((a, b) => (b.live - a.live) || a.name.localeCompare(b.name));
        html += '<div class="ssub-head">' + esc(h) + '<span class="cnt">' + ss.length + '</span></div>';
        for (const s of ss) html += sessionRow(s, true);
      }
    };

    renderGroup('User', users);
    renderGroup('Bots', bots);
    if (cua) html += '<div class="ssub-head">cua<span class="cnt">1</span></div>' +
      '<div class="cua-row"><span style="width:7px;height:7px;border-radius:50%;background:var(--ok);flex-shrink:0"></span>' +
      '<span>computer-use helper</span><span class="sstate">active</span></div>';
    if (probes.length) html += '<div class="probe-summary">' + probes.length + ' internal probe' + (probes.length === 1 ? '' : 's') + ' (hidden)</div>';

    $('#sessions').innerHTML = html;
  }
  function harnessOf(cmd) {
    if (!cmd) return null;
    const base = cmd.trim().split(/\s+/)[0].toLowerCase().split('/').pop();
    for (const k of ['hermes', 'claude', 'codex', 'kimi', 'commandcode', 'opencode', 'cursor']) {
      if (base.startsWith(k)) return k;
    }
    return null;
  }
  function shellOf(cmd) {
    if (!cmd) return 'shell';
    const base = cmd.trim().split(/\s+/)[0].toLowerCase().split('/').pop();
    return /^(bash|zsh|sh|fish|ksh)$/.test(base) ? base : 'shell';
  }

  // ── Files (docs) ──
  function renderFiles() {
    if (!selSession) { $('#filelist').innerHTML = ''; return; }
    const s = sessionsFor(selMachine).find(x => x.name === selSession);
    const docs = s ? s.documents : [], comms = s ? s.comms : [];
    if (!docs.length && !comms.length) {
      $('#filelist').innerHTML = '<div class="empty">' + (s && !s.local ? 'Remote files not exposed here — use Files tab.' : 'No documents.') + '</div>';
      return;
    }
    let html = '';
    if (comms.length) { html += '<div class="group">Comms</div>'; for (const f of comms) html += fileRow(f, 'comms'); }
    if (docs.length) { html += '<div class="group">Documents</div>'; for (const f of docs) html += fileRow(f, 'documents'); }
    $('#filelist').innerHTML = html;
  }
  function fileRow(f, type) {
    const active = (curFile === f.name && curType === type) ? ' active' : '';
    const kb = f.size != null ? (f.size >= 1024 ? (f.size/1024).toFixed(1) + 'k' : f.size + 'b') : '';
    return '<div class="fitem' + active + '" data-file="' + esc(f.name) + '" data-type="' + type + '">' +
      '<span>&#128196;</span><span>' + esc(f.name) + '</span><span class="sz">' + kb + '</span></div>';
  }

  // ── Content ──
  async function loadContent() {
    if (!selSession || !curFile) return;
    try {
      const d = await api('/api/content?session=' + encodeURIComponent(selSession) + '&type=' + encodeURIComponent(curType) + '&name=' + encodeURIComponent(curFile));
      curRaw = d.raw || '';
      $('#content').innerHTML = d.html || '<div class="empty">(empty document)</div>';
    } catch (err) { $('#content').innerHTML = '<div class="empty">Could not load document.</div>'; }
  }
  function updateTools() {
    const hasFile = !!curFile;
    $('#editBtn').style.display = hasFile && !editing ? '' : 'none';
    $('#saveBtn').style.display = editing ? '' : 'none';
    $('#cancelBtn').style.display = editing ? '' : 'none';
    $('#copyBtn').style.display = hasFile && !editing ? '' : 'none';
    $('#content').style.display = editing ? 'none' : 'block';
    $('#editor').style.display = editing ? 'block' : 'none';
  }
  function renderBreadcrumb() {
    const parts = [];
    if (selMachine) parts.push(selMachine);
    if (selSession) parts.push(selSession);
    if (curFile) parts.push(curFile);
    $('#breadcrumb').innerHTML = !parts.length ? '' : parts.map((p, i) =>
      (i === parts.length - 1) ? '<span class="cur">' + esc(p) + '</span>' : '<span>' + esc(p) + '</span><span class="sep">/</span>'
    ).join('');
  }

  // ── Tabs ──
  function switchTab(name) {
    tab = name;
    $$('#tabbar .tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));
    $$('.tab-pane').forEach(p => p.classList.toggle('active', p.id === 'pane-' + name));
    if (outputTimer) { clearInterval(outputTimer); outputTimer = null; }
    if (name === 'output') startOutput();
    if (name === 'files') renderRemoteFiles();
    if (name === 'docs') updateTools();
  }

  // ── Output ──
  function startOutput() {
    $('#output').textContent = '';
    outputOffset = 0;
    delete $('#output').dataset.err;
    if (selSession) {
      $('#outputLabel').textContent = (selMachine || '') + ' / ' + selSession;
      pollOutput();
      outputTimer = setInterval(pollOutput, 2000);
    } else {
      $('#outputLabel').textContent = 'Select a session to see live output';
      $('#output').textContent = '';
    }
  }
  async function pollOutput() {
    if (!selSession || tab !== 'output') return;
    try {
      let url = '/api/output?session=' + encodeURIComponent(selSession) + '&since=' + outputOffset;
      if (selMachine) url += '&machine=' + encodeURIComponent(selMachine);
      const d = await api(url);
      if (d.error) {
        if (!$('#output').dataset.err) { $('#output').textContent = '(output unavailable: ' + d.error + ')'; $('#output').dataset.err = '1'; }
        return;
      }
      delete $('#output').dataset.err;
      if (d.reset) $('#output').textContent = '';
      if (d.text) {
        if (d.remote) $('#output').textContent = stripAnsi(d.text);
        else $('#output').textContent += stripAnsi(d.text);
        if (outputFollow) $('#output').scrollTop = $('#output').scrollHeight;
      }
      if (typeof d.offset === 'number') outputOffset = d.offset;
    } catch (_) {}
  }

  // ── Remote files ──
  function renderRemoteFiles() {
    const fl = $('#files');
    if (!selMachine) { fl.innerHTML = '<div class="empty">Select a machine to browse its files.</div>'; return; }
    const peer = selMachine;
    const isLocal = peer === mesh.node || peer === '(local)';
    if (isLocal) { fl.innerHTML = '<div class="empty">Local files live under Documents. Use Files for remote peers.</div>'; return; }
    fl.innerHTML =
      '<h2>Files on ' + esc(peer) + '</h2>' +
      '<div class="rf-card"><div class="tt">View a file</div>' +
      '<div class="rf-row"><input id="rfPath" placeholder="/path/to/file.md"><button class="btn primary" id="rfFetch">View</button></div>' +
      '<div class="rf-hint">Enter a path on ' + esc(peer) + '. Markdown renders automatically.</div></div>' +
      '<div class="rf-card"><div class="tt">Upload text to ' + esc(peer) + '</div>' +
      '<div class="rf-row"><input id="rfUploadPath" placeholder="report.md (relative to received/)"></div>' +
      '<textarea class="rf-textarea" id="rfUploadContent" placeholder="Type or paste content…"></textarea>' +
      '<div class="rf-row" style="margin-top:8px"><button class="btn primary" id="rfUpload">Upload</button></div>' +
      '<div class="rf-hint">Sends via <code>bs file send</code> — lands under received/.</div></div>' +
      '<div class="rf-result" id="rfResult"></div>';
    $('#rfFetch').addEventListener('click', async () => {
      const p = $('#rfPath').value.trim(); if (!p) return;
      const res = $('#rfResult');
      res.innerHTML = '<div class="rf-hint">Fetching ' + esc(p) + '…</div>';
      try {
        const d = await api('/api/remote-file?machine=' + encodeURIComponent(peer) + '&path=' + encodeURIComponent(p));
        if (d.ok) res.innerHTML = '<div class="rf-card"><div class="tt">' + esc(d.name) + ' · ' + (d.size || 0).toLocaleString() + ' bytes</div><div class="content">' + (d.html || '') + '</div></div>';
        else res.innerHTML = '<div class="rf-hint">' + esc(d.error || 'Fetch failed') + '</div>';
      } catch (e) { res.innerHTML = '<div class="rf-hint">Error: ' + esc(String(e.message || e)) + '</div>'; }
    });
    $('#rfUpload').addEventListener('click', async () => {
      const p = $('#rfUploadPath').value.trim(), c = $('#rfUploadContent').value;
      if (!p || !c) { toast('Path and content required', true); return; }
      const btn = $('#rfUpload'); btn.disabled = true; btn.textContent = 'Uploading…';
      try {
        const d = await api('/api/upload', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({machine: peer, path: p, content: c}) });
        if (d.ok) { toast('Uploaded to ' + d.dest); $('#rfUploadContent').value = ''; }
        else toast(d.error || 'Upload failed', true);
      } catch (e) { toast('Upload failed', true); }
      finally { btn.disabled = false; btn.textContent = 'Upload'; }
    });
  }

  // ── Session creation ──
  function openCreateModal() {
    if (!selMachine) { toast('Select a machine first', true); return; }
    $('#cmMachine').value = selMachine;
    $('#cmName').value = '';
    $('#cmHarness').value = 'bash';
    updateCreateCommand();
    $('#createModal').classList.add('open');
    $('#cmName').focus();
  }
  function updateCreateCommand() {
    $('#cmCommand').value = buildHarnessCmd($('#cmHarness').value, $('#cmYolo').checked);
  }
  async function createSession() {
    const body = { machine: selMachine, name: $('#cmName').value.trim() || 'default', command: $('#cmCommand').value };
    const btn = $('#cmSubmit'); const old = btn.textContent;
    btn.disabled = true; btn.textContent = 'Creating…';
    try {
      const d = await api('/api/session/create', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body) });
      if (d.ok) { closeModal(); toast('Created ' + (d.session || body.name)); await refresh(); }
      else toast(d.error || 'Create failed', true);
    } catch (e) { toast('Create failed', true); }
    finally { btn.disabled = false; btn.textContent = old; }
  }
  function closeModal() { $('#createModal').classList.remove('open'); }

  // ── Events ──
  $('#search').addEventListener('input', e => { query = e.target.value.trim().toLowerCase(); renderMachines(); });
  $('#themeToggle').addEventListener('click', () => applyTheme(theme === 'dark' ? 'light' : 'dark'));
  $('#newSessionBtn').addEventListener('click', openCreateModal);
  $('#cmCancel').addEventListener('click', closeModal);
  $('#cmSubmit').addEventListener('click', createSession);
  $('#cmHarness').addEventListener('change', updateCreateCommand);
  $('#cmYolo').addEventListener('change', updateCreateCommand);
  $('#createModal').addEventListener('click', e => { if (e.target === $('#createModal')) closeModal(); });
  $('#machines').addEventListener('click', e => {
    const row = e.target.closest('.mrow'); if (!row || !row.dataset.machine) return;
    selMachine = row.dataset.machine; selSession = null; curFile = null; curType = null; editing = false;
    renderMachines(); renderSessions(); renderFiles(); renderBreadcrumb(); updateTools();
    $('#content').innerHTML = '<div class="empty">Select a session to browse documents.</div>';
    if (tab === 'files') renderRemoteFiles();
    if (tab === 'output') startOutput();
  });
  $('#sessions').addEventListener('click', e => {
    const row = e.target.closest('.srow'); if (!row || !row.dataset.session) return;
    selSession = row.dataset.session; curFile = null; curType = null; editing = false;
    renderSessions(); renderFiles(); renderBreadcrumb(); updateTools();
    $('#content').innerHTML = '<div class="empty">Select a document.</div>';
    if (tab === 'output') startOutput();
  });
  $('#tabbar').addEventListener('click', e => { const b = e.target.closest('.tab'); if (b) switchTab(b.dataset.tab); });
  $('#filelist').addEventListener('click', e => {
    const item = e.target.closest('.fitem'); if (!item) return;
    curFile = item.dataset.file; curType = item.dataset.type; editing = false;
    $$('#filelist .fitem').forEach(f => f.classList.toggle('active', f === item));
    renderBreadcrumb(); updateTools(); loadContent();
  });
  $('#editBtn').addEventListener('click', () => { editing = true; $('#editor').value = curRaw; updateTools(); $('#editor').focus(); });
  $('#cancelBtn').addEventListener('click', () => { editing = false; updateTools(); });
  $('#copyBtn').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(curRaw); toast('Copied'); } catch (_) { toast('Copy failed', true); }
  });
  $('#saveBtn').addEventListener('click', async () => {
    const btn = $('#saveBtn'); const old = btn.textContent;
    btn.disabled = true; btn.textContent = 'Saving…';
    try {
      const d = await api('/api/save', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({session: selSession, type: curType, name: curFile, content: $('#editor').value}) });
      curRaw = $('#editor').value;
      $('#content').innerHTML = d.html || '';
      editing = false; updateTools(); toast('Saved');
    } catch (e) { toast('Save failed', true); }
    finally { btn.disabled = false; btn.textContent = old; }
  });
  $('#followBtn').addEventListener('click', () => {
    outputFollow = !outputFollow;
    const b = $('#followBtn'); b.classList.toggle('on', outputFollow); b.textContent = outputFollow ? 'follow ●' : 'paused ○';
  });
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape' && editing) { editing = false; updateTools(); }
    else if (e.key === 'Escape' && $('#createModal').classList.contains('open')) closeModal();
  });

  // ── Init ──
  async function refresh() {
    try {
      const [m, t] = await Promise.all([api('/api/machines'), api('/api/tree')]);
      mesh = m; fsTree = t;
    } catch (err) { mesh = {node:'', peers:[], sessions:[], offline:true, uptime_s:0}; }
    // Auto-select the first machine (local node) on first successful load.
    if (selMachine === null) {
      const list = machineList();
      if (list.length) selMachine = list[0].name;
    }
    renderMachines(); renderSessions(); renderFiles();
    if (tab === 'files') renderRemoteFiles();
  }

  applyTheme(theme);
  refresh();
  setInterval(refresh, 5000);
})();
</script>
</body>
</html>
'''
