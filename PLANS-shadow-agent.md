# shadow-agent — Unified Cross-Machine Remote Tool Surface

> **For Hermes:** Execute this plan with the `subagent-driven-development` skill. Each task has TDD: failing test, minimal impl, run, commit.

**Goal:** Build a single Shadow-resident daemon that lets any AI on the Tailscale mesh interact with Shadow through **one** credential and **one** MCP-over-HTTP endpoint, with the full underlying tool surface (shell, CUA desktop control, filesystem, Hermes local chat, Roblox Studio, native Win32) multiplexed behind it. Today these primitives exist but require 5 different protocols + 5 different auths; this collapses them.

**Architecture:** C++23 daemon, MSVC 2026, runs as a real Windows SCM service (LocalSystem supervisor → user-session worker for GUI/CUA, per the standing rule). Reuses `bs-transport` for mTLS+ed25519, reuses the bs-client core for persistent ConPTY sessions, talks to existing primitives (Hermes FastAPI on 8787, Windows CUA on 8765, BvSshServer/SFTP, Roblox Studio MCP) over loopback HTTP/SFTP. Exposes one MCP-over-HTTP endpoint on Tailscale `100.124.169.66:9100` and one HTTP/JSON-RPC at `:9101` for non-MCP clients.

**Tech Stack:** C++23 (`/std:c++latest`), MSVC 2026, CMake 4.x + Ninja, vcpkg. Reuse `bs-transport` (TLS) + `bs-protocol` (codec). New: `cpp-httplib` (header-only HTTP server), `nlohmann/json` (already in vcpkg), `mcp-protocol` schema (vendored), `pywin32` (only for the SCM registration bootstrap script — daemon itself is pure C++).

---

## Context: What Exists Today (Audit)

Verified against the live Shadow host on 2026-06-01, post-SFTP-hygiene-cleanup:

| Primitive | Status | Endpoint | Used for |
|---|---|---|---|
| `bs-server` (ConPTY daemon) | ✅ running, PID-stateful, 3 client certs authorized, last log: `startup v26.05.31 listen=0.0.0.0:19948` | `100.124.169.66:19948` (Tailscale mTLS) | Persistent shell sessions |
| Hermes FastAPI | ✅ running, `{"ok":true,"model":"openai-codex/gpt-5.5","busy":false}` | `100.124.169.66:8787/agent` (Tailscale HTTP, no-auth on Tailscale) | Local Hermes chat from outside |
| Windows CUA MCP | ✅ listening on 127.0.0.1:8765 (local-only) | `127.0.0.1:8765` (loopback) | Desktop control: screen, click, type, hotkey |
| BvSshServer (Bitvise) | ✅ service running | `0.0.0.0:22`/SFTP, agent user → `C:\SFTP\agent\` | File staging |
| Roblox Studio MCP | ✅ installed in `version-981c8a3157934eec` (current) and `b2a9c018a1e042c6` (MCP-only) | per-`McpServiceConfig` registration | Roblox editing |

**The gap:** Five protocols, five auth models (mTLS cert fingerprint, Tailscale-allowlist, loopback-only, SSH key, MCP plugin-coupled). A remote AI has to: generate a bs-client cert, get it added to `authorized_keys`, learn ConPTY wire format, learn CUA's JSON schema, learn the FastAPI endpoint, learn SFTP path conventions — then write glue code to compose them. There's no session continuity (HTTP calls are stateless, BS sessions are PTY-bound), no shared memory across primitives, and CUA is locked to loopback.

**What "shadow-agent" adds:** A multiplexer that owns one mTLS cert per remote machine, holds the other credentials, and exposes everything as a unified MCP tool surface. Remote AI's experience: `tools/call name="shell.exec" args={...}` over HTTPS on Tailscale, session-aware, composable.

---

## File Layout

```
C:\Program Files\shadow-agent\           ← install root (system-level, not in SFTP)
├── bin\
│   ├── shadow-agent.exe                 ← the daemon
│   ├── install-service.ps1              ← SCM registration (LocalSystem supervisor)
│   ├── install-worker.ps1               ← hidden user-session task (for CUA/GUI)
│   └── uninstall.ps1
├── share\
│   ├── schema\                          ← JSON schemas for every tool
│   │   ├── shell.exec.schema.json
│   │   ├── cua.screenshot.schema.json
│   │   └── ...
│   └── mcp-manifest.json                ← the unified tool catalog
├── etc\
│   └── shadow-agent.example.yaml        ← config template
└── LICENSE, README.md

