# Changelog

All notable public releases are documented here.

> **Version scheme change (2026-08-05):** The project moved from semantic
> versioning (`2.0.x`) to **date-based versioning** (`YY.MM.DD-betaN`). This
> better reflects the continuous-deployment release cadence. The last semver
> release was `2.0.20-alpha10` (2026-08-01); the first date-based release is
> `26.08.05-beta1`. The scheme encodes year, month, day, and a per-day beta
> counter — e.g. `26.08.10-beta2` is the second beta cut on 2026-08-10.

## [26.08.10] — beta2 (2026-08-10)

**Interactive typing lag fix — TCP_NODELAY and sub-millisecond event-loop polling.**

### Performance
- **`TCP_NODELAY`** enabled on all mesh sockets: disables Nagle's algorithm so
  keystroke-sized writes are sent immediately instead of being batched by the
  kernel. Eliminates the 40 ms "first keystroke delay" on interactive sessions.
- **`select()` poll interval reduced from 10 ms to 1 ms** — the main event loop
  now wakes within 1 ms of a readable/writable socket instead of up to 10 ms,
  cutting end-to-end interactive latency by ~9 ms on average.
- Together these two changes make remote typing feel local-latency on LAN and
  tolerable on WAN links.

### Artifacts
- All three platform binaries rebuilt (Linux x86_64, macOS arm64, Windows x86_64).

## [26.08.09] — beta1 (2026-08-09)

**macOS CUA routing fix, join protocol hardening, authorized_keys edge cases,
BSMenubar.app, and install.sh launchd integration.**

### macOS CUA fix
- **CUA requests now route through `cua-helper`** instead of hitting a stub
  handler. The `bs cua` subcommands (screen, capture, click, move, type, key,
  scroll) were silently failing on macOS because the dispatch path called a
  placeholder instead of the `bs-cua-helper` process. Fixed by wiring the
  request path to spawn and communicate with the helper.
- Swift audit fixes: async polling for capture results, `/dev/null` pipe
  redirects for detached processes, `runModal` alert for TCC permission prompts.

### Join protocol fixes
- **`tls_rejected` on new node join:** the join/invite flow required a TLS
  window to be open for the invite token exchange, but the window was not always
  opened in time. Fixed with a 4-layer fix that opens the TLS window eagerly,
  serializes the exchange, and retries on transient rejection.
- **Join serialization:** `peer_pubkeys_json` field was u8-length-prefixed
  (255-byte cap), truncating the mesh-wide pubkey list for meshes with many
  peers. Widened to u16 (65 KB cap).
- **Join window cleanup:** the join/invite window is now closed after the invite
  token expires, preventing stale-window desync on rejoin attempts.

### authorized_keys edge cases
- `authorized_keys` loader now strips a `pubkey ` prefix that the join writer
  was emitting, causing entries to be silently rejected on load.

### BSMenubar.app
- New macOS menu bar app (`BSMenubar.app`) for quick daemon status and peer
  visibility from the system tray.

### Install automation
- `install.sh` now deploys a **launchd agent** for the macOS CUA helper, ensuring
  the helper starts at login and restarts on crash. Includes TCC permission
  guidance for ScreenCaptureKit accessibility.

### Verification
- 392 CTest cases passing (21 new regression tests + `bs-logging.h`
  cross-platform logging harness).

## [26.08.06] — beta1 (2026-08-06)

**Public release with documentation, CI, and audit fixes.**

### Documentation and public readiness
- **README** rewritten with hero banner, feature comparison table, and product
  demo video.
- **SECURITY.md** and **CONTRIBUTING.md** added for public forge readiness.
- Repository cleanup: removed 16 obsolete files, tidied repo root, scrubbed all
  fleet names, credentials, and internal paths from the public tree.
- GitHub CI pipeline, issue templates, checksum verification, and token docs.

### Features (carried from 26.08.05)
- **Delta gossip:** sender-side optimization reduces gossip message size by
  sending only changed fields — wire format unchanged for backward compatibility.
- **Content-addressed script library:** `bs run-script` scripts can now be
  fetched from a content-addressed library (`sha256` → script body), enabling
  script sharing without file transfer.
- All audit P0/P1/P2 fixes from the fresh audit applied (select() recovery,
  session persistence, peer dedup, UAF in `kill()`).

