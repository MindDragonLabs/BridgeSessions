# bridgesessions — PLANS.md

**Canonical source-of-truth:** `~/bridgesessions/bridgesessions.cpp` (11,805 lines, C++23 single unified binary)
**Current release:** v2.0.7-alpha2 (tagged + pushed to Codeberg, commit `1a77884`)
**Fleet mesh:** test-pc1 / test-pc2 / test-pc5 / test-pc6 / test-pc7 — all green
**CTest:** 275/275.  Full MoA audit (2026-07-20, 3 auditors): 0 P0 / 1 P1 (Windows process-group) / 2 P2; P1+P2 fixed same run.

---

## Target: v2.1 — CUA, signal safety, and deferred features

Three pillars:

1. **CUA (Computer-Use Agent) from Windows → Linux / macOS.** Interactive ConPTY TTY from Windows origins, so agent‑orchestrated desktop‑use sessions can originate from a Windows host and drive Linux/macOS PTYs. This is the #1 undelivered feature from the 2.0.6 work.
2. **Ctrl‑C safety.** In interactive mode, Ctrl‑C must reach the remote child and interrupt *only* the foreground process — the bridge session itself must survive. This is already implemented (raw‑mode passes `0x03` through; `SignalMsg::CtrlC` sends SIGINT to the child). The v2.1 work is: (a) verify it works on all four OS/shell combos with automated tests, (b) ensure the non‑interactive `--cmd` path never kills the daemon connection on local SIGINT, and (c) add a `--signal-forward-off` escape hatch for cases where the user wants the local terminal to catch Ctrl‑C.
3. **Deferred features from 2.0.6.** Multi‑hop routing, vfolder sync, WebRTC/SDP, DHT discovery, public packaging (Homebrew/AUR/Windows installer).

---

## Phase A: CUA — Interactive ConPTY from Windows origins

### What exists

- Windows ConPTY creation works (`create_session`, ~line 2478).
- ConPTY input has a bounded dedicated writer queue with `CancelSynchronousIo` shutdown (>line 5462).
- Non‑interactive one‑shot commands from Windows (`--cmd`) use plain anonymous pipes (>line 2482) — reliable for `health`/`shell --cmd`.
- Win32 raw‑mode prep (`ENABLE_VIRTUAL_TERMINAL_INPUT`, >line 4764) strips line‑editing and echo.
- ConPTY output is read via `PeekNamedPipe + ReadFile` in the event loop.

### What's missing for interactive Windows‑origin sessions

| Gap | Detail |
|-----|--------|
| Interactive client‑side raw terminal on Windows | The CLI process on the Windows origin must enter VT‑raw mode (already coded, needs wiring into the interactive shell path) |
| ConPTY resize from remote | `ResizePseudoConsole` is coded; needs to be called on `ResizeMsg` from the Linux/macOS peer |
| ConPTY signal forwarding | `GenerateConsoleCtrlEvent` is coded (>line 8034); needs to be wired into the interactive ConPTY path (currently only in the inbound‑session handler for remote peers) |
| End‑to‑end test | A test that spawns a Windows origin CLI session → Linux target → runs `bash -lc 'trap "echo GOT_SIGINT" INT; sleep 30'` and verifies Ctrl‑C forwarding |

### Tasks

| # | Task | Est. | Depends |
|---|------|------|---------|
| A.1 | Wire Windows interactive raw‑mode into the CLI `shell` path (not just `--cmd`) | 2h | — |
| A.2 | Ensure ConPTY resize is called on `ResizeMsg` from the mesh | 1h | — |
| A.3 | Ensure `GenerateConsoleCtrlEvent` works for ConPTY sessions (tests) | 2h | A.1 |
| A.4 | Cross‑direction E2E: Windows → Linux interactive CUA (type `ls`, resize, Ctrl‑C) | 2h | A.1–A.3 |
| A.5 | Cross‑direction E2E: Windows → macOS interactive CUA | 1h | A.1–A.3 |

**Estimated total:** 8h (1 day)

---

## Phase B: Ctrl‑C safety audit + hardening

### What already works

- **Interactive mode:** `cfmakeraw()` on POSIX (>line 4804) strips `ISIG` — local Ctrl‑C is sent as byte `0x03` through the mesh, reaching the remote PTY. The remote child receives it as SIGINT if the PTY's `isig` bit is set (default). The bridge session is unaffected.
- **Signal forwarding:** `SignalMsg::CtrlC` explicitly sends `SIGINT`/`CTRL_C_EVENT` to the child (>line 8027‑8043). Used for toolbar buttons or explicit "send signal" commands.
- **Non‑interactive `--cmd`:** The local CLI does NOT enter raw mode — Ctrl‑C in the local terminal sends SIGINT to the CLI process, which exits. This is the correct behavior: the user wants to kill a runaway command.

