# BridgeSessions one-line install + upgrade (Windows PowerShell)
#
#   irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
#

$ErrorActionPreference = "Stop"
$TAG = if ($env:BRIDGESESSIONS_TAG) { $env:BRIDGESESSIONS_TAG } else { "26.08.25-beta7" }
$BASE = "https://github.com/MindDragonLabs/BridgeSessions/releases/download/v$TAG"
$INSTALL_DIR = "$env:LOCALAPPDATA\bridgesessions"
$BIN_PATH = "$INSTALL_DIR\bridgesessions.exe"
$VERSION_FILE = "$INSTALL_DIR\.bridgesessions-version"
$CONFIG_DIR = "$env:USERPROFILE\.bridgesessions"
$CONFIG_PATH = "$CONFIG_DIR\config"

# -- 1. DETECT AND KILL ALL RUNNING DAEMONS (including Session 0) ------------
$ErrorActionPreference = "Stop"
function Restore-BridgeSessionsDaemon {
    try {
        $t = Get-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
        if ($t) {
            Enable-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue | Out-Null
            Start-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
        } elseif (Test-Path $BIN_PATH) {
            Start-Process -FilePath $BIN_PATH -ArgumentList "--daemon","--config",$CONFIG_PATH -WindowStyle Hidden
        }
    } catch {}
}
try {
$wasRunning = $false
try {
    # Get-CimInstance finds processes across ALL sessions (Session 0 phantoms
    # are invisible to Get-Process from WinRM but hold IPC port 19980).
    $procs = Get-CimInstance Win32_Process -Filter "Name like '%bridgesessions%'" -ErrorAction SilentlyContinue
    if ($procs) {
        $wasRunning = $true
        foreach ($p in $procs) {
            Write-Host "→ Killing bridgesessions PID $($p.ProcessId) (Session $($p.SessionId))..."
            try { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
        }
        Start-Sleep -Seconds 2
    }
} catch {}

# Also kill via taskkill as fallback
try { taskkill /f /im bridgesessions.exe 2>$null } catch {}
Start-Sleep -Seconds 1

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
    $TMP_PATH = "$INSTALL_DIR\bridgesessions.download.$PID.exe"
    Write-Host "→ Downloading bridgesessions $TAG for Windows..."
    Invoke-WebRequest -Uri $URL -OutFile $TMP_PATH

    $SUMS_URL = "$BASE/SHA256SUMS"
    $expected = $null
    try {
        $sums = (Invoke-WebRequest -Uri $SUMS_URL -UseBasicParsing -ErrorAction Stop).Content
        foreach ($line in ($sums -split "`n")) {
            $parts = $line.Trim() -split "\s+"
            if ($parts.Count -ge 2 -and $parts[1] -eq "bridgesessions-windows-x86_64.exe") {
                $expected = $parts[0].ToLower(); break
            }
        }
    } catch {
        Remove-Item $TMP_PATH -Force -ErrorAction SilentlyContinue
        throw "Could not download SHA256SUMS; refusing unverified binary."
    }
    if (-not $expected -or $expected -notmatch '^[0-9a-f]{64}$') {
        Remove-Item $TMP_PATH -Force -ErrorAction SilentlyContinue
        throw "SHA256SUMS has no valid Windows binary entry."
    }
    $actual = (Get-FileHash $TMP_PATH -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) {
        Remove-Item $TMP_PATH -Force -ErrorAction SilentlyContinue
        throw "Checksum mismatch (expected $expected, got $actual)."
    }
    $downloadedVersion = (& $TMP_PATH --version 2>&1 | Out-String).Trim()
    if ($downloadedVersion -ne $TAG.TrimStart('v')) {
        Remove-Item $TMP_PATH -Force -ErrorAction SilentlyContinue
        throw "Downloaded binary reports $downloadedVersion; expected $TAG."
    }
    Move-Item $TMP_PATH $BIN_PATH -Force
    $TAG | Set-Content $VERSION_FILE
    Write-Host "→ SHA256 verified; binary installed."
}

# -- 4. VALIDATE -------------------------------------------------------------
$ver = & $BIN_PATH --version 2>&1
Write-Host "→ Version: $ver"

# ── 4b. Ensure INSTALL_DIR is on PATH (fresh Windows won't have it) ──
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    [Environment]::SetEnvironmentVariable("Path", "$INSTALL_DIR;$userPath", "User")
    $env:Path = "$INSTALL_DIR;$env:Path"
    Write-Host "→ Added $INSTALL_DIR to user PATH (new terminals will find 'bridgesessions')"
}

