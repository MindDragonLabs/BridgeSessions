# Mobile access and authentication decision

Mobile access uses the existing private tailnet HTTPS ingress. It does not
create a new public listener or service. The panel continues to bind only to
loopback, tailnet CGNAT, or RFC1918 addresses.

The administrator browser credential is an HttpOnly, SameSite=Strict session
cookie. Password login is accepted on loopback or when the transport is
verified HTTPS (direct TLS or an explicitly trusted ingress proxy). Secure is
added to the cookie for HTTPS. Cookie state-changing requests require an
allowed Origin; bearer API requests remain suitable for native/mobile clients.

Device access uses separate scoped API tokens. Tokens are stored as hashes,
carry an expiry, and can be revoked independently. An administrator manages
registered devices through `GET /api/devices`, `POST /api/devices`,
`POST /api/devices/{id}/deactivate`, and `DELETE /api/devices/{id}`. Enrollment
returns the device-bound bearer token once; the registry is stored atomically
with mode 0600. Deactivating or revoking a device makes its credentials unusable.
These routes require the administrator credential. The administrator cookie,
device token, and BridgeSessions mesh invite are different credential types;
an invite token is never accepted as an HTTP API bearer token. Malformed
persistent auth state fails closed. Replacing the password rotates the session
secret and generation, invalidating cookies in other processes as well as the
current process.

The password file records the actual derivation scheme:

```text
PBKDF2-HMAC-SHA256, 600000 iterations, 16-byte random salt, 32-byte digest
```

Tests must use isolated temporary `BRIDGEPANEL_CONFIG` and data directories;
they must not read or mutate real credential files. A tailnet ingress must
forward HTTPS only from an explicitly trusted proxy address. Public exposure,
credential values in URLs, and new public services are out of scope.

Chat ownership uses a non-secret stable credential identity passed by the HTTP
handler. Device tokens remain separate even when they reuse a request ID.
A valid bearer is used independently of cookies; an invalid Authorization
header fails closed. Cookie mutations require a matching Origin scheme, host and
port, including DELETE. A header's mere presence cannot bypass this check.

Auth/password and API-token writes use a process file lock and a thread lock
around the full read/modify/write transaction, followed by an atomic private
file replacement. Password-reset revocation is checked against persistent
state on subsequent cookie validation. Login client tracking is capped at
4096 and sessions at 10000; inactive login entries and expired sessions are
pruned. A full active login map refuses new clients.
