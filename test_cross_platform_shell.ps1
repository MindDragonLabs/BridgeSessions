# R7.2 — cross-platform shell smoke (health gate)
param(
    [string]$Peer = "linux-b",
    [string]$BsExe = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
)
$ErrorActionPreference = "Stop"
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;$env:PATH"
if (-not (Test-Path $BsExe)) { Write-Host "FAIL: missing $BsExe"; exit 1 }
Write-Host "=== R7.2 shell smoke peer=$Peer ==="
$healthOut = & $BsExe health $Peer 2>&1 | Out-String
Write-Host $healthOut.Trim()
if ($healthOut -match "healthy") {
    Write-Host "R7.2 PASS (health OK; manual shell: bridgesessions shell $Peer)"
    exit 0
}
Write-Host "R7.2 FAIL: health not healthy"
exit 1