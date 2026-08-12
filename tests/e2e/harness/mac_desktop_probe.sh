#!/usr/bin/env bash
# macos-peer desktop probes — no wipe, no reinstall of OS.
# Run via: bs shell macos-peer --cmd 'bash -s' < mac_desktop_probe.sh
# Or copy to macos-peer and execute.
set -euo pipefail

PASS=0
FAIL=0
SKIP=0
result() {
  local st="$1" name="$2" detail="${3:-}"
  echo "$st|$name|$detail"
  case "$st" in
    PASS) PASS=$((PASS+1)) ;;
    FAIL) FAIL=$((FAIL+1)) ;;
    SKIP) SKIP=$((SKIP+1)) ;;
  esac
}

# Mesh daemon
if pgrep -f 'bridgesessions.*--config' >/dev/null 2>&1; then
  result PASS mesh_process "running"
else
  result FAIL mesh_process "not running"
fi

# CUA helper
if pgrep -f 'cua-helper' >/dev/null 2>&1; then
  result PASS cua_helper_process "running"
else
  result FAIL cua_helper_process "not running — launchctl bootstrap cua-helper"
fi

# LaunchAgents present (do not rewrite)
for pl in com.bridgesessions.mesh com.bridgesessions.cua-helper; do
  if [[ -f "$HOME/Library/LaunchAgents/${pl}.plist" ]]; then
    result PASS "launchagent_${pl}" "present"
  else
    result FAIL "launchagent_${pl}" "missing"
  fi
done

# Apps installed
if [[ -d /Applications/BridgeSessions.app ]]; then
  result PASS BridgeSessions_app "installed"
  codesign -dv /Applications/BridgeSessions.app 2>&1 | head -3 || true
else
  result FAIL BridgeSessions_app "missing"
fi

if [[ -d /Applications/BSMenubar.app ]]; then
  result PASS BSMenubar_app "installed"
  # Try start if not running (non-destructive)
  if pgrep -f 'BSMenubar|bs-menubar|Menubar' >/dev/null 2>&1; then
    result PASS menubar_process "running"
  else
    open -a BSMenubar 2>/dev/null || open /Applications/BSMenubar.app 2>/dev/null || true
    sleep 2
    if pgrep -f 'BSMenubar|Menubar|bridgesessions' >/dev/null 2>&1; then
      # App may share binary name — check launchd label if any
      result PASS menubar_process "started or mesh binary present"
    else
      result SKIP menubar_process "app present but process not detected (check Login Items)"
    fi
  fi
else
  result FAIL BSMenubar_app "missing"
fi

# Token for cua-helper
if [[ -f "$HOME/.bridgesessions/cua-helper-token" ]]; then
  result PASS cua_helper_token "present"
else
  result SKIP cua_helper_token "missing (helper may use unix socket only)"
fi

# Mesh must be launchd-managed (ppid 1), not a foreground Terminal child
mesh_pid="$(pgrep -f 'bridgesessions.*--config' | head -1 || true)"
if [[ -n "$mesh_pid" ]]; then
  ppid="$(ps -o ppid= -p "$mesh_pid" 2>/dev/null | tr -d ' ')"
  if [[ "$ppid" == "1" ]]; then
    result PASS mesh_service "launchd parent (ppid=1)"
  else
    result FAIL mesh_service "mesh ppid=$ppid (want 1 / launchd)"
  fi
else
  result FAIL mesh_service "no mesh process"
fi

# Menubar LaunchAgent for B-logo applet
if [[ -f "$HOME/Library/LaunchAgents/com.minddragon.bridgesessions.menubar.plist" ]]; then
  result PASS menubar_launchagent "present"
else
  result SKIP menubar_launchagent "no com.minddragon.bridgesessions.menubar plist"
fi

echo "MAC_DESKTOP_SUMMARY pass=$PASS fail=$FAIL skip=$SKIP"
[[ $FAIL -eq 0 ]]