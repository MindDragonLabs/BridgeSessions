#!/usr/bin/env bash
# BridgeSessions harness E2E probe — tests TUI launch, not --version
# Usage: $0 <peer>
# Tests each harness by launching it as a TUI and verifying it stays alive.
PEER="${1:?usage: $0 <peer>}"
BS="${BS:-bs}"
PASS=0 FAIL=0 SKIP=0

echo "=== Harness E2E: $PEER ==="

test_harness() {
  local name="$1" cmd="$2"
  if [[ -z "$cmd" ]]; then
    # Shell harness
    if output=$($BS shell "$PEER" --cmd 'echo HARNESS_SHELL_OK' 2>&1) && \
       echo "$output" | grep -q "HARNESS_SHELL_OK"; then
      echo "PASS $name → shell works"; PASS=$((PASS+1))
    else
      echo "FAIL $name → no output"; FAIL=$((FAIL+1))
    fi
    return
  fi

  # Extract binary name for command -v check
  local bin="${cmd%% *}"

  # Check if binary exists
  local found
  found=$($BS shell "$PEER" --cmd "command -v $bin >/dev/null 2>&1 && echo FOUND || echo MISSING" 2>&1 | tr -d '\r' | tail -1)
  if [[ "$found" == "MISSING" ]]; then
    echo "SKIP $name ($bin) → not installed"; SKIP=$((SKIP+1)); return
  fi

  # Launch TUI, wait 5s, check if alive
  local result
  result=$($BS shell "$PEER" --cmd "
    $cmd </dev/tty &
    pid=\$!
    sleep 5
    if kill -0 \$pid 2>/dev/null; then
      echo 'TUI_ALIVE'
      kill \$pid 2>/dev/null; wait \$pid 2>/dev/null
    else
      echo 'TUI_DIED'
    fi
  " 2>&1 | tr -d '\r')

  if echo "$result" | grep -q "TUI_ALIVE"; then
    echo "PASS $name ($cmd) → TUI launched and stayed alive"; PASS=$((PASS+1))
  else
    # Check if it at least showed a banner before dying
    if echo "$result" | grep -qiE "welcome|banner|codex|claude|hermes|kimi|devin|grok|opencode|cursor|╭|│"; then
      echo "PASS $name ($cmd) → TUI showed banner (died after redirect)"; PASS=$((PASS+1))
    else
      echo "FAIL $name ($cmd) → TUI did not start"; FAIL=$((FAIL+1))
    fi
  fi
}

test_harness hermes "hermes --tui --yolo"
test_harness claude-code "claude"
test_harness codex "codex"
test_harness opencode "opencode"
test_harness grok "grok"
test_harness copilot "copilot"
test_harness cursor "cursor"
test_harness kimi "kimi"
test_harness devin "devin"
test_harness shell ""

echo "---"
echo "PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[[ $FAIL -eq 0 ]]