# -- 5. APP DIRS + DEFAULT CONFIG + CLEANUP -----------------------------------
$RECEIVE_DIR = "$CONFIG_DIR\received"
New-Item -ItemType Directory -Force -Path $CONFIG_DIR | Out-Null
New-Item -ItemType Directory -Force -Path $RECEIVE_DIR | Out-Null

# Clean received/ dir if it has too many files (prevents daemon crash from
# binary files triggering JSON parse errors — known macOS/Windows issue)
$receiveFiles = Get-ChildItem -Path $RECEIVE_DIR -File -ErrorAction SilentlyContinue
if ($receiveFiles.Count -gt 50) {
    Write-Host "→ Cleaning $($receiveFiles.Count) files from received/ (prevents daemon crash)..."
    Remove-Item -Path "$RECEIVE_DIR\*" -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $CONFIG_PATH)) {
    Write-Host "→ Creating default config at $CONFIG_PATH..."
    $nodeName = $env:COMPUTERNAME
    $configContent = @"
# BridgeSessions config — generated by install.ps1
node.name $nodeName
node.listen 0.0.0.0:19949
receive_dir $RECEIVE_DIR
"@
    $configContent | Set-Content -Path $CONFIG_PATH -Encoding ASCII
}

# -- 6. ADD TO PATH ----------------------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    Write-Host "→ Adding $INSTALL_DIR to PATH..."
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$INSTALL_DIR", "User")
    $env:Path = "$env:Path;$INSTALL_DIR"
}

# -- 7. SCHEDULED TASK: DAEMON -----------------------------------------------
$daemonAction = New-ScheduledTaskAction -Execute $BIN_PATH -Argument "--daemon --config `"$CONFIG_PATH`""
$daemonTrigger = New-ScheduledTaskTrigger -AtStartup
$daemonSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)

$daemonTask = Get-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
if ($daemonTask) {
    Write-Host "→ Updating existing BridgeSessions scheduled task..."
    Set-ScheduledTask -TaskName "BridgeSessions" -Action $daemonAction -Trigger $daemonTrigger -Settings $daemonSettings | Out-Null
} else {
    Write-Host "→ Creating BridgeSessions scheduled task..."
    try {
        Register-ScheduledTask -TaskName "BridgeSessions" -Action $daemonAction -Trigger $daemonTrigger -Settings $daemonSettings -RunLevel Highest -Force | Out-Null
    } catch {
        Write-Host "→ Could not register daemon task with elevated privileges. Trying schtasks..."
        schtasks /create /tn "BridgeSessions" /tr "`"$BIN_PATH`" --daemon --config `"$CONFIG_PATH`"" /sc onstart /ru SYSTEM /f 2>$null
    }
}
Write-Host "→ Daemon scheduled task installed (BridgeSessions)."

# -- 8. SCHEDULED TASK: CUA HELPER -------------------------------------------
$cuaAction = New-ScheduledTaskAction -Execute $BIN_PATH -Argument "--config `"$CONFIG_PATH`" --cua-helper"
$cuaTrigger = New-ScheduledTaskTrigger -AtLogOn
$cuaSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

$cuaTask = Get-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -ErrorAction SilentlyContinue
if ($cuaTask) {
    Write-Host "→ Updating existing BridgeSessions-CuaHelper scheduled task..."
    Set-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -Action $cuaAction -Trigger $cuaTrigger -Settings $cuaSettings | Out-Null
} else {
    Write-Host "→ Creating BridgeSessions-CuaHelper scheduled task..."
    try {
        Register-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -Action $cuaAction -Trigger $cuaTrigger -Settings $cuaSettings -User $env:USERNAME -Force | Out-Null
    } catch {
        Write-Host "→ Could not register CUA helper task. Trying schtasks..."
        schtasks /create /tn "BridgeSessions-CuaHelper" /tr "`"$BIN_PATH`" --config `"$CONFIG_PATH`" --cua-helper" /sc onlogon /ru $env:USERNAME /f 2>$null
    }
}
Write-Host "→ CUA helper scheduled task installed (BridgeSessions-CuaHelper)."

