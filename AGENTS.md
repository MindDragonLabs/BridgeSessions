# AGENTS.md — BridgeSessions

Always-on instructions for coding agents (Codex, Claude via symlink, Cursor, Hermes, OpenCode, Grok, etc.).  
Task-specific procedures: **`skills/bridgesessions/SKILL.md`** (Agent Skills open standard).

## Product

Mesh terminal + file relay. One C++23 binary (`bridgesessions` / `bs`). Shipping source: `main.cpp` + `bs-protocol.h` + `bs-session.h`; macOS capture backend: `macos-capture.mm`; Windows/macOS CUA helper: `bs-cua-helper.h`.
Shipping release: **v26.09.19-beta5**. Probe live `--version`.

## Agent rules

1. Prefer `bs` mesh over raw SSH/SCP when the peer is on the mesh.
2. Stack remote commands in **one** shell (`&&` / PowerShell `;`) — not N separate `bs shell` calls. For anything beyond 2-3 chained commands, or multi-line/PowerShell-heavy work, use `bs run-script` instead of `--cmd` chains. One `bs shell <peer> --cmd` per micro-step is an anti-pattern: each round-trip costs a full mesh RTT (100-500ms+ on WAN peers), so 5 sequential single-command calls burn 5x the latency of one stacked call for no benefit.
   - **Do:** `bs shell peer --cmd "bash -lc 'cd /app && npm install && npm test'"` or `bs run-script peer deploy.sh` for anything longer.
   - **Not that:** `bs shell peer --cmd "cd /app"` then `bs shell peer --cmd "npm install"` then `bs shell peer --cmd "npm test"` (3 round-trips, and `cd` doesn't even persist across calls).
3. Windows peers: **Windows** commands only. No Linux-default `tar`/apt/`/tmp`. MinGW ≠ Linux.
4. Use the configured peer name and pinned key; never encode private fleet names or addresses in public instructions.
5. File transfers: `bs file send <peer> <local> [<remote>|--dest <remote>] --wait` (scp-style remote dest); `bs file recv … --wait`. Parse `PROGRESS …` lines; do not assume 120s timeout (fixed in 2.0.4). Pipelined since 2.0.20-alpha9 (8-chunk batching). `file recv` and `capture-video` use direct TLS to the target peer.
5b. Fleet: `bs fleet` shows name/addr/version/status/uptime plus **cpu/mem/disk/load/os** (peer metrics after gossip; use `bs fleet --json` for full fields).
6. PowerShell: protect `$_` from bash (`'…'` or `\$_`); v2.0.2+ skips broken cmd wrapping for powershell.exe.
7. Do not claim "production-secure SSH replacement" (public beta posture; see SECURITY.md / AUDIT).
8. No secrets in git or chat. English-only operator-facing text unless the user asks otherwise.
9. Verify claims with real command output in this session.
10. **Primary forge = GitHub** (`MindDragonLabs/BridgeSessions`). Generated binaries, checksums, and SBOMs are GitHub Release assets and are never committed.
11. **CUA automation** (`bs cua …`): 7 subcommands — `screen`, `capture`, `click`, `move`, `type`, `key`, `scroll`. Windows/macOS peers require `--cua-helper` running in the user session. Always capture a screenshot before clicking blind. HID key codes are USB usage IDs (see [docs/cua.md](docs/cua.md)). Use POSIX `sq()` quoting — no shell injection.
12. **Run-script** (`bs run-script <peer> <file>`): auto-detects interpreter (bash/powershell/python) from extension/shebang; supports `--interpreter`. Script body is base64-encoded — no escaping issues. Use `-` for stdin. Prefer `run-script` over multi-line `shell --cmd` for complex remote scripts.
13. **Peer resolution:** 4-tier fuzzy matching (exact → suffix/prefix → Levenshtein ≤ 2). Ambiguous matches return suggestions — do not guess. Use canonical resolved names from `bs peers list` in scripts. Tier-2 (config aliases) is reserved, not yet implemented.

## Key paths

| Path | Role |
|------|------|
| `main.cpp` + `bs-protocol.h` | Shipping monolith |
| `bs-cua-helper.h` | CUA helper (Windows/macOS user-session input + capture) |
| `macos-capture.mm` | macOS ScreenCaptureKit backend |
| `CHANGELOG.md` / `docs/RELEASE-PROVENANCE.md` | Public release evidence and provenance |
| `skills/bridgesessions/SKILL.md` | Portable skill for multi-harness |
| `dist/` | Prebuilt binaries (verify checksums when releasing) |
| `tests/` | Catch2 + Python panel tests |

## Build / test

```bash
cmake -S . -B build && cmake --build build -j
./build/bridgesessions --version
./build/test_config "[security]"
ctest --test-dir build --output-on-failure
```

### Fleet e2e (live mesh)

Cross-platform feature matrix (health, shell, file send/recv, run-script, medium transfer, optional CUA) against live peers:

```bash
# Pass live peer names via args or BS_E2E_PEERS — do not hardcode a private fleet.
scripts/e2e-fleet-test.sh --all
scripts/e2e-fleet-test.sh --json /tmp/bs-e2e.json --all
scripts/e2e-fleet-test.sh --quick linux-peer     # health+shell only
BS_E2E_PEERS="linux-a,linux-b,macos-peer,windows-peer" scripts/e2e-fleet-test.sh
BS_E2E_SKIP_CUA=1 scripts/e2e-fleet-test.sh --all
```

Requires a working local `bs` on PATH and reachable mesh peers. Exit 0 only if all required tests pass.

**Desktop / CUA / tray / menubar / installer** (L3–L4): see **`docs/E2E-FRAMEWORK.md`** and:

```bash
BS_E2E_PEERS="linux-a,linux-b,macos-peer,windows-peer" \
  python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/bs-e2e.json
# Windows Session-1 helper (interactive user):
#   tests/e2e/harness/win_desktop_setup.ps1 + win_cua_fix_auth.ps1
# Linux desktop KVM guest: tests/e2e/harness/linux_kvm_setup.sh
# macOS desktop (no wipe): tests/e2e/harness/mac_desktop_probe.sh
```

Run on **work completion**, not nightly.


## Load the skill

If your harness supports progressive skills, load **`bridgesessions`** from `skills/bridgesessions/SKILL.md` (or the `.claude` / `.opencode` / `.agents` symlinks).  
Hermes: install/symlink under `~/.hermes/skills/` and `skill_view(name='bridgesessions')`.