### Fixes
- Invite/join one-liner: tag prefix, `--start` flag, and install URL corrected.
- `gethostbyname` → `getaddrinfo` (was never applied to main after refactor).
- CUA `app_home` path bug (P0).

### Verification
- 392 CTest cases passing.

## [26.08.05] — beta1 (2026-08-05)

**Initial date-based release. First fleet-wide deployment under the new
versioning scheme. CUA subcommands, run-script, peer resolution, and install
automation.**

### Features
- **CUA subcommands** (`bs cua screen|capture|click|move|type|key|scroll`):
  remote computer-use automation across all three platforms. Linux uses
  xdotool; macOS and Windows use the `bs-cua-helper` backend. HID key codes are
  USB usage IDs. All text entry uses POSIX-safe single-quote escaping.
- **`run-script`** (`bs run-script <peer> <file>`): auto-detects interpreter
  (bash/powershell/python) from extension or shebang. Script body is
  base64-encoded — no escaping issues. Supports `--interpreter` override and
  stdin (`-`).
- **Peer resolution:** 4-tier fuzzy matching (exact → suffix/prefix → Levenshtein
  ≤ 2). Ambiguous matches return suggestions instead of guessing.
- **Install automation:** `install.sh` handles daemon lifecycle (kill, clean,
  restart on update) and Linux/macOS CUA helper deployment.

### Performance
- File transfer pipeline depth increased from 8 to 16 chunks (768 KB per
  ack round-trip), delivering ~3× throughput improvement on high-latency links.
- Direct TLS file-receive fallback for peers behind NAT.

### Fixes
- File transfer pipeline: relative paths resolved against `receive_dir`,
  `daemon_file_send` idle hang fixed, `direct_connect_file_recv` now calls the
  correct wait-on-transport function.
- Investigation fixes: Tailscale boot race, peer name collision, code signing
  preparation for macOS.

### Verification
- Fleet deployment across Linux, macOS, and Windows nodes.

## [2.0.20] — alpha10 (2026-08-01)

**Fleet directory, mesh-wide trust propagation, and remote version tracking.**

### Fleet directory (`bs fleet`)
- New `FLEET` IPC command returns JSON fleet table: node name, address, version, status, uptime.
- `bs fleet` CLI prints a markdown table. Daemon aggregates local + connected peer data in one call.
- `Conn` struct gains `remote_version`, populated from Hello handshake and ServerInfo gossip — each node now knows every peer's running version.

### Mesh-wide trust propagation (join flow)
- `JoinReplyMsg` now carries `peer_pubkeys_json`: a JSON array of all configured seeds with their pubkeys.
- The joiner's `bs join` handler adds all mesh seeds as configured seeds and authorizes their pubkeys.
- New nodes can now reach every mesh peer after a single join, not just the host that issued the invite.

### Fleet version audit
- Live fleet survey: 14 peers visible across the mesh, versions ranging from 1.6.0 to 2.0.20-alpha10.
- Identified version drift across macOS, Windows, and container nodes (2.0.19-alpha7/alpha8).
- Deployed 2.0.20-alpha10 to the Linux spine nodes. macOS/Windows/container nodes pending.

### Verification
- Linux CTest: 336/336 passed.
- Live fleet output clean, all peer versions match reality.

## [2.0.20] — alpha9 (2026-07-31)

**Transfer pipelining, background service on Windows, and fleet-wide
performance improvements.**

### Performance
- **Transfer pipelining:** `file_send_wait_on_transport` and
  `file_request_on_transport` now batch up to 8 chunks (384 KB) per
  ack round-trip instead of 1 chunk per ack. Bridges the gap between
  the per-chunk write loop and TCP's send-buffer window (~256 KB).
  Observed throughput on high-latency links improves ~4–8×.
- Default pipeline depth: `kTransferPipelineSize = 8` (48 KB × 8 =
  384 KB). One `sleep_for(2ms)` breath per batch instead of per chunk.
- The receive path already acks on every chunk and requires no
  protocol change — the sender simply writes more frames before
  waiting for the accumulated acks.

### Background service (Windows)
- Windows binary now compiled as `/SUBSYSTEM:WINDOWS` (GUI) instead of
  `CONSOLE`. `FreeConsole()` is no longer needed — the process never
  allocates a console, eliminating the startup flash. The `--daemon`
  flag is still accepted for compatibility.
- `CMakeLists.txt` sets `WIN32_EXECUTABLE` on the `bridgesessions`
  target. MinGW cross-builds pass `-Wl,--subsystem,windows`.