C:\Users\Shadow\.shadow-agent\           ← state dir (per-user)
├── authorized_clients                   ← accepted client cert fingerprints
├── delegations\                         ← per-client tool allowlist
│   ├── linux-b.yaml                       ← [shell.*, cua.*, fs.*, hermes.chat, roblox.*]
│   ├── linux-a.yaml                       ← [shell.*, fs.*]
│   └── macbook.yaml                     ← [fs.*]
├── sessions\<id>\                       ← per-session working dir (scrollback, last_artifact)
├── daemon.log                           ← rotating, spdlog
└── shadow-agent.yaml                    ← active config

C:\SFTP\agent\shadow-agent\              ← SOURCE under SFTP (deployment target, gitignored on Shadow)
├── CMakeLists.txt
├── include\shadowagent\                 ← public headers
│   ├── server.hpp                       ← HTTP server, MCP routing
│   ├── session.hpp                      ← session multiplexer
│   ├── tools\                           ← one header per tool family
│   │   ├── shell.hpp
│   │   ├── cua.hpp
│   │   ├── fs.hpp
│   │   ├── hermes.hpp
│   │   ├── roblox.hpp
│   │   └── win32.hpp
│   ├── delegation.hpp                   ← per-client ACL
│   └── config.hpp
├── src\                                 ← .cpp impls
├── tests\                               ← catch2 unit + integration
├── vendor\                              ← cpp-httplib, mcp-protocol schema (submodules or downloaded)
└── tools\
    ├── gen_manifest.py                  ← build-time: walk headers → mcp-manifest.json
    └── keygen.py                        ← client cert minting helper
```

**Why source under SFTP, install under `C:\Program Files\`:** the user's standing rule + the SFTP hygiene report: the SFTP share is a *deployment target*, not a working tree. The build pipeline puts `shadow-agent.exe` into `C:\Program Files\shadow-agent\bin\` (where SCM expects it), and state into the user home. This matches how the existing bs-server flow works (source in SFTP, exe in `bridgesessions\build\...`, run from there OR a service install).

---

## Phases

### Phase S0: Skill & Plan Documents (this phase) — 30 min

| Task | Description |
|---|---|
| S0.1 | `PLANS-shadow-agent.md` (this file) |
| S0.2 | `TODO-shadow-agent.md` |
| S0.3 | `docs/PLANS.md` and `docs/TODO.md` updated with pointer + summary |
| S0.4 | `SKILL.md` in-repo under `skills/devops/shadow-agent/` (peer to `windows-service-supervision`, `local-agent-http-bridges`) |

**Verify:** `ls PLANS-shadow-agent.md TODO-shadow-agent.md`, all three refs in umbrellas, skill frontmatter passes validator.

### Phase S1: Skeleton & Config — 1 day

**S1.1** CMakeLists for `shadow-agent` (statically links `bs-transport` + `bs-protocol`, uses vcpkg for `cpp-httplib` and `nlohmann/json`). Output: `shadow-agent.exe` + a smoke test exe.

**S1.2** `config.hpp` + `src/config.cpp` — parse `C:\Users\Shadow\.shadow-agent\shadow-agent.yaml` (port, TLS paths, log level, sandboxed root dir for `fs.*`).

**S1.3** `delegation.hpp` + `src/delegation.cpp` — load `delegations\<client-fingerprint-prefix>.yaml`. Each file is a YAML list of allowed tool globs (`shell.*`, `cua.screenshot`, etc.).

**S1.4** Test: parse a sample config + delegation file, assert the right tools are exposed to a fingerprint.

**Verify:** `cmake --build build\windows-msvc-debug --target shadow-agent-tests && ctest -R "config|delegation" --output-on-failure` → all green.

### Phase S2: HTTP+TLS server, MCP routing — 1.5 days

**S2.1** `server.hpp`/`src/server.cpp` — cpp-httplib + `bs-transport` mTLS. Endpoints:
- `GET /health` — `{ok:true, name:"shadow-agent", version:"0.1.0", uptime_sec:N, tools:N}` (public, no auth)
- `GET /tools` — return the mcp-manifest.json tool list (auth required)
- `POST /mcp` — JSON-RPC 2.0 over HTTP, methods: `initialize`, `tools/list`, `tools/call` (auth required)
- `POST /v1/tools/call` — raw HTTP/JSON wrapper around `tools/call` (for non-MCP clients like Python `requests`)
- `GET /v1/sessions` and `GET /v1/sessions/<id>` — session metadata (auth required)

**S2.2** Client cert extraction: at `mcp` request time, grab the verified peer cert fingerprint via `bs::transport::peer_public_key_hex(SSL*)` and match against `authorized_clients`. If not found → 401. If found, load the delegation file and use it to gate every `tools/call`.

**S2.3** Test: integration test that mints a client cert, connects over `https://localhost:9101/mcp`, calls `tools/list`, calls `tools/call` with allowed and disallowed tools, asserts 401/403/200 correctly.

