# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| 2.0.6-alpha2 | ✅ public multi-platform alpha (current) |
| 2.0.1 | ❌ superseded |
| 2.0.0 | ❌ superseded |
| < 2.0 | ❌ |

## Reporting a vulnerability

**Do not open a public issue for security reports.**

Please report vulnerabilities privately by emailing the maintainer through the
contact address in the latest signed release tag or commit metadata. If that is
not available, open a non-sensitive issue requesting a private contact channel,
but do not include vulnerability details in that issue.

Include:
- A description of the vulnerability and its impact.
- Steps to reproduce (or a proof-of-concept).
- Affected version(s).

You will receive an acknowledgement within a few days, and we will coordinate a
fix and disclosure timeline with you.

## Scope notes

- BridgePanel defaults to loopback and requires its generated bearer token for
  every write. VPN/LAN reads may be allowed only through explicit trusted-IP
  configuration. Do not expose its port to the public internet.
- ed25519 mutual TLS is the primary authentication mechanism; protect your
  `~/.bridgesessions/id_ed25519.pem` private key as you would an SSH key.
- Seed and direct CLI connections require pinned Ed25519 public keys. Do not
  disable `mesh.require_seed_pins` on untrusted networks.
- mDNS is disabled by default. Enabling it permits address refresh only for
  keys already present in pinned seeds or `authorized_keys`; multicast never
  creates a trust root. Announced addresses are unauthenticated hints and may
  be spoofed for denial of service; TLS key verification still prevents
  impersonation.
- Peer authorization is host-level, not capability-scoped: an authorized peer
  can execute shell commands and request any file readable by the daemon
  account. Treat every authorized peer key as equivalent to interactive host
  access.
- Daemon CLI IPC remains loopback-only and additionally requires the ephemeral
  owner-only token stored under the active BridgeSessions app home.
