#!/bin/bash
# macOS notarization pipeline for BridgeSessions
# Requires: Developer ID Application cert + App Store Connect API key
#
# Usage:
#   export APP_STORE_CONNECT_KEY_ID="..."
#   export APP_STORE_CONNECT_ISSUER="..."
#   export APP_STORE_CONNECT_KEY_FILE="/path/to/AuthKey_XXXX.p8"
#   ./scripts/notarize-macos.sh [binary-path] [output-path]
#
# The API key is created at: https://appstoreconnect.apple.com/access/integrations/api
# Role: Developer or higher. Key: App Manager or Admin.

set -e

SOURCE="${1:-build/bridgesessions}"
OUTPUT="${2:-dist/bridgesessions-macos-arm64}"
IDENTITY="Developer ID Application: Jefferson Nunn (QL5MD8FKPL)"
TEAM_ID="QL5MD8FKPL"
BUNDLE_ID="com.minddragon.bridgesessions"
ENTITLEMENTS="$(dirname "$0")/../macos-signing/entitlements.plist"
INFO_PLIST="$(dirname "$0")/../macos-signing/Info.plist"

if [ -z "$APP_STORE_CONNECT_KEY_ID" ] || [ -z "$APP_STORE_CONNECT_ISSUER" ] || [ -z "$APP_STORE_CONNECT_KEY_FILE" ]; then
    echo "ERROR: Set APP_STORE_CONNECT_KEY_ID, APP_STORE_CONNECT_ISSUER, APP_STORE_CONNECT_KEY_FILE"
    echo "Create at: https://appstoreconnect.apple.com/access/integrations/api"
    exit 1
fi

echo "=== 1. Create .app bundle ==="
BUNDLE="/tmp/${BUNDLE_ID##*.}.app"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"
cp "$SOURCE" "$BUNDLE/Contents/MacOS/bridgesessions"
cp "$INFO_PLIST" "$BUNDLE/Contents/Info.plist"

echo "=== 2. Code sign with Developer ID ==="
codesign --force --deep --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    --timestamp \
    "$BUNDLE"

echo "=== 3. Verify signature ==="
codesign --verify --strict --verbose=2 "$BUNDLE" 2>&1

echo "=== 4. Create ZIP for notarization ==="
ZIP="/tmp/bridgesessions-notarize.zip"
ditto -c -k --keepParent "$BUNDLE" "$ZIP"

echo "=== 5. Submit for notarization ==="
xcrun notarytool submit "$ZIP" \
    --key-id "$APP_STORE_CONNECT_KEY_ID" \
    --key "$APP_STORE_CONNECT_KEY_FILE" \
    --issuer "$APP_STORE_CONNECT_ISSUER" \
    --wait

echo "=== 6. Staple the ticket ==="
xcrun stapler staple "$BUNDLE"

echo "=== 7. Verify notarization ==="
xcrun stapler validate "$BUNDLE"
spctl --assess --type execute -vv "$BUNDLE"

echo "=== 8. Extract signed + notarized binary ==="
cp "$BUNDLE/Contents/MacOS/bridgesessions" "$OUTPUT"
chmod +x "$OUTPUT"

echo ""
echo "=== Done ==="
echo "Signed + notarized binary: $OUTPUT"
echo "Gatekeeper: $(spctl --assess --type execute -vv "$OUTPUT" 2>&1 | head -1)"

rm -rf "$BUNDLE" "$ZIP"
