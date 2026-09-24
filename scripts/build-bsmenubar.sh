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
    | sed -E 's/.*"(.+)"/\1/' || true)"
fi
if [[ -z "$IDENTITY" ]]; then
  # No Developer ID on this machine — fall back to the per-machine stable
  # identity from sign-local-stable.sh (same trust domain as the daemon;
  # keeps a constant DR across rebuilds so TCC prompts survive).
  # Avoid grep -q under pipefail: codesign closes early → SIGPIPE false-fail.
  CS_META="$(codesign -dvv /Users/jefferson/Applications/BridgeSessions.app 2>&1 || true)"
  if grep -q 'Authority=bridgesessions' <<<"$CS_META"; then
    IDENTITY="bridgesessions"
  fi
fi
if [[ -z "$IDENTITY" ]]; then
  echo "error: set BS_DEV_ID or install a Developer ID Application cert" >&2
  exit 1
fi
echo "signing identity: $IDENTITY"

# Toolchain: CommandLineTools swiftc may not match its SDK (seen: CLT swift
# 6.3.3 vs MacOSX.sdk 6.4 → "SDK is not supported by the compiler"). Prefer
# Xcode's swiftc with its matching SDK when Xcode is installed.
XC_SW="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc"
XC_SDK="$(ls -d /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk 2>/dev/null | tail -1)"
SWIFT_FLAGS=()
if [ -x "$XC_SW" ] && [ -n "$XC_SDK" ]; then
  SWIFT=("$XC_SW")
  # Explicit target: without it the binary inherits the SDK's minimum
  # (seen: "requires conditional 28.0" on macOS 27 → kLSIncompatibleSystemVersionErr).
  SWIFT_FLAGS=(-sdk "$XC_SDK" -target arm64-apple-macos12.0)
else
  SWIFT=(swiftc)
  SWIFT_FLAGS=(-target arm64-apple-macos12.0)
fi

# Must include lowercase main.swift (explicit NSApplication.delegate).
# @main + NSApplicationMain does NOT create the status item unless
# NSPrincipalClass is set — shipping dist previously launched empty.
"${SWIFT[@]}" -O \
  "${SWIFT_FLAGS[@]}" \
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

# Icon: build AppIcon.icns from source when not shipped (offline, deterministic).
if [ ! -f "$SRC/AppIcon.icns" ]; then
  SWIFT="$(xcrun --find swiftc 2>/dev/null || true)"
  if [ -z "$SWIFT" ]; then
    SWIFT="$(ls /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/swiftc 2>/dev/null | head -1)"
  fi
  SDK="$(ls -d /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk 2>/dev/null | tail -1)"
  if [ -n "$SWIFT" ] && [ -n "$SDK" ]; then
    "$SWIFT" -O -sdk "$SDK" "$SRC/gen-icon.swift" -o "$STAGE/gen-icon" \
      && "$STAGE/gen-icon" "$STAGE/AppIcon.iconset" \
      && iconutil -c icns "$STAGE/AppIcon.iconset" -o "$SRC/AppIcon.icns" || true
  fi
fi
if [ -f "$SRC/AppIcon.icns" ]; then
  cp "$SRC/AppIcon.icns" "$DEST/Contents/Resources/AppIcon.icns"
fi
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
