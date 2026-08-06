#!/bin/bash
# Sign bridgesessions with Developer ID — stops TCC re-prompts on rebuild
set -e

SOURCE="${1:-build-main/bridgesessions}"
IDENTITY="Developer ID Application: Jefferson Nunn (QL5MD8FKPL)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="$SCRIPT_DIR/../macos-signing/entitlements.plist"
INFO_PLIST="$SCRIPT_DIR/../macos-signing/Info.plist"
OUTPUT="${2:-$SOURCE-signed}"

echo "Signing $SOURCE with Developer ID..."

# Copy unsigned binary
cp "$SOURCE" "$OUTPUT"

# Add Info.plist to the Mach-O (seclndict requires it for stable identity)
# codesign --add-info-plist is not available, so we embed via the -i flag
# and the plist must be in the bundle structure. For a standalone binary,
# we use --bundle to create a .app structure.

APP_NAME="bridgesessions"
BUNDLE_DIR="/tmp/${APP_NAME}.app"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/Contents/MacOS"
cp "$SOURCE" "$BUNDLE_DIR/Contents/MacOS/$APP_NAME"
cp "$INFO_PLIST" "$BUNDLE_DIR/Contents/Info.plist"

# Sign the .app bundle
codesign --force --deep --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    --timestamp \
    "$BUNDLE_DIR"

# Verify
echo "=== Verification ==="
codesign --verify --strict --verbose=2 "$BUNDLE_DIR" 2>&1

# Extract the signed binary
cp "$BUNDLE_DIR/Contents/MacOS/$APP_NAME" "$OUTPUT"
chmod +x "$OUTPUT"

# Verify the binary itself
echo "=== Binary signature ==="
codesign -dvv "$OUTPUT" 2>&1 | head -10

echo ""
echo "Signed binary: $OUTPUT"
echo "Install to: cp $OUTPUT ~/.local/bin/bridgesessions"

rm -rf "$BUNDLE_DIR"
