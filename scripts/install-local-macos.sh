#!/bin/bash
# Install a local build into ~/.local/bin with Developer ID Application signing.
# Never leave an ad-hoc / linker-signed binary there — macOS may SIGKILL it (exit 137).
#
# Usage:
#   ./scripts/install-local-macos.sh [source-binary]
# Default source: build/bridgesessions
set -euo pipefail

# Prefer BS_DEV_ID; else first Developer ID Application identity in keychain.
IDENTITY="${BS_DEV_ID:-}"
if [[ -z "$IDENTITY" ]]; then
  IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -F 'Developer ID Application' | head -1 \
    | sed -E 's/.*"(.+)"/\1/' || true)"
fi
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE="${1:-$REPO_ROOT/build/bridgesessions}"
ENTITLEMENTS="$REPO_ROOT/macos-signing/entitlements.plist"
DEST_DIR="${HOME}/.local/bin"
DEST="$DEST_DIR/bridgesessions"

if [[ ! -f "$SOURCE" ]]; then
  echo "error: source binary not found: $SOURCE" >&2
  echo "Build first: cmake --build build -j --target bridgesessions" >&2
  exit 1
fi

if [[ -z "$IDENTITY" ]]; then
  echo "error: set BS_DEV_ID or install a Developer ID Application cert" >&2
  security find-identity -v -p codesigning 2>&1 | sed 's/^/  /' >&2
  exit 1
fi
if ! security find-identity -v -p codesigning 2>/dev/null | grep -F "$IDENTITY" >/dev/null; then
  echo "error: Developer ID identity not found in keychain:" >&2
  echo "  $IDENTITY" >&2
  echo "Available identities:" >&2
  security find-identity -v -p codesigning 2>&1 | sed 's/^/  /' >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
# Stable path so codesign Identifier is not mktemp-random (TCC keys on
# TeamID + Identifier; bs-install.XXXXXX broke Screen Recording every reinstall).
TMP="/tmp/bridgesessions-install-staging"
cp "$SOURCE" "$TMP"
chmod 755 "$TMP"
xattr -cr "$TMP" 2>/dev/null || true
codesign --remove-signature "$TMP" 2>/dev/null || true

echo "→ Signing with $IDENTITY (identifier=com.minddragon.bridgesessions) ..."
SIGN_ARGS=(
  --force --options runtime
  --sign "$IDENTITY"
  --timestamp
  --identifier com.minddragon.bridgesessions
)
if [[ -f "$ENTITLEMENTS" ]]; then
  SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
fi
if ! codesign "${SIGN_ARGS[@]}" "$TMP"; then
  echo "error: codesign with Developer ID failed — refusing ad-hoc fallback" >&2
  rm -f "$TMP"
  exit 1
fi

codesign --verify --strict --verbose=2 "$TMP" 2>&1 | sed 's/^/  /'
# Must show TeamIdentifier + stable Identifier (TCC Screen Recording).
# Capture codesign -dvv first: pipefail+grep -q causes SIGPIPE races that
# falsely fail even when Identifier/TeamIdentifier are correct.
CS_META="$(codesign -dvv "$TMP" 2>&1 || true)"
if ! grep -q 'TeamIdentifier=' <<<"$CS_META"; then
  echo "error: signed binary has no TeamIdentifier — not a proper Developer ID sign" >&2
  rm -f "$TMP"
  exit 1
fi
if ! grep -q 'Identifier=com.minddragon.bridgesessions' <<<"$CS_META"; then
  echo "error: signed binary Identifier is not com.minddragon.bridgesessions" >&2
  grep Identifier <<<"$CS_META" | sed 's/^/  /' >&2
  rm -f "$TMP"
  exit 1
fi

# Atomic-ish swap
cp "$TMP" "$DEST.new"
chmod 755 "$DEST.new"
mv -f "$DEST.new" "$DEST"
rm -f "$TMP"

# Keep BridgeSessions.app in sync (same TeamID+CFBundleIdentifier for TCC)
APP_BUNDLE="/Applications/BridgeSessions.app"
if [[ -d "$APP_BUNDLE/Contents/MacOS" ]]; then
  echo "→ Updating $APP_BUNDLE with same signature ..."
  cp "$DEST" "$APP_BUNDLE/Contents/MacOS/bridgesessions"
  chmod 755 "$APP_BUNDLE/Contents/MacOS/bridgesessions"
  if [[ -f "$REPO_ROOT/macos-signing/Info.plist" ]]; then
    cp "$REPO_ROOT/macos-signing/Info.plist" "$APP_BUNDLE/Contents/Info.plist"
  fi
  xattr -cr "$APP_BUNDLE" 2>/dev/null || true
  # Same Identifier as the Mach-O so TCC keys (TeamID+Identifier) match.
  APP_SIGN=(
    --force --deep --options runtime
    --sign "$IDENTITY" --timestamp
    --identifier com.minddragon.bridgesessions
  )
  if [[ -f "$ENTITLEMENTS" ]]; then
    APP_SIGN+=(--entitlements "$ENTITLEMENTS")
  fi
  codesign "${APP_SIGN[@]}" "$APP_BUNDLE" 2>&1 | sed 's/^/  /' || true
fi

# Symlink bs → bridgesessions
ln -sfn bridgesessions "$DEST_DIR/bs"

echo "→ Installed: $DEST"
"$DEST" --version
codesign -dvv "$DEST" 2>&1 | grep -E 'Authority|TeamIdentifier|Identifier' | sed 's/^/  /'
echo "Done. PATH should include $DEST_DIR"
