# E2E Framework

Unit tests prove local contracts. E2E lanes prove the real mesh, desktop session, installer, and platform packaging.

## Layers

| Layer | Scope | Default |
|---|---|---|
| L0 | CTest and Python tests | every change |
| L1 | local loopback daemon/client | protocol changes |
| L2 | live mesh: health, shell, files, scripts | release candidates |
| L3 | Windows/macOS/Linux desktop helper and tray | CUA changes |
| L4 | clean-profile installer and upgrade | release candidates |

## Run

```bash
# Discover every healthy configured seed
scripts/e2e-fleet-test.sh --all

# Explicit sanitized peer list
BS_E2E_PEERS="linux-peer,macos-peer,windows-peer" \
  scripts/e2e-fleet-test.sh --json /tmp/bs-l2.json

# Orchestrated desktop lanes
BS_E2E_PEERS="linux-peer,macos-peer,windows-peer" \
  python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/bs-e2e.json
```

Pass live peer names through arguments/environment variables. Never hardcode a private fleet in the repository.

## Desktop prerequisites

- **Linux:** disposable VM with a logged-in desktop and display/input tools.
- **Windows:** interactive user session, one CUA helper, Explorer desktop.
- **macOS:** logged-in user, signed helper/app, Screen Recording and Accessibility grants.

Idempotent setup/probe helpers live under `tests/e2e/harness/`. They are test infrastructure, not installation entry points.

## Evidence

A complete run records:

- exact local binary version,
- peer/version matrix,
- command exit codes and output,
- file checksum results,
- screenshots/video metadata for desktop lanes,
- JSON summary,
- skipped lanes with reasons.

E2E artifacts are local/CI outputs and must not be committed.

## Release gate

1. Run L0.
2. Run L1/L2 for protocol or transfer changes.
3. Run affected L3 platforms for CUA changes.
4. Run L4 on disposable profiles for installer/upgrade changes.
5. Fix product or harness failures; do not convert a required failure into a skip.
