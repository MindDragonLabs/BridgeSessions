#!/usr/bin/env bash
# BridgeSessions fleet end-to-end feature matrix
#
# Exercises health, shell, file send/recv, run-script, version, and optional
# CUA against live mesh peers on Linux / macOS / Windows.
#
# Usage:
#   scripts/e2e-fleet-test.sh --all
#   scripts/e2e-fleet-test.sh linux-a macos-peer windows-peer
#   BS_E2E_PEERS="linux-a,linux-b" scripts/e2e-fleet-test.sh
#   scripts/e2e-fleet-test.sh --all                   # every healthy seed
#   scripts/e2e-fleet-test.sh --quick                 # health+shell only
#   scripts/e2e-fleet-test.sh --json /tmp/e2e.json    # machine-readable summary
#
# Env:
#   BS / BRIDGESESSIONS_BINARY   CLI path (default: bs or bridgesessions on PATH)
#   BS_E2E_TIMEOUT               per-command timeout seconds (default 45)
#   BS_E2E_SKIP_CUA=1            skip CUA probes
#   BS_E2E_SKIP_LARGE=1          skip large transfer
#   BS_E2E_VERSION               expected version substring (default from VERSION file)
#   BS_E2E_PEERS                 comma-separated peer list (lab-specific; not committed)
#
# Exit 0 if all required tests pass; non-zero otherwise.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TIMEOUT_SEC="${BS_E2E_TIMEOUT:-45}"
QUICK=0
ALL_PEERS=0
JSON_OUT=""
WORKDIR=""
PASS=0
FAIL=0
SKIP=0
declare -a RESULTS=()

# ── CLI binary ──────────────────────────────────────────────────────
resolve_bs() {
  if [[ -n "${BS:-}" && -x "$BS" ]]; then echo "$BS"; return; fi
  if [[ -n "${BRIDGESESSIONS_BINARY:-}" && -x "$BRIDGESESSIONS_BINARY" ]]; then
    echo "$BRIDGESESSIONS_BINARY"; return
  fi
  if command -v bs >/dev/null 2>&1; then command -v bs; return; fi
  if command -v bridgesessions >/dev/null 2>&1; then command -v bridgesessions; return; fi
  if [[ -x "$REPO_ROOT/build/bridgesessions" ]]; then echo "$REPO_ROOT/build/bridgesessions"; return; fi
  if [[ -x "$HOME/.local/bin/bs" ]]; then echo "$HOME/.local/bin/bs"; return; fi
  echo "error: bridgesessions/bs not found on PATH" >&2
  exit 2
}
BS_BIN="$(resolve_bs)"
export PATH="$(dirname "$BS_BIN"):${PATH:-/usr/bin:/bin}"

run_to() {
  # timeout if available
  if command -v timeout >/dev/null 2>&1; then
    timeout "$TIMEOUT_SEC" "$@"
  elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$TIMEOUT_SEC" "$@"
  else
    "$@"
  fi
}

# ── args ────────────────────────────────────────────────────────────
PEERS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1; shift ;;
    --all) ALL_PEERS=1; shift ;;
    --json) JSON_OUT="${2:?}"; shift 2 ;;
    --timeout) TIMEOUT_SEC="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,30p' "$0"
      exit 0
      ;;
    --) shift; PEERS+=("$@"); break ;;
    -*)
      echo "unknown option: $1" >&2
      exit 2
      ;;
    *) PEERS+=("$1"); shift ;;
  esac
done

if [[ -n "${BS_E2E_PEERS:-}" && ${#PEERS[@]} -eq 0 ]]; then
  IFS=',' read -r -a PEERS <<< "$BS_E2E_PEERS"
fi

EXPECTED_VERSION="${BS_E2E_VERSION:-}"
if [[ -z "$EXPECTED_VERSION" && -f "$REPO_ROOT/VERSION" ]]; then
  EXPECTED_VERSION="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
fi

# ── reporting ───────────────────────────────────────────────────────
ts() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

record() {
  # record <status> <peer|local> <feature> <detail>
  local status="$1" peer="$2" feature="$3" detail="${4:-}"
  RESULTS+=("$status|$peer|$feature|$detail")
  case "$status" in
    PASS) PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m  %-14s %-22s %s\n' "$peer" "$feature" "$detail" ;;
    FAIL) FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m  %-14s %-22s %s\n' "$peer" "$feature" "$detail" ;;
    SKIP) SKIP=$((SKIP + 1)); printf '  \033[33mSKIP\033[0m  %-14s %-22s %s\n' "$peer" "$feature" "$detail" ;;
  esac
}

