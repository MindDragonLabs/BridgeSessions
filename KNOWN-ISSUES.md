# Known Issues

## Bridge Panel (Web UI) — non-functional

The Bridge Panel web dashboard (`tools/bridgepanel/`) is not operational in the
current release. The Python server and its API exist in the source tree but are
not wired to the daemon's live data plane. Do not rely on it for fleet
monitoring or session management.

**Workaround:** use the CLI directly:

```bash
bs peers list          # connected peers
bs health <peer>       # data-plane health check
bs fleet               # mesh-wide fleet table (name, addr, version, status)
bs ctl sessions        # active sessions on local daemon
bs ctl peers           # live peer connectivity
```
