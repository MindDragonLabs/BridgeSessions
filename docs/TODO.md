# bridgesessions — TODO

**Status:** v0.5.1 — all 13 phases complete, 74/74 tests, 20 message types
**Active phase:** Windows native port (MSVC 2022, no POSIX emulation)
**Last review:** 2026-05-31 — full codebase audit vs PLANS/TODO

---

## v1 Core (Phases 0–12) — COMPLETE

| Phase | Deliverable | Status |
|-------|------------|--------|
| P0 | CMake scaffold, presets, cross-compile toolchains | ✅ |
| P1 | bs-protocol (20 msg types, zstd codec, 18 tests) | ✅ |
| P2 | bs-transport (TLS 1.3 ed25519, TOFU, frame I/O, 6 tests) | ✅ |
| P3 | bs-server (poll loop, 64 conn, PTY relay) | ✅ |
| P4 | bs-client (raw terminal, SIGWINCH, Ctrl+D, 1023-line main) | ✅ |
| P5 | Ring buffer (1 MB, thread-safe, scrollback, 11 tests) | ✅ |
| P6 | Session multiplexer (7 states, auto-restart circuit breaker, 19 tests) | ✅ |
| P7 | Clipboard (macOS NSPasteboard, Linux xclip/wl-paste, OSC 52, 11 persistence tests) | ✅ |
| P8 | Reconnection (exp backoff 100ms→5s, ±25% jitter, 30s deadline) | ✅ |
| P9 | Keygen + authorize (ed25519, authorized_keys flat file) | ✅ |
| P10 | Session persistence (JSON save/load, atomic write, resurrect) | ✅ |
| P11 | BridgeSpace integration (Unix socket bridge mode) | ✅ |
| P12 | Production hardening (CLI11, spdlog, TLS cache, pong timeout, stats, pipe mode) | ✅ |
| E2E | Real PTY macOS → linux-a over Tailscale/VPN | ✅ |

---

## Wave 2 — Image & Animation Transfer — COMPLETE

| ID | Task | Status |
|----|------|--------|
| W2-1 | Image Transfer Protocol (ImageData, ImageFrame, ImageAck msg types) | ✅ |
| W2-2 | Animated GIF support (per-frame timing, parse_gif_metadata) | ✅ |
| W2-3 | Terminal rendering via chafa (fork+execl integration) | ✅ |
| W2-4 | `bs-client image <path>` and `bs-client anim <gif>` CLI commands | ✅ |
| W2-5 | Security (images over existing TLS + TOFU) | ✅ |
| W2-6 | Tests (encode/decode, size caps, compression, roundtrip) | ✅ |
| W2-7 | Documentation (ARCHITECTURE.md, GUIDELINE.md updated) | ✅ |

---

## v1.0 Polish

| ID | Task | Status |
|----|------|--------|
| v1.0-1 | Linux clipboard (xclip/wl-paste) | ✅ `clipboard_linux.cpp` — 183 lines, auto-detects Wayland vs X11 |
| v1.0-2 | libFuzzer 1M+ iterations on decode() | ❌ Needs clang on Linux |
| v1.0-3 | Memory profiling (valgrind/massif, heaptrack) | ❌ Needs Linux |
| v1.0-4 | Cross-compile toolchains (linux/amd64, linux/arm64) | ✅ `cmake/toolchain-linux-*.cmake` exist — sysroot verification pending |
| v1.0-5 | clang-tidy zero warnings | ❌ `.clang-tidy` has hardcoded `/Users/jeffersonnunn/` paths — needs rewrite |

---

## v2 Deferred — DEFERRED (after Windows port)

| ID | Feature | Deferred reason |
|----|---------|-----------------|
| v2-1 | QUIC via msquic | Transport backend swap — do after TCP path works on Windows |
| v2-2 | Bootstrapping (SSH key deploy) | Design work — useful on Windows |
| v2-3 | NAT traversal (Tailscale/WireGuard) | Integration, not core protocol |
| v2-4 | Predictive echo (Mosh-style) | Protocol change — do after stable relay |
| v2-5 | Terminal capability negotiation | Terminfo parser — useful on Windows |
| v2-6 | Session recording (`--record-sessions`) | Feature add — useful on Windows |
| v2-7 | SRV record discovery | DNS client — low priority |
| v2-8 | Rich clipboards (MIME type) | New message type — useful on Windows |
| v2-9 | Large clipboard chunking (>64KB) | Protocol change |
| v2-10 | cgroup v2 isolation | **Linux-only** — not applicable to Windows |
| v2-11 | `bs-hostd` dispatcher | New binary — architectural |
| v2-12 | mmap ring buffer | Platform-specific — Windows has `MapViewOfFile` |
| v2-13 | io_uring PTY relay | **Linux-only** — not applicable to Windows |
| v2-14 | concurrencpp (C++20 coroutines) | Threading model swap — low priority |

---

## Windows Native Port — ACTIVE

See: `PLANS-WINDOWS.md`

| Phase | Target | Status |
|-------|--------|--------|
| W0 | Toolchain: VS Build Tools 2022 + CMake + vcpkg deps | `[ ]` |
| W1 | bs-protocol: compile with MSVC, run 18 tests | `[ ]` |
| W2 | bs-transport: compile frame_io + tls, Winsock2 adaptation | `[ ]` |
| W3 | bs-client: Win32 Console API + 2-thread relay + clipboard | `[ ]` |
| W4 | bs-server: ConPTY replacement for fork/PTY (optional) | `[ ]` |
| W5 | End-to-end: Windows bs-client → Linux bs-server relay | `[ ]` |

---

## shadow-agent — PLANNED (post-Windows-port workstream)

See: [`PLANS-shadow-agent.md`](../../PLANS-shadow-agent.md) · [TODO-shadow-agent.md](../../TODO-shadow-agent.md)

**Goal:** One Shadow-resident daemon, one mTLS endpoint, multiplexes shell/cua/fs/hermes/roblox/win32 tools for any AI on the Tailscale mesh.

| Phase | Target | Status |
|-------|--------|--------|
| S0 | Plan + skill docs | `[ ]` |
| S1 | Skeleton (CMake, config, delegation) | `[ ]` |
| S2 | HTTP+TLS server, MCP routing | `[ ]` |
| S3 | shell.* | `[ ]` |
| S4 | fs.* (jailed) | `[ ]` |
| S5 | cua.* (user-session worker required) | `[ ]` |
| S6 | hermes.* (wraps existing FastAPI) | `[ ]` |
| S7 | roblox.* (proxy) | `[ ]` |
| S8 | win.* (jail-break, not in default delegation) | `[ ]` |
| S9 | Windows service install (supervisor + hidden worker) | `[ ]` |
| S10 | Audit log + rate limit | `[ ]` |
| S11 | Python SDK | `[ ]` |
| S12 | End-to-end smoke + docs | `[ ]` |

**Blocked by:** S0 starts immediately (docs only). S1+ needs Windows port W3 (bs-client refactor into library) and open-question answers (delegation defaults, port, source location, Roblox auto-detect).

---

## Code Standard Violations (audit findings)

- [ ] **14/15 headers use `#pragma once`** — AUTONOMOUS.md says traditional include guards. Fix or update AUTONOMOUS.md.
- [ ] **`.clang-tidy` hardcodes `/Users/jeffersonnunn/`** — rewrite with portable paths
