# Changelog

Notable user-visible changes. Git history contains implementation-level detail.

## Unreleased

### Fixed

- fix(spawn): fully-timed-out systemd-run worker spawns no longer orphan the
  scope. The daemon now records the deterministic worker unit name at spawn
  time and stops it on every failure path where the worker was never adopted
  (socket never appeared / READY handshake failed / no child pid / PTY
  nonblock setup failed); adopted workers remain owned by the normal reaper.
  Unit names are sanitized to systemd's `[A-Za-z0-9:._-]` charset — session
  names previously flowed into `--unit=` verbatim. Covered by
  `tests/test_unit_name_sanitization.cpp`. (Greptile P1 tail, 26.08.31-release
  review.)

## 26.08.31-release

Upgrade-safety release. Fixes three independent fleet-upgrade failure modes
found during the 26.08.28-r2 rollout RCA.

### Fixed

- fix(upgrade): config hot-reload now carries `mesh.auto_upgrade` — previously
  seeds-only, so a peer with `mesh.auto_upgrade false` on disk but seeded
  before the pin existed ran upgrades anyway (cpanel 2026-08-27 incident).
- fix(upgrade): dispatch re-checks the auto_upgrade pin at execution time,
  closing the race between policy load and dispatch fire.
- fix(upgrade): never downgrade below the running version — upgrades from a
  newer local build to an older GitHub release are refused with exit 0 and a
  clear message unless `--allow-downgrade` is passed (fleet-wide downgrade
  wave 2026-08-27).
- fix(spawn): worker-spawn latency race — the fixed 3s/60-iteration readiness
  poll is replaced with a 12s adaptive budget (exponential backoff +
  early-death detection via waitpid/pid-file) plus a daemon-start AMFI warm-up
  on macOS. fecv3 lost ~1/6 spawns, btcr/macbook lost most during load.
- fix(build): deterministic Windows PE link — MinGW ld embedded the link
  timestamp in the COFF header, so identical source produced a different
  binary on every build (release-convergence runs run1/run2 differed).
  `--no-insert-timestamp` pins it; Windows artifacts are now byte-reproducible
  like linux/macOS.
- fix(review): Greptile pre-release findings on the release PR (#13) —
  (1) the auto-upgrade execution-time pin re-check ran on a worker thread and
  mutated shared reload state (`config_mtime_`, seeds vector) against the
  event loop; replaced with a stateless snapshot read of the pin from disk;
  (2) a stale session-worker pid file (dead predecessor) could condemn a
  healthy systemd-run replacement still cold-starting; the stale file is now
  cleared before spawn; (3) the macOS worker warm-up shell command quoted the
  exe path, so paths with spaces or metacharacters no longer break it.


## 26.08.28-r2

- fix(windows): CLI construction crash — root positionals renamed PEER/SESSION
  CLI11 >= 2.x rejects a subcommand long-option (--peer/--session) whose name
  collides with a root positional. Every Windows build since 26.08.26-r1
  aborted at startup before parsing any argument (OptionAlreadyAdded: peer).
  Linux/macOS were unaffected (CLI11 1.9 in the build container).
- positional matching is case-insensitive: `bs <peer>` quick-connect unchanged.

## 26.08.28-r1

Reliability fix: session-worker zombies.

### Fixed

- macOS/Linux: a session worker that failed to come up (or was torn down)
  is now reaped, not just signalled. Previously every failed worker spawn
  leaked one zombie process; a version-skewed worker binary combined with
  frequent incoming session probes could exhaust the per-user process
  table within hours (fork() failing system-wide).
- Worker spawn failures now log the worker binary's reported version and
  exit status, so a daemon/worker version mismatch is immediately visible
  in the log instead of a generic "socket never appeared".
- Session kill, idle prune, and connection-drop teardown paths reap the
  worker process as well, so liveness checks no longer see reaped
  workers as alive.

## 26.08.27-r1

Security, privacy, and reliability hardening.

### Security

- Enforce known peer pins during the TLS handshake, scope join acceptance to
  the mesh listener, restrict home/temp transfer destinations, and require
  authentication for every BridgePanel write.
- BridgePanel accepts bearer tokens, no longer prints tokenized startup URLs,
  and resolves local inbox paths with symlink-aware containment.

### Privacy

- Support private join-token input, redact common command secrets from session
  persistence and logs, restrict persisted-session permissions, truncate TLS
  identity logging, and default operational logs to `info`.

### Reliability

- Preserve sockets for live but slow session workers, replace worker `select`
  calls with `poll`, make frame retries cancellation-aware, and ensure the
  busy-operation watchdog is always timestamped.

## 26.08.26-r2

Shell survival + selector fixes.

### New: sessions survive daemon restart and upgrade (POSIX)

- Hosted sessions now run in a detached per-session worker process
  (`bridgesessions session-worker`) that owns the PTY and shell. The daemon
  talks to workers over local sockets; killing, restarting, or upgrading the
  daemon no longer kills your shells. On startup the daemon re-adopts live
  workers with their full state and scrollback tail.
- Previously, `bs upgrade` (or any daemon restart) SIGHUP-killed every hosted
  shell: sessions "resumed" as fresh empty shells with all state lost.
- Workers are launched outside the daemon's cgroup (systemd scope when
  available, setsid otherwise), so `systemctl --user restart` also survives.
