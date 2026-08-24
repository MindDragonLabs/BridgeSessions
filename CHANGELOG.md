# Changelog

Notable user-visible changes. Git history contains implementation-level detail.

## 2026.08.24-beta7

### Onboarding (no roster files)

- New nodes join the mesh with a single `bs join <addr> <token> --start` one-liner.
  On success the controller signs a mesh-directory enrollment for the joiner and
  gossips it to every peer, so each peer auto-trusts and seeds the new node's key
  with **no manual key copying, no YAML roster edit, and no seed-sync script**.
- Removed the legacy manual `deploy/rana-shadow/` bundle (hand-written join script
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
