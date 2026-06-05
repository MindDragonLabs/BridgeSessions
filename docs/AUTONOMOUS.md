# bridgesessions — AUTONOMOUS.md

**Purpose:** Guide for autonomous coding agents (Claude Code, Codex, Hermes subagents) when working on this project.

**Effort level:** `high` (complex reasoning needed — protocol design, C++23 systems programming, TLS/PKI).

---

## Agent Dispatch Rules

### Which agent for which task

| Task type | Agent | Why |
|-----------|-------|-----|
| New protocol types / message definitions | Claude Code | Strongest at spec-level C++ design |
| Refactoring, bug fixes, test writing | Claude Code or Codex | Both handle this well |
| Code review / security audit | Claude Code | Better at nuanced security review |
| Documentation, markdown, install scripts | Codex or Hermes subagent | Lighter-weight tasks |
| Parallel independent work (W2-1 + W2-2 simultaneously) | 2× Claude Code (-p print mode) | Print mode is fastest for one-shot tasks |
| Build/test regression after changes | Hermes terminal directly | No agent needed — just `ctest` |

### Effort guidance

Set `--effort high` (or `--effort max`) for:
- Protocol design decisions (message types, framing, security)
- TLS/PKI code changes
- New C++23 patterns, template metaprogramming
- Architecture document updates

Set `--effort medium` for:
- Bug fixes in existing code
- Test additions
- Documentation

Set `--effort low` for:
- Formatting, linting, trivial fixes
- install.sh maintenance

---

## Project Knowledge (inject into agent context)

### Architecture

```
bs-protocol/     → libbsprotocol.a   (message types, codec, zstd compression)
bs-transport/    → libbstransport.a  (TLS 1.3 + ed25519, frame I/O, TOFU)
bs-server/       → bs-server         (Linux daemon — PTY relay, session mgr)
bs-client/       → bs-client         (macOS relay — clipboard, terminal, CLI)
```

- **Language:** C++23 (gcc 14+ / clang 18+)
- **Build:** CMake 3.25+, 2 link deps (OpenSSL 3.0+, zstd)
- **Tests:** Catch2, 74 tests, all pass on macOS + Linux
- **Auth:** ed25519 mutual TLS + TOFU (no CA)
- **Wire format:** `[stream_id: u16] [type: u8] [flags: u8] [len: u16] [data]`
- **Port:** TCP 9948
- **Compression:** zstd on frames >256 bytes

### Key files

