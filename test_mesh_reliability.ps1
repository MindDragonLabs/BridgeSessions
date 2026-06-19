# R7.1 — 3-node mesh reliability (health-based; CLI stats is ephemeral MeshController)
param(
    [int]$WaitSeconds = 45,
    [string]$SshKey = "$env:USERPROFILE\.ssh\id_ed25519_shadow_to_linux",
    [string]$Fecv4 = "agent@203.0.113.12",
    [string]$Fecv3 = "agent@203.0.113.11"
)
$ErrorActionPreference = "Continue"
$fail = $false
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"
$bs = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"

function Ssh-Remote($target, $cmd) {
    ssh -i $SshKey -o BatchMode=yes -o ConnectTimeout=15 $target $cmd 2>&1
}

Write-Host "=== R7.1 mesh reliability harness ==="
Ssh-Remote $Fecv4 "sudo systemctl restart bsmesh" | Out-Null
Ssh-Remote $Fecv3 "sudo systemctl restart bsmesh" | Out-Null
nssm restart bridgesessions 2>$null | Out-Null
Write-Host "restarted Linux units + Shadow NSSM service"
Start-Sleep -Seconds $WaitSeconds

$h4 = & $bs health linux-b 2>&1 | Out-String
$h3 = & $bs health linux-a 2>&1 | Out-String
Write-Host "health linux-b: $($h4.Trim())"
Write-Host "health linux-a: $($h3.Trim())"
if ($h4 -notmatch "healthy") { Write-Host "FAIL: linux-b health"; $fail = $true }
if ($h3 -notmatch "healthy") { Write-Host "FAIL: linux-a health"; $fail = $true }

$v4 = Ssh-Remote $Fecv4 "/home/agent/bridgesessions/bsmesh --version"
$v3 = Ssh-Remote $Fecv3 "/home/agent/bridgesessions/bsmesh --version"
$vs = & $bs --version 2>&1
Write-Host "versions: Shadow=$vs linux-b=$($v4.Trim()) linux-a=$($v3.Trim())"
if ($v4 -notmatch "1.3.0-reliability" -or $v3 -notmatch "1.3.0-reliability") {
    Write-Host "FAIL: version mismatch on Linux nodes"
    $fail = $true
}

if ($fail) { exit 1 }
Write-Host "R7.1 PASS"
exit 0