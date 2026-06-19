# R3.3/R3.4 — install bridgesessions as Windows service via NSSM (auto-restart)
param(
    [string]$Binary = "C:\Users\Shadow\bridgesessions\bridgesessions.exe",
    [string]$Config = "C:\Users\Shadow\.bridgesessions\config",
    [string]$ServiceName = "bridgesessions",
    [string]$LogDir = "C:\Users\Shadow",
    [string]$ProfileDir = "C:\Users\Shadow"
)
$ErrorActionPreference = "Continue"
$prodExample = Join-Path (Split-Path $Binary) "config.shadow.production.example"
if (Test-Path $prodExample) {
    $need = $false
    if (-not (Test-Path $Config)) { $need = $true }
    else {
        $first = Get-Content $Config -TotalCount 8 -ErrorAction SilentlyContinue
        if ($first -match 'gossip|19954') { $need = $true }
    }
    if ($need) {
        Copy-Item $prodExample $Config -Force
        Write-Host "Restored production config from config.shadow.production.example"
    }
}
$nssm = Get-Command nssm -ErrorAction SilentlyContinue
if (-not $nssm) {
    Write-Error "nssm not in PATH. Install: winget install NSSM.NSSM (or choco install nssm)"
}
$vcpkgBin = "C:\vcpkg\installed\x64-windows\bin"
$wrapper = Join-Path $LogDir "bridgesessions-service.cmd"
@"
@echo off
set PATH=$vcpkgBin;%PATH%
"$Binary" --config "$Config" >> "$LogDir\bridgesessions-daemon.log" 2>&1
"@ | Set-Content -Encoding ASCII $wrapper

& nssm stop $ServiceName 2>$null
& nssm remove $ServiceName confirm 2>$null
& nssm install $ServiceName "cmd.exe" "/c" $wrapper
& nssm set $ServiceName AppStdout "$LogDir\bridgesessions-nssm-stdout.log"
& nssm set $ServiceName AppStderr "$LogDir\bridgesessions-nssm-stderr.log"
& nssm set $ServiceName AppEnvironmentExtra "USERPROFILE=$ProfileDir" "HOME=$ProfileDir"
& nssm set $ServiceName AppExit Default Restart
& nssm set $ServiceName AppRestartDelay 5000
& nssm start $ServiceName
Write-Host "Service $ServiceName installed and started"