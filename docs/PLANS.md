# bridgesessions — Implementation Plan

**Spec:** [ARCHITECTURE.md](./ARCHITECTURE.md)
**Sketch:** [GUIDELINE.md](./GUIDELINE.md)
**Language:** C++23
**Build:** CMake 3.25+
**Dependencies:** 2 link-time (OpenSSL 3.0+, zstd), 4 header-only (Catch2, spdlog, CLI11, nlohmann/json)

---

## Current Sprint: Windows Native Port (v0.6.0-windows)

**Target:** Windows 11 x64, MSVC 2022, native Win32 APIs. No MinGW, WSL, POSIX emulation.

**Execution plan:** [`PLANS-WINDOWS.md`](../../PLANS-WINDOWS.md) (project root)

| Phase | What | Effort |
|-------|------|--------|
| W0 | Toolchain: VS Build Tools 2022 + CMake + vcpkg deps | 1 hour |
| W1 | bs-protocol: compile MSVC, run 18 tests | 1 hour |
| W2 | bs-transport: compile frame_io + tls, Winsock2 adaptation | 1–2 hours |
| W3 | bs-client: Win32 Console API + 2-thread relay + clipboard | 3–5 days |
| W4 | bs-server: ConPTY replacement for fork/PTY (optional) | 3–5 days |
| W5 | Integration: Windows bs-client ↔ Linux bs-server | 1–2 days |

**Portable as-is:** bs-protocol (codec, messages, zstd), bs-transport (TLS, frame I/O — OpenSSL only)
**Needs porting:** bs-client (termios→SetConsoleMode, poll→threads, clipboard→Win32), bs-server (fork→CreateProcess, PTY→ConPTY)

---

## New Workstream: shadow-agent (planned 2026-06-01)

**Target:** Single Shadow-resident daemon that multiplexes shell, CUA, fs, hermes.chat, roblox, and win32 behind **one** mTLS+ed25519 MCP endpoint on Tailscale `100.124.169.66:9100` so any AI on the mesh can drive Shadow with one credential.

**Execution plan:** [`PLANS-shadow-agent.md`](../../PLANS-shadow-agent.md) · **TODO:** [`TODO-shadow-agent.md`](../../TODO-shadow-agent.md) (project root)
**Day-2 ops skill:** [`skills/devops/shadow-agent/`](../../../../Users/Shadow/AppData/Local/hermes/skills/devops/shadow-agent/SKILL.md) (in-repo)

| Phase | What | Effort | Status |
|-------|------|--------|--------|
| S0 | Plan + skill docs (umbrellas + in-repo SKILL.md) | 0.5 hr | `[ ]` |
| S1 | Skeleton: CMake, config, delegation, tests | 1 day | `[ ]` |
| S2 | HTTP+TLS server, MCP routing, 401/403 | 1.5 days | `[ ]` |
| S3 | shell.* (reuses bs-client-core two-thread relay) | 1.5 days | `[ ]` |
| S4 | fs.* (jailed under C:\SFTP\agent\) | 0.5 day | `[ ]` |
| S5 | cua.* (requires user-session worker for GUI) | 0.5 day | `[ ]` |
| S6 | hermes.* (wraps existing FastAPI on 8787) | 0.5 day | `[ ]` |
| S7 | roblox.* (transparent MCP proxy to Studio) | 1 day | `[ ]` |
| S8 | win.* (jail-break tool, NOT in default delegation) | 0.5 day | `[ ]` |
| S9 | Windows service install (LocalSystem supervisor + hidden user worker) | 1 day | `[ ]` |
| S10 | Audit log + per-client rate limit (60/min default) | 0.5 day | `[ ]` |
| S11 | Python SDK (`shadow-agent-client`) for other Hermes instances | 1 day | `[ ]` |
| S12 | End-to-end smoke + REMOTE-OPS-GUIDE update | 0.5 day | `[ ]` |

**Reuses:** `bs-transport` (mTLS+ed25519), `bs-client-core.lib` (ConPTY two-thread relay), `bs-protocol` (codec). Talks to existing primitives: `bs-server` (loopback), Hermes FastAPI (`:8787`), Windows CUA (`:8765`), BvSshServer, Roblox Studio MCP.
**Open:** delegation defaults, port choice, source location, Roblox MCP auto-detect vs hardcoded. See `PLANS-shadow-agent.md` § Open Questions.

---

## v1 Core — COMPLETE (macOS + Linux)

13 phases, 74/74 tests on macOS arm64 + Linux x86_64. 20 message types (17 base + 3 Wave 2 image types). See [TODO.md](./TODO.md) for per-phase status.

## Wave 2 — COMPLETE

ImageData, ImageFrame, ImageAck message types. chafa terminal rendering. Client `image`/`anim` CLI commands. Full test coverage. All 7 items done.

## v1.0 Polish — 2 remaining

- [ ] **v1.0-2** libFuzzer 1M+ iterations on decode()
- [ ] **v1.0-5** clang-tidy zero warnings (`.clang-tidy` needs portable path rewrite)

## v2 Deferred — 14 items

Deferred until after Windows port. See [TODO.md](./TODO.md) for full list. Post-port priority: v2-2 (bootstrapping), v2-6 (recording), v2-8 (rich clipboard), v2-1 (QUIC).

---

## Why C++23

Zero-GC latency. `std::expected<T,E>`, `std::print`, `std::flat_map`, `std::mdspan`, `deducing this`, `[[assume(expr)]]`. OpenSSL native bindings. Single static binary deploy (~5 MB). msquic path to v2 QUIC — library swap, not rewrite. RAII everywhere.