- Kill semantics unchanged: `bs kill` / session kill asks the worker to shut
  down, which takes its shell down with it.
- Escape hatch: `BS_SESSION_WORKER=0` restores the pre-r2 direct-forkpty
  spawn. Windows sessions are unchanged (worker hosting is POSIX-only).

### Fixed: jammed local terminal after transport loss / force kill

- Interactive attach now intercepts Ctrl-D as the detach key (as documented):
  it detaches and leaves the session alive instead of forwarding 0x04 to the
  remote PTY, where it read as EOF and killed the shell. Ctrl-C remains a
  remote keystroke (never disconnects).
- On unexpected transport loss the client resets leaked TUI modes (mouse
  tracking, alt screen, bracketed paste) immediately and prints
  `[transport lost — reconnecting … Ctrl-D to quit]` instead of silently
  retrying in a terminal that looks hung. `[reconnected]` confirms recovery.
- SIGHUP/SIGTERM while the client holds raw mode now restore the local
  terminal before the process dies (best-effort signal handler). Previously a
  force kill left the terminal in raw+mouse mode — escape garbage on mouse
  movement and BEL noise until `reset`.

### Fixed: `bs connect` selector

- Arrow keys (↑/↓ or j/k) + Enter to select; q/Esc cancels. The numbered
  prompt remains as a fallback on non-terminal stdin.
- The local node no longer appears in its own server list.

### Fixed: scrambled TUIs from the selector and `-x`

- Harness commands launched from the selector (and the new `bs shell -i`)
  now use the interactive raw-terminal path. Previously any non-empty `-x`
  command took the output-capture path, which strips ANSI escape sequences —
  full-screen apps (hermes --tui, claude, etc.) arrived scrambled.
- Plain `bs shell <peer> -x <cmd>` keeps the capture behavior for automation.

### Fixed: self-connect error message

- `bs <self>` / `bs shell <self>` now print
  `Cannot connect to yourself. This is <node>.` instead of the misleading
  "Refusing untrusted first contact". Matches mesh node name and OS hostname
  (including macOS `.local` Bonjour drift).

### Misc

- `scripts/build-local.sh` — one-call local dev build
  (`--tests` / `--debug` / `--clean`).

## 26.08.26-r1

Primary main-line release. Calendar version + `-r1` (release) stamp.

### New: `bs connect` — interactive server → harness selector

- `bs` with no arguments from a terminal now opens an interactive selector:
  pick a server (live FLEET status), then pick an agent harness, and
  BridgeSessions opens an interactive shell on that server running the
  harness launch command (e.g. `hermes --tui --yolo`).
- Explicit form: `bs connect [--peer NAME] [--harness NAME]` (skip menus).
- Harness launch commands are configurable via `harness.<name> <command>`
  config lines; built-in defaults exist for hermes, claude-code, codex,
  opencode, grok, copilot, cursor, and a plain shell.
- The harness name doubles as the session name, so reattaching later is
  `bs shell <peer> -n <harness>`.
