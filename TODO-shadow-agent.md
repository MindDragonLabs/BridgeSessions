# shadow-agent — TODO

**Status:** Planning (Phase S0)
**Active plan:** [`PLANS-shadow-agent.md`](./PLANS-shadow-agent.md)
**Umbrella:** [docs/PLANS.md](./docs/PLANS.md), [docs/TODO.md](./docs/TODO.md)
**Goal:** Single Shadow-resident daemon exposing one mTLS+ed25519 MCP endpoint on Tailscale `100.124.169.66:9100` that multiplexes shell, CUA, fs, hermes, roblox, and win32 tools behind per-client delegations.

---

## S0 — Plan & Skill Documents (this phase)

- [ ] **S0.1** `PLANS-shadow-agent.md` written
- [ ] **S0.2** `TODO-shadow-agent.md` written
- [ ] **S0.3** `docs/PLANS.md` and `docs/TODO.md` updated with pointer + summary
- [ ] **S0.4** `SKILL.md` in-repo at `skills/devops/shadow-agent/SKILL.md` (passes validator)

**Verify:** `ls PLANS-shadow-agent.md TODO-shadow-agent.md` shows both; `docs/PLANS.md` references this file; skill frontmatter parses + description ≤1024 chars.

---

## S1 — Skeleton & Config

- [x] **S1.1** CMakeLists for `shadow-agent` (links `bs-transport`, `bs-protocol`; vcpkg: `cpp-httplib`, `nlohmann/json`) — *S1.1 done 2026-06-01: standalone skeleton (no bs-transport/bs-protocol yet, those land in S2). 6 deliverables, 3/3 tests pass, exe prints --version. Spec + quality reviews PASSED. 3 important issues from quality review fixed (cmake/README.md doc, const_cast in test, dead print_version function).*
- [x] **S1.2** `config.hpp`/`src/config.cpp` — parse `C:\Users\Shadow\.shadow-agent\shadow-agent.yaml` — *S1.2 done 2026-06-01: 12 config tests, 18 total ctest pass. JSON format (not YAML — documented in header). `Result<T>` from `config.hpp` (portable fallback for MSVC `std::expected`). Strict unknown-field rejection. Polish pass extracted `shadow-agent_lib` static lib, `defaults.hpp` constants, `--config` CLI flag with spdlog on success / error+exit(1) on failure. Spec review PASSED. Quality review REQUESTED CHANGES; all 4 important issues fixed.*
- [x] **S1.3** `delegation.hpp`/`src/delegation.cpp` — load `delegations\<fp-prefix>.yaml` allowlist — *S1.3 done 2026-06-01: 25 delegation tests, 50/50 ctest pass. Glob matching with anchored `.*` (matches "shell.exec" but not "my.shell.exec"). `resolve_delegation` tries .json then .yaml, errors with `.field == "delegation_file"`. Spec review PASSED. Quality review APPROVED (no fixes needed). 1:1 TEST_CASE:ctest ratio verified.*
- [x] **S1.4** Test: parse sample config + delegation, assert correct tools per fingerprint — *S1.4 done 2026-06-01: 8 integration tests, 60/60 ctest pass on clean reconfigure. End-to-end: load_config + resolve_delegation + is_allowed work together. CLI smoke: `shadow-agent.exe --config <good>` exits 0 with spdlog "config loaded"; `--config <bad>` exits 1 with field name. `run_cli` uses `CreateProcessW` + handle redirection (NOT cmd.exe / `_wsystem` — that path was broken on this host). Implementer hit 10-min timeout; orchestrator applied CreateProcessW fix + ASCII test-name fix + clean cmake reconfigure.*

**Verify:** `ctest -R "config|delegation|integration" --output-on-failure` → all 60/60 green.

---

## S2 — HTTP+TLS server, MCP routing

- [ ] **S2.1** `server.hpp`/`src/server.cpp` — cpp-httplib + bs-transport mTLS. Endpoints: `GET /health`, `GET /tools`, `POST /mcp`, `POST /v1/tools/call`, `GET /v1/sessions[/<id>]`
- [ ] **S2.2** Client cert extraction via `bs::transport::peer_public_key_hex(SSL*)`; match against `authorized_clients`; 401 if unknown
- [ ] **S2.3** Test: integration test with minted cert; assert 200/401/403 for allowed/denied/unknown

