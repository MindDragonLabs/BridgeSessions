---
name: bridgesessions
description: Use when operating or developing BridgeSessions mesh peers.
license: BUSL-1.1
metadata:
  version: "26.09.11-r1"
  product: BridgeSessions
  forge: "github.com/MindDragonLabs/BridgeSessions"
---

# BridgeSessions

BridgeSessions (`bridgesessions`, `bs`) is one C++23 executable for persistent shells, verified files, and computer-use automation across a trusted peer mesh.

Read repository [`AGENTS.md`](../../AGENTS.md) when available.

## Safety contract

1. Resolve peer names from `bs peers list`; do not guess.
2. Require pinned Ed25519 keys. Do not disable `mesh.require_seed_pins` on an untrusted network.
3. An authorized peer has near-interactive host access. Never treat peer authorization as low privilege.
4. Never expose private keys, tokens, config, private hosts, VPN addresses, or personal paths.
5. Windows peers require Windows commands. MinGW outputs Windows PE; it is not Linux.
6. Use one stacked shell or `run-script` for dependent work.
7. A final `OK`, remote exit code, checksum, or read-back is required evidence. Dispatch is not success.
8. Restarting a daemon can kill the `bs shell` carrying the restart. Use the platform service manager from an independent control path.

## Probe

```bash
bs --version
bs peers list
bs health <peer>
bs fleet
```

`health` must report data-plane health, not merely local IPC availability.

## Shells

```bash
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'hostname && uptime'
```

- `Ctrl-D` detaches an interactive session.
- Reuse the same `--name` to reattach.
- One-shot calls do not share shell state.

For complex work:

```bash
bs run-script <peer> ./task.sh
bs run-script <peer> ./task.ps1 --interpreter powershell
```

Script arguments are separate argv values and are quoted before remote execution.

## Files

```bash
bs file send <peer> /absolute/local/path --wait
bs file send <peer> ./config.toml --dest configs/config.toml --wait
bs file recv <peer> received/report.md --to ./report.md --wait
```

Parse `PROGRESS ...` lines. Success requires final `OK` after SHA-256 verification. Transfers preserve valid partial files for reconnect/resume.

By default peers serve only from `receive_dir`. Do not enable sensitive/arbitrary paths casually.

`--dest` is a path **under the peer's receive directory**, not an absolute path. The `OK`
line's `dest=` field is relative to that directory, so use it to find the file; if the peer
is too old to confirm, the sender says so instead of guessing.

The receive directory is staging, not storage. Every received file is also written to the
caller's own destination, so a leftover copy doubles the disk cost. Files older than
`receive_retention_hours` (default 24, `0` disables) are removed by an hourly sweep.
`.part` / `.part.bsmeta` files are never removed.

## Computer use

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
bs cua type <peer> --text 'hello'
```

Capture before clicking. Windows/macOS require one user-session `--cua-helper`; macOS also requires Screen Recording and Accessibility approval. Spectators cannot send CUA input.

On Windows the helper re-execs itself with `CREATE_NO_WINDOW` and the installer marks the
logon task hidden, so it must never show a console window. A visible console window means
the helper was started some other way.

## Bootstrap

```bash
# Existing pinned seed
bs invite

# New node
bs join <seed-address>:19949 <single-use-token> --start
```

Only explicitly pinned seeds can issue accepted mesh-wide enrollments. `bs enroll` is an administrative out-of-band vouching path, not the normal install flow.

## PowerShell quoting

When invoking from a POSIX shell, single-quote the outer command so bash does not expand `$_`:

```bash
bs shell windows-peer --cmd 'powershell -NoProfile -Command "Get-Process | ForEach-Object { $_.Name }"'
```

Prefer a `.ps1` file with `run-script` for multiline logic.

## Develop

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash scripts/prepublish-scan.sh
```

Source of truth:

- `main.cpp` — CLI/upgrade,
- `bs-protocol.h` — include facade,
- `bs-codec.h` / `bs-tls.h` / `bs-pty.h` / `bs-cua-dispatch.h` — codec, TLS, session/PTY, CUA,
- `bs-mesh-controller.h` + `bs-mesh-{support,conn,transfer,cli,ui}.h` — mesh event loop,
- `bs-session.h` — session lifetime,
- `bs-session-worker.h` — optional worker,
- `bs-cua-helper.h` / `macos-capture.mm` — desktop support.

