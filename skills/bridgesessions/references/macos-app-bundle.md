# macOS app bundle: identity, build, sign, launch, verify

Recipe for the BSMenubar menubar companion app; generalizes to any macOS app in this
repo. Triggers: "app shows the wrong name/version", "app has a Dock icon but should be
menubar-only", "app built and signed but will not launch".

## Symptom map: bare executable vs real bundle

A `.app` that is only `Contents/MacOS/<bin>` with no Info.plist makes LaunchServices
invent identity: a guessed display name and version, `type="Foreground"` execution
(Dock icon), and default system requirements. Every identity complaint traces here
first — check `lsappinfo list | grep -A4 <name-or-bundle-id>` before touching code.

Minimum viable bundle:

```
App.app/
  Contents/Info.plist
  Contents/MacOS/<bin>
  Contents/Resources/AppIcon.icns
```

Info.plist keys that answer the four standard identity asks:

| Ask | Key |
|---|---|
| name | CFBundleName + CFBundleDisplayName |
| version | CFBundleShortVersionString (+ CFBundleVersion) |
| icon | CFBundleIconFile (name without extension) |
| menubar-only, no Dock | LSUIElement = true |
| launch floor | LSMinimumSystemVersion matching the `-target` below |

## Build

- Pin the Xcode toolchain: bare `swiftc` can resolve to CommandLineTools, whose SDK
  mismatches its compiler. Use `$(xcrun --find swiftc) -sdk $(xcrun --show-sdk-path)`.
- ALWAYS pass an explicit deployment target (`-target arm64-apple-macos12.0`). Without
  it the Xcode SDK stamps LC_BUILD_VERSION minos at the SDK's own floor — newer than
  the Macs in this fleet — and the app dies at launch with
  kLSIncompatibleSystemVersionErr (LS error -10825) while compile and codesign both
  look fine. Verify: `otool -l <bin> | grep -A3 LC_BUILD_VERSION`; minos must be <= the
  oldest target Mac.
- icns: render PNGs (AppKit script or `sips`), then `iconutil -c icns`. Commit the
  .icns so machines without the generator can still build the app.

## Sign

Sign with the stable local identity (`scripts/sign-local-stable.sh`), not adhoc —
an adhoc signature loses Screen Recording/Accessibility TCC on every upgrade. If a
locally built + signed app still refuses to launch, clear the Gatekeeper xattrs
(`xattr -dr com.apple.provenance <app>`; also `com.apple.quarantine` if present).

## Install and launch

1. `osascript -e 'quit app "..."'` the old instance; hard-kill stragglers — a bare
   binary with no bundle identity ignores AppleScript quit.
2. Kill ALL instances before relaunching; two copies produce confusing status-item
   state.
3. Swap the bundle, then launch with `open -a /Applications/<App>.app` — this
   validates the exact path the LaunchAgent/login item resolves.
4. `lsregister` is NOT on PATH. Use the full framework path:
   `/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f <app>`.

## Verify (evidence, in order)

```bash
lsappinfo list | grep -A4 <name>       # type="UIElement", Version=, bundle id
otool -l <bin> | grep -B1 -A3 minos    # floor <= oldest target Mac
osascript -e 'tell application "System Events" to get name of every process whose visible is true'
                                       # app absent from output = no Dock icon
screencapture -x -R<x,y,w,h> /tmp/mb.png   # read back with vision
```

Status items live on the RIGHT side of the menubar: capture a wide strip (>=1600px at
Retina). A narrow crop that lands left of the item gives a false "not visible" — only
conclude the item is missing from a capture that spans the full status cluster.

## Script pitfalls that cost a cycle

- `codesign ... | grep -q ...` under `pipefail` SIGPIPEs codesign (grep exits on first
  match; codesign gets EPIPE mid-write). Capture output into a variable, then grep.
- Automated edits can leave trailing whitespace AFTER a backslash, silently breaking a
  shell line continuation. After any scripted edit to a build script, run
  `grep -n ' \+$'` (and `grep -n '\\[[:space:]]*$'`) over it.