### What needs verification / hardening

| Concern | Current state | Action |
|---------|--------------|--------|
| Ctrl‑C in interactive session kills the bridge | Should not happen (raw mode → byte 0x03 forwarded) | Write a test: `bs shell peer` + type Ctrl‑C → verify child gets SIGINT, session stays alive |
| Ctrl‑C in non‑interactive `--cmd` | Local CLI exits (correct) | Verify exit code reflects remote signal, not local kill |
| `--signal-forward-off` escape hatch | Does not exist | Add CLI flag so user can choose: forward Ctrl‑C (default) or let local terminal catch it |
| Windows ConPTY: Ctrl‑C → child | `GenerateConsoleCtrlEvent` is coded but not end‑to‑end tested with a ConPTY interactive session | Write ConPTY signal test |
| Client disconnects during long‑running command → orphaned child | `exec_busy` watchdog handles this (v2.0.6 long‑operation worker pool) | Verify watchdog cleans up within 90s (existing BUG‑1) |
| `SIGHUP` on session detach | Not sent | Add signal to child on detach so daemons know the terminal is gone (`--signal-on-detach HUP`) |

### Tasks

| # | Task | Est. | Depends |
|---|------|------|---------|
| B.1 | Write automated test: Ctrl‑C in interactive session → child gets SIGINT, session survives | 2h | — |
| B.2 | Verify non‑interactive `--cmd` Ctrl‑C behavior (exit code 130 = 128+SIGINT) | 1h | — |
| B.3 | Add `--signal-forward` flag (default: on). When off, local terminal keeps raw‑mode ISIG so Ctrl‑C kills the CLI | 1h | — |
| B.4 | Add `--signal-on-detach <signal>` flag — send HUP/TERM to child when all peers detach | 2h | — |
| B.5 | End‑to‑end scenario doc: user in interactive session, long‑running `make`, hits Ctrl‑C, build stops, prompt returns. All verified. | 1h | B.1‑B.3 |

**Estimated total:** 7h (1 day)

---

## Phase C: Deferred features from 2.0.6

### C.1 — Multi‑hop routing (`AttachMsg.routing`)

The `routing` field (v2.0.0 wire, >line 411) is already in `AttachMsg`. The daemon ignores it — sessions are always local to the receiving node.

| # | Task | Est. |
|---|------|------|
| C.1a | Daemon‑side routing: when `AttachMsg.routing` is non‑empty, forward the attach to the named peer via mesh TCP | 4h |
| C.1b | Hop‑count limit (max 3) to prevent routing loops | 1h |
| C.1c | Test: `bs shell hop2 session1 --cmd hostname` from test-pc1 → test-pc2 → test-pc5 returns `test-pc5` | 2h |

**Estimated total:** 7h

### C.2 — vfolder sync

Remote edit (`bs edit peer:path`) downloads/edits/uploads a single file. vfolder sync extends this to directory‑level bidirectional sync.

| # | Task | Est. |
|---|------|------|
| C.2a | `bs vfolder pull <peer> <remote-dir> [local-dir]` — download directory tree | 3h |
| C.2b | `bs vfolder push <peer> <local-dir> [remote-dir]` — upload directory tree | 3h |
| C.2c | `bs vfolder watch <peer> <dir>` — one‑way continuous sync (polling, 5s interval) | 2h |
| C.2d | File‑level dedup via SHA‑256 to skip unchanged files | 1h |

**Estimated total:** 9h

### C.3 — WebRTC/SDP (video streaming, low‑latency remote desktop)

D15 structs (`SdpOfferMsg`, `SdpAnswerMsg`, >lines 523‑535) are on the wire. libdatachannel is linked on Windows (`#include <rtc/rtc.hpp>`, >line 93). No implementation exists.

| # | Task | Est. |
|---|------|------|
| C.3a | Implement SDP offer/answer handshake over the mesh TCP gossip channel | 4h |
| C.3b | WebRTC data channel for keyboard/mouse input (low‑latency alternative to KeystrokeMsg) | 3h |
| C.3c | WebRTC video track for desktop capture (Windows: DXGI Desktop Duplication; Linux: PipeWire/ffmpeg; macOS: ScreenCaptureKit) | 6h |
| C.3d | Integration: `bs desktop <peer>` launches interactive remote desktop over WebRTC | 2h |

