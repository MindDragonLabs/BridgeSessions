# bridgesessions — AUTONOMOUS.md

**Purpose:** Guide for autonomous coding agents (Claude Code, Codex, Hermes subagents) when working on this project.

**Effort level:** `high` (complex reasoning — mesh protocol, C++23 systems programming, TLS/PKI, cross-platform).

---

## Agent Dispatch Rules

### Which agent for which task

| Task type | Agent | Why |
|-----------|-------|-----|
| New protocol types / message definitions | Claude Code / Hermes subagent | Strongest at spec-level C++ design |
| Refactoring, bug fixes, test writing | Any subagent | All handle this well |
| Code review / security audit | Hermes subagent | Context-aware, knows full codebase |
| Documentation, markdown, scripts | Hermes subagent | Lighter-weight, in-session |
| Parallel independent tasks | 2–3 parallel Hermes delegate_task | Isolated contexts, each gets own terminal |
| Build/test regression | Hermes terminal directly | No agent needed — just `_run_tests.ps1` |

### Effort guidance

| Level | When |
|-------|------|
| `high` | Protocol design, TLS/PKI changes, new C++23 patterns, architecture decisions |
| `medium` | Bug fixes, test additions, documentation |
| `low` | Formatting, trivial fixes |

---

## Project Knowledge (inject into agent context)

### Architecture

Single-file C++23 mesh relay: `bridgesessions.cpp` (~5,800 lines).

```
bridgesessions.cpp
├── Includes + platform abstractions   (1–60)
├── Message types (22) + variant       (100–430)
├── Codec (zstd, SHA-256)              (430–830)
├── Identity + TLS (ed25519, mTLS)     (830–1190)
├── Frame I/O                          (1190–1280)
├── Ring buffer (thread-safe)          (1280–1350)
├── OSC 52 scanner                     (1350–1390)
├── Session + ConPTY/PTY               (1390–2000)
├── Logging (spdlog)                   (2000–2100)
├── Session Registry                   (2100–2500)
├── Mesh Controller (select loop)      (2500–4100)
├── Terminal raw mode                  (4100–4200)
├── Image render (chafa on POSIX)      (4200–4320)
├── Peer helpers                       (4320–4440)
└── main() + CLI11 subcommands         (4440–4873)
```

**Key classes:**
- `MeshController` — central class: `conns_` (vector of Conn), `sessions_` (SessionRegistry), `config_` (MeshConfig), `run()` event loop
- `SessionRegistry` — thread-safe session lifecycle: attach (multi-peer), detach, kill, resurrect, reap_dead, prune_idle
- `Session` — PTY/ConPTY state machine: Running, Attached, Detached, Died

**Key patterns:**
- `connect_and_hello()` — shared TCP+TLS+Hello helper for shell/sessions/health
- `find_peer_addr()` — resolve peer name from seeds + discovered
- `set_socket_timeouts()` — apply SO_RCVTIMEO/SO_SNDTIMEO (used for handshake + steady-state)

### Build

**Windows:**
```powershell
cl /std:c++latest /EHsc /MD /utf-8 /DBS_TESTING /DBS_NO_NAT /DBS_NO_WEBRTC /DBS_NO_DHT ...
```

**Linux:**
```bash
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT -o bsmesh bridgesessions.cpp -lssl -lcrypto -lzstd -lspdlog -pthread -lfmt
```

**macOS:**
```bash
clang++ -std=c++2b -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT -I$(brew --prefix)/include -L$(brew --prefix)/lib -o bridgesessions bridgesessions.cpp -lssl -lcrypto -lzstd -lspdlog -lfmt -lpthread
```

### Dependencies

- OpenSSL 3.x (TLS 1.3, ed25519, SHA-256)
- zstd (wire compression)
- CLI11 (CLI parsing)
- spdlog (JSON logging)
- nlohmann/json (serialization)
- Catch2 (tests)
- fmt (spdlog dep)

