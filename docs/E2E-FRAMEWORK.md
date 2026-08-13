# BridgeSessions E2E Framework

**Status:** active (run-on-completion, not nightly)  
**Version:** 26.08.10-beta2

## Decisions (operator, 2026-08-11)

| Topic | Decision |
|-------|----------|
| Windows desktop | Use **Windows Desktop** (Session 1 interactive). Hermes on remote may assist bootstrap; primary transport remains `bs` (+ WinRM only for Session-0 bootstrap). |
| Linux desktop | **KVM on a Linux lab hop** (not operator Mac — macOS has no KVM). Guest: Ubuntu 24.04 cloud + XFCE auto-login for 100% automated CUA/tray. |
| macOS | **existing macOS peer as-is** — no wipe. launchctl + existing TCC. |
| Schedule | **On completion of work effort**, not nightly CI. |

## Layers

| Layer | What | Hosts |
|-------|------|--------|
| L0 | Unit (`ctest`) | any |
| L1 | Local loopback e2e | build host |
| L2 | Mesh (health/shell/file/run-script) | live peers via `BS_E2E_PEERS` |
| L3 | Desktop GUI + CUA + tray/menubar | Linux KVM guest, Windows Session 1, macOS peer |
| L4 | Foreground installer (clean) | disposable KVM guest / Win user profile / macOS *secondary user only if added later* |

## Entry points

```bash
# L2 mesh only (existing)
scripts/e2e-fleet-test.sh --json /tmp/l2.json

# Full orchestrator (L2 + L3 setup + desktop suites)
python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/e2e.json

# Desktop-only after hosts ready
python3 tests/e2e/runner.py --layers L3 --json /tmp/l3.json
```

## Host matrix

| Role | Machine | Notes |
|------|---------|--------|
| Orchestrator | Operator Mac | runs `runner.py`, has Dev ID |
| Linux mesh | `linux-a`, `linux-b` (set `BS_E2E_PEERS`) | headless OK for L2 |
| Linux desktop QA | **KVM guest** (`bs-qa-ubuntu`) | XFCE auto-login, DISPLAY set |
| Windows desktop | **windows-peer** Session 1 | explorer running; cua-helper + tray must be user-session |
| macOS desktop | **macos-peer** | BridgeSessions.app, BSMenubar.app, cua-helper launchd |

## Hermes remote support (Windows)

Hermes on the Windows desktop peer can:

1. Ensure interactive logon tasks for `--cua-helper` and `bs_tray.ps1`
2. Watch helper health and re-run setup scripts under user context
3. Not replace mesh tests — still validate with operator `bs cua …`

Bootstrap scripts (idempotent) live in `tests/e2e/harness/`:

- `win_desktop_setup.ps1` — schtasks for CUA helper + tray at logon; start now if Session 1
- `linux_kvm_setup.sh` — define/start `bs-qa-ubuntu` on the Linux lab hop
- `mac_desktop_probe.sh` — menubar/cua-helper process checks (no wipe)

## Automation constraints

| Feature | Fully automated? | Method |
|---------|------------------|--------|
| Mesh L2 | Yes | `bs` CLI |
| Linux CUA | Yes on KVM guest | auto-login XFCE + `bs cua` from orchestrator after peer join |
| Windows CUA | Yes if Session 1 alive | schtasks `/IT` interactive + mesh `bs cua` |
| Linux tray | Yes on KVM guest | start `bs_tray.py`, assert process + optional DBus |
| Windows tray | Yes if Session 1 | start `bs_tray.ps1`, assert process |
| macOS menubar | Mostly | process + launchctl + codesign; menu pixel optional |
| Installer L4 | Yes on clean KVM / disposable Win profile | not on a production macOS wipe |

## Artifacts

- JSON summary (`--json`)
- Optional screenshots under `artifacts/e2e/<run-id>/`
- Exit non-zero if any **required** suite fails

## Run-on-completion checklist

1. `python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/bs-e2e.json`
2. Review fails; fix product or harness
3. Re-run until green before claiming full feature QA