**Verify:** Hand-test with `curl --cert client.pem --key client.key --cacert ca.pem https://100.124.169.66:9101/health` from a Tailscale peer.

### Phase S3: Shell tool (reuses bs-client core) — 1.5 days

**S3.1** Link the existing `bs-client` relay as a library (extract `terminal_raw.cpp` and the two-thread relay into a `bs-client-core` static lib, like `bs-client-core.lib` which already exists in the build output). shadow-agent spawns a ConPTY in-process and owns a session manager.

**S3.2** `tools/shell.hpp`/`src/shell.cpp` — implements the MCP tool spec:
```
shell.open   { name: string, cols: int=80, rows: int=24, shell: string="powershell.exe" } → { session_id, cols, rows }
shell.exec   { session_id: string, cmd: string, timeout_ms: int=30000 } → { stdout, stderr, exit_code, session_alive }
shell.input  { session_id, data: string (base64) }  → { ok, echoed_chars }  (for binary/stdin)
shell.tail   { session_id, since_bytes: int=0 }      → { data: base64, total_bytes }
shell.resize { session_id, cols, rows }              → { ok }
shell.close  { session_id }                          → { ok, output_bytes }
```

**S3.3** Session multiplexer: in-memory map of `session_id → { hPCON, hPipeIn, hPipeOut, ring_buffer, last_heartbeat }`. Persist ring buffer to `C:\Users\Shadow\.shadow-agent\sessions\<id>\output.bin` on close so a re-attach can `shell.tail` old output. Sessions auto-expire after 24h idle.

**S3.4** Test: open a session, exec `Get-Date`, `Get-Process | Select-Object -First 3 Name`, assert stdout matches PowerShell output. Re-attach after process death, assert `shell.tail` returns the scrollback.

**Verify:** From linux-b, `curl --cert ... -X POST https://100.124.169.66:9101/v1/tools/call -d '{"name":"shell.open","args":{"name":"smoke"}}'`.

### Phase S4: Filesystem tool — 0.5 day

**S4.1** `tools/fs.hpp`/`src/fs.cpp`:
```
fs.list   { path: string }                          → { entries: [{name, type, size, mtime}] }
fs.read   { path: string, offset: int=0, max_bytes: int=1048576 } → { data: base64, total_bytes, is_binary }
fs.write  { path: string, data: base64, mode: "overwrite"|"append" } → { bytes_written }
fs.stat   { path: string }                          → { exists, type, size, mtime, is_readonly }
fs.delete { path: string, recursive: bool=false }   → { ok }
```