**Estimated total:** 15h (2–3 days)

### C.4 — DHT‑based peer discovery (D16)

DHT structs (`DhtFindNodeMsg`, `DhtFindValueMsg`, >lines 539‑551) are on the wire. No Kademlia implementation exists.

| # | Task | Est. |
|---|------|------|
| C.4a | Kademlia routing table (160‑bit node IDs = SHA‑256 of pubkey, k‑bucket size 20) | 4h |
| C.4b | Bootstrap from seed nodes in config | 2h |
| C.4c | DHT find‑node RPC: return k closest nodes | 2h |
| C.4d | DHT find‑value RPC: return stored peer address for a pubkey | 2h |
| C.4e | Periodic refresh (republish own address, refresh buckets) | 1h |

**Estimated total:** 11h

### C.5 — Public packaging

| # | Task | Est. |
|---|------|------|
| C.5a | Homebrew formula (`brew install bridgesessions`) | 2h |
| C.5b | AUR package (`yay -S bridgesessions`) | 1h |
| C.5c | Windows MSI installer (WiX toolset) | 3h |
| C.5d | macOS `.pkg` installer + launchd plist | 2h |

**Estimated total:** 8h

---

## Phase D: Bug fixes (carry‑over from v1.8)

### BUG‑1: `exec_busy` stuck flag

When CLI `shell --cmd` times out, the background exec thread's `exec_busy` flag can remain `true`, blocking all subsequent IPC to that peer. **v2.0.6 long‑operation worker pool** (lines 5124‑5205) addresses this for file transfers, but the CLI shell‑exec path still uses the older `BusyGuard` mechanism.

**Fix:** Add a 90s watchdog timer per `exec_busy` flag. If the flag hasn't been cleared after 90s, the watchdog cancels the blocking operation and resets the flag.

| # | Task | Est. |
|---|------|------|
| D.1 | Add `exec_busy` watchdog timer (90s, per‑connection) | 2h |
| D.2 | Test: `timeout 15 bs shell peer --cmd "sleep 120"` → `health peer` returns healthy within 90s | 1h |

**Estimated total:** 3h

### BUG‑2: Handle=0 (cloud-pc Roblox input injection)

Not a protocol bug — a Windows desktop‑session boundary issue. Roblox windows are only visible in the interactive user session, not in Session 0 (SYSTEM) or Session 1 (logon screen). The agent must run in the same session as the Roblox process.

| # | Task | Est. |
|---|------|------|
| D.2a | Schedule Playtest agent in the interactive user session (Session 2+) via `schtasks` with `/IT` flag | 1h |
| D.2b | Verify `EnumWindows` finds `RobloxPlayerBeta` handle from the correct session | 1h |

**Estimated total:** 2h

---

## Acceptance criteria (v2.1)

1. **Windows CUA:** Interactive ConPTY session from user‑shadow → Linux/macOS target. Resize works. Ctrl‑C forwards correctly.
2. **Ctrl‑C safety:** All four OS/shell combos pass automated signal‑forwarding tests. `--signal-forward-off` flag works.
3. **Multi‑hop:** 3‑hop routing test passes (test-pc1 → test-pc2 → test-pc5 returns `test-pc5`).
4. **exec_busy:** Watchdog clears stuck flag within 90s.
5. **CTest:** No regressions from v2.0.6 baseline (267/267).

---

## Estimated total effort

| Phase | Hours | Days |
|-------|-------|------|
| A — Windows CUA | 8h | 1 |
| B — Ctrl‑C safety | 7h | 1 |
| C — Deferred features | 50h | 6–7 |
| D — Bug fixes | 5h | 1 |
| **Total** | **70h** | **9–10** |

---

## Execution sequence

```
Week 1:  Phase A (Windows CUA) + Phase B (Ctrl‑C safety)
         → v2.1-alpha — interactive Windows → Linux CUA works

Week 2:  Phase C.1 (multi‑hop) + Phase D (bug fixes)
         → v2.1-beta — multi‑hop routing, exec_busy watchdog

Week 3:  Phase C.2–C.4 (vfolder, WebRTC, DHT)
         → v2.1-rc — deferred features shipped

Week 4:  Phase C.5 (packaging) + E2E + MoA audit
         → v2.1 final
```

---

## Done definition

1. Claim matches verified reality (command/output/HTTP/CTest)
2. `CHANGELOG.md` updated
3. MoA audit clean (0 P0, 0 P1)
4. Signed tag pushed to Codeberg
5. No secrets in tree
