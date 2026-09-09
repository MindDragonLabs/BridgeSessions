# BS-SYNC Design — live bidirectional folder sync over the BridgeSessions mesh

**Status:** design + spike (2026-08-13) · **Author:** operator + Hermes
**Target:** bs-sync v1 as a standalone tool; protocol integration into the daemon in v2.

## 1. Goal

Deliver the *effect* of a mounted drive — "my local folder is really a folder on
another machine" — to every platform the mesh already runs on, with zero new
daemons, ports, keys, or kernel drivers.

```
bs sync laptop ~/dev/project hub:/home/work/project
```

While the sync session lives, edits on either side appear on the other in real
time. Combined with `bs shell hub` (run claude on the hub, watch the results land
in your mac folder), this is the remote-first development loop..

## 2. Non-goals (v1)

- **Kernel mounts (FUSE/macFUSE/WinFsp).** Linux FUSE is the only sane one;
  macFUSE is a signed-kext/dext liability on top of existing TCC pain; Windows has
  no equivalent. Sync beats mount on all three platforms.
- **Multi-master CRDTs.** One-editor-at-a-time is the real usage. Version vectors
  per file + `.conflict` copies is enough.
- **Delta compression / content-defined chunking.** Full-file chunked transfer on
  change (reuse of the proven FileChunk/FileAck pipelining). Optional in v2.
- **Daemon integration.** The spike is standalone. Wire types are reserved now
  (`0x2C+`) so integration later is additive, not breaking.

## 3. Why this belongs in bs (vs Syncthing)

Syncthing is battle-tested and already in the fleet. bs-sync earns its existence
where Syncthing is heavy:

| Need | Syncthing | bs-sync |
|---|---|---|
| Extra service + keys + ports per machine | yes | no — rides the existing mesh TLS + pins |
| Windows hosts (shadows/avirs) | its own stack | same single binary |
| Ephemeral containers joining as peers | heavy | join mesh, sync `/workspace` |
| Agent-native control (create/pause a sync like a session) | awkward | same CLI/API shape as `bs shell` |
| Long-running cross-platform field testing | 10 years | young (mitigate: spike + live e2e before merge) |

**Decision rule:** mac↔Linux two-folder sync today → Syncthing. Windows peers,
ephemeral peers, agent-orchestrated sync sessions → bs-sync.

## 4. Architecture

### 4.1 Session model

A sync is a **session**, same family as PTY sessions: it has a name, a peer, a
lifecycle, and a payload stream. Reusing the session model buys us the
concurrency discipline for free (reaper rules, detach semantics, FD accounting —
the exact classes that bit PTYs before).

### 4.2 Transport & identity

- Direct TLS to the peer, exactly like `file recv` / one-shot shells:
  `connect_and_hello(addr, expected_pubkey)` (`bs-protocol.h:14449`).
- Same ed25519 pin verification as every other mesh path — no new trust model.
- Payload = a sync frame stream on the established connection (CONTROL stream for
  small frames, chunk stream for file bodies).

### 4.3 Wire types (reserved now, live in daemon-integration)

Free slots after `CuaVideoCaptureResult = 0x2B`:

| Type | Dir | Payload |
|---|---|---|
| `SyncInit = 0x2C` | both | {protocol_version, node_id, dir_root, session_name} |
| `SyncIndex = 0x2D` | both | {entries: [{relpath, mtime_ns, size, sha256}]} |
| `SyncBatch = 0x2E` | both | {ops: [{PUT, DEL, RENAME}, ...]} (metadata only) |
| `SyncFileMeta = 0x2F` | both | {relpath, size, sha256, chunks} (body follows via chunk stream) |
| `SyncDone = 0x30` | both | {applied, conflicts} |
| `SyncPause / SyncResume = 0x31/0x32` | both | {} |

Bulk bodies ride the existing pipelined chunk path (8-chunk batching since
2.0.20-alpha9). The spike implements these types as its own framed mini-protocol
over direct TLS; integration maps them 1:1 into the daemon's variant dispatch.

