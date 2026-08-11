#!/bin/bash
# Sign bridgesessions with Developer ID — stops TCC re-prompts on rebuild.
# Refuses ad-hoc fallback: a missing identity is a hard error.
set -euo pipefail

SOURCE="${1:-build/bridgesessions}"
IDENTITY="${BS_DEV_ID:-Developer ID Application: Jefferson Nunn (QL5MD8FKPL)}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="$SCRIPT_DIR/../macos-signing/entitlements.plist"
INFO_PLIST="$SCRIPT_DIR/../macos-signing/Info.plist"
OUTPUT="${2:-$SOURCE-signed}"

if [[ ! -f "$SOURCE" ]]; then
  echo "error: source not found: $SOURCE" >&2
  exit 1
fi
if ! security find-identity -v -p codesigning 2>/dev/null | grep -F "$IDENTITY" >/dev/null; then
  echo "error: identity not in keychain: $IDENTITY" >&2
  security find-identity -v -p codesigning 2>&1 | sed 's/^/  /' >&2
  exit 1
fi

echo "Signing $SOURCE with Developer ID..."

# Standalone Mach-O signing (do NOT extract from a signed .app — that
# invalidates the seal and AMFI kills the binary with exit 137 on other hosts).
cp "$SOURCE" "$OUTPUT"
xattr -cr "$OUTPUT" 2>/dev/null || true
codesign --remove-signature "$OUTPUT" 2>/dev/null || true

codesign --force --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    --timestamp \
    --identifier com.minddragon.bridgesessions \
    "$OUTPUT"
chmod +x "$OUTPUT"

echo "=== Verification ==="
codesign --verify --strict --verbose=2 "$OUTPUT" 2>&1
echo "=== Binary signature ==="
codesign -dvv "$OUTPUT" 2>&1 | head -12

# Also produce a signed .app for GUI installs (optional, same identity)
APP_NAME="bridgesessions"
BUNDLE_DIR="/tmp/${APP_NAME}.app"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/Contents/MacOS"
cp "$OUTPUT" "$BUNDLE_DIR/Contents/MacOS/$APP_NAME"
cp "$INFO_PLIST" "$BUNDLE_DIR/Contents/Info.plist"
codesign --force --deep --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    --timestamp \
    "$BUNDLE_DIR" 2>/dev/null || true
codesign --verify --strict "$BUNDLE_DIR" 2>/dev/null && \
  echo "Also signed app bundle: $BUNDLE_DIR" || true

echo ""
echo "Signed binary: $OUTPUT"
echo "Install to: cp $OUTPUT ~/.local/bin/bridgesessions"