### Install scripts
- `scripts/install.sh` and `scripts/install.ps1` bumped to
  `v2.0.20-alpha9`.

### Verification
- Linux CTest: 336/336 passed.
- Linux static, macOS arm64, Windows x86_64 builds confirmed with
  correct version strings.
- Windows: `PE32+ ... (GUI)` — no console subsystem.

## [2.0.19] — alpha7 (2026-07-28)

**macOS 26 remote video capture is operational end-to-end, and the release
hardens identity, health, finite-shell, installer, and artifact safety.**

### Capture and routing
- Replaced FFmpeg AVFoundation screen input on macOS with native
  **ScreenCaptureKit** frame capture. BridgeSessions captures PNG frames inside
  the approved process, then uses FFmpeg only to encode H.264. This avoids the
  macOS 26 `NSKVONotifying_AVCaptureScreenInput` failure and daemon
  WindowServer-session limitations.
- `capture-video <peer>` now uses a dedicated pinned direct-TLS request and
  waits for the matching `CuaVideoCaptureResultMsg`. The previous daemon IPC
  path only verified that the peer existed, then executed capture locally.
- ScreenCaptureKit callback errors are copied into owned C++ storage before the
  callback returns, eliminating a released-`NSString` crash on failure paths.

### Reliability and safety
- Health probes now drain late output after an exit notification until the
  expected nonce, EOF, or timeout, fixing false negatives caused by
  OutputMsg/ExitCodeMsg ordering.
- Exited sessions now release PTY masters, ConPTY pipes, and pseudo-console
  handles immediately instead of leaking one runtime descriptor set per shell.
- The mesh reaper defers attached children to the PTY output poller so final
  output and `SessionDiedMsg` delivery cannot be stolen by an earlier `waitpid`.
- Explicit `shell --cmd` requests remain finite even when stdin is a PTY;
  reconnecting interactive mode is reserved for commands with no explicit
  command string.
- `keygen` refuses to overwrite any existing `id_ed25519*` identity file,
  preventing accidental fleet-wide trust invalidation.
- The installer rejects unsupported OS/architecture pairs, validates ELF or
  Mach-O format before replacement, checks the downloaded binary's reported
  version, and installs atomically.
- Release tests verify committed Linux, macOS, and Windows artifact magic.

### Verification
- Linux CTest: 336/336 passed; macOS CTest: 335/335 passed; release pytest: 31/31 passed.
- Live macOS capture verified as H.264, 1920x810, 2 fps, 3 seconds, 6 frames;
  retrieval SHA-256:
  `dae226d32cd16e2b13d21aa2d7899156dd5ef86fc9a9471a35b89490ff348761`.

## [2.0.17] — alpha6 (2026-07-27)

**Hotfix follow-on: frame reads now tolerate WANT_READ/WANT_WRITE — Windows-source
pulls no longer die at TLS record boundaries.**

### Fixes
- **`bs file recv` (pull) from Windows peers aborted mid-transfer** with
  `SSL_read header failed: SSL error 2` (WANT_READ): `select()` readiness
  guarantees bytes, not a complete TLS record, and multi-record chunk frames
  surfaced WANT_READ inside `read_frame`, which treated any `SSL_read_ex <= 0`
  as fatal. `read_frame` now retries WANT_READ/WANT_WRITE with a bounded budget
  (400 × 25 ms = 10 s cap) before failing — benefits every frame consumer
  (transfers, health probes, edit sync).
- Carries the 2.0.16 zstd-magic receiver sniff (raw + legacy double-compressed
  senders both accepted).

## [2.0.16] — alpha6 (2026-07-27)

**Hotfix: file transfer between v2.0.14/2.0.15 nodes was broken. Receivers now
sniff the zstd magic and accept both raw (v2.0.14+) and double-compressed
(pre-2.0.14) chunk payloads.**

### Fixes
- **xfer regression in 2.0.14/2.0.15:** the alpha6 double-compression fix
  converted all 6 send paths to raw bytes (`encode()` compresses once), but the
  3 receiver sites still ran a compensating manual `zstd_decompress()` — so a
  2.0.14+ receiver tore down the connection (`zstd: invalid frame`) on the
  first chunk from a 2.0.14+ sender. New `decompress_chunk_payload()` sniffs
  the zstd magic (`28 B5 2F FD`) at all 3 receiver sites; raw payloads pass
  through untouched, legacy double-compressed payloads are unwrapped. Magic
  collisions on raw data fall back to raw (end-to-end sha256 stays the
  integrity backstop).
