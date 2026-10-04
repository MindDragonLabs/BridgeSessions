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
# PEM armor. The dashes distinguish a real block from prose about PEMs. Matching
# spans lines: a real key has BEGIN and END on separate lines, so a line-based
# grep cannot see it as one match. -z reads the whole file as one NUL-terminated
# record and [^-]* spans the body.
#
# The body class matters. A bare [^-]* is too loose: bs-codec.h holds the
# literals "-----BEGIN PRIVATE KEY-----" and "-----END PRIVATE KEY-----" as the
# pair it redacts logs down to, and anything dash-free between them (a C++
# statement, a comment) satisfies [^-]* — so a whole-file -z match would flag
# the product's own redaction code as a leaked key. A real key's body is
# base64 (A-Za-z0-9+/ with = padding); requiring that costs no real detection
# and leaves the redaction literals untouched.
PEM_PAT='-----BEGIN [A-Z ]*PRIVATE KEY-----[A-Za-z0-9+/=[:space:]]{16,}-----END [A-Z ]*PRIVATE KEY-----'

# ── BLOCK tier: operator network blocklist (PRIVATE overlay) ─────
# Anything used on the operator's network, by name or IP. Generated from the
# private fleet directory by gen-publish-blocklist.py; lives outside the repo
# because the list itself maps the network. Matching is case-insensitive;
# <angle-bracket> placeholders are stripped before matching. Test dummies
# (TEST-PC1, 192.168.1.x, RFC5737 TEST-NET) match nothing and are permitted.
#
# Two sub-tiers, because a hostname and an address are not the same risk:
#
#   ADDRESS tier — IPs, LAN segments, the tailnet identifier, and the full
#     identity of a specific machine. These say "here is my network" and are
#     BLOCKed in every file type, without exception.
#
#   CODENAME tier — short generic fleet labels (fecv3, macmini, btcr) that
#     also occur as ordinary words, product terms, and test fixtures. Naming
#     one in a code comment or a test literal discloses no route to a host;
#     the same string in prose implies a fleet layout. So: BLOCK in prose
#     (.md/.txt/.rst), WARN in code. Never silently dropped — a code hit still
#     prints, and still needs a human to look at it.
BLOCKLIST="${BS_PUBLISH_BLOCKLIST:-$HOME/.config/bridgesessions/publish-blocklist}"
ALLOWLIST="${BS_PUBLISH_ALLOWLIST:-$HOME/.config/bridgesessions/publish-allowlist}"

# Files where a codename is treated as prose (BLOCK) rather than code (WARN).
PROSE_PAT='\.(md|txt|rst|adoc)$'
# Code + fixtures, where a codename is demoted to WARN.
CODE_PAT='\.(c|h|hpp|cc|cpp|py|sh|bash|ps1|js|ts|json|ya?ml|cmake|in|am|ac)$|^CMakeLists\.txt$|^Makefile$'

# An ADDRESS pattern is one that can only be an address, never a codename.
# Patterns are matched as EREs, so metacharacters are common; the test below
# is deliberately conservative — anything ambiguous counts as a codename,
# which demotes it to advisory rather than wrongly blocking a comment.
is_address_pat() {
  local p="$1"
  # A real address always contains a digit, a dot, or an escaped dot.
  case "$p" in
    *\\.*)                       return 0 ;;   # escaped dot: 100\.112\.1\.1
    *[0-9].[0-9]*)               return 0 ;;   # literal 100.112
    tail[0-9a-z]*|tailnet*)      return 0 ;;   # tailnet identifier
    ShadowPC-*|shadowpc*)        return 0 ;;   # specific machine identity
    nunn-shadow*|Shadow-*)       return 0 ;;
    jeffersons-*|cpanel-xenovira|btcr-mag40)  return 0 ;;
    shadow-[a-z0-9]*|mysqlknights|xenovira)  return 0 ;;
    *)                           return 1 ;;
  esac
}

# ── WARN tier ────────────────────────────────────────────────────
WARN_PAT='password|ipc-token|api[_-]?key|BEGIN [A-Z ]*PRIVATE KEY'