- Bare `bs` daemon behavior is preserved: the launchd/systemd daemon runs
  with stdin not a terminal and still falls through to daemon mode;
  `bs --daemon` explicitly daemonizes.

### New: BridgePanel paste-path-to-open

- New path input in the files pane: paste a full path (absolute OS path or
  relative to the current root) including the filename and press Enter —
  BridgePanel resolves the root, navigates to the parent directory, and
  opens the file.
- New API endpoint `/api/open-path?machine=...&root=...&cwd=...&path=...`
  resolves absolute paths against the local inbox receive_dir and
  allowlisted volume roots (local and remote), rejects traversal, and
  verifies existence before navigating.

### Fixes and hygiene

- `save_config` now persists `harness.<name>` launch commands (round-trips
  with load_config).
- Config parsing, docs, installer stamps, macOS Info.plist, e2e matrix, and
  the portable skill all updated to the `26.08.26-r1` release stamp.

## 26.08.25-beta7

Rebuild of the beta7 line. Installers and release artifacts now stamp
`26.08.25-beta7`. The `26.MM.DD` and `2026.MM.DD` forms compare as the same
calendar year.

### Packaging

- Linux x86_64, Windows x86_64, and macOS arm64 binaries rebuilt from this tag.
- Default installer tag is `26.08.25-beta7`.
- macOS `Info.plist` bundle version matches the tag.

## 2026.08.24-beta7

### Security

- Spectator attachments are rejected for remote video capture (`CuaVideoCaptureMsg`), matching the existing CUA input deny.

### Reliability

- Auto-upgrade version compare treats `26.MM.DD` and `2026.MM.DD` as the same calendar year instead of using ASCII order.
- Simultaneous-dial collision livelock fixed (from `main`: `0367ef5`).
- Install/upgrade no longer persist-disables the systemd user unit. A failed
  swap used to leave the node with no listener, so inbound `bs shell` got
  TCP refused. Pause now uses a runtime mask; every failure path re-enables
  and starts the daemon. Windows installer also forces `ExecutionTimeLimit=0`.

### Interactive shell

- Ctrl-C is always a remote keystroke. Double Ctrl-C no longer sends `Kill` or
  disconnects the BridgeSessions client. The session ends only when the remote
  shell exits (`exit`) or the transport dies.
- In TUI environments (Hermes TUI), a process-group SIGINT is forwarded as one
  `0x03` keystroke instead of terminating the local client.

### Peer names

- `bs shell <this-node>` refuses immediately (`Cannot shell to this node`)
  instead of fuzzy-remapping to a sibling (`seed-a` → `seed-b`) and hanging.
- Fuzzy resolve no longer treats digit-only siblings as typos (`seed-a`/`seed-b`,
  `host-1`/`host-2`). Levenshtein auto-match is capped at distance 2.

### Onboarding (no roster files)

- New nodes join the mesh with a single `bs join <addr> <token> --start` one-liner.
  On success the controller signs a mesh-directory enrollment for the joiner and
  gossips it to every peer, so each peer auto-trusts and seeds the new node's key
  with **no manual key copying, no YAML roster edit, and no seed-sync script**.
- Removed the legacy manual `deploy/<host>/` bundle (hand-written join script
  + YAML-patching `authorize.sh` + full-path `SHA256SUMS`) that bypassed the native
  join flow and forced the YAML/roster path.

### Installer checksums

- Installers verify a GitHub Release `SHA256SUMS` entry by **basename**; the
  release checksum generator writes basenames only. A `SHA256SUMS` keyed by an
  absolute path no longer passes as a valid entry (previously surfaced as
  "SHA256SUMS has no valid binary entry").

## 26.09.19-beta6

### Security