### 4.4 Sync algorithm (v1, symmetric reconcile)

1. **Trigger:** watcher event (debounced 300 ms) or interval fallback (2 s).
2. **Index:** walk dir → `(relpath, mtime_ns, size, sha256)`. Fast path: skip hash
   when mtime+size unchanged since last sent index.
3. **Diff:** exchange indexes; per path, apply last-writer-wins on `(mtime_ns)`.
   - Local newer → PUT to remote.
   - Remote newer → GET from remote.
   - Deleted side wins with tombstone grace (deletes are explicit DEL ops, never
     "missing from index" — protects against index-walk races).
4. **Apply:** atomic write (`path.tmp-<node>` + rename), DEL, RENAME. If a file
   changed on the receiving side *after* its index was sent (simultaneous edit),
   write `path.conflict-<node>` instead of clobbering. Symlinks: never follow
   outside the sync root; skip with a warning.
5. **Idle:** no activity → no traffic (zero idle bandwidth, unlike polling-only).

### 4.5 Daemon event-loop constraints (from our own audits)

The daemon is a single-threaded event loop; synchronous transfers already starve
unrelated peers (P1 findings). Sync is a *continuous* transfer, so:

- Index walks and hashing run in bounded slices (N entries per loop pass).
- File bodies go through the existing non-blocking pipelined chunk path with
  backpressure; no sync frame may block the loop.
- Deltas enter a low-priority queue behind keystrokes/PTY output.
- Watcher registrations are per-session, removable, and paused on `SyncPause`
  (and automatically while an agent-driven build storm is detected in-root).

## 5. Platform watcher matrix

| Platform | Mechanism | Notes |
|---|---|---|
| Linux | `inotify` recursive add + `IN_CREATE/IN_MODIFY/IN_DELETE/IN_MOVED_TO/FROM` | watch newly created subdirs; queue overflow → full rescan |
| macOS | `FSEvents` | **directory-granular** — event gives a dir, not a file; reconcile by re-indexing affected subtree |
| Windows | `ReadDirectoryChangesW` | file-granular, needs re-arm on overflow; handle renames as RENAME pairs |
| Fallback (all) | 2 s interval re-index (mtime+size fast path) | correctness net; also covers missed events |

## 6. Security

- Identity/pins identical to the rest of the mesh; no new trust surface.
- Sync roots require an explicit allowlist on the *serving* side (`bs-sync` never
  syncs an arbitrary path; the listener declares which dirs it exposes).
- Path containment: `relpath` sanitization identical to `file recv` (basename +
  containment), plus `..` rejection on every frame.
- Symlinks: never followed outside root; relative symlinks within root are
  materialized as symlinks only if target stays inside root.
- Conflict copies are written with the same permissions as the original.
- Secrets: none in frames beyond what's already in the synced files themselves —
  sync is file-to-file, same hygiene rule as the rest of the fleet.

## 7. Roadmap

- **v1 (this spike):** standalone `bs-sync` — listen/connect over direct TLS with
  pin files, inotify watcher + interval fallback, index/diff/batch/apply, conflict
  copies, pause/resume. Linux first. Proven live between two fleet hosts.
- **v2:** daemon integration (`0x2C+` types, session registry, low-priority
  queue); macOS FSEvents + Windows RDCW watchers; `bs sync <peer> <local> <remote>`
  CLI; agent API parity with shells.
- **v3:** multi-peer sessions (hub topology); optional delta compression;
  optional Linux-FUSE front-end; `bsfile` projection mode for ephemeral
  containers (authoritative local store projected into a container's `/workspace` —
  the Cloudflare-computerd model, on our own mesh).

## 8. Test plan

1. **Unit (spike):** index correctness; diff matrix (create/edit/delete/rename/
   both-edited); conflict-copy behavior; atomic write; debounce coalescing.
2. **Self-host:** listen+connect on one host, two dirs, 500-file tree incl.
   binary blobs — converge, then mutate both sides simultaneously.