**S4.2** Sandbox: by default, all paths are jailed under `C:\SFTP\agent\` (read+write) and `C:\Users\Shadow\.shadow-agent\` (read+write, but can't delete the `authorized_clients` file). `win32` tool can opt out of jail.

**S4.3** Symlink guard: refuse to follow symlinks that escape the jail (use `GetFinalPathNameByHandle` with `FILE_NAME_NORMALIZED | VOLUME_NAME_DOS`).

**S4.4** Test: read the bs-server.exe, list `C:\SFTP\agent\`, attempt to read `C:\Windows\System32\config\SAM` (denied), write a file in the jail, delete it.

**Verify:** MCP `tools/call name="fs.list" args={"path":"C:/SFTP/agent"}` returns the cleaned-up tree from the hygiene report.

### Phase S5: Windows CUA tool — 0.5 day

**S5.1** `tools/cua.hpp`/`src/cua.cpp` — wraps the existing `127.0.0.1:8765` HTTP server as a tool. The `cua_http` MCP is already running; we just need:
```
cua.screenshot { monitor: int=0, max_width: int=1600 }  → { data: base64 PNG, width, height }
cua.click      { x: int, y: int, button: "left"|"right"="left", clicks: int=1 }  → { ok }
cua.type       { text: string, paste: bool=true }        → { ok }
cua.hotkey     { keys: [string, ...] }                   → { ok }
cua.mouse_move { x: int, y: int }                        → { ok }
cua.scroll     { amount: int, x: int?, y: int? }         → { ok }
cua.wait       { seconds: float }                        → { ok }
```

**S5.2** Require delegation `cua.*` AND require the *user-session worker* (next phase) to be reachable on a Unix socket or local pipe. Reject CUA calls if the worker is down with a clear 503.

**S5.3** Test: smoke that requires the live CUA service running; mark with `[!integration]` and skip in CI.

**Verify:** From linux-b: `tools/call name="cua.screenshot" args={}` returns a base64 PNG that, when decoded, shows the Shadow desktop.

### Phase S6: Hermes local chat tool — 0.5 day

**S6.1** `tools/hermes.hpp`/`src/hermes.cpp` — wraps the running FastAPI on `127.0.0.1:8787`:
```
hermes.chat        { prompt: string, timeout_sec: int=120 } → { reply, model, elapsed_sec, tools_used }
hermes.chat_async  { prompt }                                → { job_id, status_url } (POST to FastAPI, return)
hermes.job_status  { job_id }                                → { state, report_ready, ... }
hermes.job_aar     { job_id }                                → { report_text, artifacts }
```

**S6.2** Decide policy: this is a recursive-AI tool. A remote AI can spawn a *local* Hermes subagent on Shadow. This is fine but **document clearly in the tool description** that the local Hermes has full tool access. Delegation must explicitly grant `hermes.*`.

**S6.3** Test: call `hermes.chat` with the prompt "what time is it? run `date` and reply"; assert the response includes the time and a tool-call log.

**Verify:** From linux-b, a quick `hermes.chat` that the local Hermes answers.

### Phase S7: Roblox Studio tool — 1 day

**S7.1** Wrap the existing 60+ Roblox Studio MCP tools. shadow-agent speaks MCP and so does Roblox Studio; we are a transparent proxy here, with a delegation gate.

**S7.2** `tools/roblox.hpp`/`src/roblox.cpp`:
- On startup, discover the running Roblox Studio instance's MCP port (from the user's `%APPDATA%\.studio-mcp\config.json` or via the existing MCP registration in `bs-server`'s known config).
- Maintain a connection pool. Forward any `roblox.*` tool call after delegating check.

**S7.3** Test: skip in CI (requires Studio). Mark `[!integration]`.

### Phase S8: Native Win32 tool — 0.5 day

**S8.1** `tools/win32.hpp`/`src/win32.cpp`:
```
win.service.list    {}                                       → { services: [{name, state, start_type, pid, ...}] }
win.service.query   { name }                                  → { state, ... }
win.process.list    { name_filter: string? }                 → { processes: [...] }
win.process.kill    { pid, force: bool=false }               → { ok, exit_code }
win.scheduled.list  { path: string="\\" }                    → { tasks: [{name, path, last_run, last_result}] }
```

**S8.2** **Jail-break tool:** `win.*` is the only tool family that operates outside the SFTP jail. Delegation files must explicitly grant it. The default `delegations\*.yaml` does NOT include `win.*`. **The MacBook delegation explicitly does NOT include it.**

**S8.3** Test: list `BFE` service, kill nothing, assert that the SFTP-delegated client cannot call `win.*` (403).

### Phase S9: Windows Service install + worker supervision — 1 day

**S9.1** `install-service.ps1` — registers the supervisor as `ShadowAgentSupervisor` (LocalSystem, Automatic, restart on failure with 30s backoff). The supervisor's only job is health-check + spawn/restart the user-session worker.

**S9.2** `install-worker.ps1` — registers the hidden user task `ShadowAgentWorker` that runs `shadow-agent.exe --role=worker` via `pythonw.exe`-equivalent (`CREATE_NO_WINDOW`). The worker is what CUA actually talks to (it must be in the user's desktop session to capture the screen and drive the mouse).

**S9.3** IPC: supervisor and worker talk over a named pipe `\\.\pipe\ShadowAgent`. The worker registers its presence with the supervisor; supervisor's `/health` reports whether the worker is up. If the worker dies, supervisor restarts the task.

**S9.4** **Per the standing rule:** verify with `sc query ShadowAgentSupervisor`, `Get-ScheduledTask ShadowAgentWorker`, `Get-NetTCPConnection -LocalPort 9100`, and screenshot of Task Manager showing `shadow-agent.exe` running as service with no visible window. The worker must use `CREATE_NO_WINDOW` and `Get-Process` must show no `MainWindowHandle`.

**S9.5** Test: install, restart, kill worker manually, verify supervisor restarts it within 30s, verify no console popup appears. Uninstall cleanly.

**Verify:** All four checks above pass. The cleanup report from SFTP hygiene also left the bs-server alone; this does the same for shadow-agent.

### Phase S10: Audit log + per-client rate limit — 0.5 day

**S10.1** `C:\Users\Shadow\.shadow-agent\daemon.log` already captures via spdlog. Add a structured `audit` event for every `tools/call` with `{ts, client_fingerprint, tool_name, args_summary, duration_ms, ok}`. Rotate daily, keep 14 days.

**S10.2** Per-client rate limit: 60 calls/min default, configurable per delegation. Return 429 with `Retry-After` when exceeded. Logs the violation.

### Phase S11: Remote client SDK (Python, for Hermes instances) — 1 day

**S11.1** New tiny package: `shadow-agent-client` on PyPI or vendored at `C:\SFTP\agent\shadow-agent-client\`. One file: `client.py`. ~200 lines.

```python
# linux-b usage:
from shadow_agent import Shadow
s = Shadow("100.124.169.66:9100", cert="/etc/shadow-agent/linux-b-cert.pem",
           key="/etc/shadow-agent/linux-b-key.pem")
