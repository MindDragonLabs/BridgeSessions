# Security Policy

## Supported version

`26.09.19-beta6` is the current beta line. Upgrade older builds before reporting unless reproducing a regression.

## Report privately

Do not open a public issue with vulnerability details. Use:

<https://github.com/MindDragonLabs/BridgeSessions/security/advisories/new>

Include impact, reproduction, and affected versions. Remove secrets, private hosts, addresses, and personal paths.

## Trust boundary

BridgeSessions is for operator-controlled peer meshes, not hostile multi-tenant access.

- A key in `authorized_keys` has near-interactive host access.
- Seed/direct connections require explicit Ed25519 pins.
- Certificate key, Hello key/name, and configured pin must agree.
- Local daemon IPC is loopback-only and token-authenticated.
- Compromise of an authorized key can compromise hosts that trust it.

## Join and transport

Mesh traffic uses mutual TLS over TCP/19949. The current compatibility profile negotiates TLS 1.2. A bounded invite window temporarily accepts an unknown certificate only to submit a single-use random token. Only pinned seeds may issue accepted mesh-wide enrollments.

## Files

- filenames and destinations are validated and canonicalized,
- remote serving is confined to `receive_dir`, including symlink resolution,
- received data uses `.part` plus SHA-256 before atomic publish,
- identity/token/config/PEM paths are denied by default,
- remote errors do not expose local absolute paths.

`transfer.allow_sensitive_paths` deliberately removes major safeguards; treat it as arbitrary file authority.

## CUA and Bridge Panel

Spectators cannot send CUA input. Windows/macOS helpers are loopback-only and token-authenticated. Bridge Panel binds loopback by default and writes require a bearer token. Do not expose it directly to the internet.

## Release integrity

Installers require a matching GitHub Release `SHA256SUMS` entry and embedded version before replacement. Release binaries are not committed. macOS artifacts must be Developer ID signed/notarized; builders use supported dependency lines.
