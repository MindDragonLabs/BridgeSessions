# E2E Framework

Unit tests prove local contracts. E2E lanes prove the real mesh, the real desktop session, the real installer, and the real platform packaging. The framework is layered so each lane runs only when it adds information beyond the lanes below it.

## Layers

| Layer | Scope | When to run |
|---|---|---|
| L0 | CTest and Python unit tests | every change |
| L1 | local loopback daemon plus client | protocol or transfer changes |
| L2 | live mesh: health, shell, files, scripts | release candidates |
| L3 | Windows, macOS, Linux desktop helper and tray | CUA changes |
| L4 | clean-profile installer and upgrade | release candidates |

L0 is the contract suite. L1 is the protocol suite. L2 is the mesh suite. L3 is the desktop suite. L4 is the installer suite. A change that touches more than one lane runs every lane it touches.

## Run

```bash
# Discover every healthy configured seed and run every lane it can
scripts/e2e-fleet-test.sh --all

# Explicit sanitized peer list
BS_E2E_PEERS="linux-peer,macos-peer,windows-peer" \
  scripts/e2e-fleet-test.sh --json /tmp/bs-l2.json

# Orchestrated desktop lanes
BS_E2E_PEERS="linux-peer,macos-peer,windows-peer" \
  python3 tests/e2e/runner.py --layers L2,L3 --json /tmp/bs-e2e.json
```

Pass live peer names through arguments or environment variables. Never hardcode a private fleet in the repository. The peer list is sanitized at the script boundary; the runner redacts operator paths, addresses, and hostnames from the JSON summary.

### `BS_E2E_PEERS`

`BS_E2E_PEERS` controls which live mesh peers the E2E runner targets. Set it to comma-separated peer names resolved from `bs peers list`.

```bash
BS_E2E_PEERS="linux-peer,macos-peer" scripts/e2e-fleet-test.sh
```

## Peer discovery

`scripts/e2e-fleet-test.sh --discover` walks the configured mesh and lists every peer that responds to `bs health`. Use the output to compose a `BS_E2E_PEERS` list. Discovery never opens a write path; it is read-only.

## Desktop prerequisites

- **Linux.** A disposable VM with a logged-in desktop and the platform's display and input tools available. The helper starts once per test run and stops after.
- **Windows.** An interactive user session, one CUA helper, and an Explorer desktop available for window-management tests.
- **macOS.** A logged-in user, the signed helper and app, and Screen Recording and Accessibility grants confirmed by a probe step before the lanes run.

Idempotent setup and probe helpers live under `tests/e2e/harness/`. They are test infrastructure, not installation entry points. Do not run them as installers on a real machine.

## What a run records

A complete run records the following evidence:

- exact local binary version (from `bs --version`),
- the peer-and-version matrix for the run,
- command exit codes and captured output,
- file checksum results for transfer lanes,
- screenshot and video metadata for desktop lanes,
- a JSON summary file with the structured outcome,
- skipped lanes with the reason for each skip.

E2E artifacts are local or CI outputs. They are not part of the repository. Add them to `.gitignore` if you produce them locally.

## Mixed-version coverage

Protocol and transfer changes must add mixed-version tests. The mixed-version matrix runs the new binary against the previous release on every supported platform. Same-version loopback is not enough; the framework insists on a real version difference.

## CUA coverage

CUA changes run the L3 lane on every supported platform. The lane covers:

- screen capture at the configured size and quality,
- pointer move and click at known coordinates,
- keyboard type and HID key press,
- scroll with bounded amount,
- video capture at a bounded duration.

Each step asserts the helper's return value and compares a captured frame against a stored baseline when the change affects rendering.

## Installer coverage

Installer and upgrade changes run L4 on disposable profiles. The lane covers:

- clean install on a fresh profile,
- upgrade on top of the previous release,
- a failure path that the installer must recover from,
- uninstall and residue check.

The release gate fails if a required failure becomes a skip. A lane that is genuinely impossible to run must say so explicitly in the run output; it must not silently vanish.

## Release gate

The release pipeline runs the gates in order:

1. L0.
2. L1 and L2 for protocol or transfer changes.
3. L3 on every affected platform for CUA changes.
4. L4 on disposable profiles for installer and upgrade changes.
5. The pre-publish scan from [Release provenance](RELEASE-PROVENANCE.md).

A failure at any gate stops the release. Fix product or harness failures; do not convert a required failure into a skip.

## Local development

A developer can run a subset of the framework against a local mesh:

```bash
# local loopback daemon plus client
python3 tests/e2e/runner.py --layers L0,L1

# single live peer
BS_E2E_PEERS=local-peer python3 tests/e2e/runner.py --layers L2
```

Local output goes to `tests/e2e/output/`. Inspect the JSON summary first; screenshots and video only when the lane failed.

## When the framework is wrong

- A test passes because the harness was modified to match the new behavior. Revert the harness and check whether the new behavior is correct.
- A test passes because a real failure was marked as expected. The framework never marks real failures as expected; a flaky lane is fixed, not silenced.
- A test passes only on one platform. The mesh is cross-platform; a platform-specific pass is a partial result, not a green build.