**Verify:** `curl --cert ... https://100.124.169.66:9101/health` from a Tailscale peer returns `{ok:true}`.

---

## S3 — Shell tool (reuses bs-client core)

- [ ] **S3.1** Verify `bs-client-core.lib` exists and has the two-thread relay; refactor if not
- [ ] **S3.2** `tools/shell.hpp`/`src/shell.cpp` — `shell.open` / `shell.exec` / `shell.input` / `shell.tail` / `shell.resize` / `shell.close`
- [ ] **S3.3** Session multiplexer + ring buffer persistence to `C:\Users\Shadow\.shadow-agent\sessions\<id>\output.bin`; 24h idle expiry
- [ ] **S3.4** Test: open session, exec PowerShell, re-attach, tail scrollback

**Verify:** From linux-b: `tools/call name="shell.exec" args={session:"smoke", cmd:"Get-Date"}` returns the date.

---

## S4 — Filesystem tool

- [ ] **S4.1** `tools/fs.hpp`/`src/fs.cpp` — `fs.list` / `fs.read` / `fs.write` / `fs.stat` / `fs.delete`
- [ ] **S4.2** Sandbox: jail under `C:\SFTP\agent\` + `C:\Users\Shadow\.shadow-agent\`; protect `authorized_clients`
- [ ] **S4.3** Symlink guard via `GetFinalPathNameByHandle` (no jail escape)
- [ ] **S4.4** Test: read bs-server.exe; list SFTP root; deny `C:\Windows\System32\config\SAM`; write+delete in jail

**Verify:** MCP `tools/call name="fs.list" args={path:"C:/SFTP/agent"}` returns the post-cleanup tree.

---

## S5 — Windows CUA tool

- [ ] **S5.1** `tools/cua.hpp`/`src/cua.cpp` — wraps `127.0.0.1:8765` HTTP. `cua.screenshot` / `cua.click` / `cua.type` / `cua.hotkey` / `cua.mouse_move` / `cua.scroll` / `cua.wait`
- [ ] **S5.2** Require `cua.*` delegation AND user-session worker reachable (named pipe `\\.\pipe\ShadowAgent`)
- [ ] **S5.3** Test: `[!integration]` — live CUA service required

**Verify:** From linux-b: `tools/call name="cua.screenshot" args={}` returns a valid PNG > 100 KB.

---

## S6 — Hermes local chat tool

- [ ] **S6.1** `tools/hermes.hpp`/`src/hermes.cpp` — wraps `127.0.0.1:8787`. `hermes.chat` / `hermes.chat_async` / `hermes.job_status` / `hermes.job_aar`
- [ ] **S6.2** Document recursion risk; require explicit `hermes.*` in delegation
- [ ] **S6.3** Test: smoke against live FastAPI

**Verify:** From linux-b: `hermes.chat "echo 'shadow-agent works'"` reply contains the echoed string.

---

## S7 — Roblox Studio tool

- [ ] **S7.1** `tools/roblox.hpp`/`src/roblox.cpp` — MCP transparent proxy to running Roblox Studio
- [ ] **S7.2** Discover running Studio's MCP port from `%APPDATA%\.studio-mcp\config.json`
- [ ] **S7.3** Test: `[!integration]` — requires Studio running

**Verify:** End-to-end: `roblox.get_file_tree` returns the current place hierarchy.

---

## S8 — Native Win32 tool

- [ ] **S8.1** `tools/win32.hpp`/`src/win32.cpp` — `win.service.list/query` / `win.process.list/kill` / `win.scheduled.list`
- [ ] **S8.2** **Jail-break tool:** NOT in default delegation; must be explicit per-client
- [ ] **S8.3** Test: list `BFE`; refuse `win.*` from SFTP-delegated client (403)

**Verify:** Two clients with different delegations see different `tools/list` outputs.

---

## S9 — Windows Service install + worker supervision

- [ ] **S9.1** `install-service.ps1` — register `ShadowAgentSupervisor` (LocalSystem, Automatic, restart on fail w/ 30s backoff)
- [ ] **S9.2** `install-worker.ps1` — register hidden user task `ShadowAgentWorker` via `CREATE_NO_WINDOW`
- [ ] **S9.3** Named pipe IPC: `\\.\pipe\ShadowAgent` between supervisor and worker
- [ ] **S9.4** Per the standing rule: verify `sc query`, `Get-ScheduledTask`, `Get-NetTCPConnection`, Task Manager shows no window, `Get-Process | ? MainWindowHandle` returns nothing for shadow-agent.exe
- [ ] **S9.5** Test: install; restart; kill worker; verify supervisor restarts it within 30s; uninstall cleanly

**Verify:** All four checks pass. No black console popups during install/restart cycle.

---

## S10 — Audit log + per-client rate limit

- [ ] **S10.1** Structured `audit` event for every `tools/call` in `daemon.log`: `{ts, client_fingerprint, tool_name, args_summary, duration_ms, ok}`; rotate daily, keep 14 days
- [ ] **S10.2** Per-client rate limit (60/min default, configurable per delegation); 429 + `Retry-After` on exceed; log violation

**Verify:** Generate 61 calls in 60s from one client → 60 OK, 1 with 429.

---

## S11 — Remote client SDK (Python)

- [ ] **S11.1** `shadow-agent-client` package: `client.py` (~200 lines) with `Shadow(host, cert, key)` + `.shell` / `.cua` / `.fs` / `.hermes` / `.roblox` / `.win` namespaces
- [ ] **S11.2** `Shadow.shell` context manager: `with s.shell.session("deploy") as sh: sh.exec(...)`; auto-close on exit
- [ ] **S11.3** Test: from a Linux Tailscale host, install SDK, call each tool family, assert round-trip

**Verify:** A real Linux box using the SDK completes an end-to-end session.

---

## S12 — End-to-end smoke + docs

- [ ] **S12.1** `tools\smoke-e2e.py` — from linux-b: shell + cua + fs + hermes, all in <30s. Writes `C:\SFTP\agent\shadow-agent-smoke-<date>.json`. Pass 3× in a row.
- [ ] **S12.2** `REMOTE-OPS-GUIDE.md` — new "shadow-agent" section: install, delegation YAML, keygen, troubleshooting
- [ ] **S12.3** `Roblox-AI.md` — top-of-doc link + "what changed" note
- [ ] **S12.4** Day-2 skill: `skills/devops/shadow-agent-operations/SKILL.md`

**Verify:** Smoke passes 3×. REMOTE-OPS-GUIDE has the new section.

---

## Acceptance (Phase 12 — done means)

- [ ] `sc query ShadowAgentSupervisor` → RUNNING, Automatic
- [ ] `Get-ScheduledTask ShadowAgentWorker` → Ready, no console window
- [ ] `curl https://100.124.169.66:9100/health` from linux-b → `{"ok":true,...,"tools":<count>}`
- [ ] `shell.exec` from outside returns current Windows user
- [ ] `cua.screenshot` returns valid PNG > 100 KB
- [ ] `fs.read` of `C:\Windows\System32\config\SAM` → 403 (jail)
- [ ] `win.process.kill` from SFTP-delegated client → 403 (delegation)
- [ ] `hermes.chat` round-trip works
- [ ] No black console popup during install/restart
- [ ] `tools/smoke-e2e.py` passes 3× in a row

