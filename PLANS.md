# BridgeSessions v2.0.9-alpha5 — Phase Plan

**Scope:** BridgePanel "New Session" + "Connect" UX + package refactor. Two features, one release, no C++ changes (Phase 1 design skeleton is backend stubs only).

**Status:** Phase 0 PENDING. Phase 1 delivered (Kimi design skeleton in 1,840-line monolith).

## Phases

| Phase | Work | Gate |
|-------|------|------|
| **P0** | **Refactor bridgepanel.py into package.** Split 1,840-line monolith into `bridgepanel/` package: `consts.py` (constants), `files.py` (paths/safety/markdown), `api.py` (IPC/tree), `html.py` (INDEX_HTML), `server.py` (HTTP handler), `__init__.py` (CLI entry). Shim `bridgepanel.py` that delegates to package. Migrate Kimi's Phase 1 design into the new structure. | 25/25 tests pass; `python3 tools/bridgepanel/bridgepanel.py serve` works; `python3 -m bridgepanel serve` works; visual: UI loads with "+" buttons, modal, Connect tab. |
| **P1** | (DELIVERED — Kimi design skeleton already in monolith.) Restructure into P0 modules. No logic changes. | Same gates as P0. |
| **P2a** | **Daemon IPC CREATE verb.** `handle_ipc_line`: parse `CREATE <peer> <name> <cols> <rows> <term> <command>`, find mesh conn, construct `AttachMsg`, send. | CTest: IPC CREATE → OK/ERROR paths; new session appears in MESH_TREE. |
| **P2b** | **POST /api/session/create → daemon.** Replace stub with real IPC call. Parse JSON, call daemon CREATE, return `{ok: true, session: <name>}` or error. | Panel test: create returns OK; session appears in /api/machines. Visual: click + button, fill form, session spawns on target. |
| **P2c** | **BS CLI `--session` flag.** `bs shell <peer> --session <name>` sends AttachMsg with empty command → reattach to existing session. | CTest: `--session` reattaches; `bs shell <peer> --session noexist` → ERROR. |
| **P2d** | **GET /api/session/connect** — already implemented in P1. Returns 5 harness commands. | Already tested: 25/25 pass. |
| **P3** | **Tests + verify.** New C++ tests for CREATE + `--session`. New panel tests for real create flow. Visual: browser, 0 console errors, create session on test-pc2 from panel. | 329+ CTest green; 25+ panel tests green; live browser verification. |
| **P4** | **Docs + release.** CHANGELOG, skill, HEARTBEAT, tag v2.0.9-alpha5. | Pre-publish scan clean; binaries on tag HTTP 200; remote HEAD synced. |

## Sequencing

- **P0 first** — all subsequent work touches the package. Do not add features to the monolith.
- **P2a → P2b** — daemon IPC must exist before the panel endpoint can call it.
- **P2c in parallel with P2a** — independent C++ change (CLI flag only).
- **P3 after P2a+b+c** — integration tests need real IPC.
- **P4 final** — docs reference the shipped structure.

## Residual (post-2.0.9)

- Real CUA from BridgePanel (create a CUA session, not just shell)
- `--signal-on-detach` exposed in create modal
- Server-side terminal reflow (P2 stretch from 2.0.8 — still deferred)
