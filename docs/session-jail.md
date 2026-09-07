# Session Filesystem Jail

Every session shell (the PTY child a peer spawns for `bs shell` / `bs <peer>`)
runs under a filesystem jail with this policy:

> **Read everywhere. Write only in your working roots.**

- **Read** access is unrestricted — an agent working in a session may look at
  `/etc/os-release`, `/usr/include`, anything it needs *to know where things
  are*.
- **Write** access is confined to the allowed roots:
  - `$HOME`
  - the daemon's working directory (the operator's project root)
  - `/tmp` (POSIX) / `%TEMP%` (Windows)
  - anything listed explicitly in `BS_JAIL_RW="path1:path2:~/notes"`
    (colon-separated, `~` expanded against `$HOME`)
- **Explicit grants only:** servers (Windows / Linux) get write access to a
  location only when the operator names it. macOS: sessions never touch
  anything that triggers a TCC privacy prompt (Desktop/Documents/Downloads/
  screen/mic beyond the roots above). Windows desktops: same idea — no
  protected-path writes.

## Enforcement

| Platform | Mechanism | Notes |
|----------|-----------|-------|
| Linux ≥ 5.13 | **Landlock** (unprivileged LSM, `PR_SET_NO_NEW_PRIVS`) | Write-deny ruleset + path-beneath grants for each root. Applied in the `forkpty` child before `exec`, so the shell and everything it launches inherits the jail. |
| Linux (older kernels) | degrade to env markers only | A session **never fails to start** because the jail cannot be enforced. |
| macOS / Windows | env markers only (`BS_JAIL=1`, `BS_JAIL_RW=…`) | No kernel jail. Tooling in sessions is expected to honor the markers; document violations, don't mask them. |

## Opt-out

`BS_JAIL=0` (in the daemon's environment) disables the jail. This exists for
debugging, not as a mode.

## Implementation map

- `bs-jail-policy.h` — policy model: `FilesystemJailPolicy`,
  `jail_policy_from_env(home, cwd)`, `BS_JAIL_RW` parsing. Included inside
  `namespace bs::mesh`, before `bs-pty.h`.
- `bs-jail.h` — Landlock enforcement half: `landlock_available()`,
  `apply_filesystem_jail(policy)`. Included at global scope after the facade.
- `bs-pty.h` `create_session()` — calls both in the child, best-effort,
  immediately before `exec /bin/sh -c <command>`.
- `tests/test_jail.cpp` — policy unit tests + a live fork/Landlock
  enforcement test (write inside root allowed, `/var/tmp` denied, read of
  `/etc/hostname` allowed).

The policy is **declarative**: roots stay in the list even if the path does
not exist right now. A root that cannot be opened at enforcement time simply
grants nothing — a typoed `BS_JAIL_RW` entry can never silently broaden the
jail.
