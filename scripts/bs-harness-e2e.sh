#!/usr/bin/env bash
# BridgeSessions harness E2E probe
PEER="${1:?usage: $0 <peer>}"
BS="${BS:-bs}"
PASS=0 FAIL=0 SKIP=0

echo "=== Harness E2E: $PEER ==="

test_harness() {
  local name="$1" bin="$2"
  if [[ -z "$bin" ]]; then
    if output=$($BS shell "$PEER" --cmd 'echo HARNESS_SHELL_OK' 2>&1) && \
       echo "$output" | grep -q "HARNESS_SHELL_OK"; then
      echo "PASS $name → shell works"; PASS=$((PASS+1))
    else
      echo "FAIL $name → no output"; FAIL=$((FAIL+1))
    fi
    return
  fi

  local found
  found=$($BS shell "$PEER" --cmd "command -v $bin >/dev/null 2>&1 && echo FOUND || echo MISSING" 2>&1 | tr -d '\r' | tail -1)
  if [[ "$found" == "MISSING" ]]; then
    echo "SKIP $name ($bin) → not installed"; SKIP=$((SKIP+1)); return
  fi

  local output
  output=$($BS shell "$PEER" --cmd "$bin --version 2>&1 | head -1; echo EXITCODE:\$?" 2>&1 | tr -d '\r')
  if echo "$output" | grep -q "EXITCODE:0"; then
    local ver; ver=$(echo "$output" | grep -v "EXITCODE\|Using direct\|HERMES_HOME" | head -1)
    echo "PASS $name ($bin) → $ver"; PASS=$((PASS+1))
  else
    echo "FAIL $name ($bin) → non-zero exit"; FAIL=$((FAIL+1))
  fi
}

test_harness hermes hermes
test_harness claude-code claude
test_harness codex codex
test_harness opencode opencode
test_harness grok grok
test_harness copilot copilot
test_harness cursor cursor
test_harness kimi kimi
test_harness devin devin
test_harness shell ""

echo "---"
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[[ $FAIL -eq 0 ]]