| File | Purpose |
|------|---------|
| `docs/GUIDELINE.md` | Design sketch (why, what, decisions) |
| `docs/ARCHITECTURE.md` | Full spec with ADRs (how, details) |
| `docs/PLANS.md` | Implementation phases + timeline |
| `docs/TODO.md` | Active checklist (what's next) |
| `docs/AUTONOMOUS.md` | This file — agent dispatch rules |
| `install.sh` | One-command dev environment setup |
| `bs-protocol/include/bsprotocol/message.hpp` | All message type definitions |
| `bs-protocol/include/bsprotocol/codec.hpp` | encode/decode + zstd |
| `bs-transport/src/tls.cpp` | TLS 1.3 + ed25519 + TOFU |
| `bs-server/src/session_manager.cpp` | Session lifecycle + multiplexing |
| `bs-server/src/ring_buffer.hpp` | Thread-safe circular buffer |
| `bs-client/src/clipboard_bridge.mm` | ObjC++ NSPasteboard bridge |
| `bs-client/src/main.cpp` | CLI + relay loop |

### Code standards

- **RAII for all resources** — `unique_ptr` for fds, custom deleters for SSL_CTX. No raw new/delete.
- **`std::expected<T, Error>` for fallible ops** — prefer over exceptions across thread boundaries.
- **Header-only where possible** — protocol types, codec, ring buffer. `.cpp` for non-template code.
- **`std::variant` + `std::visit` for message types** — compile-time polymorphism.
- **Sanitizers in CI** — ASan + UBSan + TSan on every PR. libFuzzer on decode().
- **No `#pragma once`** — use traditional include guards (`#ifndef BS_SERVER_SESSION_HPP … #endif`).
- **`[[nodiscard]]` on all pure-value functions.**

### Test commands

```bash
# Build and test
cd ~/bridgesessions
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j$(nproc)
ctest --test-dir build/release --output-on-failure

# Individual test suite
./build/release/bs-protocol-tests
./build/release/bs-transport-tests
./build/release/bs-server-tests
./build/release/bs-client-tests

# Static analysis
clang-tidy -p build/release bs-*/**/*.cpp bs-*/**/*.hpp
```

### What NOT to do

- ❌ Don't add new dependencies without updating `install.sh` and CMakeLists.txt
- ❌ Don't change the wire format without updating ARCHITECTURE.md ADRs
- ❌ Don't commit build artifacts (everything under `build/` is .gitignored)
- ❌ Don't use `#pragma once` (include guards only)
- ❌ Don't use raw `new`/`delete` (RAII only)
- ❌ Don't hardcode personal paths (e.g. `/Users/jeffersonnunn/`)
- ❌ Don't modify other Hermes profiles without explicit direction

---

## Current Sprint: Wave 2 — Image & Animation Transfer

**Active TODO items (see TODO.md for full list):**

1. **W2-1** Image Transfer Protocol — binary frame type for PNG/JPEG
2. **W2-2** Animated GIF Support — per-frame timing
3. **W2-3** Terminal Rendering — chafa/ansi-img fallback, optional SIXEL/kitty
4. **W2-4** Client UI — `bs-client image <path>` and `bs-client anim <gif>`
5. **W2-5** Security — TLS channel + TOFU for image frames
6. **W2-6** Tests — encode/decode, TLS roundtrip, timing
7. **W2-7** Documentation — update GUIDELINE/ARCHITECTURE/PLANS/TODO

### Protocol design notes for agents

New message types needed (add to `MessageType` enum in `message.hpp`):

```cpp
ImageData   = 18,  // static image (PNG/JPEG)
ImageFrame  = 19,  // animated GIF frame
ImageAck    = 20,  // client ACK for received image
```

New message structs:

```cpp
struct ImageDataMsg {
    uint8_t format;      // 0=PNG, 1=JPEG
    std::string name;    // filename hint
    std::vector<uint8_t> data;  // raw bytes (zstd-compressed on wire)
};

struct ImageFrameMsg {
    uint8_t format;      // always GIF for now
    uint32_t delay_ms;   // frame display time
    std::vector<uint8_t> data;  // frame bytes
};
```

### Dependencies for image support

- **stb_image.h** — header-only PNG/JPEG/GIF loader (public domain, drop in `bs-client/src/`)
- **chafa** — system package, used as fallback renderer (`popen("chafa ...", "r")` on client)
- **libsixel** — optional, for SIXEL-capable terminals

---

## Agent Invocation Templates

### Claude Code (print mode — preferred for coding tasks)

```bash
claude -p "Implement W2-1: add ImageData and ImageFrame message types to bs-protocol/include/bsprotocol/message.hpp and bs-protocol/src/codec.cpp" \
  --effort high --max-turns 15 --allowedTools "Read,Edit,Write,Bash"
```

### Claude Code (interactive — for multi-turn exploration)

```bash
# Terminal 1: start Claude
tmux new-session -d -s bs-w2 -x 140 -y 40
tmux send-keys -t bs-w2 'cd ~/bridgesessions && claude' Enter
sleep 5 && tmux send-keys -t bs-w2 Enter  # trust dialog

# Terminal 2: send task
tmux send-keys -t bs-w2 'Design the image transfer protocol. Add ImageData, ImageFrame, ImageAck message types. Update codec. Write tests.' Enter

# Monitor
tmux capture-pane -t bs-w2 -p -S -50
```

### Parallel work (two Claude Code instances)

```bash
# Agent A: Protocol + codec
claude -p "Add ImageData/ImageFrame types to bs-protocol. Update codec. Write tests." \
  --effort high --max-turns 15 > /tmp/w2-codec.json &

# Agent B: Client commands
claude -p "Add `bs-client image` and `bs-client anim` CLI commands using CLI11. Pipe to chafa." \
  --effort high --max-turns 15 > /tmp/w2-client.json &

wait
```

### Codex (for lighter tasks)

```bash
codex exec "Update install.sh to include chafa and ansi-img dependencies" \
  --full-auto
```

---

## After Every Change

```bash
# 1. Build
cmake --build build/release -j$(nproc)

# 2. Test
ctest --test-dir build/release --output-on-failure

# 3. If you changed .hpp files, re-run clang-tidy
clang-tidy -p build/release bs-*/**/*.{cpp,hpp}

# 4. Commit with descriptive message
git add -A
git commit -m "W2-N: brief description"
```
