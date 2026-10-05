# Goal — BridgeSessions 26.10.04

Peer names in this document use generic role labels or placeholders.

Peer names in this document use generic role labels or placeholders.

## Result

Ship a quality release the project can build on for the rest of the program, and
leave a repository where every new test actually runs and no work can be lost by
a single mistake.

26.10.03 is **skipped**. It is not released. Its work becomes the content of
26.10.04.

The item list is `TODO-2026-10-04.md`. The evidence behind every claim is
`docs/plans/26.10.04-audit.md`. The original lane ledger is
`/home/agent/bs-candidates/26.10.03/STATUS.md`. The feature backlog, deliberately
out of scope here, is `IDEAS.md`.

## Done means

1. Every lane is committed to its own branch. A recoverable bundle or tag exists
   per lane. `git status --short` is clean in all five worktrees.
2. The new session-control tests are registered in `CMakeLists.txt` and run in a
   default `ctest`. `BS_BUILD_NATIVE` compiles the shared core and at least two
   platform shells.
3. The public installer points at a tag that exists. A user who runs it gets a
   working install.
4. The acceptance harness passes 10 consecutive runs. Its wait budget exceeds the
   product's own connect window, and the product connect window is unchanged.
5. A real built binary, driven under a live PTY, proves arrow-key selection,
   cancel, and terminal restoration for the TUI. Unicode width holds for ASCII,
   accents, CJK, and combining characters.
6. One real Windows peer and one real Mac peer pass session input, readback, and
   isolation. Skips do not count as passes.
7. Full `ctest` is green, or every remaining failure is shown to fail identically
   on pristine `v26.09.28`.
8. Panel auth is reviewed. Every concern the plan listed is fixed or explicitly
   declined with a reason written down.
9. A phone cannot obtain mesh access. Per-device API credentials are scoped, with
   enrolment, revocation, expiry, and audit.
10. The privacy scan is clean against the integrated tree, with output saved.
11. `VERSION`, `version.rc`, both installers, and the changelog agree, and the
    parity tests pass.
12. The release ships only after an explicit operator go, and a fresh download is
    verified against the published checksums.

## Hard limits

- Do not write to GitHub without explicit operator authorization. The operator
  authorized the 26.10.04 bugfix release on 2026-10-05; this does not authorize a
  fleet rollout or a push to `main`.
- Do not push `main`. The public installer's default tag must exist; a push of
  an installer pointing at a missing tag breaks every user install.
