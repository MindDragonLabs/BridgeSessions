# IDEAS — BridgeSessions backlog

Status: operator idea list. Written 2026-10-03. Not a plan, not a commitment.
Nothing here is approved, scheduled, or published.

This file answers one question: "what did I want to add to BridgeSessions and
forget?" It also answers a second question the operator did not ask: which of
these nine ideas already exist in some form, so nobody rebuilds what ships.

Read `docs/plans/26.10.03.md` for the active candidate plan. Read
`docs/PLANS.md` for the shipped roadmap. This file is the parking lot for work
that is not in either.

## Decisions — 2026-10-03 (operator)

The operator answered the four pushbacks in the summary section below, in order.

| Answer | Point it settles |
|---|---|
| "yeah so yes, I want this" | Idea 1 is wanted. BUILD the GUI. |
| "ok fine, however we do session titles, we need it" | Idea 7 is required. The method is still open. Log-file reading stays on the table. |
| "yes, as you say, that's what I wanted" | Idea 3 keeps the split. Finish the input layer now. Video, audio, and the graphics card are separate projects with separate budgets. |
| "yes, per session" | Idea 8 self-improvement is scoped per session. It dies with the session. No per-fleet or per-harness learning without a new decision. |

Idea 2 was not disputed. The interceptor is wanted; only the model-hosting
choice is open. Idea 9's pushback was not answered.

Items 4, 5, and 6 remain undecided.

## How to read the status column

| Status | Meaning |
|---|---|
| EXISTS | The capability ships today. The idea is an extension, not a build. |
| PARTIAL | A core part ships. The named gap does not. |
| NEW | No implementation found in this tree. |

## Summary

| # | Idea | Status | Largest single obstacle |
|---|---|---|---|
| 1 | Mobile and desktop GUI on the CLI | PARTIAL | Session input and scrollback are stubs in the panel API. |
| 2 | One bash-fix method for all harnesses | NEW | No interceptor exists. `openllm` routes credentials, not model calls. |
| 3 | Full remote desktop, anything to anything | PARTIAL | CUA is input only. No video, no audio, no multi-monitor. |
| 4 | One fleet-wide filesystem | PARTIAL | `bs sync pair` mirrors pairs. It is not a namespace. |
| 5 | AI-first bridgejoin and bridgeleave | NEW | Neither command name exists. The real names are `invite`, `join`, `enroll`. |
| 6 | Switch sessions inside a session | PARTIAL | The picker runs before attach. No live switcher. |
| 7 | Read the harness session title | PARTIAL | Titles come from environment variables only. No log read. |
| 8 | Universal OpenLLM harness support | NEW | One-command fleetwide enable does not exist. |
| 9 | Automatic fleet resource management | PARTIAL | Fleet reports CPU, memory, and disk. It never acts. |

---

## 1. Mobile and desktop GUI that uses the CLI

**Want:** a real app on a phone and on a desktop that drives the same `bs`
commands. Not a web mock. A binary.

**Exists:** the phase-0 native-app program. `docs/plans/26.10.02.md` and
`docs/plans/26.10.03.md` section 7 define five shells: Win32, AppKit, Linux,
UIKit, and an Android NDK boundary, over one shared C++ core. Bridge Panel is
the only backend. No app talks to the mesh protocol directly.

**The gap that blocks it:** `tools/bridgepanel/api.py` reports `"wired": false`
for session input (line 239). The scrollback endpoint
(`query_remote_scrollback`, line 280) returns remote metadata and a connect
hint, not real output. Session start is a CLI subprocess. Until input and
readback are real, a GUI is a remote keyboard with no screen.

**Smallest first slice:** one real vertical slice. Through Bridge Panel only,
select a remote session, send a unique marker, read the bytes back, and prove a
second session never sees them. The plan already names this as Gate B.

**Open question:** the old panel branch (`work/panel-invites`) has six
unintegrated commits. Do we adapt them, or write the auth layer fresh for
scoped per-device credentials? The plan recommends fresh, with the old work as
input only.

---

## 2. Bash shell fixes for harnesses, forced through one method

**Want:** every harness gets the same shell repair. One interceptor. A small
model, hosted by `openllm`, writes the fix. Claude is the worst offender today.

