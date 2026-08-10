#!/bin/bash
# Install a local build into ~/.local/bin with Developer ID Application signing.
# Never leave an ad-hoc / linker-signed binary there — macOS may SIGKILL it (exit 137).
#
# Usage:
#   ./scripts/install-local-macos.sh [source-binary]
# Default source: build/bridgesessions
set -euo pipefail

IDENTITY="${BS_DEV_ID:-Developer ID Application: Jefferson Nunn (QL5MD8FKPL)}"
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

if ! security find-identity -v -p codesigning 2>/dev/null | grep -F "$IDENTITY" >/dev/null; then
  echo "error: Developer ID identity not found in keychain:" >&2
  echo "  $IDENTITY" >&2
  echo "Available identities:" >&2
  security find-identity -v -p codesigning 2>&1 | sed 's/^/  /' >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
TMP="$(mktemp /tmp/bs-install.XXXXXX)"
cp "$SOURCE" "$TMP"
chmod 755 "$TMP"
xattr -cr "$TMP" 2>/dev/null || true

echo "→ Signing with $IDENTITY ..."
SIGN_ARGS=(--force --options runtime --sign "$IDENTITY" --timestamp)
if [[ -f "$ENTITLEMENTS" ]]; then
  SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
fi
if ! codesign "${SIGN_ARGS[@]}" "$TMP"; then
  echo "error: codesign with Developer ID failed — refusing ad-hoc fallback" >&2
  rm -f "$TMP"
  exit 1
fi

codesign --verify --strict --verbose=2 "$TMP" 2>&1 | sed 's/^/  /'
# Must show TeamIdentifier QL5MD8FKPL (or whatever is on the cert)
if ! codesign -dvv "$TMP" 2>&1 | grep -q 'TeamIdentifier='; then
  echo "error: signed binary has no TeamIdentifier — not a proper Developer ID sign" >&2
  rm -f "$TMP"
  exit 1
fi

# Atomic-ish swap
cp "$TMP" "$DEST.new"
chmod 755 "$DEST.new"
mv -f "$DEST.new" "$DEST"
rm -f "$TMP"

# Symlink bs → bridgesessions
ln -sfn bridgesessions "$DEST_DIR/bs"

echo "→ Installed: $DEST"
"$DEST" --version
codesign -dvv "$DEST" 2>&1 | grep -E 'Authority|TeamIdentifier|Identifier' | sed 's/^/  /'
echo "Done. PATH should include $DEST_DIR"