print(s.shell.exec("Get-Date").stdout)
print(s.cua.screenshot().save("/tmp/desk.png"))
print(s.fs.read("C:/SFTP/agent/v3_wave1.rbxlx", max_bytes=200).data[:64].hex())
```

**S11.2** `Shadow.shell` is a context manager: `with s.shell.session("deploy") as sh: sh.exec("cmake --build .")`. Auto-closes session on exit.

**S11.3** Test: from a Linux Tailscale host, install the SDK, call each tool family against a test shadow-agent, assert round-trip.

**Verify:** A real Linux `bs-client` connected to `100.124.169.66:9100` and a `Shadow` Python session.

### Phase S12: End-to-end smoke + docs — 0.5 day

**S12.1** `tools\smoke-e2e.py` — from linux-b: open a shell, exec `Get-Date`, screenshot the desktop, read the bs-server.exe, call `hermes.chat` "say hi", assert all five calls succeed in <30s total. The smoke exits 0 on success and writes a JSON report to `C:\SFTP\agent\shadow-agent-smoke-<date>.json`.

**S12.2** Update `REMOTE-OPS-GUIDE.md` with a new section "shadow-agent" — installation, delegation YAML format, keygen procedure for new clients, troubleshooting.

**S12.3** Update `Roblox-AI.md` (the canonical entry point) with a top-of-doc link to shadow-agent and a "what changed" note.

**S12.4** A second SKILL.md: `skills/devops/shadow-agent-operations/SKILL.md` covering day-2: log triage, adding a new delegated client, common failure modes.

**Verify:** Smoke passes 3× in a row. New section in REMOTE-OPS-GUIDE is correct.

---

## Phasing & Effort

| Phase | Effort | Blocked by | Output |
|---|---|---|---|
| S0 | 0.5 hr | — | Docs |
| S1 | 1 day | S0 | skeleton, config, delegation, tests |
| S2 | 1.5 day | S1 | MCP server, mTLS, 401/403 |
| S3 | 1.5 day | S2 | shell.* (the workhorse) |
| S4 | 0.5 day | S2 | fs.* (jailed) |
| S5 | 0.5 day | S2, S9 | cua.* (needs worker) |
| S6 | 0.5 day | S2 | hermes.* |
| S7 | 1 day | S2 | roblox.* (proxy) |
| S8 | 0.5 day | S2 | win.* (jail-break) |
| S9 | 1 day | S1 | Windows service install |
| S10 | 0.5 day | S2 | audit + rate limit |
| S11 | 1 day | S2-S9 | Python SDK |
| S12 | 0.5 day | all | smoke + docs |

**Total: ~9 days wall-clock, single developer.** Phases S3-S8 are largely parallel after S2 lands.

---

## Acceptance Criteria (the test that decides "done")

1. `sc query ShadowAgentSupervisor` → RUNNING, Automatic
2. `Get-ScheduledTask ShadowAgentWorker` → Ready, no console window
3. `curl https://100.124.169.66:9100/health` from linux-b → `{"ok":true,"name":"shadow-agent","tools":<count>}` over Tailscale
4. `curl --cert ... -X POST .../v1/tools/call -d '{"name":"shell.exec","args":{"session":"x","cmd":"whoami"}}'` → returns current Windows user
5. `curl ... -d '{"name":"cua.screenshot"}'` → returns a valid PNG, decoded image is non-empty, > 100 KB
6. `curl ... -d '{"name":"fs.read","args":{"path":"C:/Windows/System32/config/SAM"}}'` → 403 (jail)
7. `curl ... -d '{"name":"win.process.kill","args":{"pid":1}}'` from a SFTP-only delegated client → 403 (delegation)
8. `hermes.chat "echo 'shadow-agent works'"` from linux-b → reply contains the echoed string
9. No black console popup appears during install/restart (visual + `Get-Process | Where MainWindowHandle` returns nothing for shadow-agent.exe)
10. `tools/smoke-e2e.py` passes 3× in a row