**Correction to the premise:** `openllm` on this host (v2.6.36) is a credential
and routing layer. It launches `claude`, `codex`, `grok`, `hermes`, and
`opencode`, serves a unified MCP server, and runs hook verbs through
`openllm exec <group> <verb>`. It does not host a local model today. Hosting the
repair model is new work, not a config switch.

**Second correction:** there is no shell interceptor in this tree. The
harness launch path goes straight to a login shell (`bs-pty.h`, landed for
`26.10.03` so `~/.profile` PATH is honored). Interception is a new seam.

**Design options, in order of cost:**

| Option | Cost | Trade-off |
|---|---|---|
| Repair model hosted locally by `openllm` | Highest | No data leaves the host. Needs a small local model plus a serving path. |
| Repair model through the existing provider fleet | Low | Fast to build. Sends shell output to a hosted endpoint. Needs a redaction rule. |
| Deterministic repair rules, no model | Lowest | Handles the known classes. Does not generalize. |

**The real design question:** who owns the fix. If the model writes a repaired
command into the harness config, the operator must approve it once. If it
rewrites live every turn, the harness fights the fix. Recommend one write,
operator-approved, then read-only.

**Open question:** does Claude Code get a stable hook point, or do we wrap the
binary? Wrapping the binary breaks on upgrade. A hook is version-coupled.

---

## 3. Full remote desktop, anything to anything

**Want:** drive any machine from any other machine, or from a phone. macOS,
Windows, Linux, and a custom graphics card on our own hardware.

**Exists:** `bs cua` with `screen`, `capture`, `click`, `move`, `type`, `key`,
and `scroll`. A `cua-driver-rs` backend, a per-user-session helper, and
`docs/cua.md`. A spectator cannot send input. That is a working remote input
system, not a remote desktop.

**Missing, by cost:**

| Gap | Cost | Note |
|---|---|---|
| Video stream, not stills | Medium | A codec decision per platform. Codec choice is a long-term commitment. |
| Audio | Medium | New capture path per platform. Licensing is clean, plumbing is not. |
| Multi-monitor and per-monitor scaling | Low | Coordinates are global today. |
| Phone as a control surface | Medium | Touch, not click. Gestures are a new input model. |
| Docker target via Vinix | High | See below. |
| Custom graphics card add-on | Highest | Hardware. Firmware, driver, and a support burden that never ends. |

**Correction on Vinix:** Vinix is pre-alpha. Its own project states it is not
meant for daily or production use. Docker runs on Vinix on ARM64 with Alpine
binaries, and the whole root filesystem loads into a RAM disk, so it needs
8 GiB or more to boot. It is a research lane, not a fleet runtime. Recommend
Docker support on standard Linux hosts first, and keep Vinix behind a flag.

**Recommendation:** split this idea. The input layer is nearly done and is worth
finishing now. The video and hardware items are separate projects with separate
budgets.

---

## 4. One filesystem across the fleet

**Want:** one filesystem. Every host sees the same paths.

**Exists:** `bs sync pair` ships (commit `ad81f9b`). Init a pair, approve a dry-
run manifest, then run it. It is resumable and it shows status. The design is in
`docs/bs-sync-design.md`.

**What that is not:** a namespace. There is no mount. There is no path that
resolves across hosts. There is no cache you can read without a transfer. Two
hosts each hold a copy, and one copy is not the truth.

**The honest gap:** the roadmap says this is deliberate. A pair does not become
the source of truth by being synced, and the sandbox is a projection cache
(`docs/PLANS.md` line 30). That is the right call. It means idea 4 is a
different product than it sounds.

**If you want one filesystem, decide which of these you mean:**

| Meaning | Work | Risk |
|---|---|---|
| One mount point that fans out | FUSE layer over `bs file` | Cache coherency, partial writes, latency |
| One authoritative store, others are cache | Elect a source host per path | Split brain, the source goes down |
| One namespace, copies everywhere | Current `bs sync pair`, more pairs | Divergence, no locking, no conflict story |

Recommend the third, scaled up, until the first two are needed. It is what the
code already does.

---

## 5. AI-first bridgejoin and bridgeleave, approved from the app

**Want:** joining and leaving the mesh becomes an AI-driven flow. The app is
where a human approves.

