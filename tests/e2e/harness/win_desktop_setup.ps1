#Requires -Version 5.1
# BridgeSessions Windows Desktop (Session 1) setup - CUA helper + tray
# Idempotent. Run as interactive user (shadow) with a desktop session.
# Hermes on Shadow can invoke at logon:
#   powershell -NoProfile -ExecutionPolicy Bypass -File win_desktop_setup.ps1

$ErrorActionPreference = "Stop"
$InstallDir = Join-Path $env:LOCALAPPDATA "bridgesessions"
$Bin = Join-Path $InstallDir "bridgesessions.exe"
$ConfigDir = Join-Path $env:USERPROFILE ".bridgesessions"
$TrayScript = Join-Path $InstallDir "bs_tray.ps1"
$HelperTask = "BS-CUA-Helper"
$TrayTask = "BS-Tray"

function Write-Info([string]$m) { Write-Host ("-> " + $m) }

if (-not (Test-Path $Bin)) {
    throw ("Missing binary: " + $Bin + " - run install.ps1 or copy PE+DLLs first")
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null

# Prefer tray already in install dir; else from received/
$recv = Join-Path $env:USERPROFILE ".bridgesessions\received"
if (-not (Test-Path $TrayScript) -and (Test-Path $recv)) {
    $found = Get-ChildItem $recv -Filter "bs_tray.ps1*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) {
        Copy-Item -Force $found.FullName $TrayScript
        Write-Info ("Installed tray script from received: " + $found.Name)
    }
}

# --- CUA helper scheduled task (interactive / logon) ---
$helperAction = New-ScheduledTaskAction -Execute $Bin -Argument "--cua-helper" -WorkingDirectory $InstallDir
$helperTrigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$helperPrincipal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$helperSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $HelperTask -Action $helperAction -Trigger $helperTrigger `
    -Principal $helperPrincipal -Settings $helperSettings -Force | Out-Null
Write-Info ("Registered task " + $HelperTask)

# Stop old helper instances
Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.CommandLine -and ($_.CommandLine -match "cua-helper")) {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Seconds 1
Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir -WindowStyle Hidden
Start-Sleep -Seconds 3
Write-Info "Started --cua-helper"

# --- Tray task ---
if (Test-Path $TrayScript) {
    $arg = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$TrayScript`""
    $trayAction = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $arg -WorkingDirectory $InstallDir
    $trayTrigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $trayPrincipal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $TrayTask -Action $trayAction -Trigger $trayTrigger `
        -Principal $trayPrincipal -Settings $helperSettings -Force | Out-Null
    Write-Info ("Registered task " + $TrayTask)

    $trayRunning = $false
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.CommandLine -and ($_.CommandLine -match "bs_tray")) { $trayRunning = $true }
    }
    if (-not $trayRunning) {
        Start-Process powershell.exe -ArgumentList $arg -WorkingDirectory $InstallDir
        Write-Info "Started bs_tray.ps1"
    } else {
        Write-Info "Tray already running"
    }
} else {
    Write-Info "WARN: bs_tray.ps1 not found - skip tray"
}

# --- Probe ---
$token = Join-Path $ConfigDir "cua-helper-token"
Start-Sleep -Seconds 2
$helperOk = Test-Path $token
$listen = Get-NetTCPConnection -LocalPort 34960 -State Listen -ErrorAction SilentlyContinue
Write-Info ("cua-helper-token exists=" + $helperOk)
Write-Info ("port 34960 listen=" + [bool]$listen)
if (-not $helperOk) {
    Write-Host "FAIL: cua-helper did not write token" -ForegroundColor Red
    exit 1
}
Write-Host "WIN_DESKTOP_SETUP_OK"
exit 0