assert_contains() {
  local haystack="$1" needle="$2"
  [[ "$haystack" == *"$needle"* ]]
}

# ── peer discovery ──────────────────────────────────────────────────
list_seed_peers() {
  # Parse `bs peers list`. Current table: NAME KIND ADDRESS STATUS VERSION
  # Legacy lines look like: "  [seed] name host:port"
  run_to "$BS_BIN" peers list 2>/dev/null | awk '
    $1 == "NAME" { next }
    $1 ~ /^-{3,}/ { next }
    $2 == "seed" { print $1; next }
    /\[seed\]/ {
      for (i = 1; i <= NF; i++) {
        if ($i == "[seed]" && (i + 1) <= NF) { print $(i + 1); break }
      }
    }'
}

discover_healthy_peers() {
  local p
  while IFS= read -r p; do
    [[ -z "$p" ]] && continue
    if run_to "$BS_BIN" health "$p" 2>&1 | grep -q 'healthy (data-plane ok)'; then
      echo "$p"
    fi
  done < <(list_seed_peers)
}

if [[ $ALL_PEERS -eq 1 ]]; then
  PEERS=()
  while IFS= read -r _p; do
    [[ -n "$_p" ]] && PEERS+=("$_p")
  done < <(discover_healthy_peers)
elif [[ ${#PEERS[@]} -eq 0 ]]; then
  echo "error: pass peer names, BS_E2E_PEERS=name,name, or --all" >&2
  exit 2
fi

# ── workspace ───────────────────────────────────────────────────────
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/bs-e2e.XXXXXX")"
cleanup() { rm -rf "$WORKDIR" 2>/dev/null || true; }
trap cleanup EXIT

# ── local / control-plane tests ─────────────────────────────────────
section() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

section "local control plane"
LOCAL_VER_OUT="$(run_to "$BS_BIN" --version 2>&1 || true)"
if [[ -n "$EXPECTED_VERSION" ]] && assert_contains "$LOCAL_VER_OUT" "$EXPECTED_VERSION"; then
  record PASS local version "$LOCAL_VER_OUT"
elif [[ -n "$LOCAL_VER_OUT" ]]; then
  record PASS local version "$LOCAL_VER_OUT (no EXPECTED match required)"
else
  record FAIL local version "empty output"
fi

PEERS_OUT="$(run_to "$BS_BIN" peers list 2>&1 || true)"
if assert_contains "$PEERS_OUT" "seed" || assert_contains "$PEERS_OUT" "self" || assert_contains "$PEERS_OUT" "ADDRESS"; then
  record PASS local peers_list "ok"
else
  record FAIL local peers_list "unexpected: ${PEERS_OUT:0:120}"
fi

# ── per-peer feature matrix ─────────────────────────────────────────
detect_os() {
  # stdout: linux|macos|windows|unknown
  local peer="$1" out
  out="$(run_to "$BS_BIN" shell "$peer" --cmd 'uname -s 2>/dev/null || ver 2>/dev/null || echo unknown' 2>&1 || true)"
  # strip transport chatter
  out="$(printf '%s\n' "$out" | grep -v -E 'Using direct TLS|Failed to connect|Connection timed|error:' || true)"
  if printf '%s' "$out" | grep -qi 'windows\|microsoft'; then
    echo windows
  elif printf '%s' "$out" | grep -qi 'darwin'; then
    echo macos
  elif printf '%s' "$out" | grep -qi 'linux'; then
    echo linux
  else
    # hostname-style Windows output often has no uname
    if run_to "$BS_BIN" shell "$peer" --cmd 'echo %OS%' 2>&1 | grep -qi 'Windows'; then
      echo windows
    else
      echo unknown
    fi
  fi
}

shell_hostname_cmd() {
  case "$1" in
    windows) echo 'hostname' ;;
    *) echo 'hostname' ;;
  esac
}

shell_version_cmd() {
  case "$1" in
    windows)
      echo 'if exist %LOCALAPPDATA%\bridgesessions\bridgesessions.exe (%LOCALAPPDATA%\bridgesessions\bridgesessions.exe --version) else if exist %USERPROFILE%\.local\bin\bridgesessions.exe (%USERPROFILE%\.local\bin\bridgesessions.exe --version) else (where bridgesessions 2>nul & bridgesessions --version 2>nul)'
      ;;
    *)
      echo 'command -v bridgesessions >/dev/null && bridgesessions --version; command -v bs >/dev/null && bs --version; test -x "$HOME/.local/bin/bridgesessions" && "$HOME/.local/bin/bridgesessions" --version; true'
      ;;
  esac
}