**Correction on the names:** there is no `bs bridgejoin` and no `bs bridgeleave`
in this tree. The commands are `bs invite`, `bs join`, `bs enroll`, and
`bs peers add` / `bs peers remove`. The idea is a reshape of that surface, not a
new pair of commands. The old plan called this roadmap phase 3, "onboarding and
control plane", scoped and not started.

**The bug this fixes, which is real:** a join writes the new node key to the
**token issuer's** `authorized_keys` only. The joiner also gets the full mesh
directory, so it dials other seeds when the issuer is down. Those seeds reject
it, and a rejected connection cannot receive gossip. The symptom is a
one-per-second reconnect loop that reads like an attack. This is recorded in
`skills/bridgesessions/SKILL.md` with the `dave-pc` incident. An AI-first flow
must close this hole, not decorate it.

**The panel has no join surface today.** There is no approve endpoint in
`tools/bridgepanel/api.py`. This is green field.

**Design the approval, then the AI.** Order matters:

1. Signed enrollment, one approver, one audit event.
2. Panel screen that lists a pending request with the requesting key, the
   issuer, and the reason.
3. Approve and reject, with the decision written to the mesh log.
4. Leave as a first-class flow with key removal everywhere, not just the issuer.
5. Only then, the AI layer that explains a request in plain words and suggests
   approve or reject.

The AI advises. A human presses the button. Recommend the AI never approves
alone, and never holds a credential.

**Open question:** a phone must not get full mesh access to read chat. Scope
per-device API credentials separately from mesh enrollment, with enrolment,
revocation, expiry, and audit.

---

## 6. Switch to another session without leaving

**Want:** inside a live session, jump to a different session on this host, or on
a different host, and come back.

**Exists:** `bs shell <peer> --select` and `bs <peer> --select` open a picker,
then attach. `bs sessions` lists. Multi-attach already has test coverage in
`tests/test_multi_attach.cpp` and `tests/test_multi_attach_p1.cpp`.

**The gap:** the picker runs before the attach. Once you are inside a session,
there is no live switcher. And the picker lists one peer's sessions, so moving
hosts means leaving.

**Two versions, different cost:**

| Version | Behavior | Cost |
|---|---|---|
| Detach and re-attach | Leave the session, pick another, return later | Low. The session survives. This is close to shipping already. |
| Live switch inside one terminal | Change which session the local terminal renders, keep both alive | High. Needs a local multiplexer, resize handling per target, and per-session scrollback buffers. |

**Recommendation:** ship detach and re-attach as one command that remembers
where you were, and do not build the live switcher until the panel is real.
`openllm sessions attach` already solves this shape on the OpenLLM side. A cross
peer version needs a fleet session index, which does not exist.

**Open question:** a live switcher means two PTYs and one terminal. That is a
TUI design problem, not a transport problem. Budget it as UI.

---

## 7. Session titles that say what the harness calls them

**Want:** the session list shows the name the harness uses, not a random one. If
we have to read the log file to get it, read the log file.

**Exists:** `harness_session_title()` in `bs-mesh-support.h` line 706 reads
`BS_SESSION_TITLE`, `HERMES_SESSION_CHAT_NAME`, `CLAUDE_SESSION_NAME`,
`CODEX_SESSION_TITLE`, `STY`, and `TERM_PROGRAM`, in that order, then sanitizes
to 32 characters. `resolve_quick_connect_session_name()` deliberately still
returns a unique `tty-*` name, because a title is presentation and never
identity. That separation is correct. Keep it.

**The gap:** it only reads the environment at launch. It does not read a log
file, and it cannot re-read a title after launch. So a session created by a
harness that sets its title later, or names its log file, shows a `tty-` name
until it is reaped. The e2e gate `test_harness_name` in
`scripts/e2e-fleet-test.sh` line 251 already asserts the visible name.

**Recommendation:** do not read harness log files. That couples BridgeSessions
to each harness's private log format, and those formats change without notice.
The stable contract is the environment variable, plus a documented per-harness
fallback list. Extend the list when a harness adopts one. Add a live refresh so
a title that appears after launch is picked up.

**Open question:** which harnesses set which variable, verified on which
version? That table belongs in `docs/configuration.md`, and it needs evidence
per harness, not memory.