- **Interop note:** a 2.0.16 receiver accepts senders of any version.
  Pre-2.0.14 receivers cannot receive from ≥2.0.14 senders — upgrade receivers.
- **Regression test:** chunk-payload sniffer covers raw, legacy-compressed, and
  magic-collision shapes (`tests/test_config.cpp`, `[transfer][alpha6]`).

### Also found during diagnosis (not wire-related)
- Long-running daemons leak `/dev/ptmx` FDs (~1 per shell/health session),
  eventually hitting the 1024 soft limit — daemon then fails every `open()`
  (`ERROR cannot hash …` on sends). Restart clears it; lifecycle fix tracked
  for 2.0.17. Workaround: restart the daemon periodically on busy hubs.

## [2.0.15] — alpha6 (2026-07-26)

**Warning hygiene: quick-connect `join` and daemonize paths now check return values
instead of silently ignoring failures.**

### Fixes
- **`join --start` silent failures:** `save_config()` and `ensure_private_directory()`
  failures now abort the join with a clear error (previously ignored — a failed config
  save or authorized_keys dir create left a "joined" node with no persisted config/keys).
  `std::system()` auto-start of the daemon now reports non-zero rc instead of printing
  "Daemon started" unconditionally.
- **daemonize stdio detach:** all three `freopen("/dev/null", …)` calls are checked;
  failure exits the child instead of running the daemon with half-attached stdio.
- Build is now clean of `-Wunused-result` warnings on both gcc-13 (Linux) and
  MinGW g++ (Windows).

## [2.0.14] — alpha6 (2026-07-26)

**File-transfer double-compression fix + release housekeeping. First tag with all three platform binaries rebuilt post-refactor.**

### Fixes
- **file-xfer double-compression (3 sites):** `file_send_wait_on_transport`, `daemon_edit_up`,
  and `daemon_vfolder_sync` each ran a manual `zstd_compress()` on chunk data before
  `write_frame()`, whose `encode()` compresses again — the receiver decompressed once and
  got zstd frames instead of payload, breaking transfers (worst on Windows/MinGW). All 6
  send paths now pass raw bytes to `write_frame()` → single `encode()` compress.
- **SSL large frames:** `SSL_write` now retries on `WANT_WRITE`/`WANT_READ` instead of
  failing the frame (large transfers over slow links).
- **Windows file-xfer stability:** serve transfers run synchronously and worker-thread
  transfers use blocking socket mode (kills the worker-pool SSL race behind the 2.0.12-era
  hang reports).

### Housekeeping
- **Linux dist binary is properly static again** — the 2.0.10-alpha5 artifact was a
  dynamically-linked host build (libssl/libzstd/libfmt/libspdlog all `.so`), the exact
  "missing-library on older hosts" failure the static recipe exists to prevent.
- Removed 3-site manual compression; added regression coverage; 329/329 CTest green.
- PLANS.md archived as SHIPPED; dead monolith-era root `test_config.cpp` removed.
- `ARCHITECTURE.md` + `docs/HOW-TO-COMPILE.md` updated for the R1/R3/R5 layout
  (`bs-protocol.h` + `main.cpp` + `bs-session.h`); Linux static-builder Dockerfile is now
  versioned in `scripts/`.
- Scrubbed a hardcoded tailnet IP from the tree (env-var placeholder).

## [2.0.12] — alpha5 (2026-07-24)

**BridgePanel 10-tab fleet dashboard, remote screen/video capture, transfer reliability.**

> The screen-capture work landed under the in-flight name "2.0.11-alpha5"; `VERSION` moved
> 2.0.10 → 2.0.12 directly, so there is no separate 2.0.11 tag — it is folded in here.

### Features
- **Remote video capture:** `CuaVideoCaptureMsg` (0x2A) / `CuaVideoCaptureResultMsg` (0x2B)
  wired through dispatch — capture on a peer, receive frames on any other peer.
- **Native screen capture** backends for all three platforms (Linux/macOS/Windows).
- **BridgePanel Fleet dashboard:** new Fleet tab (spoke health, harness status, event log)
  plus Events, Models, Health, and Settings tabs — 10 tabs total.

### Fixes
- **file-xfer:** removed the silent fire-and-forget send path — all sends use the WAIT
  variant so errors surface instead of vanishing.