---

## Risks & Tradeoffs

1. **Recursive AI risk** (`hermes.*`): a remote AI can spawn a local Hermes, which has full tool access. This is by design (the whole point) but the delegation must be explicit. **Mitigation:** SFTP-delegation defaults exclude `hermes.*`.

2. **Session state in-process vs on-disk:** in-memory sessions are fast, but a daemon restart loses them. **Decision:** ring buffers are persisted on close; live sessions are lost (acceptable; clients can re-open). Document the behavior in the tool spec.

3. **MCP wire format vs HTTP/JSON-RPC:** MCP-over-HTTP is a moving spec. Pin to the version current on 2026-06-01 and add a version field to `initialize` so we can negotiate. If the spec changes, only `src/server.cpp` needs updating.

4. **bs-client refactor into a library:** the existing two-thread relay in `bs-client/src/main.cpp` is not currently factored as a reusable library. The plan assumes a small refactor to extract the relay into `bs-client-core` (the `.lib` is already in the build dir, so the work may already be done — verify in S1.1).

5. **GUI/CUA Session 0 boundary:** LocalSystem services can't drive the user's desktop. **Mitigation:** the supervisor/worker split is mandatory, not optional. The worker is what CUA talks to via named pipe; the supervisor is what remote AIs talk to over the network.

6. **Tailscale-only binding:** shadow-agent MUST bind to the Tailscale IP, not 0.0.0.0. If the Tailscale interface is down at startup, the daemon exits nonzero. This matches the Hermes FastAPI posture.

---

## Open Questions (resolved 2026-06-01)

| # | Question | Decision |
|---|---|---|
| 1 | Delegation defaults for linux-b (kitchen sink vs conservative) | **Kitchen sink.** linux-b gets **everything**: `[shell.*, cua.*, fs.*, hermes.*, roblox.*, win.*]`. **The delegation is NOT a security boundary — the mTLS client cert is.** If you hold a cert that the daemon recognizes in `authorized_clients`, you are trusted; the delegation YAML is a coarse *cooperation policy* (what we usually want this peer to do), not a *capability grant* (what they're allowed to do). The reasoning: a remote AI that has a cert is already authorized to connect; the cert is the gate. A separate, narrower delegation is still possible per-client if a particular peer should be sandboxed. MacBook gets `[fs.*]` only (it has a different cert posture and only needs file sync). The `authorized_clients` file holds the cert fingerprints; the `delegations/<fp-prefix>.yaml` files hold the per-client cooperation preferences. |
| 2 | Port (9100/9101 OK?) | **Yes — 9100 (MCP-over-HTTP) and 9101 (HTTP/JSON-RPC for non-MCP clients).** Both bind to `100.124.169.66` only; refuse to start if Tailscale IP is missing. |
| 3 | Source location (`C:\SFTP\agent\shadow-agent\` vs `C:\Program Files\shadow-agent\src\`) | **Yes — `C:\SFTP\agent\shadow-agent\`** (the deployment target). Build output goes to `C:\SFTP\agent\shadow-agent\build\windows-msvc-debug\` matching the existing bs-server pattern. Install (exe, scripts) is rsynced from there to `C:\Program Files\shadow-agent\bin\`. State stays in `C:\Users\Shadow\.shadow-agent\`. |
| 4 | Roblox MCP port: auto-detect from Studio, or hardcoded? | **Yes — auto-detect.** On worker startup, read `%APPDATA%\.studio-mcp\config.json` if present, else enumerate `HKCU\Software\Roblox\RobloxStudio\MCP` (the registry path Studio's plugin uses to register its port), else fall back to the well-known default `7800`. If the registered port doesn't accept a TCP connect within 2s, log a clear warning and return 503 from `roblox.*` calls with a hint about which Studio version is missing the MCP plugin. |

With these decisions locked, S0 is the docs phase (done) and S1 (skeleton + config + delegation) can start immediately.
