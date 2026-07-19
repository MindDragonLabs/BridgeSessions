#!/usr/bin/env bash
# Create a Codeberg (Forgejo) release and upload dist assets.
#
# Git push uses SSH key deploy-key (git core.sshCommand / Host codeberg.org).
# Attaching release assets requires a personal access token with repo write:
#   export FORGEJO_TOKEN=...
#   # or: ~/.vault/forgejo.token (mode 0600)
#
# Usage: ./scripts/codeberg-release.sh [--dry-run] [--draft-only] [TAG]
#
# The script is draft-first: create a draft release, upload assets, then PATCH
# it to published. Existing assets are never overwritten or deleted.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$(tr -d '\r\n' < VERSION)"
TAG="v${VERSION}"
DRY_RUN=0
DRAFT_ONLY=0

usage() {
  cat <<'EOF'
Usage: codeberg-release.sh [--dry-run] [--draft-only] [TAG]

Environment:
  CODEBERG_OWNER    default: Mind-Dragon
  CODEBERG_REPO     default: BridgeSessions
  CODEBERG_API      default: https://codeberg.org/api/v1
  CODEBERG_REMOTE   git remote used for tag verification, default: codeberg
  FORGEJO_TOKEN     API token (or ~/.vault/forgejo.token)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --draft-only)
      DRAFT_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      printf 'error: unknown option %s\n' "$1" >&2
      usage >&2
      exit 1
      ;;
    *)
      TAG="$1"
      shift
      ;;
  esac
done

if [[ "$TAG" != "v${VERSION}" ]]; then
  printf 'error: requested tag %s does not match VERSION %s\n' "$TAG" "$VERSION" >&2
  exit 1
fi

OWNER="${CODEBERG_OWNER:-Mind-Dragon}"
REPO="${CODEBERG_REPO:-BridgeSessions}"
API="${CODEBERG_API:-https://codeberg.org/api/v1}"
REMOTE="${CODEBERG_REMOTE:-codeberg}"
NOTES="docs/RELEASE-NOTES-${TAG#v}.md"

if [[ ! -f "$NOTES" ]]; then
  printf 'error: missing notes file %s\n' "$NOTES" >&2
  exit 1
fi

# ── Authenticate (skipped in dry-run so tests need no token) ──────────
auth=()
if [[ "$DRY_RUN" -eq 0 ]]; then
  if [[ -n "${FORGEJO_TOKEN:-}" ]]; then
    TOKEN="$FORGEJO_TOKEN"
  elif [[ -f "${HOME}/.vault/forgejo.token" ]]; then
    TOKEN="$(tr -d '\r\n' <"${HOME}/.vault/forgejo.token")"
  else
    printf 'Need FORGEJO_TOKEN or ~/.vault/forgejo.token\n' >&2
    printf 'SSH deploy-key can push tags/commits; it cannot attach release files.\n' >&2
    printf 'Codeberg → Settings → Applications → Generate New Token (repo write).\n' >&2
    exit 1
  fi
  auth_header="Authorization: token "
  auth=( -H "${auth_header}${TOKEN}" )

  me="$(curl --fail-with-body -sS "${auth[@]}" "${API}/user")"
  login="$(printf '%s' "$me" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("login") or "")' 2>/dev/null || true)"
  if [[ -z "$login" ]]; then
    printf 'API auth failed (token invalid/expired). Response head:\n%s\n' \
      "$(printf '%s' "$me" | head -c 240)" >&2
    exit 1
  fi
  printf 'authenticated as %s\n' "$login"
fi

# ── Verify local and remote tag point at the same commit ──────────────
if ! git show-ref --verify --quiet "refs/tags/${TAG}"; then
  printf 'error: local tag %s not found\n' "$TAG" >&2
  exit 1
fi
local_commit="$(git rev-parse "${TAG}^{}")"

remote_commit=""
remote_info="$(git ls-remote --tags "$REMOTE" \
  "refs/tags/${TAG}" "refs/tags/${TAG}^{}" 2>/dev/null || true)"
if [[ -n "$remote_info" ]]; then
  remote_commit="$(printf '%s\n' "$remote_info" | python3 -c '
import sys
tag=sys.argv[1]
rows=[line.split() for line in sys.stdin if line.strip()]
peeled=next((sha for sha,ref in rows if ref==f"refs/tags/{tag}^{{}}"), "")
direct=next((sha for sha,ref in rows if ref==f"refs/tags/{tag}"), "")
print(peeled or direct)
' "$TAG")"
fi

if [[ -z "$remote_commit" ]]; then
  printf 'error: remote tag %s not found on %s\n' "$TAG" "$REMOTE" >&2
  exit 1
elif [[ "$local_commit" != "$remote_commit" ]]; then
  printf 'error: local tag %s commit %s does not match remote %s\n' \
    "$TAG" "$local_commit" "$remote_commit" >&2
  exit 1
else
  printf 'verified local and remote tag %s at %s\n' "$TAG" "$local_commit"
fi

