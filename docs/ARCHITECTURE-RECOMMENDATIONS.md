# BridgeSessions — Architecture Recommendations (2026-08-06)

Post-audit analysis of security, scalability, and code health improvements
beyond the current P0-P3 fixes.

---

## 1. Security Hardening

### 1a. TOFU First-Connect Confirmation (P1)
**Current:** First connection to any peer pins whatever key is presented.
**Risk:** MITM on first connect = permanent compromise.
**Fix:** Add interactive confirmation prompt on first connect:
```
First contact with "example-peer" at <tailnet-ip>:19949
Fingerprint: d25519fa9cadcd62eecb73e5844d04...
Trust this peer? [y/N]
```
For scripted/agent use: `--trust-on-first-use` flag or `seed ... pubkey=` pre-pinning.

### 1b. Windows Named Pipe DACL (P1)
**Current:** `PIPE_REJECT_REMOTE_CLIENTS` blocks network, but any local user can connect.
**Fix:** Build a DACL from the current process token SID:
```cpp
PSECURITY_DESCRIPTOR sd;
BuildExplicitAccessWithName(..., SET_ACCESS, NO_INHERITANCE);
SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE);
CreateNamedPipeA(..., sd);
```

### 1c. Wire Protocol Version Negotiation (P2)
**Current:** Chunk size is a compile-time constant. Mixed-version peers fail with
"declared chunk count does not match file size" if one side changes it.
**IMPORTANT:** Adding new fields to serialized messages (HelloMsg, FileMetaMsg)
is a **wire-breaking change** — all peers must upgrade simultaneously.
Tested and confirmed: adding `protocol_version` to HelloMsg serialization
causes `hello_rejected` on all old peers.
**Fix (non-breaking):** Encode protocol version in the existing `version` string:
`"26.08.05-beta1"` → `"26.08.05-beta1+pv2"`. Parse with regex on the receiver.
Chunk size negotiation: sender already declares `total_chunks` in FileMetaMsg
which the receiver uses. This is sufficient — the receiver doesn't need to know
the raw chunk size, just how many chunks to expect.

### 1d. Session Token Rotation (P2)
**Current:** `ipc-token` and `cua-helper-token` are static files.
**Fix:** Rotate tokens on each daemon restart. CLI reads token from file each call.

---

## 2. Scalability

### 2a. Replace select() with poll() (P1 for scale)
**Current:** `select()` with FD_SETSIZE=1024 limit. Connections above 1024 silently dropped.
**Fix:** `poll()` is a near drop-in replacement, no fd limit, portable.
Already have `#include <poll.h>` in the source. ~200 LOC change.

### 2b. Non-blocking TLS Writes (P1 for scale)
**Current:** `write_frame()` blocks up to 10s per peer. One slow consumer freezes all.
**Fix:** Per-connection write buffer deferred to next select() writable event.
This is the classic C10K migration — significant but necessary for 20+ peers.

### 2c. Delta Gossip (P2)
**Current:** Full peer snapshot broadcast every 30s to every connection.
**Fix:** Version/generation comparison — only send peers that changed since last gossip.

---

## 3. Code Health / Refactoring

### 3a. Split bs-protocol.h (P1 for maintainability)
**Current:** 13,800 lines in a single header. Compile times, cognitive load.
**Recommended split:**
```
bs_msg_types.hpp      — Message structs + serialization (lines 1-1100)
bs_codec.hpp          — encode/decode/compress (lines 1100-1400)
bs_identity.hpp       — TLS/cert/auth (already exists separately)
bs_session.hpp        — SessionRegistry (already exists separately)
bs_cua.hpp            — CUA execute + helper RPC (lines 2900-3300)
bs_file_xfer.hpp      — File send/recv/transfer (lines 7400-8400)
bs_peer_resolve.hpp   — Peer resolution + levenshtein (lines 11900-12100)
bs_mesh.hpp           — MeshController (lines 6000-11800)
```
Keep `bs-protocol.h` as an umbrella that includes them all (backward compat).

