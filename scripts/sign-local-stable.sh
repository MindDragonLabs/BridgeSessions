#!/bin/bash
# scripts/sign-local-stable.sh — per-machine stable code-signing identity.
#
# Problem: release binaries are ad-hoc signed (new cdhash every build). macOS
# TCC (Screen Recording, Accessibility) keys permissions on the code signature
# Designated Requirement; an ad-hoc DR anchors on the cdhash, so every upgrade
# looks like a brand-new app and permissions must be re-granted.
#
# Fix: generate ONE local CA + leaf code-signing cert per machine (kept under
# ~/.bridgesessions/signing/), import into a dedicated keychain, and sign the
# installed binary with it. The DR then anchors on
#   identifier "bridgesessions" and certificate leaf[...]
# — constant across upgrades — so TCC permissions survive.
#
# Usage: scripts/sign-local-stable.sh <binary-or-app> [identifier]
# Exits 0 if the target ends up signed with the stable identity (or was
# already Developer-ID signed — that is stable too and left untouched).
# Exits 1 on failure. Idempotent: safe to run on every install/upgrade.

set -uo pipefail

TARGET="${1:?usage: sign-local-stable.sh <binary-or-app> [identifier]}"
IDENT="${2:-bridgesessions}"

# Resolve the Mach-O to sign (accept .app bundles or bare binaries)
if [ -d "$TARGET" ]; then
  BIN="$TARGET/Contents/MacOS/bridgesessions"
  [ -f "$BIN" ] || BIN="$(ls "$TARGET"/Contents/MacOS/* 2>/dev/null | head -1)"
else
  BIN="$TARGET"
fi
[ -f "$BIN" ] || { echo "sign-local-stable: no binary at $BIN" >&2; exit 1; }

# Already Developer ID signed? That's stable — do not touch.
SIG_AUTHORITY="$(codesign -dvv "$BIN" 2>/dev/null | grep -F 'Authority=' | head -1 || true)"
if echo "$SIG_AUTHORITY" | grep -qF 'Developer ID Application'; then
  echo "sign-local-stable: already Developer ID signed — leaving as-is"
  exit 0
fi

SIGN_DIR="${HOME}/.bridgesessions/signing"
KEYCHAIN_NAME="bridgesessions-signing"
KEYCHAIN_PATH="${HOME}/.bridgesessions/signing/${KEYCHAIN_NAME}.keychain-db"

# Keychain password: random once, stored next to the keychain. This keychain
# holds ONLY the local signing identity — not user secrets — so a stored
# password is an acceptable trade for unattended upgrades.
PASS_FILE="${SIGN_DIR}/keychain.pass"

mkdir -p "$SIGN_DIR"

ensure_keychain() {
  if [ -f "$KEYCHAIN_PATH" ]; then
    # Unlock with stored password (fall back silently — codesign may still
    # find the identity if the keychain is already unlocked).
    [ -f "$PASS_FILE" ] && security unlock-keychain -p "$(cat "$PASS_FILE")" "$KEYCHAIN_PATH" 2>/dev/null
    return 0
  fi
  PASS="$(openssl rand -hex 24)"
  printf '%s' "$PASS" > "$PASS_FILE"
  chmod 600 "$PASS_FILE"
  security create-keychain -p "$PASS" "$KEYCHAIN_PATH"
  security set-keychain-settings -lut 31536000 "$KEYCHAIN_PATH" 2>/dev/null || true
  # Import into the search list so codesign can resolve the identity
  security list-keychains -d user | grep -qF "$KEYCHAIN_PATH" || \
    security list-keychains -d user -s "$KEYCHAIN_PATH" $(security list-keychains -d user | sed 's/[",]//g' | sed 's/^ *//;s/ *$//' | tr '\n' ' ' | sed 's/  */ /g')
  return 0
}

ensure_identity() {
  # Existing leaf in the keychain?
  if security find-identity -v -p codesigning "$KEYCHAIN_PATH" 2>/dev/null | grep -qF '"bridgesessions"'; then
    return 0
  fi
  # Reuse existing CA if present (identity survived, keychain was lost)
  if [ -f "${SIGN_DIR}/ca.key" ] && [ -f "${SIGN_DIR}/leaf.crt" ]; then
    :
  else
    openssl req -x509 -newkey rsa:2048 -nodes \
      -keyout "${SIGN_DIR}/ca.key" -out "${SIGN_DIR}/ca.crt" \
      -days 3650 -subj "/CN=BridgeSessions Local Signing ${USER}" 2>/dev/null
    openssl req -newkey rsa:2048 -nodes \
      -keyout "${SIGN_DIR}/leaf.key" -out "${SIGN_DIR}/leaf.csr" \
      -subj "/CN=bridgesessions" 2>/dev/null
    openssl x509 -req -in "${SIGN_DIR}/leaf.csr" \
      -CA "${SIGN_DIR}/ca.crt" -CAkey "${SIGN_DIR}/ca.key" -CAcreateserial \
      -out "${SIGN_DIR}/leaf.crt" -days 3650 2>/dev/null \
      -extfile <(printf 'keyUsage=critical,digitalSignature\nextendedKeyUsage=critical,codeSigning')
  fi
  security import "${SIGN_DIR}/ca.crt"  -k "$KEYCHAIN_PATH" -A 2>/dev/null || true
  security import "${SIGN_DIR}/leaf.key" -k "$KEYCHAIN_PATH" -A 2>/dev/null || true
  security import "${SIGN_DIR}/leaf.crt" -k "$KEYCHAIN_PATH" -A 2>/dev/null || true
  security set-key-partition-list -S apple-tool:,apple: -s "$KEYCHAIN_PATH" \
    -k "$(cat "$PASS_FILE")" 2>/dev/null || true
}

ensure_keychain || { echo "sign-local-stable: keychain setup failed" >&2; exit 1; }
ensure_identity || { echo "sign-local-stable: identity setup failed" >&2; exit 1; }

# Sign with entitlements if the repo ships them (best effort)
ENT="${SIGN_DIR}/entitlements.plist"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "${SCRIPT_DIR}/../macos-signing/entitlements.plist" ] && \
  cp "${SCRIPT_DIR}/../macos-signing/entitlements.plist" "$ENT"

ARGS=(--force --sign "bridgesessions" --timestamp=none --identifier "$IDENT")
if [ -f "$ENT" ]; then
  ARGS+=(--entitlements "$ENT")
fi

if [ -d "$TARGET" ]; then
  codesign "${ARGS[@]}" "$TARGET" 2>&1 || { echo "sign-local-stable: codesign failed" >&2; exit 1; }
else
  codesign "${ARGS[@]}" "$BIN" 2>&1 || { echo "sign-local-stable: codesign failed" >&2; exit 1; }
fi

# Verify: DR must anchor on the certificate leaf, not cdhash
DR="$(codesign -d -r- "$BIN" 2>/dev/null | grep '^designated' || true)"
echo "sign-local-stable: $DR"
if echo "$DR" | grep -q 'certificate leaf'; then
  echo "sign-local-stable: OK — stable identity applied to $BIN"
  exit 0
fi
echo "sign-local-stable: verification failed (DR does not anchor on cert leaf)" >&2
exit 1
