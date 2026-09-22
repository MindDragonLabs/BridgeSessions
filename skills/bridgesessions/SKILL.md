---
name: bridgesessions
description: Use when operating or developing BridgeSessions mesh peers.
license: BUSL-1.1
metadata:
  version: "26.09.21-r1"
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

The local **source** path in `file send` is resolved by the local daemon against its own
cwd, not the invoking shell's — pass absolute local paths.

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

### Join propagation gap (observed 2026-09-16, dave-pc)

A join writes the new node's pubkey to the **token issuer's** `authorized_keys` only — but the joiner receives the full mesh directory and will dial **other seeds** when the issuer is unreachable or in cooldown. Those seeds reject it (`tls_verify_server result=reject`, a ~1/s reconnect loop that reads like an attack) and gossip cannot carry the key to a node that rejects the connection. Fix on each seed (26.09.15+): `bs peers add <name> <addr>:19949 --pubkey <hex>` — since the seed-pin trust fix a pinned pubkey **does** authorize inbound TLS (log: `tls_verify_server result=accept (seed pin)`), so a pin alone is now sufficient on the accepting side. Appending `pubkey <hex>` to `~/.bridgesessions/authorized_keys` also works and is hot-reloaded (no restart — proven); use it for members you do NOT want as dial targets. An `authorized_keys` entry alone does not create a dial target — direction matters both ways.

Audit of the full incident: `~/bridgesessions/.audits/AUDIT-26-09-16-windows.md`.

### Windows manual join (bypassing install.ps1)

Manual extract-and-join leaves **no boot persistence**: closing the PowerShell window kills the daemon, the issuer logs `mesh_pong_timeout` and cools down, and the joiner starts hammering other seeds (see above). After a manual join, always register the logon task (no admin needed):

