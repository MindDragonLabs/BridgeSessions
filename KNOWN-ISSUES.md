# Known Issues

## Bridge Panel (Web UI)

Bridge Panel (`tools/bridgepanel/`) is a functional web dashboard served on
`127.0.0.1:9770` by default. It connects to the local BS daemon via IPC
(port 19980) for live mesh data: machines, sessions, scrollback, comms/docs.

**Tabs:** Output (live session scrollback), Comms, Docs.

**Limitations:**
- Served on loopback only. Bind to a VPN address explicitly after configuring
  trusted sources; never expose directly to the internet.
- The `bridgepanel publish` CLI command publishes Markdown files into sessions.

**Start:**
```bash
python3 tools/bridgepanel/panel.py serve
# Or via the launcher:
python3 tools/bridgepanel/panel.py
```