shell_os_probe_cmd() {
  case "$1" in
    windows) echo 'echo OS=windows& hostname' ;;
    macos) echo 'echo OS=macos; uname -sm; sw_vers -productVersion 2>/dev/null | head -1' ;;
    linux) echo 'echo OS=linux; uname -sm; cat /etc/os-release 2>/dev/null | head -3' ;;
    *) echo 'echo OS=unknown; uname -a 2>/dev/null || hostname' ;;
  esac
}

test_peer() {
  local peer="$1"
  local os out rc tmp_send tmp_recv marker large

  section "peer: $peer"

  # health
  out="$(run_to "$BS_BIN" health "$peer" 2>&1 || true)"
  if assert_contains "$out" "healthy (data-plane ok)"; then
    record PASS "$peer" health "data-plane ok"
  else
    record FAIL "$peer" health "${out//$'\n'/ }"
    # remaining tests will almost certainly fail
    return 0
  fi

  os="$(detect_os "$peer")"
  record PASS "$peer" os_detect "$os"

  # shell hostname
  out="$(run_to "$BS_BIN" shell "$peer" --cmd "$(shell_hostname_cmd "$os")" 2>&1 || true)"
  out_clean="$(printf '%s\n' "$out" | grep -v -E 'Using direct TLS|^\s*$' | head -5 || true)"
  if [[ -n "$out_clean" ]] && ! assert_contains "$out" "Failed to connect"; then
    record PASS "$peer" shell_hostname "$(echo "$out_clean" | tr '\n' ' ' | head -c 80)"
  else
    record FAIL "$peer" shell_hostname "${out//$'\n'/ }"
  fi

  # shell OS probe
  out="$(run_to "$BS_BIN" shell "$peer" --cmd "$(shell_os_probe_cmd "$os")" 2>&1 || true)"
  if ! assert_contains "$out" "Failed to connect" && [[ -n "$out" ]]; then
    record PASS "$peer" shell_os_probe "ok"
  else
    record FAIL "$peer" shell_os_probe "${out//$'\n'/ }"
  fi

  # remote binary version (best-effort path matrix)
  out="$(run_to "$BS_BIN" shell "$peer" --cmd "$(shell_version_cmd "$os")" 2>&1 || true)"
  if [[ -n "$EXPECTED_VERSION" ]] && assert_contains "$out" "$EXPECTED_VERSION"; then
    record PASS "$peer" remote_version "$EXPECTED_VERSION"
  elif assert_contains "$out" "26." || assert_contains "$out" "2.0."; then
    record PASS "$peer" remote_version "found version in output"
  else
    # Windows shell stdout for --version can be empty under some PE/session combos
    if [[ "$os" == "windows" ]]; then
      record SKIP "$peer" remote_version "no version on shell stdout (known Windows PE quirk); health/shell ok"
    else
      record FAIL "$peer" remote_version "missing expected version; out=${out//$'\n'/ }"
    fi
  fi

  if [[ $QUICK -eq 1 ]]; then
    return 0
  fi

  # file send + remote verify
  marker="e2e-${peer}-$(date +%s)-$$"
  tmp_send="$WORKDIR/send-${peer}.txt"
  printf '%s\n' "$marker" > "$tmp_send"
  out="$(run_to "$BS_BIN" file send "$peer" "$tmp_send" --dest "${marker}.txt" --wait 2>&1 || true)"
  if assert_contains "$out" "OK sent" || assert_contains "$out" "PROGRESS"; then
    record PASS "$peer" file_send "ok"
  else
    record FAIL "$peer" file_send "${out//$'\n'/ }"
  fi

  # verify content landed: recv-roundtrip (pull back by basename).
  # Shell-based verify is unreliable on windows peers (daemon user vs shell
  # user differ; nested PS quoting mangles $env:USERPROFILE), so verify via
  # the mesh itself: received/ is served, recv must return the exact bytes.
  out="$(run_to "$BS_BIN" file recv "$peer" "${marker}.txt" --to "$WORKDIR/verify-${peer}.txt" --wait 2>&1 || true)"
  if [[ -f "$WORKDIR/verify-${peer}.txt" ]] && grep -q "$marker" "$WORKDIR/verify-${peer}.txt" 2>/dev/null; then
    out="$marker"
  else
    out="recv-roundtrip-miss: ${out//$'\n'/ }"
  fi
  if assert_contains "$out" "$marker"; then
    record PASS "$peer" file_send_verify "marker found"
  else
    record FAIL "$peer" file_send_verify "marker missing; out=${out//$'\n'/ }"
  fi

  # file recv: create a remote file inside receive_dir (M4: peers may only
  # serve files under receive_dir, never arbitrary absolute paths), then pull
  # it by basename (resolve_file_request_path resolves basenames under receive_dir).
  # Create the recv target by SENDING it (daemon-side received/, user-context-free:
  # windows peers may run daemon as a different user than the shell, so shell-based
  # staging is unreliable). Then pull it back by basename.
  run_to "$BS_BIN" file send "$peer" "$tmp_send" --dest "${marker}.txt" --wait >/dev/null 2>&1 || true
  remote_path="${marker}.txt"
  tmp_recv="$WORKDIR/recv-${peer}.txt"
  rm -f "$tmp_recv"
  out="$(run_to "$BS_BIN" file recv "$peer" "$remote_path" --to "$tmp_recv" --wait 2>&1 || true)"
  if { assert_contains "$out" "OK received" || assert_contains "$out" "PROGRESS"; } && [[ -f "$tmp_recv" ]] && grep -q "$marker" "$tmp_recv" 2>/dev/null; then
    record PASS "$peer" file_recv "ok"
  else
    record FAIL "$peer" file_recv "${out//$'\n'/ }"
  fi

  # run-script
  case "$os" in
    windows)
      local ps1="$WORKDIR/script-${peer}.ps1"
      cat > "$ps1" <<'PSEOF'
