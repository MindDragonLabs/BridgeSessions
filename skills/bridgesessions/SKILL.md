---
name: bridgesessions
description: >-
  Operate and develop BridgeSessions (bs) mesh terminal/file relay across
  Linux, macOS, and Windows. Use when running bs shell/file/health, fleet
  deploy, PowerShell $_ quoting, large file transfer with PROGRESS lines,
  Windows vs Linux commands, peer pin security, GitHub/Codeberg publish +
  dist/), or release hardening after the 2026-07 security audit. Do NOT use for
  raw SSH/SCP when a healthy bs mesh path exists. Do NOT confuse public Codeberg
  publish with a private Forgejo.
license: BUSL-1.1
compatibility: Requires bridgesessions CLI (bs) or build from this repo; OpenSSL; optional WinRM for Windows gameplay Session-1.
metadata:
  version: "26.08.12-beta4"
  product: BridgeSessions
  harnesses: "hermes,codex,claude-code,opencode,cursor,grok,copilot"
  related: "docs/RELEASE-PROVENANCE.md"
  release: "26.08.12-beta4"
  forge: "github.com/MindDragonLabs/BridgeSessions"
---

# BridgeSessions — Agent Skill

Portable skill for **Hermes**, **OpenAI Codex**, **Claude Code**, **OpenCode**,
**Cursor**, **Grok/xAI**, and other SKILL.md / AGENTS.md-aware harnesses.

**Repo root always-on context:** also read [`AGENTS.md`](../../AGENTS.md) when present.

## What this product is

- One C++ binary: mesh daemon + CLI (`bridgesessions` / `bs`).
- Replaces ad-hoc SSH + SCP + tmux + WinRM for **agent-native** shells and files.
- Default mesh port **19949**; CLI IPC **19980** (local).
- **Current version: `26.08.12-beta4`** (main branch on GitHub).
- Canonical shipping source: **`bs-protocol.h` + `main.cpp` + `bs-session.h`**
  with macOS capture in `macos-capture.mm`; CUA backends in `bs-cua-helper.h`.
- Always probe live: `bs --version` / `bridgesessions --version` (do not trust memory alone).
- **Features:** peer name resolution (4-tier fuzzy match), run-script (base64
  no-escaping), CUA (screen capture + input injection), direct TLS file transfer,
  startup network wait (Tailscale boot race fix).

## Non-negotiables

1. **bs first, always.** Never fall back to SSH/WinRM when bs fails. Never SSH
   into a peer with healthy bs mesh. SSH is ONLY for documented bootstrap
   scenarios (new peer install with no daemon yet). If `bs health` fails, run
   `bs peers list`, verify the peer name, retry with diagnostics, or prompt the
   user. The user decides the fallback — the agent does not silently switch
   transports.
2. **Pinned peers:** seeds need `pubkey=…`; `mesh.require_seed_pins` defaults true.
3. **CLI health** = data-plane (`healthy (data-plane ok)`). IPC HEALTH alone is not enough.
4. **Windows peers are Windows** — not Linux, not “MinGW ≈ Linux”.
5. **Stack commands** in one shell; do not open one `bs shell` per micro-step.
6. Credentials: never commit secrets; use env / operator vaults.
7. **Alpha posture:** public alpha is **not** a production-secure SSH replacement.
   See `SECURITY.md` and `docs/AUDIT-2.0.5-alpha2.md`.
8. **Primary forge = GitHub** (`MindDragonLabs/BridgeSessions`). Codeberg is a
   mirror. Do **not** invent a private Forgejo requirement for this product.
9. **Binaries ship in git `dist/`** and are published by **`git push` over SSH**.
   No API token is required to make binaries downloadable.

## Critical daemon and trust pitfalls

