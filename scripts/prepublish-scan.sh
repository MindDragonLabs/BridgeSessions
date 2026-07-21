#!/usr/bin/env bash
# prepublish-scan.sh — fail-closed secrets/IP gate for public pushes.
# Two tiers: BLOCK (high-precision value shapes) fails the push;
# WARN (keyword-ish) prints for review but does not fail.
# Placeholder convention: values wrapped in <angle brackets> are exempt.
# Wired into .git/hooks/pre-push via scripts/pre-push.hook.
set -u

FAILED=0

# ── BLOCK tier: high-precision value shapes ──────────────────────
# Fleet tailnet ranges (never public)
IP_PAT='100\.(112|84|126|127|115|73|102)\.[0-9]+'
# Quoted auth tuples with a real (non-placeholder) second value: auth=('u','p')
AUTH_TUPLE_PAT="auth=\\('[^']+','[^<][^']*'\\)"
# PEM armor (the dashes distinguish a real block from prose about PEMs)
PEM_PAT='-----BEGIN [A-Z ]*PRIVATE KEY-----'

# ── WARN tier ────────────────────────────────────────────────────
WARN_PAT='Year25careful|password|ipc-token|api[_-]?key|BEGIN [A-Z ]*PRIVATE KEY'

# Paths never scanned: release binaries and the scanner itself
EXCL='^(dist/|scripts/prepublish-scan\.sh|scripts/pre-push\.hook)'

scan_files() {
  local label="$1"; shift
  local files="$1"; shift
  files=$(echo "$files" | grep -vE "$EXCL" || true)
  [ -z "$files" ] && return 0
  local hits
  hits=$(echo "$files" | xargs grep -lnE "$IP_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: tailnet IPs in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE "$AUTH_TUPLE_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: credential tuple auth=('u','p') in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE "$PEM_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: PEM private-key block in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lniE "$WARN_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "WARN: secret-adjacent keywords in $label (review, not blocked):"; echo "$hits" | sed 's/^/  /'; }
}

# 1) tracked tip
scan_files "tracked tip" "$(git ls-files)"

# 2) files changed since last public tag
LAST_TAG=$(git tag -l 'v*' --sort=-creatordate | head -1)
if [ -n "$LAST_TAG" ] && git rev-parse "$LAST_TAG" >/dev/null 2>&1; then
  CHANGED=$(git diff --name-only "$LAST_TAG"..HEAD -- 2>/dev/null || true)
  scan_files "delta since $LAST_TAG" "$CHANGED"
fi

if [ "$FAILED" -eq 1 ]; then
  echo
  echo "prepublish-scan: FAILED — scrub to <placeholders> before pushing."
  exit 1
fi
echo "prepublish-scan: clean"
exit 0