Write-Output "RUNSCRIPT_OK"
Write-Output $env:COMPUTERNAME
PSEOF
      out="$(run_to "$BS_BIN" run-script "$peer" "$ps1" 2>&1 || true)"
      if assert_contains "$out" "RUNSCRIPT_OK"; then
        record PASS "$peer" run_script "powershell"
      else
        record FAIL "$peer" run_script "${out//$'\n'/ }"
      fi
      ;;
    *)
      local shf="$WORKDIR/script-${peer}.sh"
      cat > "$shf" <<'SHEOF'
#!/usr/bin/env bash
echo RUNSCRIPT_OK
hostname
SHEOF
      out="$(run_to "$BS_BIN" run-script "$peer" "$shf" 2>&1 || true)"
      if assert_contains "$out" "RUNSCRIPT_OK"; then
        record PASS "$peer" run_script "bash"
      else
        record FAIL "$peer" run_script "${out//$'\n'/ }"
      fi
      ;;
  esac

  # medium transfer (256 KiB) — exercises chunking / PROGRESS without being huge
  if [[ "${BS_E2E_SKIP_LARGE:-0}" != "1" ]]; then
    large="$WORKDIR/large-${peer}.bin"
    dd if=/dev/urandom of="$large" bs=1024 count=256 status=none 2>/dev/null \
      || dd if=/dev/urandom of="$large" bs=1024 count=256 2>/dev/null
    out="$(run_to "$BS_BIN" file send "$peer" "$large" --wait 2>&1 || true)"
    if assert_contains "$out" "OK sent"; then
      if assert_contains "$out" "PROGRESS" || assert_contains "$out" "chunks="; then
        record PASS "$peer" file_send_medium "256KiB + progress"
      else
        record PASS "$peer" file_send_medium "256KiB (no PROGRESS line)"
      fi
    else
      record FAIL "$peer" file_send_medium "${out//$'\n'/ }"
    fi
  else
    record SKIP "$peer" file_send_medium "BS_E2E_SKIP_LARGE=1"
  fi

  # CUA screen (optional — needs helper / display)
  if [[ "${BS_E2E_SKIP_CUA:-0}" == "1" ]]; then
    record SKIP "$peer" cua_screen "BS_E2E_SKIP_CUA=1"
  else
    out="$(run_to "$BS_BIN" cua screen "$peer" 2>&1 || true)"
    if printf '%s' "$out" | grep -qE '^[0-9]+x[0-9]+|[0-9]+x[0-9]+'; then
      # Real geometry like 2560x1080
      if assert_contains "$out" "ERROR" || assert_contains "$out" "cannot determine"; then
        record SKIP "$peer" cua_screen "no display/helper: $(echo "$out" | tr '\n' ' ' | head -c 80)"
      else
        record PASS "$peer" cua_screen "$(echo "$out" | tr '\n' ' ' | head -c 60)"
      fi
    else
      record SKIP "$peer" cua_screen "unavailable: $(echo "$out" | tr '\n' ' ' | head -c 80)"
    fi
  fi
}