- Daemon mode is `bridgesessions --daemon` (normally with `--config <path>`). A positional `daemon` is parsed as a quick-connect peer name and produces the misleading error `Refusing untrusted first contact to daemon`.
- Trust has two layers: TLS `authorized_keys`, then Hello node-name/pubkey identity binding. If TLS accepts but the peer returns `hello_rejected`, inspect `~/.bridgesessions/bs-mesh.log`; `Hello node name is pinned to a different certificate key` means a stale seed/discovered-gossip pin, not an `authorized_keys` failure.
- After intentional key rotation, update the explicit `seed <node> ... pubkey=<new>` pin on every direct peer that must accept the rotated node. Restart daemons to clear in-memory discovered gossip. Explicit seed pins should be authoritative over discovered gossip; regression coverage is `authoritative seed pin overrides stale discovered key after rotation`.
- On macOS 26, FFmpeg AVFoundation screen capture can enumerate `Capture screen 0` yet produce no frames (`NSKVONotifying_AVCaptureScreenInput not linked into application`). Use native **ScreenCaptureKit inside the BridgeSessions process**, then let ffmpeg encode captured PNG frames. Linux `x11grab` and macOS AVFoundation are not valid fallbacks.
- `capture-video <peer>` must send `CuaVideoCaptureMsg` over a dedicated direct-TLS connection and wait for the matching `CuaVideoCaptureResultMsg`. A historical IPC handler merely checked that `<peer>` was connected and then called `video_capture_execute()` locally, silently capturing the operator machine instead of the target.
- macOS TCC approval is tied to the executable's code-signing requirement. Ad-hoc signing produces a CDHash requirement; every rebuilt binary must be removed and re-added under **Screen & System Audio Recording**, then its GUI LaunchAgent restarted. Toggling a stale row is insufficient.
- Before debugging capture, verify exactly one launchd job owns ports 19949/19980 and inspect the listener path with `lsof` + `ps`. Disable stale jobs pointing at old build trees; otherwise requests can hit an obsolete daemon even when `~/.local/bin/bridgesessions` is current.
- Verify end-to-end: capture remotely, `bs file recv ... --wait`, then inspect the retrieved MP4 with ffprobe (codec, dimensions, frame rate, duration, frame count).

## Runtime/resource invariants

- Exited sessions must release PTY masters, ConPTY pipe handles, and pseudo-console handles immediately. Verify leak fixes live by comparing daemon FD and `/dev/ptmx` counts before and after repeated finite one-shot shells.
- The generic reaper must defer `Attached` sessions to the PTY output poller; otherwise it can steal `waitpid()` before final output and `SessionDiedMsg` delivery.
- A remote build launched as a daemon-shell child dies when that daemon is restarted. On systemd hosts use a transient user unit (`systemd-run --user --no-block ...`); on macOS use a unique launchd one-shot and remove it after completion. Completion markers must be run-specific—`launchctl submit` can rerun jobs and overwrite logs.

## Public release (`v26.08.12-beta4`)

| Fact | Value |
|------|--------|
| Tag | `v26.08.12-beta4` (commit locally; public tag/push require operator approval) |
| Branch | `main` |
| Repo | https://github.com/MindDragonLabs/BridgeSessions |
| Artifacts | Linux x86_64, Windows x86_64 PE, macOS arm64 (`dist/`) |
| Tests | Linux 336/336 + macOS 335/335 CTest; release pytest 31/31; ASan/UBSan regression 22/22 |
| Notes | `docs/RELEASE-NOTES-26.08.12-beta4.md` · provenance `docs/RELEASE-PROVENANCE.md` |

### Download (raw from tag — preferred)

```text
https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-linux-x86_64
https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-windows-x86_64.exe
https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-macos-arm64
https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-26.08.12-beta4-source.tar.gz
https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/SHA256SUMS
```

Tree: https://github.com/MindDragonLabs/BridgeSessions/src/tag/v26.08.12-beta4/dist

```bash
curl -fL -o bridgesessions \
  https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-linux-x86_64
chmod +x bridgesessions
./bridgesessions --version   # → 26.08.12-beta4
```

### Build matrix (how the 3 platform binaries are produced)

