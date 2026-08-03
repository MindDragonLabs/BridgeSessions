# AGENTS.md — BridgeSessions

Always-on instructions for coding agents (Codex, Claude via symlink, Cursor, Hermes, OpenCode, Grok, etc.).  
Task-specific procedures: **`skills/bridgesessions/SKILL.md`** (Agent Skills open standard).

## Product

Mesh terminal + file relay. One C++23 binary (`bridgesessions` / `bs`). Shipping source: `main.cpp` + `bs-protocol.h` + `bs-session.h`; macOS capture backend: `macos-capture.mm`.
Shipping release: **v2.0.19-alpha7**. Probe live `--version`.

## Agent rules

1. Prefer `bs` mesh over raw SSH/SCP when the peer is on the mesh.
2. Stack remote commands in **one** shell (`&&` / PowerShell `;`) — not N separate `bs shell` calls.
3. Windows peers: **Windows** commands only. No Linux-default `tar`/apt/`/tmp`. MinGW ≠ Linux.
4. Use the configured peer name and pinned key; never encode private fleet names or addresses in public instructions.
5. File transfers: `bs file send|recv … --wait`; parse `PROGRESS …` lines; do not assume 120s timeout (fixed in 2.0.4).
6. PowerShell: protect `$_` from bash (`'…'` or `\$_`); v2.0.2+ skips broken cmd wrapping for powershell.exe.
7. Do not claim “production-secure SSH replacement” (public alpha posture; see SECURITY.md / AUDIT).
8. No secrets in git or chat. English-only operator-facing text unless the user asks otherwise.
9. Verify claims with real command output in this session.
10. **Public forge = Codeberg** (`Mind-Dragon/BridgeSessions`) via SSH deploy key. Binaries ship in git `dist/` (raw/tag URLs). Do **not** route this product through any other forge or require FORGEJO_TOKEN for binary delivery.

## Key paths

| Path | Role |
|------|------|
| `bridgesessions.cpp` | Shipping monolith |
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

## Load the skill

If your harness supports progressive skills, load **`bridgesessions`** from `skills/bridgesessions/SKILL.md` (or the `.claude` / `.opencode` / `.agents` symlinks).  
Hermes: install/symlink under `~/.hermes/skills/` and `skill_view(name='bridgesessions')`.
