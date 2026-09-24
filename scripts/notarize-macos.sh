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
# Prefer BS_DEV_ID / BS_TEAM_ID; else discover from keychain (no hardcoded name).
IDENTITY="${BS_DEV_ID:-}"
if [[ -z "$IDENTITY" ]]; then
  IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -F 'Developer ID Application' | head -1 \
    | sed -E 's/.*"(.+)"/\1/' || true)"
fi
TEAM_ID="${BS_TEAM_ID:-}"
if [[ -z "$TEAM_ID" && -n "$IDENTITY" ]]; then
  TEAM_ID="$(echo "$IDENTITY" | sed -nE 's/.*\(([A-Z0-9]+)\)/\1/p')"
fi
BUNDLE_ID="com.minddragon.bridgesessions"
if [[ -z "$IDENTITY" || -z "$TEAM_ID" ]]; then
  echo "ERROR: set BS_DEV_ID and BS_TEAM_ID, or install Developer ID Application cert"
  exit 1
fi

ENTITLEMENTS="$(dirname "$0")/../macos-signing/entitlements.plist"
INFO_PLIST="$(dirname "$0")/../macos-signing/Info.plist"

if [ -z "$APP_STORE_CONNECT_KEY_ID" ] || [ -z "$APP_STORE_CONNECT_ISSUER" ] || [ -z "$APP_STORE_CONNECT_KEY_FILE" ]; then
    echo "ERROR: Set APP_STORE_CONNECT_KEY_ID, APP_STORE_CONNECT_ISSUER, APP_STORE_CONNECT_KEY_FILE"
    echo "Create at: https://appstoreconnect.apple.com/access/integrations/api"
    exit 1
fi

echo "=== 1. Sign the standalone binary ==="
# Sign the bare Mach-O, not a copy inside an .app. A Mach-O signed inside a
# bundle binds to the bundle's Info.plist; extracting it later yields
# "invalid Info.plist" and Gatekeeper kills the binary (SIGKILL 9).
SIGNED="/tmp/bridgesessions-standalone-signed"
cp "$SOURCE" "$SIGNED"
xattr -cr "$SIGNED" 2>/dev/null || true
codesign --remove-signature "$SIGNED" 2>/dev/null || true
codesign --force --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    --timestamp \
    --identifier "$BUNDLE_ID" \
    "$SIGNED"

echo "=== 2. Verify signature ==="
codesign --verify --strict --verbose=2 "$SIGNED" 2>&1

echo "=== 3. Zip the bare binary for notarization ==="
ZIP="/tmp/bridgesessions-notarize.zip"
rm -f "$ZIP"
( cd /tmp && ditto -c -k "$(basename "$SIGNED")" "$(basename "$ZIP")" )

echo "=== 4. Submit for notarization ==="
xcrun notarytool submit "$ZIP" \
    --key-id "$APP_STORE_CONNECT_KEY_ID" \
    --key "$APP_STORE_CONNECT_KEY_FILE" \
    --issuer "$APP_STORE_CONNECT_ISSUER" \
    --wait

echo "=== 5. Staple (best effort; online cdhash check is the real gate) ==="
# stapler on bare Mach-O often fails with Error 73 (no header pad room).
# That is fine: Gatekeeper verifies the cdhash against Apple's notarization
# service online on first run. A stapled ticket only matters offline.
xcrun stapler staple "$SIGNED" 2>&1 || \
    echo "note: staple skipped (Error 73 is normal for bare Mach-O; online notarization still applies)"

echo "=== 6. Verify ==="
codesign --verify --strict "$SIGNED"
"$SIGNED" --version >/dev/null && echo "runs locally: OK"

echo "=== 7. Ship the standalone binary ==="
cp "$SIGNED" "$OUTPUT"
chmod +x "$OUTPUT"

echo ""
echo "=== Done ==="
echo "Signed + notarized binary: $OUTPUT"
echo "(spctl on a bare Mach-O always says 'not an app'; Gatekeeper validates the cdhash online.)"

rm -f "$ZIP" "$SIGNED"
