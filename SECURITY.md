# Security Policy

## Supported version

`26.08.26-r1` is the current beta line. The repository stamps the same version in `VERSION`, in the installer, and in the binary itself. Upgrade older builds before reporting unless you are reproducing a regression that requires the older code.

The previous beta line is `2026.08.24-beta7`. Mixed beta lines on the same mesh still talk to each other through the protocol compatibility profile; mixed major protocol lines may not. The release pipeline enforces version negotiation at handshake time.

## Report privately

Do not open a public issue with vulnerability details. Use the GitHub private-advisory form:

<https://github.com/MindDragonLabs/BridgeSessions/security/advisories/new>

Include the impact, the reproduction steps, and the affected versions. Strip secrets, private hostnames, addresses, and personal paths from the report before you send it. A reproduction that needs a private network can use a synthetic seed and joiner on the reporter's own infrastructure; do not include real operator topology.

The maintainers triage private reports within a few business days. A confirmed vulnerability gets a coordinated disclosure window with a fix release and a release note that names the issue class without naming the reporter.

## Trust boundary

BridgeSessions is for operator-controlled peer meshes. It is not a hostile multi-tenant access service.

- A key in `authorized_keys` has near-interactive host access.
- Seed and direct connections require explicit Ed25519 pins.
- Certificate key, Hello key, and configured pin must agree before the mesh promotes a transport to live.
- Local daemon IPC is loopback-only and token-authenticated. The token is owner-readable only.
- Compromise of an authorized key can compromise every host that trusts it. Treat peer authorization with the same care as root credentials.

## Join and transport

Mesh traffic uses mutual TLS over TCP `19949`. The current compatibility profile negotiates TLS 1.2. A bounded invite window temporarily accepts an unknown certificate only to submit a single-use random token. Only pinned seeds may issue accepted mesh-wide enrollments. After the join, the new node's key is gossiped to every peer; every peer auto-trusts the new node without a roster edit.

`mesh.require_seed_pins true` is the safe default. An unpinned seed is a path to silent re-keying and silent denial of service; do not disable it on an untrusted network.

## Files

- Filenames and destinations are validated and canonicalized.
- Remote serving is confined to `receive_dir`, including symlink resolution.
- Received data uses `.part` plus SHA-256 before atomic publish.
- Identity, token, config, and PEM paths are denied by default in listings.
- Remote errors do not expose local absolute paths.

`transfer.allow_sensitive_paths` deliberately removes major safeguards. Treat it as arbitrary file authority. The Bridge Panel uses the same confinement for its own listings and writes.

## CUA and Bridge Panel

Spectators cannot send CUA input or trigger desktop capture. Windows and macOS CUA helpers are loopback-only and token-authenticated; the helper is one process per interactive user session. Bridge Panel binds loopback by default. Writes need a bearer token unless the client IP is in the panel's trust list. Do not expose the panel directly to the internet. Do not run it as a more privileged user than you need.

## Release integrity

Installers require a matching GitHub Release `SHA256SUMS` entry and the embedded version before they replace a binary. Release binaries are not committed to the repository. macOS artifacts are Developer ID signed and notarized; the installer does not re-sign the file, and re-signing on a machine without the Developer ID certificate strips the seal and can make Gatekeeper kill the process. Builders use supported dependency lines (OpenSSL 3.5 LTS, spdlog newer than 1.15.1) and the release hardening flags listed in [Release provenance](docs/RELEASE-PROVENANCE.md).

## Hardening checklist

A reasonable default posture for a new mesh:

1. Pin every seed explicitly in the seed lines.
2. Keep `mesh.require_seed_pins true`.
3. Confine `receive_dir` to the inbox path. Leave `transfer.allow_sensitive_paths` off.
4. Bind the mesh listener on a private or VPN address. Do not expose `19949` to the public internet.
5. Bind Bridge Panel on loopback or a private address. Do not expose the panel.
6. Run the CUA helper only in the interactive user session that needs it.
7. Upgrade the mesh through the installer or `bs upgrade`. Verify the embedded version after each upgrade.
8. Rotate or revoke peer keys by editing `authorized_keys` and restarting the affected daemon.

## What is not in scope

- Host-level hardening. BridgeSessions trusts the host account it runs as.
- Network-level hardening. The operator owns the firewall and the VPN.
- Multi-tenant authorization. There is no fine-grained per-command policy.
- Browser security beyond the panel origin. The panel CSP is tight; the rest of the operator's browser is the operator's responsibility.