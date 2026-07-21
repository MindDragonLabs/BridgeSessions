# BridgeSessions v2.0.8-alpha3 — Phase Plan

**Scope decision (operator override, 2026-07-20):** build **all five** features to
full functionality in 2.0.8-alpha3. The MoA review recommended a thin slice + 2.0.9
deferral; the operator opted for the full build. This plan reflects that. Item-level
checklists live in `TODO.md`; this file is the phase/sequencing/gate authority.

**MoA provenance:** planned via 3-model MoA (Grok 4.5 + Kimi K3 workers,
`stack.moa.worker` judge). Claude/Fable-5 judge unavailable (local `claude` CLI not
logged in; barred from Hermes/9warp route). Judge verdict: workers PASS-WITH-FIXES;
strong convergence. Workers' factual corrections (below) are encoded in `TODO.md` and
`ARCHITECTURE.md`.

**✅ COMPLETED 2026-07-21:** All six phases built. 315/315 CTest green.
bridgesessions.cpp: 12,570 lines (+ ~800 from 2.0.7-alpha2).
New: 5 test files (test_multi_attach_p1, test_display_harness, test_streaming,
test_conversations, test_cua); doctor display check; per-conn output queues;
conversation store; Linux CUA (xdotool backend).

## Phases (verification gate per phase)

| Phase | Work | Gate |
|-------|------|------|
| **P0** | Protocol sync + version bump. Decide `bs-protocol` drift (regenerate from monolith **or** freeze as test-only). Extend `AttachMsg` with `client_instance_id` (trailing, backward-compat). Add `AttachAck` 0x21, `OutputGap` 0x22, `ConversationAppend` 0x23, `ConversationQuery` 0x24, `ConversationBatch` 0x25, `CuaRequest` 0x26, `CuaResponse` 0x27. `VERSION`→`2.0.8-alpha3`. | codec round-trip tests for all new/changed types; full CTest green (baseline 267); tolerant trailing-field decode proven. |
| **P1** | **#3 multi-attach (FULL).** `attachments` map keyed by `attach_id`; `attach_id`-keyed detach; MIN-geometry (min-wins) policy; spectator role (read-only, no Keystroke/ComputerUse); `AttachAck` reports effective size. | CTest: 3 same-key conns attach + receive output; close 2 → session+child survive; close last → `--signal-on-detach` fires; resize from 2 conns → min-wins; spectator receives Output but Keystroke rejected. Manual: 3 panes → `shell shadow --name hms` test-pc1→cloud-pc. |
| **P2** | **#4 cross-resolution display (FULL).** Parameterized harness 80×24/120×40/160×50/200×100 + intermediate; byte-exact scrollback replay **at same geometry**; CJK/emoji/box-drawing capture→transfer→render round-trip; `doctor` display self-check (size + glyph sample + Wayland/X11/TCC env). **STRETCH (risk R-scrollback):** server-side terminal emulator for true cross-geometry reflow. | CTest matrix green on Linux; manual glyph matrix on test-pc5 (conhost + Windows Terminal) + cloud-pc. STRETCH: 200×100 reattach of 80×24 history renders without corruption (best-effort; if slips → documented limitation, not a gate failure). |
| **P3** | **#5 streaming (FULL).** Per-conn output queues + `OutputGap` + slow-conn close (replaces `catch(...){}` at :8401); NO `exec_busy` on stream frames. **Plus 5b** progressive `file recv` + live media stream (screenshots/video from a peer to a viewer without full capture-then-transfer); **plus 5c** panel incremental render (conversation messages + long docs append live). | CTest: long cmd → 2 clients identical ordered bytes + bounded lag; throttle one → `OutputGap`, other unaffected; kill one → survivor unaffected. E2E: live media stream cloud-pc→viewer peer with sub-second frame cadence. Panel render verified in **real browser, 0 console errors**. |
| **P4** | **#2 conversations (FULL).** JSONL store under `sessions/<name>/conversations/` (namespaced by agent pubkey); `message` CLI; `ConversationAppend`/`Query`/`Batch` mesh relay (any peer appends/reads); Bridge Panel "Conversations" tree node; **virtualized** message list (no jank at 5000 msgs); `?since_seq=` JSON + paginated HTML. | pytest: multi-agent interleave ordered by seq; 5000-msg virtualization scroll smooth (no layout thrash); append-without-token rejected; mesh relay A→B→C ordering. **Real browser, 0 console errors** (operator standard). |
| **P5** | **#1 cross-platform CUA (FULL).** `CuaRequest`/`CuaResponse` wire types (USB HID usage IDs on wire). Backends for **all three OSes**: Windows `SendInput` via per-user `cua-helper` agent (named-pipe delegation → resolves `Handle=0`/Session-1); Linux X11 `XTest` + `uinput`/evdev fallback; macOS `CGEvent` (TCC grant, `doctor` reports state). 6-pair from→to matrix (Linux/Win/Mac × Linux/Win/Mac); vision leg (capture on one OS → analyzed on another); **WebRTC live media** path for CUA frames. | **RISK GATE (Windows):** `cua-helper` PoC on cloud-pc — injected keystroke visible in capture. Then CTest/e2e: all 6 pairs dispatch + verify via screenshot diff; vision leg cross-OS; `CuaResponse` ack reliable (no orphaned action). Linux/mac ship regardless of Windows gate; if Windows helper fails, Windows CUA is the sole blocker (documented, not a silent half-feature). |
| **P6** | **Release.** MoA audit (0 P0 / 0 P1 per fleet convention); refresh `HOW-TO-COMPILE.md` + `bridgesessions-static-build` skill; build 3 portable static binaries (linux-x86_64 / macos-arm64 / windows-x86_64.exe) into `dist/`; tag `v2.0.8-alpha3`. | CTest full green; portable binaries present; tag pushed to Codeberg; release notes per feature. |

## Sequencing notes
- P0 before everything (wire types block all messaging features).
- P1 first (smallest, unblocks P3 fanout tests + P5 CUA dispatch routing confidence).
- P5 Windows helper is the long pole + only hard research risk — start the cloud-pc
  PoC in parallel with P2/P3 so a failure surfaces early, not at P5 end.
- P2 reflow stretch is the only item that may slip without blocking release.

## Residual 2.0.9 candidates (only if a P-gate genuinely fails)
- Server-side terminal emulator / true cross-geometry reflow (if P2 stretch slips).
- Windows CUA inject (if P5 `Handle=0` helper PoC fails — everything else still ships).

## Top risks (full register)
- **R-CUA-Win:** `Handle=0`/Session-1 — per-user helper agent; gate on cloud-pc proof.
- **R-CUA-Linux:** Wayland input fragmentation — X11+XTest + uinput first.
- **R-CUA-Mac:** TCC Accessibility one-time human grant; `doctor` reports state.
- **R-keycode:** wire = USB HID usage IDs; per-OS translation table.
- **R-scrollback:** raw-ANSI ring (no TE) → reflow is the P2 stretch risk.
- **R-fanout:** silent byte loss → `OutputGap` + per-conn queue (P3).
- **R-execbusy:** streams must not borrow `exec_busy`.
- **R-convstore:** concurrent appends / unbounded growth → append-only + lock + fsync + per-conv cap + rate limit.
- **R-codecdrift:** new types land in monolith first; reconcile/freeze `bs-protocol` (P0).