---

## 8. Universal OpenLLM harness support, one command fleetwide

**Want:** one command turns on any tool for any harness across the fleet. No
hand-edited JSON or YAML. OpenLLM hands the tool to the harness and takes it
back when the tool is not needed. It reads session history and improves itself
on demand. OpenLLM-only implementation is fine.

**What OpenLLM gives us today (verified, v2.6.36 on fecv3):**

| Capability | Command | Note |
|---|---|---|
| Run a harness through OpenLLM | `openllm claude`, `codex`, `grok`, `hermes`, `opencode` | The substitution point. |
| Unified MCP server over stdio | `openllm mcp --only <group>` | This is the tool handoff. |
| Hook verbs and scripting | `openllm exec <group> <verb>` | The lifecycle seam for grant and revoke. |
| Session list, attach, kill | `openllm sessions` | Session history already. |
| Install and PATH setup | `openllm setup` | Per host, not fleetwide. |
| Health and diagnosis | `openllm doctor` | Fleet roll gate. |
| Embedded API spec | `openllm api --spec` | Write the BridgeSessions contract against this, not against guesswork. |

**The gap:** `openllm setup` is per host. `bs run-script <peer> ./x.sh` can
replicate it, but the grant, revoke, and history-read lifecycle is not defined
anywhere. The most important missing piece is the revoke path. A tool that is
handed out and never taken back is how harnesses accumulate config rot.

**Recommended shape, in build order:**

1. `bs harness enable <tool> --all-peers`. It uses `bs run-script`. It is
   idempotent. It writes one manifest, not per-host edits.
2. Grant and revoke both go through `openllm exec`, so the lifecycle has one
   implementation.
3. The grant record lives in the mesh, not in a host config, so a new peer
   inherits it and a departed peer loses it.
4. History read is read-only and bounded. Never send raw transcripts to a model
   without a redaction rule and an operator switch.
5. Self-improvement is on demand, never automatic. The model proposes a change.
   An operator approves the change.

**Open question:** "improves it dynamically" needs a scope limit. Propose which
limit first — per-session memory, per-harness prompt, or per-fleet policy? Each
has a different blast radius. Recommend per-session, because it dies with the
session.

---

## 9. Automatic fleet resource management, so AI slop cannot fill a drive

**Want:** never think about disk, CPU, or memory again. An AI watches the fleet
and stops AI slop from filling drives, without filling them either.

**Exists, as reporting only:**

| Signal | Where | Behavior |
|---|---|---|
| CPU, memory, disk | `bs health` via `bs-config.h` lines 1355-1380 | Read-only. No action. |
| Idle session reaper | `bs-session-registry.h` `prune_idle` | Kills idle sessions. |
| Finished session reaper | `prune_finished_sessions` | 48-hour default. |
| Ephemeral session reaper | `prune_ephemeral_sessions` | 90-second default for `tty-*`. |
| Received-file sweep | `receive_retention_hours` | 24-hour default, hourly sweep. |

**The gap:** everything above is blind by age, not by size or by value. A
reaper cannot tell a 4-gigabyte build cache from a 4-gigabyte log of AI
nonsense. It deletes both at the same age. That is why the fleet runs at 76
percent on fecv3 today with 199 gigabytes free and no one knowing why.

**The real design question:** what is the reclaimable set. Age is a proxy.
A better proxy is: what did a person create, and what did an agent create. That
is knowable, because the mesh already records session origin. `Session::Kind`
is `user`, `harness`, or `probe`.

**Rules this needs, before any AI is allowed near a file:**

1. Never delete anything the operator created. Enforce by origin, not by name.
2. Never delete without an audit event that names the rule that fired.
3. Every delete is reversible for a stated window. Move to a quarantine area,
   then purge.
4. Hard floors. A node below a disk threshold stops accepting new work. It does
   not start eating its own history.
5. The AI classifies and proposes. A deterministic rule executes. Same split as
   idea 5.

**Pushback:** the AI is the weakest part of this idea and the least necessary.
The high-value 80 percent is a quota, a quarantine area, and four deterministic
rules. Add the classifier after those exist, so it has something safe to run
inside.

---

## Cross-cutting notes

