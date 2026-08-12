"""BridgePanel — HTML/CSS/JS template (single-file SPA)."""
from __future__ import annotations

# ── SVG Favicon ────────────────────────────────────────────────

FAVICON_SVG = b"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <rect x="3" y="3" width="26" height="26" rx="7" fill="#ffffff" stroke="rgba(0,0,0,0.08)" stroke-width="1"/>
  <rect x="8" y="9" width="11" height="2.5" rx="1.25" fill="#0d0d0d"/>
  <rect x="8" y="14.75" width="16" height="2.5" rx="1.25" fill="#888888"/>
  <rect x="8" y="20.5" width="9" height="2.5" rx="1.25" fill="#18E299"/>
</svg>"""


# ── HTML ───────────────────────────────────────────────────────


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
button, input, textarea, select { font: inherit; color: inherit; }

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
.machine-row > span:nth-child(2) {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  flex: 1;
}
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
.new-session-btn {
  width: 22px; height: 22px;
  display: flex; align-items: center; justify-content: center;
  border: 1px solid var(--border-2);
  border-radius: 5px;
  background: var(--bg);
  color: var(--text-3);
  font-size: 15px; line-height: 1;
  cursor: pointer; opacity: 0.7;
}
.machine-row:hover .new-session-btn { opacity: 1; }
.new-session-btn:hover {
  background: var(--accent-soft); color: var(--accent-deep); border-color: var(--accent);
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
  width: 20px; height: 20px;
  display: flex; align-items: center; justify-content: center;
  border: 1px solid var(--border-2);
  border-radius: 5px;
  background: var(--bg);
  color: var(--accent-deep);
  font-size: 10px;
  cursor: pointer;
  opacity: 0;
  transition: opacity 0.15s ease;
}
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

.harness-card {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  padding: 12px 14px;
  border: 1px solid var(--border-2);
  border-radius: 8px;
  background: var(--bg);
}
.harness-card .harness-info {
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 0;
}
.harness-card strong { font-size: 14px; color: var(--text); }
.harness-card code {
  font-family: var(--mono);
  font-size: 12px;
  color: var(--text-2);
  white-space: pre-wrap;
  word-break: break-all;
}
.harness-card .copy-btn {
  padding: 5px 12px;
  font-size: 12px;
  font-weight: 500;
  border: 1px solid var(--accent);
  border-radius: 6px;
  background: var(--bg);
  color: var(--accent-deep);
  cursor: pointer;
  flex-shrink: 0;
}
.harness-card .copy-btn:hover { background: var(--accent-soft); }
.connect-hint {
  font-size: 12px;
  color: var(--text-4);
  line-height: 1.5;
}
.connect-hint code {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-3);
}

/* ── Providers ── */
.provider-card {
  border: 1px solid var(--border-2);
  border-radius: 10px;
  padding: 14px 16px;
  background: var(--bg);
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.provider-card .pc-head {
  display: flex;
  align-items: center;
  gap: 8px;
}
.provider-card .pc-name {
  font-size: 15px;
  font-weight: 600;
  color: var(--text);
}
.provider-card .pc-tier {
  font-family: var(--mono);
  font-size: 10px;
  color: var(--text-4);
  margin-left: auto;
}
.provider-card .pc-status {
  width: 8px; height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}
.pc-status.ok { background: var(--accent); }
.pc-status.warn { background: #f0a020; }
.pc-status.expired { background: #b3261e; }
.provider-card .pc-auth {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-3);
  text-transform: uppercase;
  letter-spacing: 0.4px;
}
.provider-card .pc-details {
  font-size: 12px;
  color: var(--text-4);
  line-height: 1.5;
}
.provider-card .pc-details code {
  font-family: var(--mono);
  font-size: 11px;
  background: rgba(0,0,0,0.04);
  padding: 1px 4px;
  border-radius: 3px;
}
.provider-card .pc-actions {
  display: flex;
  gap: 6px;
  margin-top: 4px;
}
.provider-card .pc-actions button {
  padding: 4px 10px;
  font-size: 11px;
  font-weight: 500;
  border: 1px solid var(--border-2);
  border-radius: 5px;
  background: var(--bg);
  color: var(--text-2);
  cursor: pointer;
}
.provider-card .pc-actions button:hover {
  background: var(--hover);
}
.pc-balance {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--text-3);
}
.pc-balance .bar-track {
  height: 4px;
  background: var(--border);
  border-radius: 2px;
  margin-top: 3px;
}
/* ── Fleet ── */
.fleet-stat { border: 1px solid var(--border-2); border-radius: 8px; padding: 12px 14px; text-align: center; }
.fleet-stat .fs-v { font-size: 28px; font-weight: 700; color: var(--text); }
.fleet-stat .fs-l { font-size: 11px; color: var(--text-4); text-transform: uppercase; letter-spacing: 0.5px; margin-top: 2px; }
.fleet-stat .fs-v.ok { color: var(--accent); }
.fleet-stat .fs-v.warn { color: #f0a020; }

.spoke-card { border: 1px solid var(--border-2); border-radius: 10px; padding: 14px 16px; background: var(--bg); }
.spoke-card .sc-head { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
.spoke-card .sc-ip { font-size: 14px; font-weight: 600; color: var(--text); font-family: var(--mono); }
.spoke-card .sc-status { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }
.spoke-card .sc-status.ok { background: var(--accent); }
.spoke-card .sc-status.warn { background: #f0a020; }
.spoke-card .sc-meta { font-size: 12px; color: var(--text-4); line-height: 1.6; }
.spoke-card .sc-meta code { font-family: var(--mono); font-size: 11px; color: var(--text-3); }
.spoke-card .sc-harness { margin-top: 8px; }
.spoke-card .sc-harness .sh-row { display: flex; align-items: center; gap: 6px; padding: 3px 0; font-size: 12px; }
.spoke-card .sc-harness .sh-name { font-weight: 500; color: var(--text-2); width: 80px; flex-shrink: 0; }
.spoke-card .sc-harness .sh-v { font-family: var(--mono); font-size: 11px; color: var(--text-4); }
.spoke-card .sc-harness .sh-badge { font-family: var(--mono); font-size: 10px; padding: 1px 6px; border-radius: 4px; }
.sh-badge.ok { background: var(--accent-soft); color: var(--accent-deep); }
.sh-badge.missing { background: rgba(0,0,0,0.04); color: var(--text-4); }
.sh-badge.degraded { background: #fef3e0; color: #c77700; }
.sh-badge.hil { background: #fce4ec; color: #c62828; }

.event-row { padding: 4px 0; font-size: 12px; border-bottom: 1px solid var(--border); display: flex; gap: 8px; }
.event-row .ev-time { font-family: var(--mono); color: var(--text-4); width: 60px; flex-shrink: 0; }
.event-row .ev-type { font-family: var(--mono); font-size: 10px; color: var(--text-3); width: 70px; flex-shrink: 0; text-transform: uppercase; }
.event-row .ev-msg { color: var(--text-2); }
.event-row .ev-hil { color: #c62828; font-weight: 600; }

/* ── Models / Registry ── */
.pc-balance .bar-fill {
  height: 100%;
  border-radius: 2px;
  background: var(--accent);
}
.pc-balance .bar-fill.warn { background: #f0a020; }
.pc-balance .bar-fill.drain { background: #b3261e; }
.connect-form label {
  display: block;
  font-size: 12px;
  font-weight: 500;
  color: var(--text-3);
  margin: 8px 0 5px;
}
.connect-form select {
  width: 100%;
  padding: 7px 10px;
  border: 1px solid var(--border-2);
  border-radius: 6px;
  background: var(--bg);
  outline: none;
  font-size: 14px;
}
.connect-form select:focus { border-color: var(--accent); }

.empty-state {
  padding: 60px 20px;
  text-align: center;
  color: var(--text-4);
  font-size: 14px;
}
.empty-state .hint { font-size: 12px; margin-top: 6px; font-family: var(--mono); }

/* ── Modal ── */
.modal-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.35);
  backdrop-filter: blur(2px);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 100;
}
.modal-box {
  background: var(--bg);
  border: 1px solid var(--border-2);
  border-radius: 12px;
  padding: 22px;
  width: min(420px, calc(100vw - 32px));
  box-shadow: 0 12px 40px rgba(0,0,0,0.12);
}
.modal-box h3 { margin: 0 0 16px; font-size: 16px; color: var(--text); }
.modal-box label { display: block; font-size: 12px; font-weight: 500; color: var(--text-3); margin: 12px 0 5px; }
.modal-box input, .modal-box select {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--border-2);
  border-radius: 6px;
  background: var(--bg);
  outline: none;
}
.modal-box input:focus, .modal-box select:focus { border-color: var(--accent); }
.modal-box .row { display: flex; gap: 12px; }
.modal-box .row > div { flex: 1; }
.modal-actions { display: flex; justify-content: flex-end; gap: 10px; margin-top: 20px; }
.modal-actions button {
  padding: 7px 14px;
  font-size: 13px;
  font-weight: 500;
  border-radius: 6px;
  border: 1px solid var(--border-2);
  background: var(--bg);
  cursor: pointer;
}
.modal-actions button.primary { background: var(--text); color: var(--bg); border-color: var(--text); }
.modal-actions button.secondary:hover { background: var(--hover); }
.checkbox-row { margin-top: 12px; }
.check-label {
  display: flex !important;
  align-items: center;
  gap: 7px;
  font-size: 13px !important;
  font-weight: 400 !important;
  color: var(--text-2);
  cursor: pointer;
  margin: 0 !important;
}
.check-label input[type=checkbox] { width: auto; accent-color: var(--accent); }
.modal-box .hint { margin-top: 10px; font-size: 11px; color: var(--text-4); }

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
    <div class="tabbar" id="tabbar">
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
<div class="modal-overlay" id="createModal" style="display:none">
  <div class="modal-box">
    <h3>New session on <span id="createMachineName"></span></h3>
    <label for="createName">Session name</label>
    <input type="text" id="createName" placeholder="my-session">
    <label for="createHarness">Harness</label>
    <select id="createHarness">
      <option value="bash">Bash</option>
      <option value="hermes">Hermes</option>
      <option value="claude">Claude Code</option>
      <option value="codex">Codex CLI</option>
      <option value="kimi">Kimi Code</option>
      <option value="commandcode">CommandCode</option>
      <option value="opencode">OpenCode</option>
      <option value="cursor">Cursor</option>
    </select>
    <label for="createCommand">Command</label>
    <input type="text" id="createCommand" placeholder="/bin/bash -l" value="/bin/bash -l">
    <div class="checkbox-row">
      <label class="check-label">
        <input type="checkbox" id="createYolo" checked>
        --yolo / auto-approve
      </label>
    </div>
    <div class="hint">Terminal size auto-detected on connect (like SSH).</div>
    <div class="modal-actions">
      <button id="createCancel" class="secondary">Cancel</button>
      <button id="createSubmit" class="primary">Create</button>
    </div>
  </div>
</div>
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
  const createModal = document.getElementById('createModal');
  const createMachineNameEl = document.getElementById('createMachineName');
  const createNameEl = document.getElementById('createName');
  const createHarnessEl = document.getElementById('createHarness');
  const createCommandEl = document.getElementById('createCommand');
  const createYoloEl = document.getElementById('createYolo');
  const createSubmitEl = document.getElementById('createSubmit');

  const HARNESS_CATALOG = {
    bash:      { name: 'Bash',        cmd: '/bin/bash -l' },
    hermes:    { name: 'Hermes',      cmd: 'hermes --tui' },
    claude:    { name: 'Claude Code', cmd: 'claude' },
    codex:     { name: 'Codex CLI',   cmd: 'codex' },
    kimi:      { name: 'Kimi Code',   cmd: 'kimi' },
    commandcode: { name: 'CommandCode', cmd: 'commandcode' },
    opencode:  { name: 'OpenCode',    cmd: 'opencode' },
    cursor:    { name: 'Cursor',      cmd: 'cursor' },
  };
  const YOLO_FLAGS = {
    hermes: ' --yolo', claude: ' -p', codex: ' --yolo',
    kimi: ' -p', commandcode: '', opencode: ' --auto',
    cursor: ' --yolo', bash: '',
  };

  function buildHarnessCmd(harness, yolo) {
    var cmd = HARNESS_CATALOG[harness] ? HARNESS_CATALOG[harness].cmd : '/bin/bash -l';
    if (yolo && YOLO_FLAGS[harness]) cmd += YOLO_FLAGS[harness];
    return cmd;
  }

  // ── State ──
  let mesh = {node:'', peers:[], sessions:[], offline:true};
  let fsTree = {sessions:[]};
  let selMachine = null;       // '' = local node
  let selSession = null;
  let selSessionLive = false;
  let selSessionCommand = '';
  let createMachine = null;
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

  async function copyToClipboard(text) {
    try {
      await navigator.clipboard.writeText(text);
      toast('Copied');
    } catch (_) {
      toast('Copy failed', true);
    }
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
      '<button class="new-session-btn" data-machine="' + esc(m.name) +
      '" title="New session on ' + esc(m.name) + '">+</button>' +
      '</div>'
    ).join('');
  }

  machinesEl.addEventListener('click', e => {
    const plus = e.target.closest('.new-session-btn');
    if (plus) {
      e.stopPropagation();
      openCreateModal(plus.dataset.machine);
      return;
    }
    const row = e.target.closest('.machine-row');
    if (!row || !row.dataset.machine) return;
    selMachine = row.dataset.machine;
    selSession = null;
    renderMachines();
    renderSessions();
    renderWork();
  });

  // ── New session modal ──
  function openCreateModal(machineName) {
    createMachine = machineName;
    createMachineNameEl.textContent = machineName || '(local)';
    createNameEl.value = '';
    createHarnessEl.value = 'bash';
    updateCreateCommand();
    createModal.style.display = '';
    createNameEl.focus();
  }

  function updateCreateCommand() {
    createCommandEl.value = buildHarnessCmd(createHarnessEl.value, createYoloEl.checked);
  }

  createHarnessEl.addEventListener('change', updateCreateCommand);
  createYoloEl.addEventListener('change', updateCreateCommand);

  function closeCreateModal() {
    createModal.style.display = 'none';
  }

  async function createSession() {
    const body = {
      machine: createMachine,
      name: createNameEl.value.trim() || 'default',
      command: createCommandEl.value,
    };
    const originalText = createSubmitEl.textContent;
    createSubmitEl.disabled = true;
    createSubmitEl.textContent = 'Creating…';
    try {
      const r = await fetch(base + '/api/session/create', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body)
      });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const d = await r.json();
      if (d.ok) {
        closeCreateModal();
        toast('Created ' + d.session);
        await refreshMesh();
        renderSessions();
      } else {
        toast(d.error || 'Create failed', true);
      }
    } catch (err) {
      toast(err.message || 'Create failed', true);
    } finally {
      createSubmitEl.disabled = false;
      createSubmitEl.textContent = originalText;
    }
  }

  createModal.addEventListener('click', e => {
    if (e.target === createModal) closeCreateModal();
  });
  document.getElementById('createCancel').addEventListener('click', closeCreateModal);
  createSubmitEl.addEventListener('click', createSession);
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape' && createModal.style.display !== 'none') closeCreateModal();
  });

  // ── Sessions column ──
  function sessionsFor(machineName) {
    const m = machineList().find(x => x.name === machineName);
    const live = (m && m.sessions) || [];
    // Merge filesystem sessions for the local node only
    const isLocal = !machineName || machineName === mesh.node ||
                    machineName === '(local)' || machineName === '';
    const HARNESS_KEYS = /hermes|claude|codex|kimi|commandcode|opencode|cursor/i;
    const out = live.map(s => ({
      name: s.name, state: s.state || '', command: s.command || '',
      bytes: s.bytes || 0, live: true,
      harness: HARNESS_KEYS.test(s.command || s.name || '')
    }));
    if (isLocal) {
      const liveNames = new Set(out.map(s => s.name));
      for (const fs of (fsTree.sessions || [])) {
        if (!liveNames.has(fs.name)) {
          out.push({name: fs.name, state: 'stored', command: '', bytes: 0,
                    live: !!fs.live, harness: HARNESS_KEYS.test(fs.name)});
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
    var has = !!selSession;
    tabbarEl.style.display = has || !selSession ? '' : 'none';
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
    copyToClipboard(curRaw);
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
