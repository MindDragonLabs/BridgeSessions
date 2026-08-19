# BridgePanel auth: beyond loopback — mTLS device pairing (design)

**Status:** design only, not implemented. No code in this repo currently does
device pairing, cert issuance, or client-cert verification for BridgePanel.
See `docs/bridge-panel.md` for the current (loopback-only) posture.

## Problem

BridgePanel today binds `127.0.0.1` by default and gates writes with a
generated bearer token (`docs/bridge-panel.md:62-66`). That token model is
fine for "same machine, same user" but has no answer for the moment an
operator wants the panel reachable from a phone or a second desktop over the
mesh/VPN: a bearer token alone is bad currency over any interface bigger than
loopback — it's static, unrevocable per-device, and phishable/leakable
(browser history, shoulder-surf, shared clipboard). This doc specifies how
BridgePanel grows device identity before it grows its bind surface.

## Non-goals

- Not replacing the beta5 loopback token. That stays as-is for local-only use.
- Not building a general-purpose CA or PKI product. Pairing reuses BS's
  existing ed25519 pin model (`bs-protocol.h:1980-2033`,
  `bs-protocol.h:2169-2302`) rather than inventing a second trust system.
- Not OIDC. See "Why not OIDC" below.

## Requirements (from the task)

1. Loopback sidecar token stays authoritative for beta5 — no regression.
2. Before ANY non-loopback bind, per-device mTLS pairing is mandatory —
   the panel refuses to bind non-loopback until at least one device is paired.
3. Pairing reuses the existing ed25519 pin infrastructure BS already uses for
   peer trust — same key type, same pin-file shape, same verify callback
   pattern, not a parallel mechanism.
4. `bs panel pair` prints a QR/console code; the device presents a per-device
   cert; the server pins it via TLS client auth exactly like a peer key.
5. A revocation list exists and is checked on every non-loopback connection.
6. Origin/allow-list checks apply once the SPA is reachable from something
   other than loopback (CSRF/rebinding surface changes with the bind).
7. OIDC is explicitly out — no external IdP for a mesh tool.

## Design

### 1. Trust model reuse

BS already has the primitives this needs:

- **Key type:** ed25519, same as peer mesh identity
  (`generate_ed25519_cert()`, `bs-protocol.h:1980-2010`).
- **Pin storage:** newline-delimited hex pubkeys, `#`-comment support,
  hot-reload on mtime change — the `AuthorizedKeys` struct
  (`bs-protocol.h:2169-2232`) is the pattern to clone, not reinvent. Panel
  device pins live in a sibling file, `~/.bridgesessions/panel_devices`,
  same format, same reload semantics.
- **Verify callback shape:** panel's TLS server-side client-cert check is
  structurally `server_cert_verify_cb` (`bs-protocol.h:2254-2279`) retargeted
  at `panel_devices` instead of `authorized_keys` — extract raw pubkey from
  the presented client cert, check membership, reject otherwise. No new
  cert-parsing code; the extraction helper (`extract_raw_pubkey`/
  `pubkey_hex`, `bs-protocol.h:2014-2033`) is shared.

This means "pairing" is really: **generate an ed25519 keypair + self-signed
cert on the device, get its pubkey into `panel_devices`, and require that
cert on every non-loopback panel TLS handshake.** No new crypto primitives,
no CA, no cert chains — same carrier-cert-around-a-raw-key model BS mesh
already uses.

### 2. Pairing flow

```
Operator (server side)              Device (browser / app)
─────────────────────              ───────────────────────
$ bs panel pair
  → generates a one-time
    pairing token T (random,
    32B, 5 min TTL, single-use)
  → prints:
      - QR code encoding
        {panel_addr, T}
      - fallback console code
        (8-char base32, for
        headless/no-camera)
  → opens a short-lived
    pairing listener on the
    panel port (loopback or
    the address being paired
    toward), accepting POSTs
    bearing token T only

                                     Device scans QR (or operator
                                     types the console code into
                                     the device's "Pair" screen)
                                     → device generates its own
                                       ed25519 keypair + self-signed
                                       cert (client-side, WebCrypto
                                       or platform keychain)
                                     → POST /pair {T, device_cert_pem,
                                       device_label} over the pairing
                                       listener (TLS, but pre-auth —
                                       token T is the only gate)

  → validates T (matches,
    unexpired, unused)
  → extracts raw pubkey from
    device_cert_pem
  → appends "pubkey_hex
    device_label added=<ts>"
    to panel_devices
  → marks T used, closes
    pairing listener
  → prints confirmation:
    "Paired: <device_label>
     (<pubkey_hex[:12]>...)"

                                     Device stores its private key
                                     (platform keychain / IndexedDB
                                     non-extractable CryptoKey) and
                                     retries the real panel connection
                                     with mTLS: presents device_cert
                                     on every future handshake.
```

Key properties:

- The pairing listener is a **narrow, time-boxed exception** — it's the only
  thing on the panel port that accepts unauthenticated input, it only accepts
  the `/pair` verb, it dies after 5 minutes or first use, whichever is first,
  and it never leaves loopback+the token check to reach panel data routes.