```powershell
$a=New-ScheduledTaskAction -Execute "$env:LOCALAPPDATA\bridgesessions\bridgesessions.exe" -Argument "--daemon --config `\"$env:USERPROFILE\.bridgesessions\config`\"";$t=New-ScheduledTaskTrigger -AtLogOn;Register-ScheduledTask -TaskName "BridgeSessions" -Action $a -Trigger $t -Force
```

A Windows peer with the daemon down and no SSH has **no remote recovery path** (`bs shell`/`bs file` die with the daemon; `file recv` refuses paths outside receive_dir by design) — the boot task is the only remote lifeline. To rename a peer, patch `node.name` in place; do not regenerate the whole config file, and leave a backup before overwriting.

## PowerShell quoting

When invoking from a POSIX shell, single-quote the outer command so bash does not expand `$_`:

```bash
bs shell windows-peer --cmd 'powershell -NoProfile -Command "Get-Process | ForEach-Object { $_.Name }"'
```

Prefer a `.ps1` file with `run-script` for multiline logic.

Inside the PowerShell command, put `$env:...` paths in double quotes — PowerShell single
quotes do not expand environment variables.

Caveat (observed 2026-09-16, Windows 11 peer, daemon 26.09.15-r1): nested
`powershell -NoProfile -Command "Get-Content $env:..."` through `bs shell --cmd`
**hung until timeout**. Plain cmd.exe syntax worked immediately: `type "%USERPROFILE%\..."`,
`copy /Y`, `start`, `echo`. If a nested PowerShell one-shot stalls, drop to cmd.exe syntax
or push a `.ps1` and use `run-script`.

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
- `bs-cua-helper.h` / `macos-capture.mm` — desktop support,
- `BSMenubar/` — macOS menubar companion app: bundle identity keys (name, version, icon,
  `LSUIElement`), Xcode toolchain pin + explicit `-target`, signing, launch, and
  verification recipe — see `references/macos-app-bundle.md` in this skill.
- `tools/bridgepanel/` — Python-stdlib web panel (server.py, panel_html.py, invites.py) run
  on peers as the `bridgepanel.service` systemd **user** unit; token-auth model, in-process
  test pattern, and peer deploy/verify recipe — see `references/bridgepanel.md` in this skill.

Generated artifacts are ignored and published through GitHub Releases. Release gate: [`docs/RELEASE-PROVENANCE.md`](../../docs/RELEASE-PROVENANCE.md).

## Release versioning and artifact portability

Version policy: the base stays for the whole release day; intraday fixes take an `-rN`
suffix (`26.09.10` → `26.09.10-r1` → `26.09.10-r2`). Do not bump the base for a same-day fix.
VERSION in the repo can lead the published tags: a bump without a release cut means the
next release inherits it — confirm against `gh api repos/$R/releases` before assuming
`VERSION == latest tag`.

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

**Checksum manifests:** generate once, in one step, from the final artifact set. Never
write the manifest into the directory it hashes — `sha256sum * > SHA256SUMS` truncates
SHA256SUMS before `sha256sum` reads it, leaving the empty-file SHA of itself — and never
let a redundant second workflow step regenerate it with a narrower glob; that is how a
platform's line goes missing. Verify downloads against the **full** manifest: an
inverted grep (`grep -v windows`) masks a genuinely missing line. Checksum/SBOM steps
must fail the job — `|| true` turns a broken manifest into a silently shipped release.

Handy evidence commands:

```bash
gh api "repos/$R/releases?per_page=40" --jq '.[] | "\(.tag_name) pr=\(.prerelease) n=\(.assets|length)"'
gh api repos/$R/releases/tags/v$V --jq '.assets[] | "\(.name) \(.digest)"'
git diff --stat v$V^{commit} origin/main      # tag vs main, authoritative
```

## Fleet binary roll (manual upgrade of a peer)

Sequence: **resolve install path → stage + verify SHA → pre-kill watchdogs → detached swap → service-manager restart → evidence gate**. Read the install path from the host (`systemctl cat` ExecStart on Linux; launchd plist on macOS; scheduled-task `Actions.Execute` on Windows) — a stale binary often sits next to the real one, and Windows install dirs differ per host (`%USERPROFILE%\bridgesessions\` on avirserver*/nunn-shadow-1, `%LOCALAPPDATA%\bridgesessions\` on shadow-ph2lmg3f).

- Stage with `bs file send <peer> /absolute/local/path --dest 15/ --wait`; read back size + SHA-256 on the peer. The swap script must abort on missing file / hash mismatch **without stopping the daemon**. Old daemons may drop `--dest` subdirs at the receive root — read the `OK` line's `dest=`.
- **Pre-kill upgrade watchdogs** (`pkill -f watchdog` or equivalent) before stopping the daemon. A stale watchdog from a failed `auto_upgrade` outlives its daemon, probes during the swap's unbound window (old stopped, new not yet bound), and silently reverts the binary — daemon cycles start→stop seconds apart, ends on the old version. Check `journalctl -u <unit>` before blaming the new binary.
- Run the swap **detached from the `bs` transport** — stopping the daemon drops the carrying shell mid-run by design. Double-fork/`nohup` on systemd, WMI one-shot `.ps1` on Windows. The script: stop service → backup → install new binary → `--version` (new, BEFORE start) → start → assert active + **new pid owns the mesh port** (`ss -ltnp` / `netstat -ano`).
- Windows scheduled tasks: `schtasks /End` kills the task's processes but the AtStartup trigger does not re-fire on /End — re-arm with `schtasks /Run`, and expect elevated orphan daemons to survive; a reboot is the proven clear. `$env:` paths need double quotes inside the `.ps1` (single quotes do not expand).
- macOS: swap the app-bundle binary → `scripts/sign-local-stable.sh` (TCC persistence; ad-hoc signing loses Screen Recording/Accessibility every upgrade) → `launchctl kickstart -k`.
- Evidence gate: new `--version` + SHA read-back on the peer, new pid on the port, then `bs health <peer>` from the operator. A `peers list` offline label can stay stale after that peer's daemon restarted — a live health check outranks the label.

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
| `peers list` says offline but `health`/shell work | peer-table label is stale after that daemon restarted | trust a live `bs health <peer>` over the label |
| swapped binary silently reverts to the old version | stale upgrade-watchdog loop rolls back during the unbound swap window | pre-kill watchdog processes before the swap (see Fleet binary roll) |
| macOS app shows an invented name/version or a Dock icon it should not have | `.app` is a bare executable with no Info.plist — LaunchServices invents identity and runs it Foreground | real bundle: CFBundleName/ShortVersionString, CFBundleIconFile, LSUIElement (see `references/macos-app-bundle.md`) |
| locally built macOS app compiles and signs but will not launch (LS error -10825) | no explicit `-target`, so the SDK stamped a minos above this macOS | rebuild with `-target arm64-apple-macos<floor>`; verify `otool -l \| grep -A3 LC_BUILD_VERSION` |
| new joiner rejected in a ~1/s loop by a seed (`tls_verify_server result=reject`), user sees `tls_handshake_failed` | join registered the key only on the token issuer; this seed never saw it | 26.09.15+: `bs peers add <name> <addr>:19949 --pubkey <hex>` (pin authorizes inbound TLS); or append `pubkey <hex>` to that seed's `authorized_keys` (hot-reloaded), verify next handshake flips to `accept` |
| `bs health <new-peer>` says `unknown peer` but the peer's key is in local `authorized_keys` | no local seed pin — auth and dial-targets are separate stores | `bs peers add <name> <addr>:19949 --pubkey <hex>` |
| nested `powershell` one-shot on a Windows peer hangs | shell transport + powershell spawn interaction | use cmd.exe syntax (`type`, `copy`, `start`) or `run-script` with a `.ps1` |
| one-shot `--cmd` on a Windows peer returns exit 120 with EMPTY output | command runtime exceeded the shell's per-command timeout; the kill discards partial output (quick probes like `echo` still succeed, which reads like a broken package instead of a timeout) | detach long work: send a `.ps1` that `Start-Process`es it with redirected absolute log paths and returns immediately, then poll the log with short one-shots (see PowerShell quoting) |
| Windows peer offline, no SSH, daemon dead | manual join left no boot task; daemon died with its console | hands-on start + `Register-ScheduledTask` (see Windows manual join) |
| one-shot `bs shell --cmd` returns only some lines of a long compound command | shell-transport output handling, not the remote command | buffer each result into a file on the peer (`echo ... >> /tmp/v.txt`) and `cat` it in one final command; `tr -d '\r'` before parsing |
| every one-shot `bs shell <linux-peer> --cmd` takes a flat ~13.5s | session worker for a fast-exiting child vanished (socket+pid unlinked) before the daemon's spawn-wait connected; systemd-run path has no waitpid death evidence → full 12s budget burned → forkpty fallback re-ran the command | fixed in `cc1643c` (worker lingers 4s for a late controller, replays READY+scrollback+DIED on accept, bounded final flush). Diagnose on the peer: `bs-mesh.log` shows `session_worker_spawn via=systemd-run` → 12s gap → `session_worker_spawn_fallback reason=worker socket never appeared` |
| one-shot returns `exit=-1` with `dur_s=0` and empty output (Bug A/B) | two daemon bugs: (A) `apply_min_geometry_locked` sent WMSG_RESIZE to a dead worker → EPIPE → `worker_died` marked before pump ran → buffered OUTPUT/DIED never parsed; (B) fast-exit child output went to worker scrollback with no clients → never fanned out | fixed in `f517ecb`: `worker_queue_frame` no longer marks death on write error; hosted drain path ignores EPIPE; poller fans scrollback as fresh output; `signal(SIGPIPE, SIG_IGN)` added. Verify: `bs shell <peer> --cmd 'echo hi'` returns `hi` + exit 0 in <2s |
| reconnecting to a remote TTY sprays escape garbage / arrows print `ESC O A` / Ctrl-C looks dead | stale private modes (mouse 1000-1016, DECCKM `?1`, bracketed paste) replayed in scrollback re-armed the LOCAL terminal | fixed in `29ba210`: `strip_mode_sequences()` filters mode-setters from replay server- AND client-side; `reattach_surface_reset` kills every private mode before clearing |
| every interactive session is `tty-20260918-…` and unreadable in pickers | no harness title inheritance | `29ba210`: unnamed `bs <peer>` derives `tty-<title>` from BS_SESSION_TITLE / HERMES_SESSION_CHAT_NAME / CLAUDE_SESSION_NAME / CODEX_SESSION_TITLE / STY / TERM_PROGRAM (slugified, 32-char cap); timestamp fallback stays |
| need per-peer latency view | PING/PONG RTT was internal only | `29ba210`: `bs fleet` RTT column; `--json` adds `rtt_ms` (our view) + `latency` (peer's reported table); ServerInfoMsg.latency_json gossips every node's RTT table each cycle |
| `auto_upgrade_complete <peer> rc=32512` repeats hourly in `bs-mesh.log` | auto-upgrade dispatch ran bare `bridgesessions` via `std::system()`; daemon env (launchd/systemd minimal PATH) lacks `~/.local/bin` → exit 127 forever | fixed in `cc1643c` (dispatch uses the daemon's own absolute exe path). Also: peers whose daemon binary was swapped without restart advertise stale `Hello.version` strings, which is what triggers dispatch |
| multiple healthy nodes restart the same stale peer concurrently | auto-upgrade had no designated dispatcher — every node with a newer binary shoots | `mesh.auto_upgrade_origin <node>` (e.g. `fecv3`): only that node dispatches; others log `auto_upgrade_deferred_to_origin`. Hot-reloaded; empty value = legacy any-node behavior |

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
