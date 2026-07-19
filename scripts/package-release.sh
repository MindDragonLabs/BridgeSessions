#!/usr/bin/env bash
# Build a deterministic source archive from a git commit.
#
# Release mode (--release) is strict: the working tree must be clean, no
# untracked files may be present, and HEAD must be exactly v2.0.6.
# For tests/development use --commit <sha> to archive an arbitrary commit.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$(tr -d '\r\n' < VERSION)"
TAG="v${VERSION}"
PREFIX="bridgesessions-${VERSION}/"
DIST="$ROOT/dist"
ARCHIVE="$DIST/bridgesessions-${VERSION}-source.tar.gz"
ZIP_ARCHIVE="$DIST/bridgesessions-${VERSION}-source.zip"

RELEASE_MODE=0
COMMIT_OVERRIDE=""

usage() {
  cat <<'EOF'
Usage: package-release.sh [OPTIONS]

Options:
  --release          Strict release mode: clean tree, no untracked files,
                     HEAD must be exactly v2.0.6.
  --commit <sha>     Archive a specific commit (dev/test override).
  -h, --help         Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --release)
      RELEASE_MODE=1
      shift
      ;;
    --commit)
      if [[ $# -lt 2 ]]; then
        printf 'error: --commit requires a SHA argument\n' >&2
        exit 1
      fi
      COMMIT_OVERRIDE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'error: unknown option %s\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$RELEASE_MODE" -eq 1 && -n "$COMMIT_OVERRIDE" ]]; then
  printf 'error: --commit cannot be used with --release\n' >&2
  exit 1
fi

ref="${COMMIT_OVERRIDE:-HEAD}"

if [[ "$RELEASE_MODE" -eq 1 ]]; then
  if [[ "$VERSION" != "2.0.6" ]]; then
    printf 'error: release mode requires VERSION 2.0.6 (got %s)\n' "$VERSION" >&2
    exit 1
  fi

  exact_tag="$(git describe --exact-match --tags HEAD 2>/dev/null || true)"
  if [[ "$exact_tag" != "$TAG" ]]; then
    printf 'error: release mode requires HEAD tag to be exactly %s (got %s)\n' \
      "$TAG" "${exact_tag:-<no exact tag>}" >&2
    exit 1
  fi

  if ! git diff-index --quiet HEAD -- || ! git diff-files --quiet; then
    printf 'error: dirty working tree\n' >&2
    exit 1
  fi

  if git ls-files --others --exclude-standard | grep -q .; then
    printf 'error: untracked files present\n' >&2
    git ls-files --others --exclude-standard >&2
    exit 1
  fi
fi

mkdir -p "$DIST"

# Deterministic archive: git archive produces a tar from the given commit,
# gzip -n strips timestamp/filename noise from the gzip header.
LC_ALL=C git archive --format=tar --prefix="$PREFIX" "$ref" | gzip -n > "$ARCHIVE"
LC_ALL=C git archive --format=zip --prefix="$PREFIX" "$ref" > "$ZIP_ARCHIVE"

tar -tzf "$ARCHIVE" >/dev/null
python3 - "$ZIP_ARCHIVE" <<'PY'
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as archive:
    if archive.testzip() is not None:
        raise SystemExit("invalid zip member")
PY
printf 'Wrote %s\n' "$ARCHIVE"
printf 'Wrote %s\n' "$ZIP_ARCHIVE"
