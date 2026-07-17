# AGENTS.md — BridgeSessions

Always-on instructions for coding agents (Codex, Claude via symlink, Cursor, Hermes, OpenCode, Grok, etc.).  
Task-specific procedures: **`skills/bridgesessions/SKILL.md`** (Agent Skills open standard).

## Product

Mesh terminal + file relay. One C++23 binary (`bridgesessions` / `bs`). Monolith source: `bridgesessions.cpp`.  
Shipping candidate: **v2.0.5-alpha2**. Probe live `--version`.

## Agent rules

1. Prefer `bs` mesh over raw SSH/SCP when the peer is on the mesh.
2. Stack remote commands in **one** shell (`&&` / PowerShell `;`) — not N separate `bs shell` calls.
3. Windows peers: **Windows** commands only. No Linux-default `tar`/apt/`/tmp`. MinGW ≠ Linux.
4. Use the configured peer name and pinned key; never encode private fleet names or addresses in public instructions.
5. File transfers: `bs file send|recv … --wait`; parse `PROGRESS …` lines; do not assume 120s timeout (fixed in 2.0.4).
6. PowerShell: protect `$_` from bash (`'…'` or `\$_`); v2.0.2+ skips broken cmd wrapping for powershell.exe.
7. Do not claim “production-secure SSH replacement” until `docs/remediation-20260716/TODO-AUDIT-CLOSURE.md` P0 is fully `[x]`.
8. No secrets in git or chat. English-only operator-facing text unless the user asks otherwise.
9. Verify claims with real command output in this session.

## Key paths

| Path | Role |
|------|------|
| `bridgesessions.cpp` | Shipping monolith |
| `docs/remediation-20260716/` | Security + transfer remediation TODOs |
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

## Load the skill

If your harness supports progressive skills, load **`bridgesessions`** from `skills/bridgesessions/SKILL.md` (or the `.claude` / `.opencode` / `.agents` symlinks).  
Hermes: install/symlink under `~/.hermes/skills/` and `skill_view(name='bridgesessions')`.