All three are **portable static** (no runtime dylld/DLL deps beyond OS libs):

| Platform | Built on | Method |
|----------|----------|--------|
| Linux x86_64 | Linux host via `ubuntu:22.04` container | static OpenSSL/zstd/fmt/spdlog + `-static-libstdc++ -static-libgcc`; glibc kept dynamic (DNS). Dockerfile `/tmp/bs-static/Dockerfile` (see `bridgesessions-static-build` skill). |
| macOS arm64 | macOS host (Apple clang 17) | native build, static deps into `~/local`, `cmake -DCMAKE_OSX_ARCHITECTURES=arm64`. Links only system `libc++`/`libSystem`. |
| Windows x86_64 PE | Linux cross-compile (`x86_64-w64-mingw32-g++`) | static deps into `~/bs-win`; compile `main.cpp` directly with `-static` plus the `CLI/CLI.hpp` shim. PE imports only OS DLLs. |

- **Windows-only peers (no sshd)** — ship the PE via **WinRM** or `bs file send` from a mesh-connected host.
- mac/win release binaries ARE committed to `dist/` (the `.gitignore` only ignores the dev `bridgesessions.exe` + `*.o`/`*.obj`). `SHA256SUMS`/`SBOM` stay gitignored (downloader regenerates).
- Re-tag after changing `dist/`: `git tag -f v26.08.12-beta4 HEAD && git push --force origin main && git push --force origin v26.08.12-beta4`.

### Publish to Codeberg (SSH only)

On the operator host that has the **Mind-Dragon** Codeberg key:

```bash
# Use the operator deploy key configured for GitHub (and Codeberg mirror).
export GIT_SSH_COMMAND='ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes -o BatchMode=yes'

cd /path/to/BridgeSessions
git push origin main
git push origin v26.08.12-beta4
```

**Do not:**

- Use a private Forgejo for this public product release
- Block on `FORGEJO_TOKEN` / Codeberg “Releases” API for binary delivery
- Claim binaries missing if `dist/` on the tag is already pushed (raw URLs return 200)

Optional Codeberg “Releases” UI assets are cosmetics only; **git `dist/` is the source of truth**.

## Quick commands

```bash
export PATH="$HOME/.local/bin:$PATH"
bs --version                          # expect 26.08.12-beta4
bs health <peer>                      # must say healthy (data-plane ok)
bs fleet                              # name/addr/ver/status + cpu mem disk load os
bs fleet --json                       # full metrics JSON
bs shell <peer> --cmd '…'             # one-shot; exit code propagates
bs file send <peer> /local/path --wait
bs file send <peer> /local/path /remote/path --wait   # scp-style dest
bs file send <peer> ./script.ps1 --dest script.ps1 --wait   # under receive_dir
bs file recv <peer> /remote/path --to ./out --wait
bs run-script <peer> /local/script.sh # base64-encoded, no escaping issues
bs cua screen <peer>                  # get remote screen dimensions
bs cua capture <peer> -o screen.png   # remote screenshot
bs doctor
```

### Command stacking (required pattern)

```bash
# Windows cmd
bs shell windows-peer --cmd 'cmd /c "hostname && dir C:\temp && type C:\temp\a.txt"'

# Windows PowerShell (bash single-quotes protect $_)
bs shell windows-peer --cmd 'powershell -NoProfile -Command "Get-Process | Select-Object -First 3 | ForEach-Object { $_.ProcessName }"'

# Linux/macOS
bs shell linux-peer --cmd "bash -lc 'hostname && df -h && uptime'"
```

**Bad:** three separate `bs shell` calls for dependent steps.

### One-shot shell notes (Hermes / agents)

- Noninteractive `bs shell <peer> --cmd '…'` always uses **direct TLS** (daemon
  IPC returns `ERROR direct TLS required` by design — avoids SSL races on mesh
  conns). The line `Using direct TLS shell transport.` is expected, not a failure.
