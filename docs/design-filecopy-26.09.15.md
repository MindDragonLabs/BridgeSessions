# Design: `bs cp` — direct cross-host file copy (26.09.15)

## Problem (evidence)

Three real failure patterns, all from the Heimdall IR bot's document-ingest loop
and the operator's own use:

1. **Pull requires pre-staging on the source peer.** `bs file recv <peer> <path>`
   only serves paths under the source peer's `receive_dir`
   (`resolve_file_request_path(path, receive_dir_)`, bs-mesh-transfer.h:736).
   The IR bot's actual flow is: `mdfind` locates a PDF anywhere on the MacBook
   (`~/Heimdall/`, `~/Downloads/`), then `bs file recv macbook <that path>`
   fails or silently resolves against the receive dir. The bot fell back to
   `bs shell` + `cat`/ssh + manual `mv` — the transfer subsystem was bypassed.

2. **Push lands in staging, not at the destination.** `bs file send --dest` is
   documented as a path *under* the peer's `receive_dir` (enforced since the
   symlink-laundering fix). The operator's video copy (2026-09-10) needed a
   manual `mv ~/.bridgesessions/received/x.mp4 ~/videos/` after the transfer,
   and the double-write doubled disk cost of every large file.

3. **Receive-dir confusion.** Two plausible roots existed
   (`~/bridgesessions/received/` scratch vs `~/.bridgesessions/received/`),
   and the success line reports `dest=` relative to a directory the caller
   often does not know. The bot burned turns running `--help` and probing.

Root cause: `file send`/`file recv` model the receive dir as a mailbox, but
every real consumer (agents, humans) wants robocopy/scp semantics: **path in,
path out**.

## Non-goals

- No change to legacy `file send` / `file recv` wire behavior (old peers keep working).
- No bandwidth scheduling, no delta/rolling checksum, no multi-peer fanout.

## Design

### CLI

```
bs cp [-r] [--update] [--overwrite] [--flat] [--dry-run] [-v] <src> <dst>
bs file ls <peer>:<dir>
```

- Either side may be `peer:path` (scp convention). Exactly one remote side, or
  both (relay); local→local is rejected (`use cp`).
  - `bs cp macbook:~/Heimdall/stmt.pdf ./stmt.pdf`        (pull)
  - `bs cp ./report.md fecv3:/srv/reports/report.md`      (push)
  - `bs cp 'macbook:~/Heimdall/*.pdf' ~/heimdall-ingest/raw/`  (glob, quoted)
  - `bs cp -r macbook:~/Heimdall ./Heimdall`              (tree)
- Bare peer name resolves as today; no path guessing — a `peer:` prefix with
  empty path is an error.
- `--wait` is implied: `cp` is synchronous, always (agents must not add it).

### Semantics (robocopy-ish)

- File→file, file→dir (basename appended), dir→dir with `-r`.
- **Globs expand on the host that owns the path** (daemon-side for remote
  operands, CLI-side for local). Quote remote globs so the local shell does
  not eat them — documented in usage.md next to the example.
- **Collisions fail loud by default**: dest exists → `ERROR dest exists (use
  --overwrite)` + nonzero exit. Never a silent `.1`/`.2` suffix. `--overwrite`
  replaces via the same atomic `.part` → rename path. (The `.1/.2` suffix trap
  in the legacy receive dir is what pushed agents back to scp.)
- `--update`: skip when destination exists with same size + mtime (±2s).
- Always SHA-256 verify at completion; `.part` + `.part.bsmeta` during transfer
  for reconnect/resume, atomic rename at the end.
- **mtime is preserved** on the destination (daemon sets it after rename) —
  required for statement/audit document trails.
- Machine-parsable output for agents — one line per file plus summary:
  - `DONE <bytes> <sha256> <final-absolute-path>` per file
  - `OK copied=N skipped=M bytes=B` summary; nonzero exit on any failure.
  This kills the verify-by-second-round-trip pattern (`ls -la` + byte-compare).
