#Requires -Version 5.1
$ErrorActionPreference = "Continue"
$InstallDir = Join-Path $env:LOCALAPPDATA "bridgesessions"
$Bin = Join-Path $InstallDir "bridgesessions.exe"
$ConfigDir = Join-Path $env:USERPROFILE ".bridgesessions"
$err = Join-Path $InstallDir "cua-helper.err"
$out = Join-Path $InstallDir "cua-helper.out"

if (-not (Test-Path $Bin)) { Write-Host "NO_BIN"; exit 1 }

Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.CommandLine -and ($_.CommandLine -match "cua-helper")) {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Seconds 1
Remove-Item $err, $out -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir `
    -RedirectStandardError $err -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 4

Write-Host ("PID=" + $p.Id + " HAS_EXITED=" + $p.HasExited)
if ($p.HasExited) { Write-Host ("EXIT=" + $p.ExitCode) }
Write-Host ("token=" + (Test-Path (Join-Path $ConfigDir "cua-helper-token")))
$listen = Get-NetTCPConnection -LocalPort 34960 -State Listen -ErrorAction SilentlyContinue
Write-Host ("listen34960=" + [bool]$listen)
if (Test-Path $out) { Write-Host "OUT:"; Get-Content $out }
if (Test-Path $err) { Write-Host "ERR:"; Get-Content $err }

Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ("PROC PID=" + $_.ProcessId + " SID=" + $_.SessionId + " CL=" + $_.CommandLine)
}
