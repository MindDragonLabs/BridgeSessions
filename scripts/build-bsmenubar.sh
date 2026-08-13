#!/bin/bash
# Build + Developer ID-sign BSMenubar.app
# Usage: scripts/build-bsmenubar.sh [dest.app]
# Default dest: dist/BSMenubar.app
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/BSMenubar"
DEST="${1:-$ROOT/dist/BSMenubar.app}"
STAGE="$(mktemp -d /tmp/bsmenubar-build.XXXXXX)"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

IDENTITY="${BS_DEV_ID:-}"
if [[ -z "$IDENTITY" ]]; then
  IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -F 'Developer ID Application' | head -1 \
    | sed -E 's/.*\"(.+)\"/\1/' || true)"
fi
if [[ -z "$IDENTITY" ]]; then
  echo "error: set BS_DEV_ID or install a Developer ID Application cert" >&2
  exit 1
fi

# Must include lowercase main.swift (explicit NSApplication.delegate).
# @main + NSApplicationMain does NOT create the status item unless
# NSPrincipalClass is set — shipping dist previously launched empty.
swiftc -O \
  -module-name BSMenubar \
  -framework Cocoa \
  -framework AppKit \
  -framework ApplicationServices \
  -o "$STAGE/BSMenubar" \
  "$SRC/main.swift" \
  "$SRC/BSMenubarApp.swift" \
  "$SRC/StatusItemController.swift" \
  "$SRC/SettingsController.swift" \
  "$SRC/HelperProcessManager.swift" \
  "$SRC/FleetStatusController.swift"

rm -rf "$DEST"
mkdir -p "$DEST/Contents/MacOS" "$DEST/Contents/Resources"
cp "$STAGE/BSMenubar" "$DEST/Contents/MacOS/BSMenubar"
chmod +x "$DEST/Contents/MacOS/BSMenubar"
cp "$SRC/Info.plist" "$DEST/Contents/Info.plist"
printf 'APPL????' > "$DEST/Contents/PkgInfo"
cp "$STAGE/BSMenubar" "$SRC/BSMenubar-bin"
xattr -cr "$DEST" 2>/dev/null || true

codesign --force --options runtime \
  --entitlements "$SRC/BSMenubar.entitlements" \
  --sign "$IDENTITY" \
  --timestamp \
  --identifier com.minddragon.bridgesessions.menubar \
  "$DEST/Contents/MacOS/BSMenubar"

codesign --force --options runtime \
  --entitlements "$SRC/BSMenubar.entitlements" \
  --sign "$IDENTITY" \
  --timestamp \
  --identifier com.minddragon.bridgesessions.menubar \
  "$DEST"

codesign --verify --strict --verbose=2 "$DEST" 2>&1
echo "Built $DEST"
