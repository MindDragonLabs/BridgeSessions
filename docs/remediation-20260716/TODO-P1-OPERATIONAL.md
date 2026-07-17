# TODO — P1 Operational Correctness (Fleet-found, post-review)

**Status:** PARTIAL  
**Source:** Independent review + live test-pc1/test-pc2/test-pc7 operations (2026-07-16…17)  
These flaws were **not all in the written review** but would block real GoatBall / agent use of BridgeSessions.

---

## P1-OP-1 — PowerShell `$_` / nested quotes destroyed by `cmd /c` wrap

**Finding:** ClientOverride always wrapped as `cmd /c "…"`, breaking nested quotes so `|` became a cmd pipe; `ForEach-Object` / `$_` never reached PowerShell.

**Remediation status:**
- [x] **P1-OP-1a:** Direct-exec path for `powershell`/`pwsh`/`*.exe` (skip cmd wrap when resolvable)
- [x] **P1-OP-1b:** `is_windows_cli_oneshot_command` so PS `-Command` still uses anonymous pipes (not ConPTY)
- [x] **P1-OP-1c:** Unit tests `[windows_cmd]`
- [x] **P1-OP-1d:** Deploy candidate **v2.0.2** binary to test-pc7; live `$_.ProcessName` pipeline PASS
- [ ] **P1-OP-1e:** Tag/release v2.0.2 (or fold into next security release) + fleet deploy test-pc1/test-pc2/test-pc5
- [ ] **P1-OP-1f:** CTest e2e (Windows) for `ForEach-Object { $_ }` join + process list
- [ ] **P1-OP-1g:** Skill/docs: single-quote bash `--cmd` pattern (done in skills; mirror in REMOTE-OPS-GUIDE)

**Done when:** Tagged release + Windows CI test; all fleet nodes report same version.

---

## P1-OP-2 — Peer name confusion (`shadow` vs `test-pc7`)

**Finding:** Two live Windows peers; GoatBall harness/docs used peer `shadow` (wrong host). Mesh healthy ≠ correct target.

**Remediation status:**
- [x] **P1-OP-2a:** Harness `bridgesession.py` default peer `test-pc7` + binary resolve PATH/2.x
- [x] **P1-OP-2b:** test-pc2 retired seed line for legacy `shadow`; GOATBALL_PEER note
- [x] **P1-OP-2c:** Align test-pc2 `build/bridgesessions` to 2.0.1+ (was 1.8.3)
- [ ] **P1-OP-2d:** Repo-wide grep gate: fail CI if goatball docs say `shell shadow` without `test-pc7`
- [ ] **P1-OP-2e:** `bs doctor` warns if both `shadow` and `test-pc7` seeds present without annotation

---

## P1-OP-3 — CLI health vs IPC HEALTH confusion

**Finding:** Operators treat control-plane HEALTH as data-plane OK (fixed in docs for 2.0.1; still needs doc consistency).

### Tasks
- [ ] **P1-OP-3a:** Doctor output prints both: `mesh_peer=…` and `data_plane=…`
- [ ] **P1-OP-3b:** README table already exists — ensure INSTALL/REMOTE-OPS match

---

## P1-OP-4 — Session 0 / SYSTEM oneshot vs interactive desktop

**Finding:** BS one-shots run as SYSTEM; Roblox/gameplay needs Session 1. WinRM interactive tasks remain canonical for GoatBall waves.

### Tasks
- [ ] **P1-OP-4a:** Document split: BS = shell/file; WinRM/Session-1 runner = gameplay (skill done; repo docs)
- [ ] **P1-OP-4b:** Optional: named profile `session.gameplay` that documents PsExec path (no false “BS replaces WinRM” claim)

---

## Exit criteria
- [ ] OP-1e release + fleet version matrix
- [ ] OP-2d grep gate in goatball or bridgesessions CI
- [ ] No production doc claims BS handles interactive desktop input without Session-1 helper
