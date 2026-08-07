# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| 26.08.06-beta1 | ✅ current public beta |
| < 26.08.06 | ❌ superseded |

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

## Invite token lifecycle

Mesh join uses single-use invite tokens with the following properties:

| Property | Value |
|----------|-------|
| **Generation** | 16 random bytes from `RAND_bytes` (OpenSSL CSPRNG), hex-encoded (32 chars) |
| **Lifetime** | 2 hours from creation (`steady_clock`, not wall clock) |
| **Single-use** | Yes — claimed on first successful `JoinRequest`; `claimed_by` field set to joiner's Ed25519 pubkey |
| **Scope** | Node-specific — token is valid only on the node that generated it (`bs invite`) |
| **Revocation** | Restart the daemon — all pending tokens are in-memory only, cleared on restart |
| **Rotation** | Generate a new token with `bs invite` at any time; old tokens expire naturally |
| **Storage** | In-memory only (`pending_invites_` vector). Never written to disk. No persistence across restarts. |
| **Transport** | TLS-encrypted mesh channel — token never traverses plaintext |

If a token is compromised before use: restart the daemon on the inviting node.
All pending tokens are invalidated. Generate a new one with `bs invite`.