# ── Discover release assets dynamically ───────────────────────────────
shopt -s nullglob
raw_assets=(
  "dist/bridgesessions-${TAG#v}-"*
  dist/bridgesessions
  dist/bridgesessions-linux-x86_64
  dist/bridgesessions-macos-arm64
  dist/bridgesessions-windows-x86_64.exe
  dist/SHA256SUMS
  dist/SBOM-binaries.json
)
assets=()
for f in "${raw_assets[@]}"; do
  [[ -f "$f" ]] || continue
  assets+=("$f")
done

if [[ ${#assets[@]} -eq 0 ]]; then
  printf 'error: no release assets found in dist/ for %s\n' "$TAG" >&2
  exit 1
fi

# Sort for deterministic output.
IFS=$'\n' mapfile -t assets < <(printf '%s\n' "${assets[@]}" | LC_ALL=C sort -u)

if [[ "$DRY_RUN" -eq 1 ]]; then
  printf 'dry-run: create draft release %s for %s/%s\n' "$TAG" "$OWNER" "$REPO"
  printf 'dry-run: notes %s\n' "$NOTES"
  for f in "${assets[@]}"; do
    printf 'dry-run: upload %s\n' "$f"
  done
  if [[ "$DRAFT_ONLY" -eq 1 ]]; then
    printf 'dry-run: leave release %s as draft\n' "$TAG"
  else
    printf 'dry-run: publish release %s\n' "$TAG"
  fi
  exit 0
fi

# ── Build draft release payload ───────────────────────────────────────
payload="$(python3 - "$TAG" "$NOTES" "$local_commit" <<'PY'
import json, pathlib, sys
tag, notes_path, commit = sys.argv[1], sys.argv[2], sys.argv[3]
body = pathlib.Path(notes_path).read_text()
print(json.dumps({
    "tag_name": tag,
    "target_commitish": commit,
    "name": f"BridgeSessions {tag.lstrip('v')}",
    "body": body,
    "draft": True,
    "prerelease": True,
}))
PY
)"

# ── Create draft release ──────────────────────────────────────────────
resp="$(curl --fail-with-body -sS -X POST "${auth[@]}" -H 'Content-Type: application/json' \
  "${API}/repos/${OWNER}/${REPO}/releases" -d "$payload")"
REL_ID="$(printf '%s' "$resp" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("id") or "")' 2>/dev/null || true)"

if [[ -z "$REL_ID" ]]; then
  printf 'could not create release for %s\n%s\n' "$TAG" "$(printf '%s' "$resp" | head -c 400)" >&2
  exit 1
fi
printf 'release id %s (draft)\n' "$REL_ID"

# ── Upload assets; never mutate a published asset ─────────────────────
for f in "${assets[@]}"; do
  name="$(basename "$f")"
  encoded_name="$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$name")"
  printf 'upload %s\n' "$name"

  existing_resp="$(curl --fail-with-body -sS "${auth[@]}" \
    "${API}/repos/${OWNER}/${REPO}/releases/${REL_ID}/assets")"
  aid="$(printf '%s' "$existing_resp" | N="$name" python3 -c 'import sys,json,os
name=os.environ["N"]
try:
  rs=json.load(sys.stdin)
  print(next((str(a["id"]) for a in rs if isinstance(a,dict) and a.get("name")==name), ""))
except Exception:
  print("")')"

  if [[ -n "$aid" ]]; then
    printf 'error: asset %s already exists (id %s); refusing to mutate published assets\n' \
      "$name" "$aid" >&2
    exit 1
  fi

  upload_resp="$(curl --fail-with-body -sS -X POST "${auth[@]}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"$f" \
    "${API}/repos/${OWNER}/${REPO}/releases/${REL_ID}/assets?name=${encoded_name}")"
  download_url="$(printf '%s' "$upload_resp" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("browser_download_url") or "")')"
  if [[ -z "$download_url" ]]; then
    printf 'error: upload response for %s had no download URL\n' "$name" >&2
    exit 1
  fi
  verify_tmp="$(mktemp)"
  if ! curl --fail-with-body -sS -L "${auth[@]}" "$download_url" -o "$verify_tmp"; then
    rm -f "$verify_tmp"
    printf 'error: could not download uploaded asset %s for verification\n' "$name" >&2
    exit 1
  fi
  if ! cmp -s "$f" "$verify_tmp"; then
    rm -f "$verify_tmp"
    printf 'error: uploaded asset %s does not match local bytes\n' "$name" >&2
    exit 1
  fi
  rm -f "$verify_tmp"
done

if [[ "$DRAFT_ONLY" -eq 1 ]]; then
  printf 'OK draft staged: https://codeberg.org/%s/%s/releases/edit/%s\n' \
    "$OWNER" "$REPO" "$REL_ID"
  exit 0
fi

# ── Publish the release ───────────────────────────────────────────────
curl --fail-with-body -sS -X PATCH "${auth[@]}" -H 'Content-Type: application/json' \
  "${API}/repos/${OWNER}/${REPO}/releases/${REL_ID}" -d '{"draft":false}' >/dev/null

printf 'OK https://codeberg.org/%s/%s/releases/tag/%s\n' "$OWNER" "$REPO" "$TAG"