3. **Live mesh (two hosts):** `bs file send` the binary to the second node; sync a real
   folder both directions over Tailscale; verify with independent `sha256sum`
   comparison; kill/restart one side mid-sync and confirm resume + no corruption.
4. **Soak:** 10 min with an agent editing on one side, watcher storms (builds),
   daemon restart, network flap.

## 9. Where this fits the fleet vision

The hub stays the auth/identity home. `bs sync` carries the files, `bs shell`
carries the keyboard, and Cloudflare Sandboxes — if adopted — become another
peer syncing `/workspace` over the same protocol instead of a new sync stack.

## 10. Spike findings (2026-08-13, `tools/bs-sync/`)

Standalone spike built and proven live: hub ↔ node over Tailscale, all
mutations converged, trees byte-identical, zero idle traffic. Three design
lessons were learned the hard way — each is now baked into v2 requirements:

1. **Never infer deletion from the peer's index.** First-connect with an empty
   dir deleted the peer's files before the tombstone fix. Deletes must be
   explicit tombstones (`seen` set), and a DEL is only (re)sent while the peer's
   index still contains the path — **the peer's index is the delete ack**;
   re-sending after ack causes permanent churn (observed live).
2. **Hash-diff alone has no direction — it regresses data.** The symmetric
   hash-only diff made the stale side overwrite the fresh side. LWW by mtime
   (`push only if mine.m > theirs.m`; apply rejects `local_m > put.m`) is the
   v1 minimum. Cross-host clock skew is a known limitation; v2 must move to
   per-node logical clocks / version vectors.
3. **Sync-loop hygiene:** hash-equality must suppress transfer regardless of
   mtime (mtime was triggering full re-send of identical content every round);
   atomic `.tmp-*`+rename writes prevent the walker reading half-written files;
   watcher self-noise (own applies, state writes) must be ignored.

Spike artifacts: `bs-sync.cpp` (single file, OpenSSL + nlohmann_json + inotify),
`CMakeLists.txt`, self-test sequence (8 checks) and live two-host matrix (6
checks) documented in session history. Wire types `0x2C–0x32` remain reserved
for daemon integration; the spike speaks its own framed protocol over direct
TLS with cert pinning, independent of the shipping binary's wire format.

## 11. Roadmap phase 2 status — `bs sync pair` (lane 2, 2026-09)

Phase 2 of this design ("cross-machine folder mirroring") now ships in the
main binary as `bs sync pair`:

- **Pair specs** live in `state/sync-pairs.json` under the BS home
  (`id`, `local_dir`, `peer`, `remote_dir`, logical `clock`,
  `via_run_script`, `approved`). Logic in `bs-sync-pair.h`; CLI wiring in
  `main.cpp` (`bs sync pair init|approve|run|status`).
- **Dry-run manifest gate**: `init` scans and prints a manifest and moves
  nothing; `run` refuses unapproved pairs. Deletion is never inferred on the
  first scan (spike lesson #1) — deletes are explicit tombstones derived from
  the previously approved manifest.
- **Logical clocks, not mtime**: classification is sha256-based; every
  manifest scan takes one Lamport tick persisted per pair (spike lesson #2).
  No watcher/daemon loop in this increment — `run` is one-shot through the
  resumable, hash-verified `file send` verbs.
- **Projection-cache rule**: the remote copy is downstream of approved
  manifests; the local dir remains the source of truth for content.
- **Windows peers**: `--via-run-script` only (no PowerShell bodies pushed);
  default is POSIX push for Unix peers.
- **Default exclusions**: `.git`, `.bridgesessions`, secrets basenames,
  bs-sync working files; symlinks never followed.
- **Tests**: `tests/test_sync_pair.cpp` (pair persistence, manifest
  classification, exclusion rules, clock monotonicity, mtime-independence,
  Windows-drive-path rejection).
- Remaining for later lanes: pull/bidirectional reconcile, watcher-driven
  continuous sync (§5), daemon integration of wire types `0x2C+` (v2).

