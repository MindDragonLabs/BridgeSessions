---
name: bridgesessions
description: Use when operating or developing BridgeSessions mesh peers.
license: BUSL-1.1
metadata:
  version: "26.09.19-beta6"
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

## Computer use

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
bs cua type <peer> --text 'hello'
```

Capture before clicking. Windows/macOS require one user-session `--cua-helper`; macOS also requires Screen Recording and Accessibility approval. Spectators cannot send CUA input.

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
- `bs-protocol.h` — TLS, codec, mesh, transfers, IPC,
- `bs-session.h` — session lifetime,
- `bs-session-worker.h` — optional worker,
- `bs-cua-helper.h` / `macos-capture.mm` — desktop support.

Generated artifacts are ignored and published through GitHub Releases. Release gate: [`docs/RELEASE-PROVENANCE.md`](../../docs/RELEASE-PROVENANCE.md).

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