- **Fleet rolls are allowed, in one order only: prove locally, publish, then
  deploy, and a deploy is not done until an in-band upgrade is proven on a real
  peer.** Revised 2026-10-04 at operator request; the previous wording ("do not
  roll the fleet") forbade the deploy this release is supposed to end with. The
  constraint that survives is the ordering, not a blanket ban:
  - `ctest` green on the integrated tree **before** anything is published.
  - A published, checksum-verified artifact **before** any host is upgraded.
  - A host is not "deployed" until a **running** daemon reports the new version
    and `bs upgrade` from the prior version has been observed to work. An in-band
    upgrade that returns "Already up to date" is not a pass — the version must
    actually differ.
  - The Windows hash mismatch is still traced before the Windows leg: running
    `AE2B61A6`, published `684d4149`.
- Windows proof uses WinRM. Do not use `bs shell` as the Windows proof. The
  Shadow PC is `SHADOW-<node-windows>` at `<tailnet-ip>`.
- Do not add a 1-second reconnect, upgrade, or ping loop. A test wait must stay
  inside the test. `bs-mesh-transfer.h` already carries a deliberate fast path
  that is justified only because this limit holds; do not weaken it.
- Do not shorten `kTieBreakAcceptWindowMs` to make a test pass. Fix the test.
- Do not apply the old broad stash to the integrated tree. Export, review, then
  apply hunks deliberately.
- Do not widen a security setting to make a test pass.
- Do not restore `._*` or `*.bak` files from the pre-clean stash. Review
  `auth.py` and `invites.py` in that stash before any panel copy.
- A subagent report is not proof. The parent re-checks the file, the test, or the
  host.

## Orchestration

You are the orchestrator. You do not do the work yourself when a lane can do it,
and you do not accept a lane's word for it either. The parent verifies.

**Dispatch through `cli-sub`, never by re-deriving flags.** It is at
`~/.hermes/bin/cli-sub` and already carries the corrected flag forms.

```
cli-sub <claude|codex|grok|kimi> <brief-file|inline> [output-file]
```

**Model roster — every entry verified live on 2026-10-04.** Re-verify before
changing any of them.

| Lane | Model | How to pin it | Verified by |
|---|---|---|---|
| Codex | `gpt-6.1-sol` | config default; no flag needed | `~/.codex/config.toml`, live `codex exec` |
| Claude | `claude-sonnet-5-5` | `--model sonnet` | live `claude -p --model sonnet` self-report |
| Kimi | `kimi-code/k3` | `-m kimi-code/k3` | `~/.kimi-code/config.toml`, live `kimi -p -m` |
| Grok | `grok-4.7` | `-m grok-4.7` | `grok models`, live `grok -m grok-4.7 -p` |

**Two dispatch traps, both live right now:**

1. `cli-sub grok` hardcodes `-m grok-4.6`. Grok's own default is now
   `grok-4.7`. Dispatching grok through `cli-sub` silently gets the *older*
   model. Call `grok -m grok-4.7 -p "<brief>"` directly, or fix `cli-sub` first.
2. `cli-sub` has no model pin for claude, kimi, or codex. It inherits whatever
   each CLI defaults to. Pin the model explicitly whenever the roster value
   differs from that default, or the roster is a lie.

**Use each lane for what it is good at.** Codex for mechanical, exact, test-first
work. Claude for judgement calls and review. Kimi for long-context reading and
breadth. Grok for adversarial review — "what is wrong with this, and what would a
hostile reviewer say?"

**Two rules that override the roster:**

- **Phase 1 is parent-only.** Committing the lanes is mechanical, verifiable, and
  has one correct answer. A worker adds a second unverified claim. The parent
  does it.
- **Never two lanes on one file.** Split by file set, not by task, or you get
  silent conflict. Sequence when the sets overlap.

Every worker brief must name the binary, the subcommand, and the expected first
line of output. If the first line differs, the worker stops and reports. A
guessed command that fails quietly is worse than one that errors.

Every worker returns exact changed files, the exact test command and its exit
code, and DONE or one specific blocker. **A worker claiming PASS is a claim, not
evidence.** The parent re-runs the proof before the phase gate closes.

| Phase | Work units | Parallel? |
|---|---|---|
| 1. Preserve | one: commit four lanes | No, parent does it |
| 2. Unbreak | B2 installers, B3 CMake, B4 harness | Yes, disjoint files |
| 3. Prove locally | U1 TUI PTY, U2 native, U4 full ctest | Yes, disjoint trees |
| 4. Secure | S1 auth review, S2 device creds, S3 scrub | Yes, disjoint files |
| 5. Prove remotely | U3 Windows/Mac, U5 fleet e2e | Yes, disjoint peers |
| 6. Release | mechanics, one train at a time | No, sequential |

## Order of work

| Phase | Items | Gate |
|---|---|---|
| 1. Preserve | B1 | Every lane committed, SHAs recorded, bundles written |
| 2. Unbreak | B2, B3, B4 | Installer points at a live tag; `ctest -N` lists the new tests; harness passes 10/10 |
| 3. Prove locally | U1, U2, U4 | Real PTY acceptance; native core and two shells compile; full `ctest` |
| 4. Secure | S1, S2, S3 | Every auth concern resolved or declined in writing; scan clean |
| 5. Prove remotely | U3, U5 | Real Windows and Mac peers; fleet e2e exit 0 |
| 6. Release | mechanics 1-6 | Explicit operator go, then a verified fresh download |

Phase 1 is the cheapest and the most urgent. The work is one mistake from gone
right now, and every later phase depends on it existing.

## Not this goal

- 26.10.03 is not released under its own name. It becomes 26.10.04.
- No fleet rollout *before* publication and a green `ctest`. After that a rollout
  is in scope, finished only when an in-band upgrade is proven on a real peer.
- No new feature work. The nine ideas in `IDEAS.md` are out of scope. Only the
  session-control capability the 26.10.03 lanes already built is in scope.
- No SonarCloud, no coverage percentage target. Neither is a release gate.
- Do not restart the audit. It is done and its evidence is saved.

## Status

**Phase 1 is DONE (2026-10-04).** All six worktrees are clean, every lane is
committed, and recovery bundles are written and proven by an actual restore.

| Lane | SHA |
|---|---|
| backend | `c4b3c98` + `e800604` (B3) + `d883292` (B4 diagnosis) + `9bf66a7` (B4 fix) |
| panel | `136d4bb` |
| native | `98c2cba` |
| tui | `c3330c9` |
| scrub | `1053f85` |
| integration | `3b0d127` + `bf737ea` (B2) |

Bundles: `/home/agent/bs-candidates/26.10.03/bundles/`, SHA-256 recorded in
`TODO-2026-10-04.md` B1.

**Phase 2 is DONE. All three blockers closed.**

- B2 `bf737ea` — installers point at `26.09.28`; all five release assets
  verified HTTP 200 against the live remote.
- B3 `e800604` — both new tests registered; `ctest -N` lists them.
- B4 `9bf66a7` — 10/10 consecutive passes. Root cause was a one-sided seed,
  not a test budget and not a product race. Zero production code changed.

Full default `ctest`: **620 tests — 619 unit plus `panel_session_acceptance`
(#620) at 100%.** Pristine v26.09.28 baseline is 614, so the delta is the 6
new session-control tests.

**Correction to an earlier figure in this file.** I had been reporting
"619/619 plus the e2e" as though the 619 excluded it. It does not: the Catch2
suite reports 619 on its own line, and `ctest -N` in `build/test` shows 620
total with the two new tests at #619 (`test_panel_session_control`) and #620
(`panel_session_acceptance`). 620 is the number.

**U4 measured:** 3 consecutive full `./build.sh test` runs, all rc=0 at 100%.
The two new tests were then run 5 consecutive times through ctest directly:
100% out of 2 every time.

**Next: Phase 3 (Prove locally) — U1 TUI PTY against the real binary, U2 native
core + two shells, U4 full `ctest`.** All three are local and touch disjoint
trees, so they run in parallel.

**Windows and macOS peers are U3, and U3 is Phase 5, not Phase 3.** An earlier
Status line in this file said otherwise; the phase table above is authoritative.
U3 needs real peers and a skip is not a pass, so it cannot be pulled forward
into a local phase.

## Fleet

`docs/plans/26.10.04-fleet-map.md` is the live map, taken from `tailscale status`
on 2026-10-04. It supersedes any host or IP named elsewhere. mac mini,
`<node-windows>`, <node-linux>, <peer> and <node-linux> are online; **no Shadow PC is
online**, and **<node-windows> is offline**. `<tailnet-ip>` is `shadow-<node-windows>`, not
<node-windows>. Devin cloud is Linux-only and unprovisioned, so the Windows leg uses
`<node-windows>` (BS 19949 and WinRM 5985 both open, running 26.09.08).
