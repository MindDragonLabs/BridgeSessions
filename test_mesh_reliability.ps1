# R7.1 — 3-node mesh reliability smoke (Shadow → SSH start daemons, check stats)
# R7.3 — clear PASS/FAIL exit codes
param(
    [int]$WaitSeconds = 30,
    [string]$SshKey = "$env:USERPROFILE\.ssh\id_ed25519_shadow_to_linux",
    [string]$Fecv4 = "agent@203.0.113.12",
    [string]$Fecv3 = "agent@203.0.113.11"
)
$ErrorActionPreference = "Continue"
$fail = $false

function Ssh-Remote($target, $cmd) {
    ssh -i $SshKey -o BatchMode=yes -o ConnectTimeout=15 $target $cmd 2>&1
}

Write-Host "=== R7.1 mesh reliability harness ==="

# Kill stray daemons on Linux nodes (best-effort)
Ssh-Remote $Fecv4 "pkill -f bsmesh || true; pkill -f bridgesessions || true" | Out-Null
Ssh-Remote $Fecv3 "pkill -f bsmesh || true; pkill -f bridgesessions || true" | Out-Null

# Start via systemd if unit exists, else nohup fallback
foreach ($pair in @(@($Fecv4,"linux-b"), @($Fecv3,"FECv3"))) {
    $t = $pair[0]; $name = $pair[1]
    $out = Ssh-Remote $t "sudo systemctl restart bsmesh 2>/dev/null || (cd ~/bridgesessions && nohup ./bsmesh --config ~/.bridgesessions/config >/tmp/bsmesh.log 2>&1 &)"
    Write-Host "started $name : $out"
}

# Shadow local daemon — user must have one listener or skip
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"
$bs = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
if (Test-Path $bs) {
    $statsLocal = & $bs stats 2>&1 | Out-String
    Write-Host "--- Shadow stats ---"
    Write-Host $statsLocal
    if ($statsLocal -notmatch "connections:\s*(\d+)") {
        Write-Host "WARN: Shadow stats missing connections line (daemon may not be running)"
    }
} else {
    Write-Host "WARN: no local bridgesessions.exe — skip Shadow stats"
}

Write-Host "waiting ${WaitSeconds}s for mesh..."
Start-Sleep -Seconds $WaitSeconds

$stats = & $bs stats 2>&1 | Out-String
Write-Host $stats
if ($stats -match "connections:\s*(\d+)") {
    $n = [int]$Matches[1]
    if ($n -lt 2) { Write-Host "FAIL: connections=$n (expected >= 2)"; $fail = $true }
    else { Write-Host "PASS: connections=$n" }
} else {
    Write-Host "FAIL: could not parse connections from stats"
    $fail = $true
}

if ($fail) { exit 1 }
Write-Host "R7.1 PASS"
exit 0