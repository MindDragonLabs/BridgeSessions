# BridgeSessions one-line install + upgrade (Windows PowerShell)
#
#   irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
#

$ErrorActionPreference = "Stop"
# Windows PowerShell on Server 2016 can otherwise negotiate TLS 1.0 when
# downloading release assets; GitHub requires TLS 1.2.
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
} catch {
    throw "TLS 1.2 is unavailable; update .NET Framework before installing BridgeSessions."
}
$TAG = if ($env:BRIDGESESSIONS_TAG) { $env:BRIDGESESSIONS_TAG } else { "26.09.24-a1" }
$BASE = "https://github.com/MindDragonLabs/BridgeSessions/releases/download/v$TAG"
$STAGED_DIR = $env:BRIDGESESSIONS_DIST_DIR
if ($STAGED_DIR -and -not (Test-Path -LiteralPath $STAGED_DIR -PathType Container)) {
    throw "BRIDGESESSIONS_DIST_DIR does not exist: $STAGED_DIR"
}
$INSTALL_DIR = "$env:LOCALAPPDATA\bridgesessions"
$BIN_PATH = "$INSTALL_DIR\bridgesessions.exe"
$VERSION_FILE = "$INSTALL_DIR\.bridgesessions-version"
$CONFIG_DIR = "$env:USERPROFILE\.bridgesessions"
$CONFIG_PATH = "$CONFIG_DIR\config"
$script:ReleaseSums = $null