# -- 9. ALWAYS START DAEMON ----------------------------------------------------
Write-Host "→ Starting daemon..."
Start-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
try {
    $newProcs = Get-CimInstance Win32_Process -Filter "Name like '%bridgesessions%'" -ErrorAction SilentlyContinue
    $daemonProc = $newProcs | Where-Object { $_.CommandLine -like "*--config*" -and $_.CommandLine -notlike "*--cua-helper*" } | Select-Object -First 1
    if ($daemonProc) {
        Write-Host "→ Daemon running (PID $($daemonProc.ProcessId))."
    } else {
        Write-Host "→ WARNING: Daemon did not start. Run manually:"
        Write-Host "   $BIN_PATH --daemon --config `"$CONFIG_PATH`""
    }
} catch {
    Write-Host "→ WARNING: Could not verify daemon status."
}

Write-Host "→ Daemon started."

} finally {
    Restore-BridgeSessionsDaemon
}

# -- 10. INSTALL TRAY APP -----------------------------------------------------
$TRAY_SCRIPT_SRC = "$PSScriptRoot\bs_tray.ps1"
$TRAY_SCRIPT_DST = "$INSTALL_DIR\bs_tray.ps1"

if (Test-Path $TRAY_SCRIPT_SRC) {
    Write-Host "→ Installing tray app to $INSTALL_DIR..."
    Copy-Item -Path $TRAY_SCRIPT_SRC -Destination $TRAY_SCRIPT_DST -Force

    # Create startup shortcut (shell:startup)
    $startupDir = [Environment]::GetFolderPath("Startup")
    $shortcutPath = "$startupDir\BridgeSessions Tray.lnk"
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath = "powershell.exe"
        $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$TRAY_SCRIPT_DST`""
        $shortcut.WorkingDirectory = $INSTALL_DIR
        $shortcut.IconLocation = "$BIN_PATH,0"
        $shortcut.Description = "BridgeSessions System Tray"
        $shortcut.WindowStyle = 7  # Minimized
        $shortcut.Save()
        Write-Host "→ Startup shortcut created: $shortcutPath"
    } catch {
        Write-Host "→ WARNING: Could not create startup shortcut: $($_.Exception.Message)"
    }
} else {
    Write-Host "→ WARNING: bs_tray.ps1 not found at $TRAY_SCRIPT_SRC. Skipping tray install."
}

# -- 11. LAUNCH TRAY APP ------------------------------------------------------
if (Test-Path $TRAY_SCRIPT_DST) {
    Write-Host "→ Launching tray app..."
    try {
        Start-Process -FilePath "powershell.exe" `
            -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", "`"$TRAY_SCRIPT_DST`"" `
            -WindowStyle Hidden
        Write-Host "→ Tray app launched."
    } catch {
        Write-Host "→ WARNING: Could not launch tray app: $($_.Exception.Message)"
    }
}

# -- 12. SCREEN RECORDING PERMISSION PROMPT (Windows Security) -----------------
# CUA helper needs Screen Recording permission on Windows 11. Open Settings to
# the "Allow screen recording" page so the user can grant it manually.
try {
    $osVer = [System.Environment]::OSVersion.Version
    if ($osVer.Major -ge 10) {
        Write-Host "→ Opening Windows Security for Screen Recording permission..."
        Write-Host "   If prompted, allow bridgesessions.exe to capture screen."

        # Open Settings > Privacy > Screen Recording (Windows 10/11)
        Start-Process "ms-settings:privacy-capability" -ErrorAction SilentlyContinue

        # Also show a helpful dialog
        Add-Type -AssemblyName System.Windows.Forms
        $msgResult = [System.Windows.Forms.MessageBox]::Show(
            "BridgeSessions needs Screen Recording permission for CUA automation.`n`n" +
            "If the Settings window opened, find 'bridgesessions' and enable it.`n`n" +
            "Click OK when done (or Cancel to skip).",
            "BridgeSessions — Permission Required",
            [System.Windows.Forms.MessageBoxButtons]::OKCancel,
            [System.Windows.Forms.MessageBoxIcon]::Information)

        if ($msgResult -eq [System.Windows.Forms.DialogResult]::OK) {
            Write-Host "→ User acknowledged permission prompt."
        }
    }
} catch {
    Write-Host "→ Could not open permission prompt: $($_.Exception.Message)"
}

Write-Host ""
Write-Host "============================================"
Write-Host " BridgeSessions $TAG installed successfully!"
Write-Host "============================================"
Write-Host ""
Write-Host " Tray app: Right-click the 'B' icon in the"
Write-Host " system tray for fleet status and controls."
Write-Host ""
Write-Host " Daemon:  Scheduled task 'BridgeSessions' (auto-starts)"
Write-Host " CUA:     Scheduled task 'BridgeSessions-CuaHelper' (at logon)"
Write-Host " Tray:    Startup shortcut (at logon)"
Write-Host ""