**In flight elsewhere — do not rebuild.** A candidate lane at
`/home/agent/bs-candidates/26.10.03/backend` (branch
`candidate/26.10.03-backend`) is already building the session-control backend
that idea 1 needs. Its IPC contract is frozen:

| Verb | Shape |
|---|---|
| `SESSION_INPUT` | `<machine> <session> <b64>` |
| `SESSION_SCROLLBACK` | `<machine> <session> <offset> <limit>` |
| `SESSION_KILL` | `<machine> <session>` |

`.` means local, a named machine is a remote dispatch. It uses strict base64, a
64 KiB request cap, machine and session name validation, and monotonic byte
offsets with a `RESET` signal when the ring evicts. Read `STATUS.md` in that
directory before writing any session-control code. The original tree must not be
edited by that lane, and the two must not define the same verb twice.

**Sequencing.** Items 1, 6, and 7 all need real panel session input first. Build
that once. Item 5 needs the same input plus an approval surface. Item 8 needs
item 2's interceptor. Item 9 is independent and can start now. Item 3 splits
into a cheap input-completion lane and an expensive hardware lane. Item 4 is
mostly done and needs a decision, not code.

**What is missing from all nine:** a test that proves cross-peer behaviour on a
real Windows and a real Mac. The e2e matrix exists and skips are tolerated
today. Every one of these ideas should refuse to be called done with a skip in
its row.

**Hard limits that still apply.** Do not push `main`. Do not publish a tag. Do
not roll the fleet. The Windows running hash and the published hash still
differ. Read `goal.md` before acting on any of this.

---

## Ideas found during the 26.10.04 audit

Added 2026-10-04. These are not product features. They are the defects and
missing capabilities the audit surfaced while preparing 26.10.04. Each one
either blocks the release or would have made it weaker. They are ordered by what
blocks first.

| # | Idea | Severity | Where it belongs |
|---|---|---|---|
| A1 | A test that exists but never runs | Blocker | `goal.md` done-condition 2 |
| A2 | Installer pointing at a tag that does not exist | Blocker | `goal.md` done-condition 3 |
| A3 | A wait budget below the product's own window | Blocker | `goal.md` done-condition 4 |
| A4 | Uncommitted work with no recoverable copy | Blocker | `goal.md` done-condition 1 |
| A5 | A commit message that understates what it changed | Process | Review checklist |
| A6 | A self-report treated as proof | Process | Subagent contract |
| A7 | No CI gate that a new test was registered | Durability | `scripts/` or CI |
| A8 | Provider and service facts living only in chat | Durability | `docs/` |
| A9 | Flags that were guessed instead of read | Process | Subagent contract |
| A10 | A default-build option nobody turns on | Durability | `CMakeLists.txt` |

### A1. A test that exists but never runs

**The shape of it.** `test_panel_session_control.cpp` and
`panel_session_acceptance.cpp` are complete, correct, and passing when run by
hand. Neither appears in `CMakeLists.txt`. They built only because the worker
passed a private `-DCMAKE_PROJECT_bridgesessions_INCLUDE=...` on the command
line. A default `ctest` will never run them.

**Why it matters.** A test that is not registered is a comment. Worse, it is a
false signal: STATUS.md reports "52 backend tests pass", which is true and
misleading at the same time.

**The idea.** A CI check that fails when a new file lands in `tests/` without a
matching target and `add_test` in `CMakeLists.txt`. Cheap, mechanical, and it
makes the class of bug impossible rather than fixed once.

### A2. Installer pointing at a tag that does not exist

**The shape of it.** Commit `afe4af8` set `install.sh` and `install.ps1` to
`26.10.03`. Upstream has only `v26.09.25-r1` and `v26.09.28`. Its subject line
says "without changing public installers"; it changed both.

**Why it matters.** This is a broken install for every user who runs the script,
and it broke silently because nothing compares installer defaults against real
remote tags. The current parity tests check the *shape* of the installer, never
whether the tag exists.

**The idea.** A release gate that resolves the installer default against
`git ls-remote --tags` and fails if the tag is absent. Do this before the tag is
created only as a warning; after publishing it must be a hard failure. That one
check would have caught the 26.10.03 problem on the day it was written.

### A3. A wait budget below the product's own window

