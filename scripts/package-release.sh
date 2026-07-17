#!/usr/bin/env bash
# Build a deterministic source archive from the current working tree.
# This does not publish, tag, sign, or modify git history.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '\r\n' < "$ROOT/VERSION")"
DIST="$ROOT/dist"
PREFIX="bridgesessions-${VERSION}/"
ARCHIVE="$DIST/bridgesessions-${VERSION}-source.tar.gz"
MANIFEST="$(mktemp)"
trap 'rm -f "$MANIFEST" "$MANIFEST.sorted"' EXIT

cd "$ROOT"
mkdir -p "$DIST"

while IFS= read -r -d '' file; do
  [[ -e "$file" || -L "$file" ]] || continue
  attribute="$(git check-attr export-ignore -- "$file")"
  [[ "$attribute" == *': set' ]] && continue
  base="${file##*/}"
  case "$base" in
    .env|.env.*|authorized_keys|authorized_keys.*|id_ed25519|id_ed25519.*|known_hosts|known_servers)
      printf 'refusing secret-like release path: %s\n' "$file" >&2
      exit 1
      ;;
  esac
  printf '%s\0' "$file" >> "$MANIFEST"
done < <(git ls-files --cached --others --exclude-standard -z)

LC_ALL=C sort -z -u "$MANIFEST" > "$MANIFEST.sorted"
tar --create --null --files-from="$MANIFEST.sorted" \
    --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    --transform="flags=r;s,^,${PREFIX}," \
    --format=posix \
  | gzip -n > "$ARCHIVE"

tar -tzf "$ARCHIVE" >/dev/null
printf 'Wrote %s\n' "$ARCHIVE"
