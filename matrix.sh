#!/usr/bin/env bash
set +e
analyze() {
  local label="$1"; shift
  echo "=== $label ==="
  "$@" | awk '
    /mesh_peer_connected_outbound/ {oc++}
    /mesh_peer_connected"/ {ic++}
    /mesh_pong_timeout/ {pt++}
    /tls_accept_failed/ {af++}
    /tls_connect_failed/ {cf++}
    /config_reload/ {cr++}
    END {
      printf "  connected_inbound=%d outbound=%d pong_timeout=%d accept_fail=%d connect_fail=%d reload=%d\n", ic, oc, pt, af, cf, cr
    }'
}
analyze SHADOW tail -400 /c/Users/Shadow/.bridgesessions/bs-mesh.log
analyze FECV3 ssh -i /c/Users/Shadow/.ssh/id_ed25519_shadow_to_linux agent@203.0.113.11 'tail -400 ~/.bridgesessions/bs-mesh.log'
analyze FECV4 ssh -i /c/Users/Shadow/.ssh/id_ed25519_shadow_to_linux agent@203.0.113.12 'tail -400 ~/.bridgesessions/bs-mesh.log'
analyze MAC ssh macos-peer 'tail -400 ~/.bridgesessions/bs-mesh.log'
echo "=== HEALTH MATRIX (IPC, timeouts guarded) ==="
echo "-- Shadow --"
for p in linux-a linux-b macos-peer; do printf "shadow->%s: " $p; powershell.exe -NoProfile -Command "& C:\\Users\\Shadow\\bridgesessions\\bridgesessions.live.exe --config \$env:USERPROFILE\\.bridgesessions\\config health $p" 2>/dev/null; done
echo "-- linux-a --"
ssh -i /c/Users/Shadow/.ssh/id_ed25519_shadow_to_linux agent@203.0.113.11 'cd ~/bridgesessions; for p in shadow linux-b macos-peer; do printf "linux-a->%s: " $p; timeout 12 ./bsmesh --config ~/.bridgesessions/config health $p || echo "(hang)"; done'
echo "-- linux-b --"
ssh -i /c/Users/Shadow/.ssh/id_ed25519_shadow_to_linux agent@203.0.113.12 'cd ~/bridgesessions; for p in shadow linux-a macos-peer; do printf "linux-b->%s: " $p; timeout 12 ./bsmesh --config ~/.bridgesessions/config health $p || echo "(hang)"; done'
echo "-- macos-peer --"
ssh macos-peer 'cd ~/bridgesessions; for p in shadow linux-a linux-b; do printf "macos-peer->%s: " $p; ./bridgesessions --config ~/.bridgesessions/config health $p; done'
