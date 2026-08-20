#!/usr/bin/env bash
# prepublish-scan.sh — fail-closed secrets/IP gate for public pushes.
# Two tiers: BLOCK (high-precision value shapes) fails the push;
# WARN (keyword-ish) prints for review but does not fail.
# Placeholder convention: values wrapped in <angle brackets> are exempt.
# Wired into .git/hooks/pre-push via scripts/pre-push.hook.
set -u

FAILED=0

# ── BLOCK tier: high-precision value shapes ──────────────────────
# Full CGNAT range — tailnet/overlay IPs are never public, regardless of subnet
IP_PAT='100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.[0-9]{1,3}\.[0-9]{1,3}'
# Real tailnet hostnames (ts.net with a concrete tail identifier)
TSNET_PAT='tail[0-9a-z]{6,}\.ts\.net'
# Quoted auth tuples with a real (non-placeholder) second value: auth=('u','p')
AUTH_TUPLE_PAT="auth=\\('[^']+','[^<][^']*'\\)"
# PEM armor (the dashes distinguish a real block from prose about PEMs)
PEM_PAT='-----BEGIN [A-Z ]*PRIVATE KEY-----'

# ── BLOCK tier: operator network blocklist (PRIVATE overlay) ─────
# Anything used on the operator's network, by name or IP. Generated from the
# private fleet directory by gen-publish-blocklist.py; lives outside the repo
# because the list itself maps the network. Matching is case-insensitive;
# <angle-bracket> placeholders are stripped before matching. Test dummies
# (TEST-PC1, 192.168.1.x, RFC5737 TEST-NET) match nothing and are permitted.
BLOCKLIST="${BS_PUBLISH_BLOCKLIST:-$HOME/.config/bridgesessions/publish-blocklist}"

# ── WARN tier ────────────────────────────────────────────────────
WARN_PAT='Year25careful|password|ipc-token|api[_-]?key|BEGIN [A-Z ]*PRIVATE KEY|fecv3|nunn-shadow|shadow-df8uluc8'

# Paths never scanned: release binaries and the scanner itself
EXCL='^(dist/|scripts/prepublish-scan\.sh|scripts/pre-push\.hook)'

scan_files() {
  local label="$1"; shift
  local files="$1"; shift
  files=$(echo "$files" | grep -vE "$EXCL" || true)
  [ -z "$files" ] && return 0
  local hits
  hits=$(echo "$files" | xargs grep -lnE "$IP_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: tailnet/overlay IPs in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE "$TSNET_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: ts.net tailnet hostname in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE "$AUTH_TUPLE_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: credential tuple auth=('u','p') in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE "$PEM_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: PEM private-key block in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  if [ -f "$BLOCKLIST" ]; then
    while IFS= read -r pat; do
      case "$pat" in ''|'#'*) continue ;; esac
      hits=""
      while IFS= read -r f; do
        [ -f "$f" ] || continue
        sed 's/<[^>]*>//g' "$f" 2>/dev/null | grep -qiE -- "$pat" && hits="$hits$f\n"
      done <<< "$files"
      hits=$(printf '%b' "$hits" | sed '/^$/d')
      [ -n "$hits" ] && { echo "BLOCK: network blocklist pattern [$pat] in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
    done < "$BLOCKLIST"
  fi
  hits=$(echo "$files" | xargs grep -lniE "$WARN_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "WARN: secret-adjacent keywords in $label (review, not blocked):"; echo "$hits" | sed 's/^/  /'; }
}

# ── dist/ binaries: strings-level leak check ──────────────────────
# Binaries are exempt from content grep (binary noise), but baked-in build
# paths (/home/<user>, /Users/<user>) and blocklisted names must not ship.
scan_dist() {
  local b hits
  for b in dist/*; do
    [ -f "$b" ] || continue
    case "$b" in *.json|*.txt|SHA256SUMS) continue ;; esac
    # Flag personal homes; allow generic CI/build accounts (agent, runner, builder, github).
    # Neutralized scrubbed names may carry a trailing-pad (builder__, builder2) to
    # preserve binary layout when a personal name is overwritten same-length.
    hits=$(strings "$b" 2>/dev/null | grep -E '/home/[a-z]+/|/Users/[a-z]+/'       | grep -Ev '/home/(agent|runner|builder|github|ubuntu|root)/|/Users/(builder[_0-9]*|runner)/'       | head -3 || true)
    [ -n "$hits" ] && { echo "BLOCK: personal build-path bake-in in $b:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
    if [ -f "$BLOCKLIST" ]; then
      while IFS= read -r pat; do
        case "$pat" in ''|'#'*) continue ;; esac
        strings "$b" 2>/dev/null | grep -qiE -- "$pat" && { echo "BLOCK: network blocklist pattern [$pat] in binary $b"; FAILED=1; }
      done < "$BLOCKLIST"
    fi
  done
}
scan_dist

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