- **BridgePanel:** phantom sessions filtered from the tree, Create endpoint wired,
  `html.escape` fix.
- **Windows:** static PE rebuilt from `main.cpp` (post-refactor source layout).

## [2.0.10] — alpha5 (2026-07-23)

**Structural refactor + Windows -x hang fix.**

### Refactor
- **R1+R3:** Extracted `bs-protocol.h` (12,193 lines) + `main.cpp` (835 lines) from the 13,490-line monolith. Protocol is header-only; CLI/daemon entry is a separate translation unit. 25 test files updated.
- **R5:** Extracted `RingBuffer` + `Session` types into `bs-session.h` (470 lines, self-contained).

### Fixes
- **Windows -x hang:** `SessionDiedMsg` now fans out to `DirectSession` connections (non-interactive mode). Previously only `attached_session` connections received the death notice, causing `shell -x` to spin forever when the child process exited.

### Build
- CTest: 329/329 pass. Fleet: healthy.

## [2.0.9] — alpha5 (2026-07-22)

**BridgePanel remote session discovery — sessions from all mesh peers now appear in the session tree.**

### BridgePanel v3.1

- `build_tree()` now merges remote peer sessions from `MESH_TREE` gossip into the session tree.
  Previously only local daemon sessions were shown; sessions on other mesh nodes were invisible
  unless the user navigated to the separate machines pane.
- Each peer session carries a `peer` attribute so the UI can show which node owns it.
- Live/dead state from gossip is reflected correctly (sessions marked `"live"` when the owning
  peer reports `state=live`).

## [2.0.8] — alpha3 (2026-07-21)

**Multi-attach + spectator roles, conversation store, streaming hardening, cross-platform CUA, BridgePanel v3, and a full MoA security audit (4 P0 + 10 P1 fixed).**

### Multi-attach (P1)

- Multiple connections from the same source (same pubkey) can now attach to one
  session concurrently: `attachments` map keyed by `attach_id`, per-attachment
  detach, MIN-geometry (min-wins) resize policy, `AttachAck` (0x21) reports the
  effective size. Closing N−1 attachments leaves the session + child alive; the
  last detach fires `--signal-on-detach`.
- **Spectator role** (`AttachMsg.spectator`): read-only attachment — receives
  `OutputMsg` fanout but Keystroke / CUA / Signal frames are rejected server-side.

### Conversations (P4)

- Session-independent conversation store: `ConversationAppend` (0x23),
  `ConversationQuery` (0x24), `ConversationBatch` (0x25) with mesh relay.
  Server is the seq authority; store bounded (10k msgs/conv, 1024 convs).
  Body wire is u16-prefixed (65535 B cap, enforced at IPC `CONV_APPEND`).

### Streaming hardening (P3)

- Per-connection output queues with backpressure + `OutputGap` (0x22) — slow
  clients no longer lose bytes silently.
- RingBuffer oversized-write alignment fix; `read_since` clamps to the newest
  64 KiB window with RESET semantics; IPC `SCROLLBACK` verb for incremental sync.

### Cross-platform computer use (P5)

- `CuaRequest` (0x26) / `CuaResponse` (0x27) with `cua_execute` dispatch.
- Linux backend via xdotool (keyboard/mouse); Windows/macOS backends wired;
  screen capture reports honestly where not yet deployed.
- CUA text entry uses POSIX-safe single-quote escaping (shell-injection fix).

### Cross-resolution display correctness (P2)

- Display harness: 11 geometry/glyph/control tests; `doctor` display check.

### BridgePanel v3 (`tools/bridgepanel/`)

- Token-authenticated local IPC (`~/.bridgesessions/ipc-token`) replacing the
  legacy unauthenticated channel. Write verbs require the token.
- New 3-column UI: machines (mesh tree via `MESH_TREE` gossip) → sessions →
  live output (incremental `SCROLLBACK` sync), plus comms/docs tabs.
- New IPC verbs: `PEERS`, `SCROLLBACK`, `MESH_TREE`, `CONV_APPEND`.
- Gossip: `ServerInfoMsg.sessions_summary_json` (legacy-tolerant trailing field)
  with a shape validator on receive (envelope-injection fix).

### MoA security audit (2026-07-21, `.audit/moa-2.0.8a3/AUDIT.md`)

4-lane mixture-of-agents audit of the full alpha3 surface; every finding gated
by a regression test or live reproduction. **4 P0 + 10 P1 + 5 P2 fixed:**

