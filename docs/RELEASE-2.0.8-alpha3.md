# BridgeSessions v2.0.8-alpha3 — Release Plan

**Status:** planning. **Source of truth for scope/phasing:** `TODO.md` (MoA verdict + phased plan)
and `ARCHITECTURE.md §15`. This doc is the human-readable rollup.

**MoA provenance:** planned via 3-model mixture-of-agents (Grok 4.5 + Kimi K3 workers,
`stack.moa.worker` judge). The requested `claude -p --model claude-fable-5` judge could **not**
run — the local `claude` CLI is not authenticated (`Not logged in · Please run /login`) and routing
Claude through Hermes/9warp is barred by standing directive. `stack.moa.worker` (a distinct 9warp MoA
model) substituted as the third independent model. Worker outputs: `/tmp/bs-moa/worker-{grok,kimi}.md`;
judge: `/tmp/bs-moa/judge-fable.md`.

---

## 1. Scope decision (operator override)

The five required features are built **to full functionality in 2.0.8-alpha3** (operator
decision 2026-07-20, overturning the MoA "thin slice + 2.0.9" recommendation). Phase
sequencing + per-phase gates: **`PLANS.md`**. Per-feature item checklists: **`TODO.md`**.

| # | Feature | alpha3 disposition (FULL) |
|---|---------|---------------------------|
| 3 | Same-source multi-attach | FULL + spectator role |
| 4 | Cross-resolution display | FULL harness + `doctor`; server-side reflow = stretch |
| 5 | Streaming | FULL: harden fanout + progressive transfer + panel incremental |
| 2 | Panel conversations | FULL: store + CLI + virtualized render + mesh relay |
| 1 | Cross-platform CUA | FULL: all-3-OS backends + 6-pair matrix + WebRTC; Windows helper = risk gate |

## 2. Key corrections to the original TODO

- **#5 streaming was misdiagnosed.** Shell output *is* already live-streamed via `pty_output_poller`
  (`:8386-8442`). Real gaps: silent byte loss on slow clients (`catch(...){}` at `:8401`), no
  backpressure/`OutputGap`, and `exec_busy` exclusive-borrow (`:7079/:7424`) conflicting with long-lived
  streams. → Harden, don't reinvent.
- **#3 premise was wrong.** Monolith sessions are keyed by **name only** (`:4061`), not by client
  pubkey. The same-key collapse is a `peer_ids` **dedup-by-pubkey** bug (`:4068-4072`) + value-erase
  detach (`:4168-4179`), papered over by `has_replacement_transport` (`:5771-5782`).
- **#1 CUA is the largest risk.** Zero code in repo; `Handle=0`/Session-1 is an *unsolved research
  problem* requiring a per-user helper agent. alpha3 = design + Windows PoC only.

## 3. Phases (verification gate per phase)

Full phase table + per-phase gates are the authority in **`PLANS.md`**. Summary:

| Phase | Work |
|-------|------|
| **P0** | Protocol sync + version bump (wire types 0x21–0x27, `client_instance_id`); `bs-protocol` drift decision. |
| **P1** | #3 multi-attach (FULL + spectator). |
| **P2** | #4 display harness + `doctor` (server-side reflow = stretch). |
| **P3** | #5 streaming (FULL: fanout + progressive transfer + panel incremental). |
| **P4** | #2 conversations (FULL: store + CLI + virtualized render + mesh relay). |
| **P5** | #1 CUA (FULL: all-3-OS backends + 6-pair matrix + WebRTC; Windows `cua-helper` = risk gate). |
| **P6** | Release (MoA audit, 3 portable binaries, tag). |

**Risk gate:** P5 Windows `cua-helper` PoC on cloud-pc is the only hard research risk. Linux/mac CUA ship regardless. If it fails, Windows injection is the sole blocked item (documented, not half-built) — do not let it block the rest of the release.

## 4. P0 decision required (bs-protocol drift)

`bs-protocol/` library enum stops at 0x14; monolith is 0x01–0x1F. Decide in P0:
**(a) regenerate library codec from monolith, or (b) freeze as test-only.** Monolith is wire SoT.

## 5. Top risks (full register in worker outputs)

- **R-CUA-Win:** `Handle=0`/Session-1 injection — needs per-user helper agent; gate on cloud-pc proof.
- **R-CUA-Linux:** Wayland input fragmentation — ship X11+XTest + uinput first; don't promise "any Linux".
- **R-CUA-Mac:** TCC Accessibility prompt — one-time human grant; `doctor` reports state.
- **R-keycode:** cross-OS keycodes differ — wire format = USB HID usage IDs; per-OS translation table.
- **R-scrollback:** 200×100 reattach of 80×24 history may show artifacts (raw-ANSI ring, no reflow) —
  test byte-exact only at same geometry; server-side reflow is a P2 **stretch** (best-effort, not a gate).
- **R-fanout:** silent byte loss on slow client — `OutputGap` + per-conn queue.
- **R-execbusy:** streaming must not borrow `exec_busy`.
- **R-convstore:** concurrent appends / unbounded growth — append-only + lock + fsync + per-conv cap + rate limit.
- **R-codecdrift:** new types must land in monolith; reconcile/freeze `bs-protocol`.

## 6. Wire deltas (monolith-first, 0x20+)

`AttachAck` 0x21, `OutputGap` 0x22, `ConversationAppend` 0x23, `ConversationQuery` 0x24,
`ConversationBatch` 0x25, `CuaRequest` 0x26, `CuaResponse` 0x27. `AttachMsg` extended with optional
`client_instance_id`. Full payloads: `ARCHITECTURE.md §2.2a`.
