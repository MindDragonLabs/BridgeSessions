# R7.2 — cross-platform shell smoke (requires live mesh + authorized keys)
# R7.3 — runnable from Shadow with existing SSH keys
param(
    [string]$Peer = "linux-b",
    [string]$BsExe = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
)
$ErrorActionPreference = "Stop"
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"

if (-not (Test-Path $BsExe)) {
    Write-Host "FAIL: missing $BsExe"
    exit 1
}

Write-Host "=== R7.2 shell smoke peer=$Peer ==="
Write-Host "NOTE: full interactive shell test is manual; this checks health + connect path."

$healthOut = & $BsExe health $Peer 2>&1 | Out-String
Write-Host $healthOut
if ($healthOut -match "healthy") {
    Write-Host "R7.2 PASS (health OK — run manual: bridgesessions shell $Peer -x `"echo RELIABILITY_OK`")"
    exit 0
}
Write-Host "R7.2 FAIL or SKIP: health not healthy — mesh/TLS/keys may be down"
exit 1