- P0: spectator `SignalMsg` guard (read-only role could execute arbitrary
  commands via `Restart.process`); conversation body u8→u16 wire (feature broke
  on >255 B chat text); RingBuffer oversized-write slot corruption; CUA
  text-entry shell injection.
- P1: gossip JSON envelope validator; IPC framing (128 KiB request buffer +
  RST-avoiding drain on truncated lines); IPC replies `send_all` (short-write
  truncation); re-attach attachment leak; 0×0 resize floor; conversation seq
  authority + store bounds; session-name validation; IPC token-file race;
  vacuous spectator test replaced with a real PTY-echo test.
- 1 P2 deferred (per-session ACL design); 1 lane finding rejected with evidence.

**Tests:** 329/329 CTest green (7 new audit regression tests; 5 new alpha3 test
files). Panel: 20/20 unittest + ruff clean.

## [2.0.7] — alpha2 (2026-07-19)

**CUA from Windows origins, Ctrl-C signal safety, and bug fixes.**

### CUA — interactive ConPTY from Windows origins (Phase A)

- Windows CLI now enters VT-raw mode in the interactive `shell` path (not just
  `--cmd`), so keystrokes — including `Ctrl-C` — are forwarded as bytes rather
  than raising a local console control event. (`enable_raw_mode(forward_ctrl_c)`)
- ConPTY resize is wired: an incoming `ResizeMsg` from a peer calls
  `ResizePseudoConsole` on the attached ConPTY (`resize_pty(hpcon, cols, rows)`).
- `GenerateConsoleCtrlEvent` is wired into the interactive ConPTY path so a
  forwarded `Ctrl-C` reaches the Windows child as `CTRL_C_EVENT`. Both Windows
  spawn paths now pass `CREATE_NEW_PROCESS_GROUP` (ConPTY + one-shot) so the
  event targets the child's own process group, not the daemon's — fixing a
  defect where `Ctrl-C` / `--signal-on-detach` silently dropped for Windows-origin
  children (audited P1, fixed 2026-07-20). NOTE: the live verification above
  covered the Win→POSIX *receiver* path; the Windows-*child* sender path was the
  broken one and is now corrected.
- **Verified end-to-end** (live, from a Windows origin):
  - Win → Linux: interactive session, `Ctrl-C` delivered to the remote
    foreground child as SIGINT, session survives.
  - Win → macOS: same — `Ctrl-C` delivered to the remote (zsh) child
    as SIGINT, session survives.

### Ctrl-C safety (Phase B)

- `--signal-forward` flag (default: on). When off, the local terminal keeps
  raw-mode `ISIG` (POSIX) / `ENABLE_PROCESSED_INPUT` (Windows) so `Ctrl-C`
  raises a local control event and kills the CLI instead of forwarding.
- `--signal-on-detach <HUP|TERM|INT|QUIT|KILL>`: the server sends the requested
  signal to the session's child when the last peer detaches. Carried on the wire
  in `AttachMsg.signal_on_detach` (backward-compatible with v1.6/v1.7 clients);
  unknown names are ignored (no crash).
- Non-interactive `--cmd` path: local `Ctrl-C` exits the CLI (exit code 130),
  and the daemon terminates the one-shot child. Correct by design.
- New automated tests `tests/test_cua_signal.cpp` (3 cases): interactive Ctrl-C
  → child SIGINT + session survives; detach signal delivered to child; unknown
  signal name is a no-op.
- Scenario doc: `docs/cua-signal-scenarios.md`.

### Bug fixes (Phase D)

- **BUG-1 `exec_busy` watchdog (D.1):** `check_stale_exec()` force-releases a
  `exec_busy` flag stuck >90s — sets `exec_cancelled`, requests conn close, and
  `shutdown()`s the socket so the blocking worker errors out and releases the
  flag, unblocking future IPC to that peer. New tests in
  `tests/test_mesh_reliability.cpp` (fires at >90s; does NOT fire for sub-90s).
  **Fix (was P1-01):** the 90s deadline is now measured from the last transfer
  *progress* tick (`exec_last_progress_at`), not from exec start. A healthy,
  actively-streaming transfer (file send / vfolder sync / edit upload) is never
  killed at 90s; a *stalled* transfer (no progress for 90s) still trips the
  watchdog, preserving the original BUG-1 guarantee.