**The shape of it.** The acceptance harness polls 300 × 10 ms = 3 s. The
product's tie-break defers the outbound dial for 12 s
(`kTieBreakAcceptWindowMs`, `bs-mesh-conn.h:824`). Half of all runs failed. The
obvious "fix" is to shrink the product window, which would be a real
regression to suit a test.

**Why it matters.** This class of bug is invisible until it flakes in CI, and
the tempting fix is always the wrong one.

**The idea.** A test-wait constant derived from the product constant, not a
literal. One source of truth, and the test cannot silently fall behind again.

### A4. Uncommitted work with no recoverable copy

**The shape of it.** Four of five lanes had zero commits. 29 dirty files
including the whole session-control backend, the panel auth work, the native
shells, and the TUI width fix. One `rm -rf` from gone.

**Why it matters.** This is not a code defect. It is the single reason the work
was one accident from unrecoverable, and it is invisible to every automated
check because the working tree looks fine.

**The idea.** A lane is not done until it is committed. Add to the subagent
contract: a worker that exits with uncommitted work has not finished, regardless
of what it reports.

### A5. A commit message that understates what it changed

**The shape of it.** "align local version metadata without changing public
installers" changed both public installers. The message is a *claim*, and
nothing checked it.

**Why it matters.** Whoever reads that message later will believe the installers
were untouched. That is worse than a bad message, because it will be trusted.

**The idea.** For anything touching `install.sh`, `install.ps1`, or `VERSION`,
the commit body must enumerate the files and the resulting value. Reviewers
check the list, not the prose.

### A6. A self-report treated as proof

**The shape of it.** STATUS.md claimed 12 + 29 + 7 + 4 tests passing. Those
numbers were right. Other claims in the same file were not: the acceptance
harness "fails on routing", the panel chat adapter is a stub. Re-running the
tests separated them in minutes.

**The idea.** A worker writes the exact command and its exit code. The parent
re-runs. This is already in `goal.md` as a hard limit; it needs to be a habit,
not a line in a file.

### A7. No CI gate that a new test was registered

See A1. The durable fix is the CI check, not just the two registrations this
release.

### A8. Provider and service facts living only in chat

**The shape of it.** The Windows proof host, the fleet host list, the release
verification method — all real, all repeated every session, all unrecorded in
the repo. They exist in chat history and in one operator's memory.

**Why it matters.** A new host, a moved service, a retired machine. Then the
recorded path is wrong and nobody knows until a test fails oddly.

**The idea.** A short `docs/FLEET.md` naming the peer roles, the proof hosts, and
the verification method per platform. Small, and it outlives everyone who
remembers it.

### A9. Flags that were guessed instead of read

**The shape of it.** Workers were told to run a CLI "with the verified command,
do not guess flags" and guessed anyway. Each guess produced a no-op that read
like progress.

**Why it matters.** A guessed command that fails quietly is worse than one that
errors. It produces a file with wrong content and a report that says it worked.

**The idea.** A worker brief that names the binary, the subcommand, and the
expected first line of output. If the first line differs, stop.

### A10. A default-build option nobody turns on

**The shape of it.** `BS_BUILD_NATIVE` exists, is uncommitted, and defaults
`OFF`. The entire `native/` tree — 15 files, five platform shells — is dead code
to a default build.

**The idea.** Any platform shell that ships must be built by the default
configuration or excluded from the tree. A tree of unbuilt code is a claim about
a product, not a product.

---

## Ideas worth considering later

Not from this audit. Worth a decision when there is room.

- **Session diff.** Export what changed in a session since a checkpoint. Cheap
  once scrollback is real, and useful for every review workflow.
- **Panel keyboard-first.** The panel is mouse-driven. Full keyboard operation
  would make it usable from a phone keyboard and from a terminal multiplexer.
- **One `bs doctor`.** A single command that reports version, mesh health, disk,
  memory, installer tag, and test registration. Most of this is scattered across
  five tools today.
- **Harness contract fixture.** A recorded harness session that session-title
  tests replay against, so idea 7 stops depending on which harness happens to be
  installed.
- **Bandwidth-shaped scrollback.** Let a peer ask for a time range rather than a
  byte offset. Useful when ring eviction has already dropped what you wanted.