Generated artifacts are ignored and published through GitHub Releases. Release gate: [`docs/RELEASE-PROVENANCE.md`](../../docs/RELEASE-PROVENANCE.md).

## Release versioning and artifact portability

Version policy: the base stays for the whole release day; intraday fixes take an `-rN`
suffix (`26.09.10` → `26.09.10-r1` → `26.09.10-r2`). Do not bump the base for a same-day fix.

Every platform is built by `./build.sh` (`linux`, `macos`, `windows`, `all`, `package`,
`test`, `deps`, `clean`). It is the only builder; do not hand-roll a cmake invocation.

**Never promise a fleet roll before testing the published artifact on a real peer of each
platform.** Run `ldd <asset> | grep 'not found'` and `<asset> --version` on the target.
A release note claiming portability is not evidence.

Which image, and why:

- Linux release artifact: build in `ubuntu:22.04`. The glibc floor comes from the image,
  not the host — a binary built on Arch or 24.04 will not start on a 22.04 host.
- Windows: `mingw-w64` runs on Linux and emits a Windows PE, so it is always a cross build.
  The host mingw version decides C++23 support: Ubuntu 22.04 ships GCC 10.3 (rejects
  `-std=c++23`), 24.04 ships 13.2, Arch ships 16.x. `build.sh windows` uses a capable
  native mingw first and falls back to the 24.04 container.
- macOS: builds OpenSSL from source by default. A Homebrew OpenSSL leaves the binary
  linked to `/opt/homebrew/opt/openssl@3/...`, which does not exist on a Mac without
  Homebrew. Sign AFTER stripping — stripping invalidates a Mach-O signature and the
  kernel kills the process (SIGKILL, "Killed: 9").

Dependency portability: spdlog is built from source with its **bundled** fmt. Linking a
distribution's spdlog pulls in an external `libfmt` whose soname differs per distro, which
is how a release asset ended up needing `libspdlog.so.1` + `libfmt.so.8` and could not load
on Arch. Only `libssl.so.3` / `libcrypto.so.3` come from the system.

**Build the most constrained platform first.** The Windows mingw build caught a
`rel.native()` wide-string error that the Linux and macOS builds accepted, because
`path::native()` is `std::wstring` only on Windows. A Linux-only or macOS-only green build
is not proof a change is portable.

Publish gate: `scripts/github-release.sh` refuses a dirty tree, a local tag that is not
HEAD, or an origin tag that is not HEAD. Publish **before** amending the release commit —
an amend after the push desyncs the tag and blocks the gate.

Handy evidence commands:

```bash
gh api "repos/$R/releases?per_page=40" --jq '.[] | "\(.tag_name) pr=\(.prerelease) n=\(.assets|length)"'
gh api repos/$R/releases/tags/v$V --jq '.assets[] | "\(.name) \(.digest)"'
git diff --stat v$V^{commit} origin/main      # tag vs main, authoritative
```

## Common failure patterns

| Symptom | Likely cause | Action |
|---|---|---|
| unknown peer | wrong/ambiguous name | inspect `bs peers list`; do not guess |
| TLS rejected | stale/wrong key pin | verify identity out of band before changing pin |
| healthy control but broken command | data-plane/session failure | run `health`, then one finite shell probe |
| transfer stalls | busy transport or connectivity | allow reconnect/resume; inspect final error |
| CUA auth error | duplicate/stale helper token | stop duplicate helpers; restart one helper + daemon |
| macOS capture/input denied | TCC permissions | grant to signed install, restart helper |
| daemon restart cuts command | control path depended on daemon | use systemd/launchd/Task Scheduler independently |

Two identical non-progressing failures: stop retrying and diagnose a different layer.

## Windows: never spawn a console from the daemon

The daemon runs in the interactive desktop session (it hosts the CUA overlay),
so any subprocess it creates with a normal console gets a visible window on the
user's screen. `std::system("taskkill /F /T /PID …")` in `Session::kill_tree()`
flashed a console on every session teardown; replaced with `CreateProcessW`
flagged `CREATE_NO_WINDOW`. Rule: every Windows subprocess spawn in daemon-side
code (`bs-pty.h`, `bs-session.h`, `bs-cua-dispatch.h`) must pass
`CREATE_NO_WINDOW` (or use the oneshot/ConPTY paths that already do).
`bs-cua-helper.h`'s `win_detach_cua_helper()` is the reference pattern.