- **BUG-2 Handle=0 (D.2):** verified out of scope — a Windows desktop-session
  boundary issue for a separate Roblox playtest agent, not a BridgeSessions
  protocol bug. No code change.

## [2.0.6] — 2026-07-19

**Release engineering hardening** for the 2.0.6 public tag.

Tag: `v2.0.6` · License: BSL-1.1 (→ Apache-2.0 on 2030-07-16)

### Security and runtime

- LAN mDNS discovery is disabled by default and can update addresses only for
  keys already pinned in seeds or present in `authorized_keys`.
- Local daemon IPC now requires a fresh owner-only per-process token; there is
  no unauthenticated loopback fallback.
- A second Hello is accepted only when byte-for-byte idempotent; identity
  rebinding closes the connection.
- Inbound and outbound daemon handshakes use bounded nonblocking state rather
  than blocking the main event loop.
- File send/receive wait operations and peer file requests run in a bounded,
  joinable worker pool with per-transport ownership and cancellation.
- POSIX PTY input uses a bounded ordered queue with TCP backpressure, preserving
  partial writes, `EINTR`, and `EAGAIN` remainders.
- Windows ConPTY input uses a bounded dedicated writer queue, keeping blocking
  pipe writes off the mesh event loop.
- Async receive destinations are scoped to the requesting connection.
- Image message IDs remain reserved only; 2.0.6 does not advertise large image
  payload fragmentation or receiver rendering.
- Remote edit and vfolder sync IPC commands fail closed in 2.0.6 until they use
  a dedicated transfer transport; they no longer block the daemon event loop.

### Release engineering

- Exact `VERSION` 2.0.6 across CLI, CMake, packaging, and SBOM.
- Deterministic source packaging via `git archive --format=tar | gzip -n`.
- `scripts/package-release.sh` gains `--release` mode with strict dirty-tree
  refusal, no untracked files, and exact `v2.0.6` tag requirement.
- `--commit <sha>` safe override for tests and development packaging.
- `scripts/release-checksums.sh` generates a unique valid CycloneDX UUID per run
  and includes the SBOM hash in `SHA256SUMS`; neither `SHA256SUMS` nor
  `SBOM-binaries.json` hash or list themselves.
- `scripts/codeberg-release.sh` is draft-first, verifies local and remote tag
  commits, uses `curl --fail-with-body`, never mutates published assets, and
  supports both `--dry-run` and explicit `--draft-only` staging.
- `docs/RELEASE-PROVENANCE.md` no longer embeds self-referential source-archive
  hashes inside the source archive.
- Release scripts covered by `tests/test_release.py` registered in CTest.

### Artifacts

| File | Notes |
|------|-------|
| `bridgesessions-linux-x86_64` | ELF x86_64 |
| `bridgesessions-windows-x86_64.exe` | PE32+ MinGW static (OpenSSL+zstd) |
| `bridgesessions-macos-arm64` | Mach-O arm64 (Homebrew OpenSSL/fmt/spdlog dylibs; static zstd) |
| `bridgesessions-2.0.6-source.tar.gz` / `.zip` | Deterministic exact-tag `git archive` source |
| `SHA256SUMS` / `SBOM-binaries.json` | Provenance |

### Validation

- Linux CTest **267/267**
- Linux ASan/UBSan CTest **267/267**
- Focused TSan responsiveness **5 cases / 42 assertions**
- Native Apple Silicon CTest **266/266**
- Native Windows 11 runtime acceptance: **18 test executables, 252 cases /
  1,430 assertions**, including ConPTY queue and resize paths
- Release-engineering pytest **25/25**
- BridgePanel **13/13** and BridgePane **13/13**

## [2.0.5-alpha2] — 2026-07-17

**Public multi-platform alpha** — security/reliability closeout after the 2026-07
independent review. **Not** a production-secure SSH replacement.

Tag: `v2.0.5-alpha2` · License: BSL-1.1 (→ Apache-2.0 on 2030-07-16)

### Highlights

- Hardened mesh identity: pin ↔ TLS cert ↔ Hello binding; direct CLI requires pins
- Transfer integrity: per-connection receive state, exact size/chunk checks,
  streaming SHA-256 before publish
- Canonical `VERSION` drives CLI, CMake, Hello, BridgePanel, packaging, SBOM
- Release artifacts for **Linux x86_64**, **Windows x86_64**, and **macOS arm64**
  from the same source tree

### Security