Optional (behind `#ifndef BS_NO_*`): miniupnpc (NAT), libdatachannel (WebRTC), libjuice (ICE).

### Tests

16 suites, ~1,009 assertions:

```
Layer unit:  test_message, test_codec, test_frame_io, test_osc52,
             test_ring_buffer, test_identity, test_config
Layer integration: test_tls, test_tls_reliability,
             test_authorized_keys_reload, test_session,
             test_session_registry, test_relay, test_multi_attach,
             test_mesh, test_mesh_reliability
```

Run:
```powershell
powershell -ExecutionPolicy Bypass -File _run_tests.ps1 -Suite <name>
```

All tests include `bridgesessions.cpp` directly with `BS_TESTING` defining out `main()`.

### Deployment

| Node | Daemon | Binary name | Config path |
|------|--------|-------------|-------------|
| Shadow (Win) | NSSM | bridgesessions.exe | `~/.bridgesessions/config` |
| linux-b (Linux) | systemd | bsmesh | `~/.bridgesessions/config` |
| linux-a (Linux) | systemd | bsmesh | `~/.bridgesessions/config` |
| macos-peer (macOS) | LaunchAgent | bridgesessions | `~/.bridgesessions/config` |

Health check: `bash matrix.sh` (project root) runs full 12-direction health matrix.

### Common pitfalls (for agents)

1. **Single-file — no library targets.** There are no `bs-protocol`, `bs-transport`, `bs-server`, `bs-client` libs or directories. Everything is in `bridgesessions.cpp`.
2. **MSVC on Windows.** POSIX-only constructs (`forkpty`, `<poll.h>`, `sigaction`) are behind `#ifdef _WIN32` / `#else`. Test macros: `BS_CMD(win_cmd, posix_cmd)` for cmd args.
3. **`BS_TESTING` guard.** All test builds must define `BS_TESTING` to exclude `main()`. Test files `#include "bridgesessions.cpp"` directly.
4. **Config file format is `key value` with `#` comments** (NOT YAML/JSON). CRLF-safe parsing strips trailing `\r`.
5. **Do NOT persist discovered peers to config** — creates config_reload churn loop. Runtime state only.
6. **Always use `connect_and_hello()` helper** for outbound connections, never inline the TCP+TLS+Hello pattern.
7. **`ssl_handshake_blocking()`** replaces `SSL_connect` — uses `select()` readiness wait + deadline.
8. **`bootstrap_identity()`** takes the FULL `~/.bridgesessions` directory path, not just `HOME`.

### Subagent rules (from experience)

1. **One subagent at a time for code changes** — parallel delegates stomp each other on the single-file.
2. **Always git stash before dispatching** — safe undo if generated code breaks compilation.
3. **Never `git checkout --` to undo subagent damage** — destroys uncommitted work. Use `git stash` or `git diff HEAD > /tmp/backup`.
4. **Treat subagent output as draft** — expect to fix compilation errors yourself.
5. **Pass include file signatures in context** — subagents don't know exact API signatures.

### Cross-platform test helpers

```cpp
#ifdef _WIN32
#define BS_CMD(win_cmd, posix_cmd) win_cmd
#else
#define BS_CMD(win_cmd, posix_cmd) posix_cmd
#endif

// Temp file creation
#ifdef _WIN32
// GetTempPathA + GetTempFileNameA
#else
// mkstemp
#endif
```

---

## Key files

| File | Purpose |
|------|---------|
| `bridgesessions.cpp` | Single source — all mesh code |
| `tests/*.cpp` | 16 Catch2 test suites |
| `_run_tests.ps1` | Build + run all tests on Windows |
| `matrix.sh` | 4-node health matrix across cluster |
| `install-daemon.ps1` | Windows NSSM service setup |
| `configure-firewall.ps1` | Windows firewall rules |
| `docs/GUIDELINE.md` | Design sketch and vision |
| `docs/PLANS.md` | Implementation plan |
| `docs/TODO.md` | Task checklist |
| `todo.md` (root) | Reliability deployment log |