function Get-ReleaseAssetHash([string]$AssetName) {
    if ($null -eq $script:ReleaseSums) {
        if ($STAGED_DIR) {
            $sumsPath = Join-Path $STAGED_DIR "SHA256SUMS"
            if (-not (Test-Path -LiteralPath $sumsPath -PathType Leaf)) {
                throw "Staged SHA256SUMS is missing; refusing unverified release asset."
            }
            $script:ReleaseSums = Get-Content -LiteralPath $sumsPath -Raw
        } else {
            $sumsUrl = "$BASE/SHA256SUMS"
            try {
                $script:ReleaseSums = (Invoke-WebRequest -Uri $sumsUrl `
                    -UseBasicParsing -ErrorAction Stop).Content
            } catch {
                throw "Could not download SHA256SUMS; refusing unverified release asset."
            }
        }
    }
    foreach ($line in ($script:ReleaseSums -split "`n")) {
        $parts = $line.Trim() -split "\s+"
        if ($parts.Count -ge 2 -and $parts[1] -eq $AssetName -and
            $parts[0] -match '^[0-9a-fA-F]{64}$') {
            return $parts[0].ToLower()
        }
    }
    throw "SHA256SUMS has no valid entry for $AssetName."
}

# Stopping the daemon also tears down a run-script/shell transport launched by
# that daemon. Refuse this path before any process or file mutation; use an
# independent Windows console, WinRM, or another management channel instead.
function Test-IsRunningUnderBridgeSessions {
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId = $PID" `
        -ErrorAction SilentlyContinue
    for ($depth = 0; $proc -and $depth -lt 12; $depth++) {
        $parentId = [int]$proc.ParentProcessId
        if ($parentId -le 0) { break }
        $proc = Get-CimInstance Win32_Process -Filter "ProcessId = $parentId" `
            -ErrorAction SilentlyContinue
        if ($proc -and $proc.Name -ieq "bridgesessions.exe") { return $true }
    }
    return $false
}
if (Test-IsRunningUnderBridgeSessions) {
    throw "Refusing upgrade through BridgeSessions shell; use an independent Windows management channel."
}

# A version marker alone is not sufficient to decide that an install is current:
# same-version rebuilds and partial/corrupt installs can have different bytes.
# Resolve the trusted release hash before stopping any running service so a
# missing/malformed manifest cannot strand the peer offline.
$CURRENT = ""
if (Test-Path $VERSION_FILE) { $CURRENT = (Get-Content $VERSION_FILE -Raw).Trim() }
$needsDownload = $true
if ($CURRENT -eq $TAG -and (Test-Path $BIN_PATH -PathType Leaf)) {
    $expectedInstalledHash = Get-ReleaseAssetHash "bridgesessions-windows-x86_64.exe"
    $installedHash = (Get-FileHash $BIN_PATH -Algorithm SHA256).Hash.ToLower()
    if ($installedHash -eq $expectedInstalledHash) {
        Write-Host "-> bridgesessions $TAG already installed and checksum verified."
        $needsDownload = $false
    } else {
        Write-Host "-> Installed binary hash differs from release; reinstalling $TAG."
    }
}

# Stage and fully validate the replacement before disrupting the daemon. Keep
# the temporary file on the install volume so the later atomic-ish rename does
# not cross filesystem boundaries. A failed download, checksum, or version
# check must leave the running installation untouched.
$TMP_PATH = $null
if ($needsDownload) {
    New-Item -ItemType Directory -Force -Path $INSTALL_DIR | Out-Null
    $TMP_PATH = "$INSTALL_DIR\bridgesessions.download.$PID.exe"
    $URL = "$BASE/bridgesessions-windows-x86_64.exe"
    Write-Host "-> Downloading bridgesessions $TAG for Windows..."
    try {
        if ($STAGED_DIR) {
            $stagedBinary = Join-Path $STAGED_DIR "bridgesessions-windows-x86_64.exe"
            if (-not (Test-Path -LiteralPath $stagedBinary -PathType Leaf)) {
                throw "Staged Windows binary is missing: $stagedBinary"
            }
            Copy-Item -LiteralPath $stagedBinary -Destination $TMP_PATH -Force
        } else {
            Invoke-WebRequest -Uri $URL -OutFile $TMP_PATH -UseBasicParsing
        }

        $expected = Get-ReleaseAssetHash "bridgesessions-windows-x86_64.exe"
        $actual = (Get-FileHash $TMP_PATH -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $expected) {
            throw "Checksum mismatch (expected $expected, got $actual)."
        }
        $downloadedVersion = (& $TMP_PATH --version 2>&1 | Out-String).Trim()
        if ($downloadedVersion -ne $TAG.TrimStart('v')) {
            throw "Downloaded binary reports $downloadedVersion; expected $TAG."
        }
    } catch {
        Remove-Item $TMP_PATH -Force -ErrorAction SilentlyContinue
        throw
    }
    Write-Host "-> Downloaded binary SHA256 and version verified; ready to install."
}

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
            Write-Host "-> Killing bridgesessions PID $($p.ProcessId) (Session $($p.SessionId))..."
            try { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
        }
        Start-Sleep -Seconds 2
    }
} catch {}

# Also kill via taskkill as fallback
try { taskkill /f /im bridgesessions.exe 2>$null } catch {}
Start-Sleep -Seconds 1

# -- 3. INSTALL THE PRE-VERIFIED BINARY --------------------------------------
if ($needsDownload) {
    Move-Item $TMP_PATH $BIN_PATH -Force
    $TAG | Set-Content $VERSION_FILE
    Write-Host "-> Pre-verified binary installed."
}

# -- 4. VALIDATE -------------------------------------------------------------
$ver = & $BIN_PATH --version 2>&1
Write-Host "-> Version: $ver"

# -- 4b. Ensure INSTALL_DIR is on PATH (fresh Windows won't have it) --
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    [Environment]::SetEnvironmentVariable("Path", "$INSTALL_DIR;$userPath", "User")
    $env:Path = "$INSTALL_DIR;$env:Path"
    Write-Host "-> Added $INSTALL_DIR to user PATH (new terminals will find 'bridgesessions')"
}

# -- 5. APP DIRS + DEFAULT CONFIG + CLEANUP -----------------------------------
$RECEIVE_DIR = "$CONFIG_DIR\received"
New-Item -ItemType Directory -Force -Path $CONFIG_DIR | Out-Null
New-Item -ItemType Directory -Force -Path $RECEIVE_DIR | Out-Null

# Clean received/ dir if it has too many files (prevents daemon crash from
# binary files triggering JSON parse errors - known macOS/Windows issue)
$receiveFiles = Get-ChildItem -Path $RECEIVE_DIR -File -ErrorAction SilentlyContinue
if ($receiveFiles.Count -gt 50) {
    Write-Host "-> Cleaning $($receiveFiles.Count) files from received/ (prevents daemon crash)..."
    Remove-Item -Path "$RECEIVE_DIR\*" -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $CONFIG_PATH)) {
    Write-Host "-> Creating default config at $CONFIG_PATH..."
    $nodeName = $env:COMPUTERNAME
    $configContent = @"
# BridgeSessions config - generated by install.ps1
node.name $nodeName
node.listen 0.0.0.0:19949
receive_dir $RECEIVE_DIR
"@
    $configContent | Set-Content -Path $CONFIG_PATH -Encoding ASCII
}

# -- 6. ADD TO PATH ----------------------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    Write-Host "-> Adding $INSTALL_DIR to PATH..."
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$INSTALL_DIR", "User")
    $env:Path = "$env:Path;$INSTALL_DIR"
}

# -- 7. SCHEDULED TASK: DAEMON -----------------------------------------------
$daemonAction = New-ScheduledTaskAction -Execute $BIN_PATH -Argument "--daemon --config `"$CONFIG_PATH`""
$daemonTrigger = New-ScheduledTaskTrigger -AtStartup
$daemonSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)

$daemonTask = Get-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
if ($daemonTask) {
    Write-Host "-> Updating existing BridgeSessions scheduled task..."
    try {
        $daemonSetParams = @{
            TaskName = "BridgeSessions"
            Action = $daemonAction
            Trigger = $daemonTrigger
            Settings = $daemonSettings
        }
        if ($daemonTask.Principal.LogonType -eq "Password" -and
            $env:BRIDGESESSIONS_TASK_PASSWORD) {
            # WinRM cannot reuse the remote logon token as the task's stored
            # password. Accept it through a protected management-session
            # environment variable; never put it in the executable command line.
            $daemonSetParams.User = $daemonTask.Principal.UserId
            $daemonSetParams.Password = $env:BRIDGESESSIONS_TASK_PASSWORD
        }
        Set-ScheduledTask @daemonSetParams | Out-Null
    } catch {
        if ($daemonTask.Principal.LogonType -eq "Password") {
            if (-not $env:BRIDGESESSIONS_TASK_PASSWORD) {
                throw "BridgeSessions task uses password logon; provide BRIDGESESSIONS_TASK_PASSWORD via the protected management session."
            }
            throw "Password-aware update of the BridgeSessions task failed; refusing the schtasks fallback to avoid a hung or partially updated task. $($_.Exception.Message)"
        }
        # Some existing tasks cannot be re-registered through a WinRM logon
        # token because Task Scheduler would need the task principal's stored
        # password. Preserve that principal and update only its action.
        Write-Host "-> Set-ScheduledTask could not preserve the existing principal; falling back to schtasks /Change..."
        $daemonRun = '"' + $BIN_PATH + '" --daemon --config "' + $CONFIG_PATH + '"'
        & "$env:SystemRoot\System32\schtasks.exe" /Change /TN "BridgeSessions" /TR $daemonRun
        if ($LASTEXITCODE -ne 0) {
            throw "Could not update BridgeSessions task action (schtasks exit $LASTEXITCODE)."
        }
    }
    if ($daemonSetParams.ContainsKey("Password")) {
        $daemonSetParams.Password = $null
        Remove-Item Env:BRIDGESESSIONS_TASK_PASSWORD -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "-> Creating BridgeSessions scheduled task..."
    try {
        Register-ScheduledTask -TaskName "BridgeSessions" -Action $daemonAction -Trigger $daemonTrigger -Settings $daemonSettings -RunLevel Highest -Force | Out-Null
    } catch {
        Write-Host "-> Could not register daemon task with elevated privileges. Trying schtasks..."
        schtasks /create /tn "BridgeSessions" /tr "`"$BIN_PATH`" --daemon --config `"$CONFIG_PATH`"" /sc onstart /ru SYSTEM /f 2>$null
    }
}
Write-Host "-> Daemon scheduled task installed (BridgeSessions)."

# -- 7b. NO-WINDOW LAUNCHER ------------------------------------------------
# schtasks has no "hidden" option. When the Register-ScheduledTask path is
# unavailable we fall back to schtasks, which would otherwise show a console
# window at logon. This wrapper launches the helper with window style 0.
$HELPER_VBS = Join-Path $CONFIG_DIR "cua-helper-hidden.vbs"
@"
' Generated by the BridgeSessions installer. Launches the CUA helper with no
' console window. Regenerated on every install; safe to delete.
Set sh = CreateObject("WScript.Shell")
sh.Run """$BIN_PATH"" --config ""$CONFIG_PATH"" --cua-helper", 0, False
"@ | Set-Content -Path $HELPER_VBS -Encoding ASCII

# -- 8. SCHEDULED TASK: CUA HELPER -------------------------------------------
# -Hidden suppresses the task's window in Task Scheduler. The helper binary
# additionally re-execs itself with CREATE_NO_WINDOW, so no console window is
# shown on the desktop at logon.
$cuaAction = New-ScheduledTaskAction -Execute $BIN_PATH -Argument "--config `"$CONFIG_PATH`" --cua-helper"
$cuaTrigger = New-ScheduledTaskTrigger -AtLogOn
$cuaSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -Hidden -ExecutionTimeLimit ([TimeSpan]::Zero)

$cuaTask = Get-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -ErrorAction SilentlyContinue
if ($cuaTask) {
    Write-Host "-> Updating existing BridgeSessions-CuaHelper scheduled task..."
    try {
        Set-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -Action $cuaAction -Trigger $cuaTrigger -Settings $cuaSettings | Out-Null
    } catch {
        Write-Host "-> Set-ScheduledTask could not preserve the existing helper principal; falling back to schtasks /Change..."
        $helperRun = 'wscript.exe /nologo /b "' + $HELPER_VBS + '"'
        & "$env:SystemRoot\System32\schtasks.exe" /Change /TN "BridgeSessions-CuaHelper" /TR $helperRun
        if ($LASTEXITCODE -ne 0) {
            throw "Could not update BridgeSessions-CuaHelper task action (schtasks exit $LASTEXITCODE)."
        }
    }
} else {
    Write-Host "-> Creating BridgeSessions-CuaHelper scheduled task..."
    try {
        Register-ScheduledTask -TaskName "BridgeSessions-CuaHelper" -Action $cuaAction -Trigger $cuaTrigger -Settings $cuaSettings -User $env:USERNAME -Force | Out-Null
    } catch {
        Write-Host "-> Could not register CUA helper task. Trying schtasks..."
        schtasks /create /tn "BridgeSessions-CuaHelper" /tr "wscript.exe /nologo /b `"$HELPER_VBS`"" /sc onlogon /ru $env:USERNAME /f 2>$null
    }
}
Write-Host "-> CUA helper scheduled task installed (BridgeSessions-CuaHelper)."

# -- 9. ALWAYS START DAEMON ----------------------------------------------------
Write-Host "-> Starting daemon..."
Start-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
try {
    $newProcs = Get-CimInstance Win32_Process -Filter "Name like '%bridgesessions%'" -ErrorAction SilentlyContinue
    $daemonProc = $newProcs | Where-Object { $_.CommandLine -like "*--config*" -and $_.CommandLine -notlike "*--cua-helper*" } | Select-Object -First 1
    if ($daemonProc) {
        Write-Host "-> Daemon running (PID $($daemonProc.ProcessId))."
    } else {
        Write-Host "-> WARNING: Daemon did not start. Run manually:"
        Write-Host "   $BIN_PATH --daemon --config `"$CONFIG_PATH`""
    }
} catch {
    Write-Host "-> WARNING: Could not verify daemon status."
}

Write-Host "-> Daemon started."

} finally {
    Restore-BridgeSessionsDaemon
}

# -- 10. INSTALL TRAY APP -----------------------------------------------------
$TRAY_SCRIPT_DST = "$INSTALL_DIR\bs_tray.ps1"
# In the documented `irm ... | iex` install, PSScriptRoot is empty. Use the
# staged artifact directory for offline QA or fetch the versioned release asset.
$trayTmp = "$INSTALL_DIR\bs_tray.download.$PID.ps1"
try {
    if ($STAGED_DIR) {
        $stagedTray = Join-Path $STAGED_DIR "bs_tray.ps1"
        if (-not (Test-Path -LiteralPath $stagedTray -PathType Leaf)) {
            throw "Staged tray script is missing: $stagedTray"
        }
        Copy-Item -LiteralPath $stagedTray -Destination $trayTmp -Force
    } else {
        Invoke-WebRequest -Uri "$BASE/bs_tray.ps1" -OutFile $trayTmp `
            -UseBasicParsing
    }
    $trayExpected = Get-ReleaseAssetHash "bs_tray.ps1"
    $trayActual = (Get-FileHash $trayTmp -Algorithm SHA256).Hash.ToLower()
    if ($trayActual -ne $trayExpected) {
        throw "Tray script checksum mismatch (expected $trayExpected, got $trayActual)."
    }
    Move-Item $trayTmp $TRAY_SCRIPT_DST -Force
    Write-Host "-> Verified and installed tray script to $INSTALL_DIR."
} catch {
    Remove-Item $trayTmp -Force -ErrorAction SilentlyContinue
    Write-Host "-> WARNING: Could not install verified tray script: $($_.Exception.Message)"
}

if (Test-Path -LiteralPath $TRAY_SCRIPT_DST -PathType Leaf) {
    # Create startup shortcut (shell:startup)
    $startupDir = [Environment]::GetFolderPath("Startup")
    $shortcutPath = "$startupDir\Bridge Sessions Tray.lnk"
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath = "powershell.exe"
        $shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$TRAY_SCRIPT_DST`""
        $shortcut.WorkingDirectory = $INSTALL_DIR
        $shortcut.IconLocation = "$BIN_PATH,0"
        $shortcut.Description = "Bridge Sessions System Tray"
        $shortcut.WindowStyle = 7  # Minimized
        $shortcut.Save()
        Write-Host "-> Startup shortcut created: $shortcutPath"
    } catch {
        Write-Host "-> WARNING: Could not create startup shortcut: $($_.Exception.Message)"
    }
} else {
    Write-Host "-> WARNING: bs_tray.ps1 not found at $TRAY_SCRIPT_SRC. Skipping tray install."
}

# -- 11. LAUNCH TRAY APP ------------------------------------------------------
if (Test-Path $TRAY_SCRIPT_DST) {
    Write-Host "-> Launching tray app..."
    try {
        Start-Process -FilePath "powershell.exe" `
            -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", "`"$TRAY_SCRIPT_DST`"" `
            -WindowStyle Hidden
        Write-Host "-> Tray app launched."
    } catch {
        Write-Host "-> WARNING: Could not launch tray app: $($_.Exception.Message)"
    }
}

# -- 12. SCREEN RECORDING PERMISSION PROMPT (Windows Security) -----------------
# CUA helper needs Screen Recording permission on Windows 11. Open Settings to
# the "Allow screen recording" page so the user can grant it manually.
if ($env:BRIDGESESSIONS_SKIP_UI -eq "1") {
    Write-Host "-> Skipping screen-capture prompt for non-interactive install."
} else {
try {
    $osVer = [System.Environment]::OSVersion.Version
    if ($osVer.Major -ge 10) {
        Write-Host "-> Opening Windows Security for Screen Recording permission..."
        Write-Host "   If prompted, allow bridgesessions.exe to capture screen."

        # Open Settings > Privacy > Screen Recording (Windows 10/11)
        Start-Process "ms-settings:privacy-capability" -ErrorAction SilentlyContinue

        # Also show a helpful dialog
        Add-Type -AssemblyName System.Windows.Forms
        $msgResult = [System.Windows.Forms.MessageBox]::Show(
            "BridgeSessions needs Screen Recording permission for CUA automation.`n`n" +
            "If the Settings window opened, find 'bridgesessions' and enable it.`n`n" +
            "Click OK when done (or Cancel to skip).",
            "BridgeSessions - Permission Required",
            [System.Windows.Forms.MessageBoxButtons]::OKCancel,
            [System.Windows.Forms.MessageBoxIcon]::Information)

        if ($msgResult -eq [System.Windows.Forms.DialogResult]::OK) {
            Write-Host "-> User acknowledged permission prompt."
        }
    }
} catch {
    Write-Host "-> Could not open permission prompt: $($_.Exception.Message)"
}
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
