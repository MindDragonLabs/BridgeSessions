# BridgeSessions one-line install + upgrade (Windows PowerShell)
#
#   irm https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/26.08.05-beta1/scripts/install.ps1 | iex
#

$ErrorActionPreference = "Stop"
$TAG = if ($env:BRIDGESESSIONS_TAG) { $env:BRIDGESESSIONS_TAG } else { "26.08.05-beta1" }
$BASE = "https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/$TAG/dist"
$INSTALL_DIR = "$env:LOCALAPPDATA\bridgesessions"
$BIN_PATH = "$INSTALL_DIR\bridgesessions.exe"
$VERSION_FILE = "$INSTALL_DIR\.bridgesessions-version"
$CONFIG_DIR = "$env:USERPROFILE\.bridgesessions"
$CONFIG_PATH = "$CONFIG_DIR\config"

# -- 1. DETECT RUNNING DAEMON ------------------------------------------------
$wasRunning = $false
$existingPid = $null
try {
    $proc = Get-Process -Name bridgesessions -ErrorAction SilentlyContinue
    if ($proc) {
        $wasRunning = $true
        $existingPid = $proc.Id
        Write-Host "→ bridgesessions daemon running (PID $existingPid) — will restart after upgrade."
    }
} catch {}

# -- 2. STOP DAEMON ----------------------------------------------------------
if ($wasRunning) {
    Write-Host "→ Stopping daemon..."
    try { taskkill /f /im bridgesessions.exe 2>$null } catch {}
    Start-Sleep -Seconds 2
}

# -- 3. DOWNLOAD + INSTALL ---------------------------------------------------
New-Item -ItemType Directory -Force -Path $INSTALL_DIR | Out-Null

$CURRENT = ""
if (Test-Path $VERSION_FILE) { $CURRENT = Get-Content $VERSION_FILE }

$needsDownload = $true
if ($CURRENT -eq $TAG -and (Test-Path $BIN_PATH)) {
    Write-Host "→ bridgesessions $TAG already installed."
    $needsDownload = $false
}

if ($needsDownload) {
    $URL = "$BASE/bridgesessions-windows-x86_64.exe"
    Write-Host "→ Downloading bridgesessions $TAG for Windows..."
    Invoke-WebRequest -Uri $URL -OutFile $BIN_PATH
    $TAG | Set-Content $VERSION_FILE
    Write-Host "→ Download complete."
}

# -- 4. VALIDATE -------------------------------------------------------------
$ver = & $BIN_PATH --version 2>&1
Write-Host "→ Version: $ver"

# -- 5. ADD TO PATH ----------------------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    Write-Host "→ Adding $INSTALL_DIR to PATH..."
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$INSTALL_DIR", "User")
    $env:Path = "$env:Path;$INSTALL_DIR"
}

# -- 6. RESTART DAEMON (if it was running) ------------------------------------
if ($wasRunning -and (Test-Path $CONFIG_PATH)) {
    Write-Host "→ Restarting daemon..."
    Start-Process -WindowStyle Hidden -FilePath $BIN_PATH -ArgumentList "--config", $CONFIG_PATH
    Start-Sleep -Seconds 2
    try {
        $newProc = Get-Process -Name bridgesessions -ErrorAction SilentlyContinue
        if ($newProc) {
            Write-Host "→ Daemon restarted (PID $($newProc.Id))."
        } else {
            Write-Host "→ WARNING: Daemon did not start. Run manually:"
            Write-Host "   $BIN_PATH --config $CONFIG_PATH"
        }
    } catch {
        Write-Host "→ Starting daemon silently..."
    }
} elseif (Test-Path $CONFIG_PATH) {
    Write-Host "→ Daemon not running. To start:"
    Write-Host "   Start-Process -WindowStyle Hidden -FilePath $BIN_PATH -ArgumentList '--config', '$CONFIG_PATH'"
}

Write-Host "→ Done."