- Require complete Hello key/name identity; reject inbound configured name/key
  collisions (peer-name impersonation)
- Reject direct CLI connections without a pinned peer key before DNS/TCP
- Reject malformed hexadecimal authorized keys
- Create identity and authorization files as owner-only (`0600`) and app homes as
  `0700` from first write (including legacy migration)
- Launch local editors with argv paths (no shell-parsed remote filenames)
- Reject hostile frame lengths without pointer-arithmetic overflow
- TLS policy: minimum 1.2, maximum/prefer 1.3 (shipping binary)

### Reliability

- Isolate file receive state per connection
- Enforce declared file size/chunk shape; overflow-safe metadata
- Hash partial files before final rename; corrupt/incomplete transfers never land
  under the destination name
- Bounded nonblocking TCP connect on mesh and direct CLI paths
- Honor explicit config paths on daemon reload; logs stay under `--config-dir`
- Fix cumulative duplicate `PROGRESS` lines on long IPC transfers
- Close BridgePanel IPC sockets on failure
- Size-aware + idle transfer timeouts (no fixed 120s wall for large AI assets)

### Release engineering

- Single `VERSION` file is source of truth
- Deterministic source packaging (`scripts/package-release.sh`) excludes
  operator-only and non-shipping modular trees (see `LEGACY_CODE.md`)
- `scripts/release-checksums.sh` validates embedded version, writes `SHA256SUMS`
  and CycloneDX 1.5 `SBOM-binaries.json` (works for PE/Mach-O on Linux hosts)
- Multi-harness agent packaging: `AGENTS.md` + `skills/bridgesessions/`

### Artifacts

| File | Notes |
|------|--------|
| `bridgesessions-linux-x86_64` | ELF x86_64 |
| `bridgesessions-windows-x86_64.exe` | PE32+ MinGW static (OpenSSL+zstd) |
| `bridgesessions-macos-arm64` | Mach-O arm64 (Homebrew OpenSSL/fmt/spdlog dylibs; static zstd) |
| `bridgesessions-2.0.5-alpha2-source.tar.gz` | Deterministic source |
| `SHA256SUMS` / `SBOM-binaries.json` | Provenance |

Verify: `cd dist && sha256sum -c SHA256SUMS` — see [docs/RELEASE-PROVENANCE.md](docs/RELEASE-PROVENANCE.md).

### Validation (release host)

- CTest **237/237**
- BridgePanel HTTP **13/13**
- ASan/UBSan serial suite green
- Targeted TSan (ring + Hello)
- clang-tidy / scan-build clean
- Windows peer `--version` smoke
- macOS build-host `--version`
- 500 MB transfer soak (byte-identical)

### Known limitations

- Public **alpha** — do not treat as a production SSH replacement
- macOS binary links Homebrew dylibs (not fully static)
- Windows artifact is cross-compiled; full Windows CI suite is operator soak
- Checksums ship; forge/minisign signing is optional operator step
- Modular `bs-*` trees remain in git as non-shipping history only

### Upgrade notes

From v2.0.0 / v2.0.1 binaries:

1. Replace the binary; keep `~/.bridgesessions/` (identity + config)
2. Ensure every `seed` line has `pubkey=<64-hex>` (`mesh.require_seed_pins` defaults true)
3. Re-run `bridgesessions doctor` and `health <peer>` (must say `healthy (data-plane ok)`)

---

## [2.0.1] — 2026-07-16

- Windows one-shot shell/health pipe capture fix
- Stale seed hygiene
- Prebuilt Linux / macOS / Windows binaries on the public tree

## [2.0.0] — 2026-07-16

First public release.

### Added

- Business Source License 1.1 (Change Date 2030-07-16 → Apache-2.0)
- Full `docs/` tree (design, building, usage, configuration, protocol, bridge-panel)
- Public README, CONTRIBUTING, SECURITY
- Prebuilt binaries in `dist/`: Linux x86_64, macOS arm64, Windows x86_64
- Bridge Panel web surface + `bridgesessions pane publish`
- Stale-exec watchdog (force-release stuck `exec_busy` after 90s)

### Changed

- Version unified to 2.0.0
- Public tree stripped of internal notes, fleet configs, and build cruft
- Example configs use placeholder hosts only (no real infrastructure IPs)

### Prior history

Earlier internal tags (v1.7.x–v1.8.x) were development releases and are not part
of this public history (orphan rewrite for a clean initial public commit).