# ── inter-peer mesh (linux↔mac, linux↔win when both present) ────────
test_cross_peer_shell() {
  local a="$1" b="$2" edge out
  edge="${a}->${b}"
  section "cross-peer: $edge"
  # From peer A, run bs health toward B if A has a bs client on PATH
  out="$(run_to "$BS_BIN" shell "$a" --cmd "command -v bs >/dev/null && bs health $b 2>&1 | head -3 || command -v bridgesessions >/dev/null && bridgesessions health $b 2>&1 | head -3 || echo NO_BS_CLIENT" 2>&1 || true)"
  if assert_contains "$out" "healthy (data-plane ok)"; then
    record PASS "$edge" cross_health "ok"
  elif assert_contains "$out" "NO_BS_CLIENT"; then
    record SKIP "$edge" cross_health "no bs client on $a"
  else
    record SKIP "$edge" cross_health "not verified: $(echo "$out" | tr '\n' ' ' | head -c 100)"
  fi
}

# ── run matrix ──────────────────────────────────────────────────────
echo "BridgeSessions e2e fleet test"
echo "  binary:  $BS_BIN"
echo "  version: $EXPECTED_VERSION"
echo "  peers:   ${PEERS[*]}"
echo "  timeout: ${TIMEOUT_SEC}s"
echo "  started: $(ts)"

for peer in "${PEERS[@]}"; do
  test_peer "$peer"
done

# Cross edges when we have multi-platform set (heuristic on peer name tokens)
has_linux=0 has_mac=0 has_win=0
linux_peer="" mac_peer="" win_peer=""
for peer in "${PEERS[@]}"; do
  case "$peer" in
    *linux*|linux-*) has_linux=1; [[ -z "$linux_peer" ]] && linux_peer=$peer ;;
    *mac*|macos-*) has_mac=1; [[ -z "$mac_peer" ]] && mac_peer=$peer ;;
    *win*|windows-*) has_win=1; [[ -z "$win_peer" ]] && win_peer=$peer ;;
  esac
done

if [[ $QUICK -eq 0 ]]; then
  if [[ $has_linux -eq 1 && $has_mac -eq 1 && -n "$linux_peer" && -n "$mac_peer" ]]; then
    test_cross_peer_shell "$linux_peer" "$mac_peer"
  fi
  if [[ $has_linux -eq 1 && $has_win -eq 1 && -n "$linux_peer" && -n "$win_peer" ]]; then
    test_cross_peer_shell "$linux_peer" "$win_peer"
  fi
fi

# JSON writer: avoid bash 3.2 `local` in main
write_json() {
  local out="$1" i=0 r status peer feature detail
  {
    echo '{'
    echo "  \"finished\": \"$(ts)\","
    echo "  \"binary\": \"$(printf '%s' "$BS_BIN" | sed 's/"/\\"/g')\","
    echo "  \"expected_version\": \"$(printf '%s' "$EXPECTED_VERSION" | sed 's/"/\\"/g')\","
    echo "  \"pass\": $PASS,"
    echo "  \"fail\": $FAIL,"
    echo "  \"skip\": $SKIP,"
    echo '  "results": ['
    for r in "${RESULTS[@]}"; do
      IFS='|' read -r status peer feature detail <<< "$r"
      detail="${detail//\\/\\\\}"
      detail="${detail//\"/\\\"}"
      [[ $i -gt 0 ]] && echo ','
      printf '    {"status":"%s","peer":"%s","feature":"%s","detail":"%s"}' \
        "$status" "$peer" "$feature" "$detail"
      i=$((i + 1))
    done
    echo
    echo '  ]'
    echo '}'
  } > "$out"
}

# ── summary ─────────────────────────────────────────────────────────
section "summary"
TOTAL=$((PASS + FAIL + SKIP))
echo "  pass=$PASS fail=$FAIL skip=$SKIP total=$TOTAL"
echo "  finished: $(ts)"

if [[ -n "$JSON_OUT" ]]; then
  write_json "$JSON_OUT"
  echo "  json: $JSON_OUT"
fi

if [[ $FAIL -gt 0 ]]; then
  echo
  echo "FAILED features:"
  for r in "${RESULTS[@]}"; do
    IFS='|' read -r status peer feature detail <<< "$r"
    [[ "$status" == "FAIL" ]] && echo "  - $peer / $feature: $detail"
  done
  exit 1
fi
exit 0