- Token T is single-use and short-TTL specifically so a shoulder-surfed QR
  code is worthless after the window closes.
- After pairing, the device is indistinguishable from a peer in trust model:
  its pubkey sits in a pin file, checked on every handshake, revocable by
  editing that file (matches `AuthorizedKeys::reload()` semantics — no daemon
  restart needed).

### 3. Cert profile

- **Key:** ed25519 (matches BS mesh identity keys — one key type in the
  product, one code path to audit).
- **Cert:** self-signed X.509 wrapping the ed25519 pubkey, same shape as
  `generate_ed25519_cert()` produces for mesh nodes. No CA, no intermediate,
  no chain validation — trust is the pin, not the cert's issuer.
- **Subject/SAN:** cosmetic only (device label, e.g. `CN=alice-iphone`) — not
  trusted for authorization, exactly like mesh node certs today (trust is on
  raw pubkey bytes, not cert fields, per `extract_raw_pubkey`).
- **Validity window:** long (e.g. 10 years) since the pin file — not cert
  expiry — is the actual revocation mechanism, matching how peer certs work.
- **Storage on device:** private key stays in platform keychain / a
  non-extractable WebCrypto `CryptoKey`; never leaves the device, never
  transits the pairing POST (only the cert/pubkey does).

### 4. Revocation

- `panel_devices` file, same format as `authorized_keys`
  (`pubkey_hex [label] [# comment]` per line).
- New CLI: `bs panel devices list` / `bs panel devices revoke <label-or-hex>`
  — the revoke path deletes the line and touches the file's mtime so the
  running panel's `reload()` (same hot-reload pattern as `AuthorizedKeys`,
  `bs-protocol.h:2169-2232`) picks it up on the next handshake without a
  restart.
- Revocation is checked on **every** non-loopback connection (TLS handshake
  time, not just at session start) — a device revoked mid-session should have
  its *next* reconnect rejected; killing an already-established TLS session
  on revoke is a stretch goal, not a requirement for v1 (mirrors how mesh
  peer revocation works today — hot-reloaded pin file, no live-session kill).
- Revocation list is local to the panel host, not gossiped — a device paired
  to one panel instance has no standing anywhere else.

### 5. Origin / allow-list (once non-loopback)

- While bound to loopback only: no Origin check needed (matches today).
- Once bound to a VPN/LAN address, the SPA's `Origin` header must match an
  explicit allow-list (`panel.allowed_origins` config, analogous to
  `mesh.require_seed_pins`-style boolean/list config) — default allow-list is
  empty, meaning **the operator must explicitly list origins** before
  non-loopback binding does anything useful. This blocks a browser tab on an
  unrelated origin from riding an authenticated session via CSRF once the
  panel is reachable off-box — the allow-list check is the actual guard,
  mTLS proves the device, Origin proves the tab.
- Non-browser clients (a future native app, `bs panel pair`'s own listener)
  are unaffected — Origin is a browser-only concept; mTLS is the real gate
  for them.

### 6. Failure modes

| Condition | Behavior |
|---|---|
| Non-loopback bind requested, zero devices paired | Refuse to bind; print `bs panel pair` hint |
| Device cert not in `panel_devices` | TLS handshake rejected (mirrors `server_cert_verify_cb` reject path) |
| Device cert revoked mid-connection | Next reconnect rejected; existing session not force-killed in v1 |
| Pairing token expired / already used | `/pair` POST rejected, operator re-runs `bs panel pair` |
| Pairing token guessed/brute-forced | Mitigated by 32B random token + 5 min TTL + single-use; not rate-limited beyond that in v1 |
| Origin header missing or not allow-listed (non-loopback bind) | Request rejected before reaching panel data routes |
| Operator loses all paired devices (file deleted) | Falls back to "zero devices" state — non-loopback bind refuses until re-paired; loopback+token access unaffected |

### Why not OIDC

No external IdP for a mesh tool: BS's entire trust model is local-first,
pin-based, and works offline/air-gapped between operator-controlled peers.
Wiring in an external OIDC provider would (a) add a hard network dependency
for auth on a tool whose whole pitch is working without one, (b) introduce a
second, foreign trust root alongside the ed25519 pin system, doubling the
attack surface and the audit burden, and (c) hand device-identity decisions
to a third party for a product that pins peer identity locally by design.
Device pairing via the existing pin infrastructure keeps one trust model for
the whole product: mesh peers and panel devices are both "an ed25519 pubkey
someone explicitly pinned."

## Implementation sequencing (for whenever this is built, not now)

1. `panel_devices` pin file + `bs panel devices list/revoke` CLI (pure
   reuse of `AuthorizedKeys` pattern).
2. `bs panel pair` pairing listener + QR/console code.
3. Panel TLS server gains client-cert-required mode, gated on non-loopback
   bind; verify callback checks `panel_devices`.
4. Origin allow-list config + enforcement in the SPA-serving path.
5. Revocation hot-reload wiring (mtime watch, same as `AuthorizedKeys`).
6. Docs: update `docs/bridge-panel.md` non-loopback section to point here
   instead of describing bearer-token-only trust.

Each step ships independently and loopback+token behavior is unchanged until
step 3 actually flips a non-loopback bind live.
