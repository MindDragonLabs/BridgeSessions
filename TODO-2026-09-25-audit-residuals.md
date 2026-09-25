# TODO — 2026-09-25 audit residuals

Wave-3 Devin audit (swe-2) flagged 12 items against commit 2c97305. Fixes landed in d0404f1 (panel syntax), in-progress here (auto-update, watchdog, e2e gaps), and known-deferred.

## Fixed in d0404f1 / this commit
- 1. BLOCKER panel JS parse — frozenset→Set, apiCall→api("/api/machines"), files col reachable on phone (row layout)
- 2. Linux detach BS_SESSION recursion — env -u BS_SESSION -u BS_SESSION_ID before setsid
- 3. Windows watchdog liveness — curl exit != 7 to mean alive (was 1; wrong because TLS-port probe always exits non-zero)
- 6. transfer-resume fast backoff — documented as in-flight CLI exception (not a production loop)
- 7. auto-upgrade staged backoff — 60s → 300s → configured-cooldown, attempts counter (success-reset TODO)

## Deferred (operator-level decision required)
- 4. panel sessions-tab integration — wiring `refreshHostSessions` to `selectHost` across all paths; needs `server.py` endpoint work + integration test for harness names
- 5. panel files column on phones — fixed layout, but the file picker UX is row-layout and could be improved (toggle, sheet, etc.)
- 8. auto-upgrade platform dispatch — needs PeerEntry.platform field (schema change, not 26.09.25 scope)
- 9. e2e gates: session isolation, idle reaper survival, harness name assertion, byte-count check
- 10. .bak file in tree (scripts/e2e-fleet-test.sh.bak-r2harness)
- 11. POSIX rename return ignored in main.cpp:3606
- 12. Dead `selectedHost()` and "content table wrapper" claim accuracy

## Hard limits still in effect
- Do not push `main` (installer tag is 26.09.25; not on GitHub)
- Do not publish `v26.09.25`
- Do not roll the fleet
- Windows binary hash mismatch (running `AE2B61A6` vs published `684d4149`) — trace before any roll
