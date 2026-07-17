# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| 2.0.5-alpha2 | ✅ public alpha |
| 2.0.0–2.0.4 | ❌ superseded |
| < 2.0 | ❌ |

## Reporting a vulnerability

**Do not open a public issue for security reports.**

Please report vulnerabilities privately by opening a security advisory on the
forge (Codeberg/GitHub "Security" tab → "Report a vulnerability"), or by
emailing the maintainer directly.

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
