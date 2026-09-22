# Reliability loop

`scripts/reliability_loop.py` is the repository's repeatable verification
coordinator. It builds once, runs independent lanes concurrently, and writes
one log and JSON result per lane under `artifacts/reliability/`.

It does not edit source, merge changes, or retry deterministic failures.

## Local run

```sh
python3 scripts/reliability_loop.py --iterations 3 --sleep-seconds 10
```

Useful slices:

```sh
python3 scripts/reliability_loop.py --no-python
python3 scripts/reliability_loop.py --no-panel
python3 scripts/reliability_loop.py --no-build --build-dir build-audit-1790057366
```

The command exits nonzero on a configure, build, CTest, or Python failure.
`summary.json` is the handoff artifact for CI or a remote worker.

## Atomic task contract

Workers should receive one bounded hypothesis, not a request to “improve the
app”. A task must declare:

```yaml
id: session-reconnect-001
owner: session-lifecycle
hypothesis: reconnect after peer restart preserves the requested session
scope: [bs-session-registry.h, tests/test_session_registry.cpp]
commands:
  - ctest --test-dir build-reliability -R session_registry --output-on-failure
acceptance:
  - session remains independently addressable
  - no orphaned child process
  - unrelated CTest lanes remain green
artifacts: [stdout.log, stderr.log, summary.json, git.diff]
```

One worker gets one worktree. Workers may run in parallel when their scopes do
not overlap. Versioning, protocol changes, CMake, release packaging, and the
final merge remain single-owner operations.

## Scaling plan

The first useful scale-out is CPU-only:

1. local macOS: fast edit/build and macOS terminal behavior;
2. Linux x86: authoritative CTest and two-node mesh;
3. Linux ARM: architecture coverage;
4. Windows: ConPTY, PowerShell, installer, and service behavior;
5. disposable Linux nodes: 3–5-node partitions, restart, loss, and recovery.

Cloud workers should receive a clean checkout, an ephemeral identity, a task
manifest, and an artifact upload location. They should be destroyed after the
run. Never put signing keys or production trust material on them.

GPUs are not required for the current reliability loop. Add them only if the
product later introduces an ML workload; for BridgeSessions, CPU, network
quality, OS diversity, and controllable failure injection are the scarce
resources.
