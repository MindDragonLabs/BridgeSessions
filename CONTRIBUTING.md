# Contributing to BridgeSessions

Thanks for your interest in contributing. This document is deliberately short —
the goal is a low-friction, high-quality bar.

## Getting started

1. Fork and clone.
2. Build from source: see [docs/building.md](docs/building.md).
3. Run the tests: see the Testing section of [docs/building.md](docs/building.md).
4. Make your change on a feature branch.

## Before you open a PR

- **Build is green** with `./build.sh` (C++23).
- **Tests pass.**
- **Docs updated** if your change affects behavior, the CLI, or the protocol.
- **No secrets, no hardcoded private paths, no internal notes.** Never commit
  `*.pem`, `authorized_keys`, or machine-specific `C:\` paths.
- Keep PRs focused — one logical change per pull request.

## Code style

- C++23. Match the existing style in the file you edit.
- `clang-format` and `clang-tidy` configs are in the repo root; run them on
  changed code before submitting.

## Reporting bugs

Open an issue with:
- The exact command you ran.
- Expected vs. actual behavior.
- Your platform and `bridgesessions --version`.
- Relevant config (redacted of secrets).

## License

By contributing, you agree that your contributions are licensed under the
**Business Source License 1.1** (see [LICENSE](LICENSE)), which converts to
Apache-2.0 on 2030-07-16.