---

## Open questions (resolved 2026-06-01)

- [x] **Q1** Delegation defaults: **kitchen sink for linux-b** → `[shell.*, cua.*, fs.*, hermes.*, roblox.*, win.*]`. **The delegation is a cooperation policy, NOT a security boundary** — the mTLS client cert is the gate. If a remote AI holds a recognized cert, it's trusted; the YAML just declares *what we usually want this peer to do*. A separate, narrower delegation is still possible per-client if a particular peer should be sandboxed. MacBook → `[fs.*]` (different cert posture, file-sync only). See PLANS-shadow-agent.md § Open Questions.
- [x] **Q2** Port: **9100/9101** (MCP-over-HTTP / HTTP-JSON-RPC). Tailscale-only bind, refuse to start without `100.124.169.66`.
- [x] **Q3** Source location: **`C:\SFTP\agent\shadow-agent\`** (deployment target). Build → `…\shadow-agent\build\windows-msvc-debug\`. Install rsynced to `C:\Program Files\shadow-agent\bin\`.
- [x] **Q4** Roblox MCP port: **auto-detect.** Read `%APPDATA%\.studio-mcp\config.json`, fall back to `HKCU\Software\Roblox\RobloxStudio\MCP` registry, fallback default `7800`. If no Studio is reachable in 2s, log a clear warning and return 503 from `roblox.*` calls.
