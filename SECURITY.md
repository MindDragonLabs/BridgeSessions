# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| 2.0.x | ✅ |
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

- The Bridge Panel is bound to the node's VPN address, not localhost, and
  performs **no authentication of its own** — its security boundary is the
  mesh/VPN. Do not expose its port to the public internet.
- ed25519 mutual TLS is the primary authentication mechanism; protect your
  `~/.bridgesessions/id_ed25519.pem` keypair as you would an SSH key.