- One-shots now auto-use an ephemeral session name when `-n` is omitted (no more
  silent reattach to a live `default` shell that ignored `--cmd`). Health still
  uses unique `health-*` names.
- Overall wait is bounded (default 120s; `BS_SHELL_TIMEOUT_SEC=N`). Exit **124**
  means the remote session never ended in time — check peer version and reaper.
- Prefer `bs run-script` for multi-line / PowerShell-heavy work.

### run-script (eliminates escaping hell)

For complex scripts — especially on Windows — use `bs run-script` instead of
`--cmd`. It base64-encodes the script and decodes on the remote, eliminating
shell quoting issues entirely:

```bash
# POSIX script
bs run-script linux-peer /path/to/script.sh

# PowerShell script (auto-detected from .ps1 extension)
bs run-script windows-peer /path/to/script.ps1

# Stdin (heredoc)
bs run-script peer - << 'EOF'
echo "Hello from $(hostname)"
EOF

# Explicit interpreter
bs run-script peer script.py --interpreter python
```

### CUA (computer-use automation)

Remote desktop automation through the BS mesh:

```bash
bs cua screen <peer>                    # screen dimensions
bs cua capture <peer> -o shot.png       # screenshot
bs cua click <peer> --x 100 --y 200     # mouse click
bs cua move <peer> --x 100 --y 200      # mouse move
bs cua type <peer> --text "hello"       # type text
bs cua key <peer> --code 40             # HID key code (Enter)
bs cua scroll <peer> --direction down   # scroll wheel
```

On Windows/macOS, CUA requires the `--cua-helper` running in the user desktop
session. Install adds it automatically. Linux works via xdotool directly.

**Tray / menubar (B logo):** mesh must run as a **service** (launchd / systemd user /
Windows scheduled task), not a foreground Terminal window. The only desktop UI is
the tray/menubar applet (`BSMenubar.app`, `bs_tray.ps1`, `bs_tray.py`). Menus:
Fleet Status, Restart Daemon, Open Logs, Restart Helper (where applicable), Quit.
Windows: one Session‑1 helper only — dual helpers brick IPC auth (`ERROR: auth`).
See `docs/cua.md` tray section.

### Large files / flaky Wi‑Fi (resume + agent timeouts)

- `bs file send|recv` uses direct TLS with pipelined chunks + `FileAck.next_requested`.
- **Idle stall budget is 300s** (survives ~60s Wi‑Fi blackouts). Progress resets idle.
- **Direct-TLS send reconnects up to 12 times** after transport errors, resuming from
  the last acked chunk. Receiver keeps `.part` + `.part.bsmeta` for checksum-matched resume.
- Streams AI-parseable progress:

```text
PROGRESS phase=send|recv file=x.bin chunks=a/b bytes=c/d pct=P rate_mibs=R eta_sec=E
RESUME phase=send file=x.bin from_chunk=N/M
RETRY phase=send peer=P attempt=K from_chunk=N backoff_ms=…
```

#### Hermes / agent harness rules (required)

1. **Never** run multi‑MB `bs file send|recv --wait` with a short tool timeout (10–100s).
   Use **timeout ≥ 600s**, or `background=true` and poll PROGRESS / final OK line.
2. Exit **124** from the harness is *your* timeout, not necessarily a BS protocol failure.
3. On `ERROR send chunk` / `SSL_*` / `transfer idle`, **re-run the same command** — resume
   is automatic when the peer still has the matching `.part` sidecar.
4. Prefer `bs file send … --wait` over daemon fire-and-forget for reliability evidence.

- Streaming SHA-256 (no full-file RAM). Default `transfer.max_bytes` is large (8 GiB).
- `receive_dir` config option overrides received/ path (for SYSTEM daemons on Windows)

### Multi-step remote jobs (prefer over `cmd1 && cmd2 && cmd3`)

Stacked bash on `bs shell --cmd` fails the whole string on one error and is quoting-hostile
(PowerShell, nested quotes, Windows). Use **JSON jobs**:

```bash
# job.json
# {
#   "job_id": "playwright-probe",
#   "stop_on_error": false,
#   "steps": [
#     {"id": "find", "cmd": "cd /path/to/app && find . -maxdepth 2 -type f \\( -name 'playwright.config.*' -o -path './e2e/*' \\) -print"},
#     {"id": "head", "cmd": "sed -n '1,80p' /path/to/app/playwright.config.ts"}
#   ]
# }
bs job run <peer> job.json
```

Each step prints one JSON object (`exit`, `stdout`, `stderr`). Default continues after
non-zero exits; use `--stop-on-error` or per-step `"continue_on_error": false` to abort.

Prefer `argv` arrays when possible: `{"id":"u","argv":["uname","-a"]}`.

### NL→shell helper (optional hub: Linux mesh peers)

For natural-language command *generation* only (not multi-step orchestration), deploy
[whatisit-nl2sh](https://github.com/ThorOdinson246/whatisit-nl2sh) +
`ThorOdinson246/nl2sh-1.5b-Q4_K_M` via `scripts/nl2sh-hub-setup.sh`. Always execute
generated commands through `bs job` / `run-script`, never raw stacked `&&`.
### PowerShell `$_` / pipes

- v2.0.2+: powershell/pwsh skip broken `cmd /c` quote destruction.
- Still quote carefully: bash double-quotes expand `$_` — use single quotes or `\$_`.
- Prefer `powershell -NoProfile -Command "…"` or `bs run-script` for complex scripts.

## Peer naming

- Resolve peer names from the active config; do not guess aliases.
- **Never SSH into a peer with healthy bs mesh.** SSH is fallback-only for
  documented bootstrap scenarios (new peer install), not a co-equal option.
- If `bs health <name>` returns `Peer not found`, run `bs peers list` and prompt
  the user. Do NOT silently fall back to SSH/WinRM.
- Peer name aliases resolve via `bs peers list`. Use canonical names from that
  output in scripts. Common ambiguities (two peers sharing a prefix) should
  prompt the user rather than guessing.

- Require a pinned public key for every seed and direct command.
- Keep private fleet names and VPN addresses out of public skills and examples.

### Error handling: peer exists but is unreachable

| `bs health` result | Meaning | Agent action |
|--------------------|---------|--------------|
| `healthy (data-plane ok)` | Fully working | Proceed with `bs shell` / `bs file` |
| `unhealthy` | TLS+Hello OK, data plane broken | Check OpenSSL version match. Do NOT retry more than twice. |
| `refused` | TCP connect failed — daemon down | For SSH-capable hosts: restart daemon, then retry bs. For bs-only hosts: prompt user. |
| `tls_rejected` | TLS handshake failed — key mismatch | Peer's key changed (rotation/reinstall) or this is a different machine. Re-authorize: on the peer run `bs invite`, here run `bs accept <code>`. Or update `seed <name> <addr> pubkey=<new>` in config. |
| `hello_rejected` | Connected at TCP+TLS but peer rejected Hello | Version incompatibility or stale key pin. Check `bs ctl logs` on the peer. |
| `unknown peer` | No seed entry | Run `bs peers list`. Prompt user with available names. Do NOT guess. |
| Timeout (no response) | Network/Tailscale issue | Check `tailscale status`. One retry, then prompt user. |

**Golden rule:** Two identical failures = stop and prompt the user. Never loop
on the same diagnostic command. Never fall back to SSH/WinRM when a peer name
is not found or bs fails — the user decides the fallback.

## Windows vs Linux (read every time)

| Never on Windows | Use instead |
|------------------|-------------|
| Default `tar xvf` / apt / yum | `Expand-Archive`, `winget`, `msiexec` |
| `chmod +x`, shebang scripts | `.ps1` / `.cmd` / `.bat` |
| `/tmp`, `/home` | `%TEMP%`, `C:\Users\…` |
| Linux ELF as `.exe` | Native PE build / MinGW **only** for this project’s Windows binary |
| Treat MinGW as Linux userspace | MinGW builds PE; not apt/systemd/Linux ABI |

Gameplay GUI / desktop input: **WinRM Session-1** (or documented Session-1 helper). BS one-shots often run as **SYSTEM/Session 0**.

## Security posture (v26.08.12-beta4)

- Outbound mesh: pin ↔ TLS cert Ed25519 key ↔ Hello before trusting peer / `merge_peers`.
- Direct CLI: reject unpinned peers before DNS/TCP.
- File recv: basename sanitize + path containment; hash `.part` before publish.
- TLS: min 1.2, max 1.3 (prefer 1.3; not 1.3-only for fleet self-signed compatibility).
- Audit narrative: `docs/AUDIT-2.0.5-alpha2.md`
- Release evidence and checklist: `docs/RELEASE-PROVENANCE.md` and `CHANGELOG.md`

## Develop / build

```bash
# Linux
cmake -S . -B build && cmake --build build -j
./build/bridgesessions --version   # 26.08.12-beta4
ctest --test-dir build --output-on-failure
./build/test_config "[security]"

# Or one-shot:
./build.sh
```

Cross-platform build notes: `docs/building.md`.  
Release packaging: `scripts/package-release.sh` + `scripts/release-checksums.sh`.

Primary source: `bs-protocol.h` + `main.cpp` + `bs-session.h`; macOS capture: `macos-capture.mm`.

## Deploy (fleet sketch)

1. Never overwrite live `~/.bridgesessions/{config,authorized_keys}` with empty templates.
2. Order typical: build host → peers; Windows via PE binary (not Linux ELF).
3. After deploy: `--version`, `health <peer>`, `shell <peer> --cmd hostname`.
4. Seeds without `pubkey=` are skipped when `require_seed_pins` is true.

## Harness install map

| Harness | Discovery path |
|---------|----------------|
| **Claude Code** | `.claude/skills/bridgesessions/SKILL.md` |
| **OpenCode** | `.opencode/skills/bridgesessions/SKILL.md` |
| **Codex / AGENTS** | repo `AGENTS.md` (always-on) + optional skill dir |
| **Cursor** | skills under project / `.cursor` + `AGENTS.md` |
| **Hermes** | `~/.hermes/skills/**/bridgesessions*/SKILL.md` or `skill_view` |
| **Generic Agent Skills** | `.agents/skills/bridgesessions/SKILL.md` |

Canonical in-repo path: **`skills/bridgesessions/`**. Symlink others here.

```bash
# From repo root — recreate harness links
./scripts/install-agent-skill.sh
# Hermes (prefer symlink to this repo tree)
mkdir -p ~/.hermes/skills/devops
ln -sfn "$(pwd)/skills/bridgesessions" ~/.hermes/skills/devops/bridgesessions
```

## Progressive docs (load on demand)

- Release notes: `docs/RELEASE-NOTES-26.08.12-beta4.md`
- Provenance / checksums: `docs/RELEASE-PROVENANCE.md`
- Audit: `docs/AUDIT-2.0.5-alpha2.md`
- Usage and transfer workflows: `docs/usage.md`
- Cross-platform builds: `docs/building.md`
- Why BridgeSessions: `docs/why-bridge-sessions.md`
- Config: `docs/configuration.md`
- Protocol: `docs/protocol.md`
- Publish pitfalls: `references/codeberg-publish.md`

## Verification before claiming success

```bash
bs --version                          # 26.08.12-beta4
bs health <peer>                      # healthy (data-plane ok)
bs shell <peer> --cmd "…"             # real stdout, correct host
bs file send <peer> /tmp/big.bin --wait   # PROGRESS then OK
# Public tag contains the reviewed platform binaries:
curl -fsI https://github.com/MindDragonLabs/BridgeSessions/raw/main/dist/bridgesessions-linux-x86_64
```

Subagent claims are not evidence — re-probe in this session.