### 3b. Extract Test Harness (P2)
**Current:** test_mesh.cpp is 2200 lines with massive boilerplate duplication.
**Fix:** Extract `MeshTestHarness` fixture (setup, accept_and_hello, temp home, certs).

### 3c. CMake Modernization (P3)
- Add `CMakePresets.json` for common build configs
- Add `FetchContent` for Catch2 instead of manual vendoring
- Add static analysis targets (clang-tidy, cppcheck)

---

## 4. CI/CD Pipeline

### 4a. Codeberg CI (P1)
**Missing:** No CI pipeline. All builds and tests are manual.
**Fix:** Add `.woodpecker.yml` (Codeberg CI):
```yaml
pipeline:
  build-linux:
    image: archlinux:latest
    commands:
      - pacman -Sy --noconfirm cmake ninja gcc openssl fmt spdlog zstd catch2 nlohmann-json
      - cmake -S . -B build -G Ninja
      - cmake --build build
      - ctest --test-dir build --output-on-failure
  cross-compile-windows:
    image: archlinux:latest
    commands:
      - pacman -Sy --noconfirm mingw-w64-gcc
      - # ... static OpenSSL + cross-compile
```

### 4b. Notarization Pipeline (P1 for macOS)
**Current:** Developer ID cert present but binaries not notarized. macOS 26 kills them.
**Fix:** CI step using `xcrun notarytool submit` + `xcrun stapler staple`.
Needs App Store Connect API key (create in App Store Connect → Keys).

### 4c. Automated Release Checksums (P2)
**Fix:** CI generates SHA256SUMS + SBOM on tag push. No manual checksum step.

---

## 5. Feature Recommendations

### 5a. Content-Addressed Script Library (P2)
**For:** Eliminating repeated script transfer.
**Design:**
```bash
bs script add cleanup.sh          # SHA256 → cache on all peers
bs script push                     # sync to fleet
bs script run cleanup.sh --peer linux-a  # skip transfer if hash matches
```
Scripts stored in `~/.bridgesessions/scripts/<sha256>.sh`. Named symlinks.

### 5b. Session Groups / Tabs (P3)
**For:** Better fleet management UX.
```bash
bs shell fleet --group web-servers  # attach to a round-robin session across peers
```

### 5c. Mesh-wide Search (P3)
```bash
bs search "OOM" --all-peers
# Searches dmesg/journalctl on all peers, returns results
```

### 5d. Health Watchdog (P2)
```bash
bs watch --peers all --interval 30s --on-fail "notify slack"
```
Daemon-level periodic health checks with alert callbacks.

---

## 6. Dependency Audit

| Dependency | Version | Risk | Action |
|------------|---------|------|--------|
| OpenSSL | 3.x | Low (well-maintained) | Keep current |
| zstd | 1.5.x | Low | Keep current |
| fmt | 12.x | Low | Keep current |
| spdlog | 1.17.x | Low | Keep current |
| CLI11 | vendored | Low | Keep current |
| nlohmann-json | vendored | Low | Keep current |
| MinGW (Windows build) | system | Medium | Pin version for reproducibility |
| Catch2 (tests) | vendored | Low | Consider FetchContent |

No known CVEs in current dependency versions. OpenSSL 3.x receives regular
security patches — set up automated dependency bump alerts.

---

## Priority Summary

| Priority | Item | Effort |
|----------|------|--------|
| **P1** | TOFU first-connect confirmation | 2h |
| **P1** | Windows named pipe DACL | 4h |
| **P1** | Split bs-protocol.h into modules | 4h |
| **P1** | Codeberg CI pipeline | 2h |
| **P1** | macOS notarization | 2h |
| **P2** | poll() migration (FD_SETSIZE) | 8h |
| **P2** | Non-blocking TLS writes | 16h |
| **P2** | Wire protocol version negotiation | 4h |
| **P2** | Delta gossip | 4h |
| **P2** | Content-addressed script library | 8h |
| **P3** | Session groups | 16h |
| **P3** | Mesh-wide search | 8h |
