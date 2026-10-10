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
# Dereference annotated tags (^{commit}) — ls-remote returns the tag object
# ID for annotated tags, which is never equal to HEAD itself.
HEAD_COMMIT="$(git rev-parse HEAD)"
[[ "$HEAD_COMMIT" == "$(git rev-list -n1 "$TAG")" ]] || { echo 'error: tag != HEAD' >&2; exit 1; }
REMOTE_TAG="$(git ls-remote origin "refs/tags/$TAG" | awk '{print $1}')"
[[ "$(git rev-parse "${REMOTE_TAG}^{commit}" 2>/dev/null || echo "$REMOTE_TAG")" == "$HEAD_COMMIT" ]] || { echo 'error: origin tag != HEAD' >&2; exit 1; }
assets=(
  "$ASSET_DIR/bridgesessions-linux-x86_64"
  "$ASSET_DIR/bridgesessions-macos-arm64"
  "$ASSET_DIR/bridgesessions-windows-x86_64.exe"
  "$ASSET_DIR/bs_tray.ps1"
  "$ASSET_DIR/bridgesessions-${VERSION}-source.tar.gz"
  "$ASSET_DIR/bridgesessions-${VERSION}-source.zip"
  "$ASSET_DIR/SHA256SUMS"
  "$ASSET_DIR/SBOM-binaries.json"
)
for asset in "${assets[@]}"; do [[ -f "$asset" ]] || { echo "error: missing $asset" >&2; exit 1; }; done
# Refuse staging that changed after its manifest was generated, then validate
# formats and embedded versions and regenerate the deterministic SBOM/sums.
(cd "$ASSET_DIR" && sha256sum --check --strict SHA256SUMS >/dev/null)
BS_RELEASE_DIR="$ASSET_DIR" bash "$ROOT/scripts/release-checksums.sh"
gh api user --jq .login >/dev/null
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then echo "error: release exists" >&2; exit 1; fi
# Pre-release policy: any version carrying a suffix (-rN, -aN, -betaN) is a
# beta and must not become the Latest release (goal: upgrade clients must not
# pick up a beta). Plain versions (26.09.28) ship as Latest.
EXTRA=()
if [[ "$VERSION" == *-* ]]; then
  EXTRA=(--prerelease)
else
  EXTRA=(--latest)
fi
gh release create "$TAG" "${assets[@]}" --repo "$REPO" --verify-tag "${EXTRA[@]}" --title "BridgeSessions $VERSION" --generate-notes
gh release view "$TAG" --repo "$REPO" --json url,tagName,isPrerelease,assets
