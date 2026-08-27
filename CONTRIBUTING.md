# Contributing

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

See [Building](docs/building.md) for dependencies and sanitizers.

## Before a pull request

- Add regression coverage for behavior changes.
- Run `bash scripts/prepublish-scan.sh`.
- Format changed C++; run ShellCheck/Ruff where available.
- Update an existing document instead of adding one-off release/audit notes.
- Keep private hosts, addresses, credentials, machine paths, generated binaries, and local reports out of git.
- Keep one logical concern per PR.

## Autonomous builder

- Issues labeled `builder:todo` are claimed by the autonomous builder on fleet hardware (`fecv3`).
- Work happens in an isolated worktree, with builds and tests run locally before a pull request is opened.
- Pull requests include build and test evidence; GitHub CI checks are the merge authority.
- Issues that cannot be completed receive the `builder:failed` label and an explanatory comment.

## Systems rules

- Never block the mesh event loop.
- Treat `SSL*`, sockets, PTYs, handles, and temp files as single-owner resources.
- Bound remote lengths, queues, buffers, retries, and worker pools.
- Preserve mixed-version protocol behavior and test it.
- Use deadlines for network and child-process work.

Report vulnerabilities privately through [SECURITY.md](SECURITY.md).

Contributions use the license in [LICENSE](LICENSE).
