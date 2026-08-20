#!/usr/bin/env bash
# Publish verified local artifacts to the matching GitHub release.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
VERSION="$(tr -d '\r\n' < VERSION)"
TAG="v${VERSION}"
ASSET_DIR="${BS_RELEASE_DIR:-dist}"
REPO="${BS_GITHUB_REPO:-MindDragonLabs/BridgeSessions}"
[[ -z "$(git status --porcelain)" ]] || { echo 'error: dirty tree' >&2; exit 1; }
[[ "$(git rev-parse HEAD)" == "$(git rev-list -n1 "$TAG")" ]] || { echo 'error: tag != HEAD' >&2; exit 1; }
[[ "$(git ls-remote origin "refs/tags/$TAG" | awk '{print $1}')" == "$(git rev-parse HEAD)" ]] || { echo 'error: origin tag != HEAD' >&2; exit 1; }
gh api user --jq .login >/dev/null
assets=(
  "$ASSET_DIR/bridgesessions-linux-x86_64"
  "$ASSET_DIR/bridgesessions-macos-arm64"
  "$ASSET_DIR/bridgesessions-windows-x86_64.exe"
  "$ASSET_DIR/bridgesessions-${VERSION}-source.tar.gz"
  "$ASSET_DIR/bridgesessions-${VERSION}-source.zip"
  "$ASSET_DIR/SHA256SUMS"
  "$ASSET_DIR/SBOM-binaries.json"
)
for asset in "${assets[@]}"; do [[ -f "$asset" ]] || { echo "error: missing $asset" >&2; exit 1; }; done
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then echo "error: release exists" >&2; exit 1; fi
gh release create "$TAG" "${assets[@]}" --repo "$REPO" --verify-tag --prerelease --title "BridgeSessions $VERSION" --generate-notes
gh release view "$TAG" --repo "$REPO" --json url,tagName,assets