- Remote file requests are confined to the canonical `receive_dir`; prefix-collision and symlink escapes are rejected.
- Local absolute paths are no longer returned in transfer errors/ACKs.
- Mesh-wide enrollment accepts signatures only from explicitly pinned seed keys.
- Receive buffers, handshake buffers, IPC response lines, PTY input, and TLS transmit queues are bounded.
- Remote peer names are validated before auto-upgrade shell dispatch; upgrades use a joinable worker pool.
- Script-cache aliases reject traversal, symlink targets stay inside the cache, hashes are validated, and script argv is shell-quoted per argument.
- Linux release hardening: PIE, RELRO/NOW, NX stack, stack protector, and fortified libc. Windows hardening: Control Flow Guard or ASLR/NX linker flags.
- Installers verify a GitHub Release `SHA256SUMS` entry before executing or replacing a binary. Windows uses a verified temporary PE and atomic move.
- Static release dependencies move to supported lines: OpenSSL 3.5 LTS, spdlog 1.17, fmt 12.2, Catch2 3.15, CLI11 2.7, nlohmann-json 3.12.

### Reliability and performance

- Fresh incoming transfers hash incrementally instead of re-reading the whole file on the single-threaded event loop.
- Valid partial transfers survive transport loss and can resume; protocol/write failures still remove corrupt partials.
- File ACKs, control replies, PTY output, CUA results, and death notifications use non-blocking bounded TX queues.
- Per-peer TLS reads have a fairness budget and protocol-sized hard cap.
- Removed an unreachable event-loop file sender that performed blocking hashes and writes.
- Fixed `Session` move-assignment use-after-lifetime undefined behavior.
- Session/worker teardown now distinguishes reaped children, kills process groups, and bounds graceful wait time.
- Session workers use non-blocking PTY input with a 1 MiB high-water mark; stalled controller writes have deadlines.
- Worker READY messages now carry the child PID and remain compatible with legacy payloads.
- Linux process-group tests treat a zombie as dead, eliminating a container-specific CI false failure.
- Reconnect backoff defaults to 300 seconds and established receive connections are reserved from concurrent transport borrowers.

### Repository and GitHub

- Removed abandoned modular prototypes, obsolete wrappers, duplicated service templates, unrelated setup helpers, per-release Markdown files, and stale audit/RCA fragments.
- Removed compiled binaries, app bundles, checksums, and SBOMs from git. GitHub Releases are the artifact channel.
- Added CodeQL, Gitleaks, Dependabot, and a verified GitHub release script.
- Rewrote active docs around the shipping monolith and current trust model.
- Scrubbed private fleet names and personal build paths from tracked text and release checks.

### Platform work

- macOS ScreenCaptureKit, signed helper/app flow, and CUA permission guidance consolidated.
- Windows child-tree cleanup retains the Job Object path and Task Scheduler-compatible tree-kill fallback.
- File-transfer framing keeps legacy u16 frames and advertises `+frm2` for bounded 4 MiB u32 frames.

## 26.08.16-beta4

- Improved cross-platform packaging, installer behavior, session lineage, fleet status, direct file paths, CUA/video flow, and macOS application packaging.

## 26.08.12-beta4

- Hardened daemon IPC, connection ownership, transfer cancellation/progress, process cleanup, and key-rotation behavior.

## 26.08.12-beta3

- Added the calendar-version beta line, `+frm2` capability negotiation, direct file transfer, fleet view, auto-upgrade, and broad regression coverage.

## 26.08.10-beta2

- Added computer-use automation, Windows/macOS helpers, large media transfer, improved Windows command handling, and desktop/tray groundwork.

## 26.08.09-beta1

- Added invite/join bootstrap, pinned mesh identities, peer discovery, persistent sessions, and the first public beta packaging.

## 26.08.05–26.08.06-beta1

- Consolidated the project into one executable and established Linux, macOS, and Windows build/release lanes.

## 2.0.x alpha series (July–August 2026)

The alpha series established the core protocol and then replaced several early designs:

| Line | Main result |
|---|---|
| 2.0.0–2.0.5 | TLS mesh, terminal sessions, key auth, initial file transfer |
| 2.0.6–2.0.8 | monolith source of truth, CMake/Catch2 suite, IPC, CUA message families |
| 2.0.9–2.0.12 | invite/join, direct sessions, reconnect behavior, video capture |
| 2.0.14–2.0.17 | transfer framing, Windows/macOS fixes, lifecycle hardening |
| 2.0.19–2.0.20 | pipeline performance, telemetry, release engineering, beta transition |

Historical per-tag prose was removed from the working tree because it duplicated this file and frequently contradicted current code. It remains available in git history.