- Path acceptance is **symmetric**: `~`-relative and absolute both work on
  both ends, including `/tmp`. The old asymmetry (`/tmp` fine as source,
  rejected as dest) was learned-by-trial pain; `bs cp` does not inherit it.
  Dest writes follow the literal typed path (normal fs symlink resolution at
  write time); the audit log records both lexical and canonical final path.
  The r1 hidden-dir lexical rule stays on legacy `file send/recv` only.

### Wire

New `FilePull` variant field `direct=1`:

- The requesting peer sends the **absolute resolved source path** (daemon-side
  `expand_home` on the serving peer — `~` expands under the *serving* peer's
  daemon user, matching shell semantics).
- The serving daemon streams chunks from that path after a symlink-checked
  lexical resolution (reuse the containment/sanitization helpers, minus the
  receive-dir root).
- Destination write happens **directly at the final path** on the destination
  daemon — no receive-dir staging copy, no retention sweep involvement,
  no double disk cost.

Legacy peers receiving `direct=1` answer `ERROR unsupported direct` and the
CLI prints "peer too old for bs cp; upgrade to 26.09.15" (same pattern as the
scp-style `--dest` compat shim).

### Security posture (unchanged threat model, stated plainly)

Safety contract #3: an authorized pinned peer already has near-interactive
host access (`bs shell` can `cat`/`mv` anything). `bs cp` adds no new
capability class; it makes an existing capability honest and auditable:

- Transfers stay behind pinned-key mTLS (unchanged).
- Every direct-path read/write is logged by the daemon with full absolute
  paths + peer identity (audit line, not just debug).
- Config `file.copy_scope = anywhere | receive_dir` (default `anywhere`) for
  peers that genuinely want mailbox-only semantics; `receive_dir` restores the
  old confinement for `cp` too.
- Symlink checks: source must not traverse symlinks out of its lexical path
  for **dest writes** we keep the existing hidden-component rule; source reads
  follow the serving daemon's own fs view (same as shell).

### UX fixes bundled

- `bs file send` `OK` line gains absolute `dest_abs=` when the peer is new
  enough, killing the "where did it land" guesswork.
- `bs file ls <peer>:<dir>` (new, cheap): daemon-side directory listing
  (size, mtime, type) so listing stops requiring `bs shell -x 'ls -la'`
  one-shots. Defaults to the receive dir; any directory works on new peers.
- Docs: usage.md leads with `bs cp`; send/recv documented as legacy/staging.

### Consumer feedback incorporated (Heimdall IR bot, 2026-09-14)

Its four "biggest wins" are all in: direct dest, no staging (1); fail-loud
collisions, no `.1/.2` suffix (4); `DONE <bytes> <sha256> <final-path>` line
so agents verify without a second round trip (5); `bs file ls` (8).
Also incorporated: glob support (2), `/tmp`-as-dest symmetry (3),
`--overwrite` semantics, mtime preservation (7), and implied `--wait`.
Third-party relay (`fecv3:path -> macbook:path` without bytes through the
caller) is deferred — the mesh protocol routes via the requesting node today;
true source→dest streaming is a protocol-level change for a later release.
The bot's staging+mv workaround and its scp-everywhere drift are the direct
evidence for this whole design.

## Test plan

- Unit: path-pair parsing (`peer:path`, `~/`, both-remote rejection of weird
  combos), update-skip logic, glob expansion, relay chunk accounting.
- Integration (existing harness): pull from non-receive path; push to absolute
  non-receive path; tree copy with `--update` re-run skipping 100%; direct
  write creates parent dirs; collision → exit≠0 + exact error; `--overwrite`
  round trip; mtime preserved ±2s; `DONE <bytes> <sha256> <path>` line
  format; old-peer error string; `file.copy_scope = receive_dir` refusal;
  `bs file ls` on receive dir and arbitrary dir.
- E2E: macmini↔macbook real transfer incl. a `~/Documents` pull (the IR flow),
  glob pull `macbook:~/Heimdall/*.pdf` → `~/heimdall-ingest/raw/`, SHA
  round-trip, mtime round-trip, no leftover staging copy on the destination.
