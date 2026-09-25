# Goal — BridgeSessions 26.09.25

## Result

Ship a clean `26.09.25` beta that is safe to publish. The tree is already stamped `26.09.25` on local `main` at commit `99afdf3`. That commit is not pushed. GitHub still serves `v26.09.24-a1`.

This file is the goal for the beta work. The task list is `TODO-2026-09-25.md`. The file-level plan is `docs/plans/26.09.25-beta.md`.

## Done means

1. The menu-bar B icon is centered on both axes. The app icon, tray icon, and installer icon are set on macOS, Windows, and Linux.
2. A future GitHub release has a changelog, a README status block, green CI, current docs, and a matching site at `/home/work/bridgesessions-site`. An alpha tag is a pre-release, not Latest.
3. Bridge Panel works in a PC browser and in a phone-width browser.
4. E2e lives in this repo. A small file copy finishes in under 2 seconds. A short command returns in under 2 seconds. Two sessions do not mix output. Live sessions are not killed by the idle reaper. The shown name is the harness name.
5. The panel has a Sessions tab for the selected host. It shows harness sessions. It hides cron jobs and agent jobs.
6. Auto-update has one origin, verifies the published hash before the swap, restores the old binary on failure, and never retries faster than 10 seconds.
7. No production path retries every second. Devin `swe-2` has reviewed Linux, macOS, and Windows. Each finding is fixed or declined in the TODO with a reason.

## Hard limits

- Do not push `main`. The installer default tag is `26.09.25`. That tag is not on GitHub. A push breaks the public installer.
- Do not roll the fleet. The running Windows hash is `AE2B61A6`. The published Windows hash is `684d4149`. Trace that hash before any roll.
- Windows proof uses WinRM. Do not use `bs shell` as the Windows proof. The Shadow PC is `SHADOW-OLNM5J3N` at `100.127.41.92`.
- Do not add a 1-second reconnect, upgrade, or ping loop. A test wait must stay inside the test.
- Do not restore `._*` files or `*.bak` files from the pre-clean stash. Review `auth.py` and `invites.py` in that stash before any panel copy.
- A subagent report is not proof. The parent re-checks the file, the test, or the host.

## Subagent plan

Do not use `delegate_task` for this beta. That lane is out of quota until 2026-09-27. Use `cli-sub`. Load skill `subagents-cli` before a dispatch. Do not invent CLI flags.

The parent writes each brief to a file. Each brief names the repo path `/home/agent/bridgesessions`, the commit `99afdf3`, the write set, and the done check. Workers do not see this chat.

### Wave 1 — read only, run together

| Lane | Command | Write set | Job |
|---|---|---|---|
| Icons | `cli-sub grok` | none | Read `BSMenubar/StatusItemController.swift`, `scripts/bs_tray.ps1`, and `scripts/bs_tray.py`. Propose the center fix and the icon file list. |
| Loops | `cli-sub codex` | none | Find `sleep 1` and 1-second retries. Mark each hit as a test wait or a live retry. |
| Panel | `cli-sub claude` | none | Review `tools/bridgepanel/` for a phone layout. Do not edit. |

### Wave 2 — one writer per file set

Start only after wave 1 briefs are checked.

| Lane | Write set | Job |
|---|---|---|
| Icons | `BSMenubar/`, tray scripts, icon assets | Center the B mark. Set the icon on every package path. |
| Panel | `tools/bridgepanel/` | Fix PC and phone layout. Review the stash before copying old panel files. |
| E2e | `scripts/e2e-fleet-test.sh`, `tests/e2e/` | Add time limits for file copy and typing. Do not run the fleet matrix until a `26.09.25` binary exists. |
| Update | upgrade and reconnect code | Wait until the loop list is accepted. One origin. Verify the published hash. Back off at 60 seconds, then 5 minutes. Never 1 second. |

Two lanes must not edit the same file. Auto-update and the loop fix share the reconnect path. One lane owns that path.

### Wave 3 — after the edits compile

| Lane | Job |
|---|---|
| Sessions tab | Add the host Sessions tab. Hide cron jobs and agent jobs. Install it on one Linux panel host. |
| Release docs | Update `README.md`, `CHANGELOG.md`, `docs/`, and `/home/work/bridgesessions-site` together. |
| Devin | Run Devin CLI `swe-2` last. Linux and macOS run on their own hosts. Windows runs on the Shadow PC through WinRM. Load the verified Devin command first. Do not guess flags. |

## Not this goal

- Do not publish `v26.09.25`.
- Do not start sven-bot. Its hypervisor is still unknown.
- Do not log in to `devin-bs-mac` until an SSH user and key are named.
- Do not commit this file unless the operator asks.