# Paths never scanned: release binaries and the scanner itself
EXCL='^(dist/|scripts/prepublish-scan\.sh|scripts/pre-push\.hook)'

scan_files() {
  local label="$1"; shift
  local files="$1"; shift
  files=$(echo "$files" | grep -vE "$EXCL" || true)
  [ -z "$files" ] && return 0
  local hits
  # NOTE: `--` before the pattern is load-bearing, not decoration. Without it,
  # xargs splits the pattern on whitespace and any pattern containing a space
  # (PEM_PAT has one: "BEGIN [A-Z ]*PRIVATE KEY") is silently mangled into two
  # arguments, so the match never happens and grep exits non-zero into `|| true`.
  # That is how a private key in a tracked file could pass the gate unnoticed.
  # The blocklist loop below does not use xargs for the same reason.
  hits=$(echo "$files" | xargs grep -lnE -- "$IP_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: tailnet/overlay IPs in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE -- "$TSNET_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: ts.net tailnet hostname in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  hits=$(echo "$files" | xargs grep -lnE -- "$AUTH_TUPLE_PAT" 2>/dev/null || true)
  [ -n "$hits" ] && { echo "BLOCK: credential tuple auth=('u','p') in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  # -z: match across lines, since a real PEM's BEGIN and END are on separate
  # lines and a line-based grep cannot see them as one pattern.
  hits=$(echo "$files" | xargs grep -lznE -- "$PEM_PAT" 2>/dev/null | tr '\0' '\n' || true)
  [ -n "$hits" ] && { echo "BLOCK: PEM private-key block in $label:"; echo "$hits" | sed 's/^/  /'; FAILED=1; }
  if [ -f "$BLOCKLIST" ]; then
    while IFS= read -r pat; do
      case "$pat" in ''|'#'*) continue ;; esac
      addr=0; is_address_pat "$pat" && addr=1
      hits=""; soft=""
      while IFS= read -r f; do
        [ -f "$f" ] || continue
        # Case-sensitive: blocklist entries are operator hostnames (always
        # lowercase). Case-insensitive matching false-positives on strings
        # like "Visual Studio" that merely contain a blocked token.
        sed 's/<[^>]*>//g' "$f" 2>/dev/null | grep -qE -- "$pat" || continue
        if [ "$addr" -eq 1 ]; then hits="$hits$f\n"; continue; fi
        # CODENAME: fatal in prose, advisory in code.
        if echo "$f" | grep -qE "$PROSE_PAT"; then hits="$hits$f\n"
        else soft="$soft$f\n"; fi
      done <<< "$files"
      hits=$(printf '%b' "$hits" | sed '/^$/d')
      soft=$(printf '%b' "$soft" | sed '/^$/d')
      if [ -n "$hits" ]; then
        if [ "$addr" -eq 1 ]; then
          echo "BLOCK: network address [$pat] in $label:"
        else
          echo "BLOCK: network blocklist codename [$pat] in prose under $label:"
        fi
        echo "$hits" | sed 's/^/  /'; FAILED=1
      fi
      if [ -n "$soft" ]; then
        echo "WARN: codename [$pat] in code under $label (advisory, not blocking):"
        echo "$soft" | sed 's/^/  /'
      fi
    done < "$BLOCKLIST"
  fi
  hits=$(echo "$files" | xargs grep -lniE -- "$WARN_PAT" 2>/dev/null || true)
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
        # ALLOWLIST (PRIVATE overlay, lives outside the repo like the blocklist)
        # exempts legitimate baked-in strings — e.g. the Developer ID
        # certificate subject embedded in a codesign signature, which release
        # provenance REQUIRES. Matching strings lines are dropped before the
        # blocklist match. Build-path bake-in checks above stay fully active.
        s=$(strings "$b" 2>/dev/null)
        [ -f "$ALLOWLIST" ] && s=$(printf '%s\n' "$s" | grep -vE -f "$ALLOWLIST" || true)
        printf '%s\n' "$s" | grep -qiE -- "$pat" && { echo "BLOCK: network blocklist pattern [$pat] in binary $b"; FAILED=1; }
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
