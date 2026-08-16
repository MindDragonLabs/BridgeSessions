#requires -Version 5.1
# BridgeSessions system tray app (PowerShell + WinForms NotifyIcon)
# No external dependencies - uses only built-in .NET assemblies.
#
# Right-click tray icon for: Fleet Status, Settings, Restart CUA Helper,
# Restart Daemon, Quit. Polls fleet every 10s. Auto-restarts CUA helper
# on crash. Runs until Quit is selected.

$ErrorActionPreference = "Stop"

# -- Paths --------------------------------------------------------------------
# Resolve install dir: script location first, then common deploy paths.
# (Deployments use %USERPROFILE%\bridgesessions or LOCALAPPDATA\bridgesessions.)
function Resolve-InstallDir {
    $candidates = @()
    if ($PSScriptRoot) { $candidates += $PSScriptRoot }
    if ($MyInvocation.MyCommand.Path) {
        $candidates += (Split-Path -Parent $MyInvocation.MyCommand.Path)
    }
    $candidates += @(
        "$env:LOCALAPPDATA\bridgesessions",
        "$env:USERPROFILE\bridgesessions"
    )
    foreach ($d in $candidates) {
        if ($d -and (Test-Path (Join-Path $d "bridgesessions.exe"))) { return $d }
    }
    # Fall back even if exe missing (error surfaces later)
    if ($env:LOCALAPPDATA) { return "$env:LOCALAPPDATA\bridgesessions" }
    return "$env:USERPROFILE\bridgesessions"
}
$INSTALL_DIR  = Resolve-InstallDir
$BIN_PATH     = Join-Path $INSTALL_DIR "bridgesessions.exe"
# Config: prefer USERPROFILE; when shell is SYSTEM, infer from install parent.
$CONFIG_DIR = if (Test-Path "$env:USERPROFILE\.bridgesessions\config") {
    "$env:USERPROFILE\.bridgesessions"
} elseif ($INSTALL_DIR -match '^(.*)\\bridgesessions$') {
    $parent = $Matches[1]
    if (Test-Path "$parent\.bridgesessions\config") { "$parent\.bridgesessions" }
    elseif (Test-Path (Join-Path $INSTALL_DIR "..\.bridgesessions\config")) {
        (Resolve-Path (Join-Path $INSTALL_DIR "..\.bridgesessions")).Path
    } else { "$env:USERPROFILE\.bridgesessions" }
} else {
    "$env:USERPROFILE\.bridgesessions"
}
$CONFIG_PATH  = Join-Path $CONFIG_DIR "config"

# -- Settings (persisted to JSON) ---------------------------------------------
$SETTINGS_FILE = "$CONFIG_DIR\tray-settings.json"
$script:Settings = @{
    AutoLaunch      = $true
    RestartHelper   = $true
    FleetPollSeconds = 10
}

function Load-Settings {
    if (Test-Path $SETTINGS_FILE) {
        try {
            $loaded = Get-Content $SETTINGS_FILE -Raw | ConvertFrom-Json
            $script:Settings.AutoLaunch       = $loaded.AutoLaunch
            $script:Settings.RestartHelper    = $loaded.RestartHelper
            $script:Settings.FleetPollSeconds = $loaded.FleetPollSeconds
        } catch {
            # Corrupt settings - keep defaults
        }
    }
}

function Save-Settings {
    $script:Settings | ConvertTo-Json | Set-Content $SETTINGS_FILE -Encoding UTF8
}

# -- Create the "B" tray icon (static ICO/PNG if present, else draw) ----------
function Create-TrayIcon {
    Add-Type -AssemblyName System.Drawing
    $static = @(
        (Join-Path $INSTALL_DIR "icon-b.ico"),
        (Join-Path $INSTALL_DIR "icon-b.png"),
        (Join-Path $CONFIG_DIR "icon-b.ico")
    )
    foreach ($p in $static) {
        if (Test-Path $p) {
            try {
                if ($p -like "*.ico") { return New-Object System.Drawing.Icon($p) }
                $bmpFile = [System.Drawing.Bitmap]::FromFile($p)
                $hicon = $bmpFile.GetHicon()
                return [System.Drawing.Icon]::FromHandle($hicon)
            } catch {}
        }
    }
    $bmp = New-Object System.Drawing.Bitmap(32, 32)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # Dark blue circle background
    $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 63, 169, 224))
    $g.FillEllipse($brush, 1, 1, 30, 30)
    $brush.Dispose()

    # White "B"
    $font = New-Object System.Drawing.Font("Segoe UI Bold", 16)
    $textBrush = [System.Drawing.Brushes]::White
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $g.DrawString("B", $font, $textBrush, (New-Object System.Drawing.RectangleF(0, 0, 32, 32)), $sf)
    $font.Dispose()
    $g.Dispose()

    $hicon = $bmp.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($hicon)
    return $icon
}

# -- Run a bridgesessions command and return output ---------------------------
function Invoke-BsCommand {
    param([string]$Arguments)
    if (-not (Test-Path $BIN_PATH)) { return "bridgesessions.exe not found at $BIN_PATH" }
    try {
        $result = & $BIN_PATH $Arguments.Split(' ') 2>&1
        return ($result | Out-String).Trim()
    } catch {
        return "Error: $($_.Exception.Message)"
    }
}

# -- Fleet status polling -----------------------------------------------------
$script:FleetStatusText = "No data yet..."
$script:LastFleetPoll = [DateTime]::MinValue

function Update-FleetStatus {
    $now = Get-Date
    if (($now - $script:LastFleetPoll).TotalSeconds -lt $script:Settings.FleetPollSeconds) { return }
    $script:LastFleetPoll = $now

    $output = Invoke-BsCommand "fleet"
    if ([string]::IsNullOrWhiteSpace($output)) {
        $script:FleetStatusText = "No peers connected."
    } else {
        $script:FleetStatusText = $output
    }
}

# -- CUA helper lifecycle management ------------------------------------------
$script:CuaHelperProcess = $null
$script:CuaHelperLastCheck = Get-Date

$script:CuaHelperStarting = $false

function Stop-Session0CuaHelpers {
    # Session-0 (services) helpers cannot capture the interactive desktop and
    # still overwrite cua-helper-token — always remove them.
    try {
        Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and ($_.CommandLine -match 'cua-helper') } |
            ForEach-Object {
                try {
                    $gp = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
                    if ($gp -and $gp.SessionId -eq 0) {
                        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
                    }
                } catch {}
            }
    } catch {}
}

function Find-ExistingCuaHelper {
    # Prefer an already-running Session-1 helper (schtasks /IT). Starting a second
    # helper overwrites cua-helper-token and bricks auth for the first instance.
    Stop-Session0CuaHelpers
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and ($_.CommandLine -match 'cua-helper') }
        foreach ($p in $procs) {
            try {
                $gp = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
                if ($gp -and $gp.SessionId -ne 0) {
                    return $gp
                }
            } catch {}
        }
    } catch {}
    return $null
}

function Count-CuaHelpers {
    try {
        return @(Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and ($_.CommandLine -match 'cua-helper') }).Count
    } catch { return 0 }
}

function Stop-AllCuaHelpers {
    # Kill every helper process (any session) so only one can own the token/port.
    try {
        Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and ($_.CommandLine -match 'cua-helper') } |
            ForEach-Object {
                try { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
            }
    } catch {}
    $script:CuaHelperProcess = $null
}

function Start-CuaHelper {
    if (-not (Test-Path $BIN_PATH)) { return }
    if ($script:CuaHelperStarting) { return }
    if ($script:CuaHelperProcess -and -not $script:CuaHelperProcess.HasExited) { return }

    $existing = Find-ExistingCuaHelper
    if ($existing) {
        # Extra helpers already running → kill extras, keep the first Session-1.
        if ((Count-CuaHelpers) -gt 1) {
            Write-Host "-> Multiple CUA helpers detected; reducing to one"
            $keep = $existing.Id
            Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
                Where-Object { $_.CommandLine -match 'cua-helper' -and $_.ProcessId -ne $keep } |
                ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        }
        $script:CuaHelperProcess = $existing
        Write-Host ("-> CUA helper already running (PID {0}, session {1}) - attaching" -f $existing.Id, $existing.SessionId)
        return
    }

    $script:CuaHelperStarting = $true
    try {
        # Prefer interactive scheduled task (Session 1). Never Process.Start when
        # the task exists — that races schtasks and dual-starts helpers.
        $task = $null
        try { $task = Get-ScheduledTask -TaskName 'BS-CUA-Helper' -ErrorAction SilentlyContinue } catch {}
        if ($task) {
            Start-ScheduledTask -TaskName 'BS-CUA-Helper' -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt 12; $i++) {
                Start-Sleep -Milliseconds 500
                $existing = Find-ExistingCuaHelper
                if ($existing) {
                    $script:CuaHelperProcess = $existing
                    Write-Host ("-> CUA helper started via BS-CUA-Helper task (PID {0})" -f $existing.Id)
                    return
                }
            }
            Write-Host "-> BS-CUA-Helper task did not produce a Session-1 helper"
            return
        }

        # No schtask: last-resort spawn (still only if zero helpers)
        if ((Count-CuaHelpers) -gt 0) { return }
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $BIN_PATH
        $psi.Arguments = "--config `"$CONFIG_PATH`" --cua-helper"
        $psi.UseShellExecute = $false
        $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
        $psi.CreateNoWindow = $true
        $script:CuaHelperProcess = [System.Diagnostics.Process]::Start($psi)
        Write-Host ("-> CUA helper started (PID {0})" -f $script:CuaHelperProcess.Id)
    } catch {
        Write-Host ("-> Failed to start CUA helper: {0}" -f $_.Exception.Message)
    } finally {
        $script:CuaHelperStarting = $false
    }
}

function Restart-CuaHelper {
    # Always wipe all helpers first so we never leave two tokens fighting.
    Stop-AllCuaHelpers
    Start-Sleep -Seconds 1
    Start-CuaHelper
}

function Watch-CuaHelper {
    if (-not $script:Settings.RestartHelper) { return }
    if ($script:CuaHelperStarting) { return }
    if ($script:CuaHelperProcess -and -not $script:CuaHelperProcess.HasExited) {
        # Still enforce single-instance while healthy
        if ((Count-CuaHelpers) -gt 1) {
            $keep = $script:CuaHelperProcess.Id
            Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue |
                Where-Object { $_.CommandLine -match 'cua-helper' -and $_.ProcessId -ne $keep } |
                ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        }
        return
    }
    # Re-attach or start exactly one helper (via task / single-flight).
    $existing = Find-ExistingCuaHelper
    if ($existing) {
        $script:CuaHelperProcess = $existing
        return
    }
    if ($script:CuaHelperProcess) {
        Write-Host ("-> CUA helper crashed (exit {0}). Restarting..." -f $script:CuaHelperProcess.ExitCode)
    }
    Start-CuaHelper
}

# -- Restart daemon (via scheduled task or direct) ----------------------------
function Restart-Daemon {
    try {
        # Kill existing daemon
        $procs = Get-CimInstance Win32_Process -Filter "Name like '%bridgesessions%'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like "*--config*" -and $_.CommandLine -notlike "*--cua-helper*" }
        foreach ($p in $procs) {
            try { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue } catch {}
        }
        Start-Sleep -Seconds 1

        # Restart via scheduled task
        Start-ScheduledTask -TaskName "BridgeSessions" -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2

        # Verify
        $daemonProc = Get-CimInstance Win32_Process -Filter "Name like '%bridgesessions%'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like "*--config*" -and $_.CommandLine -notlike "*--cua-helper*" } |
            Select-Object -First 1
        if ($daemonProc) {
            [System.Windows.Forms.MessageBox]::Show(
                "Daemon restarted successfully (PID $($daemonProc.ProcessId)).",
                "BridgeSessions", 0, 64)
        } else {
            [System.Windows.Forms.MessageBox]::Show(
                "Daemon restart attempted but process not found.`nCheck Task Scheduler.",
                "BridgeSessions", 0, 48)
        }
    } catch {
        [System.Windows.Forms.MessageBox]::Show("Error restarting daemon: $($_.Exception.Message)", "BridgeSessions", 0, 16)
    }
}

# -- Show fleet status popup --------------------------------------------------
function Show-FleetStatus {
    Update-FleetStatus
    $title = "BridgeSessions Fleet Status"
    $body = if ([string]::IsNullOrWhiteSpace($script:FleetStatusText)) { "No peers connected." } else { $script:FleetStatusText }
    [System.Windows.Forms.MessageBox]::Show($body, $title, 0, 64)
}

# -- Build the tray icon and context menu -------------------------------------
function Start-Tray {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    Load-Settings

    $notify = New-Object System.Windows.Forms.NotifyIcon
    $notify.Icon = Create-TrayIcon
    $notify.Text = "BridgeSessions"
    $notify.Visible = $true

    # Double-click shows fleet status
    $notify.Add_DoubleClick({ Show-FleetStatus })

    # -- Context menu --
    $menu = New-Object System.Windows.Forms.ContextMenuStrip

    # Fleet Status
    $miFleet = New-Object System.Windows.Forms.ToolStripMenuItem("Fleet Overview")
    $miFleet.Add_Click({ Show-FleetStatus })
    $menu.Items.Add($miFleet) | Out-Null

    $menu.Items.Add((New-Object System.Windows.Forms.ToolStripSeparator)) | Out-Null

    # Settings submenu
    $miSettings = New-Object System.Windows.Forms.ToolStripMenuItem("Settings")

    $cbAutoLaunch = New-Object System.Windows.Forms.ToolStripMenuItem("Auto-launch at startup")
    $cbAutoLaunch.CheckOnClick = $true
    $cbAutoLaunch.Checked = $script:Settings.AutoLaunch
    $cbAutoLaunch.Add_Click({
        $script:Settings.AutoLaunch = $cbAutoLaunch.Checked
        Save-Settings
        # Actually create/remove Startup shortcut (not JSON-only).
        $startupDir = [Environment]::GetFolderPath("Startup")
        $lnk = Join-Path $startupDir "BridgeSessions Tray.lnk"
        if ($cbAutoLaunch.Checked) {
            $ws = New-Object -ComObject WScript.Shell
            $sc = $ws.CreateShortcut($lnk)
            $sc.TargetPath = "powershell.exe"
            $sc.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`""
            $sc.WorkingDirectory = $INSTALL_DIR
            $sc.WindowStyle = 7
            $sc.Description = "BridgeSessions System Tray"
            $sc.Save()
        } else {
            if (Test-Path $lnk) { Remove-Item -Force $lnk -ErrorAction SilentlyContinue }
        }
    })
    $miSettings.DropDownItems.Add($cbAutoLaunch) | Out-Null

    $cbRestartHelper = New-Object System.Windows.Forms.ToolStripMenuItem("Auto-restart CUA helper")
    $cbRestartHelper.CheckOnClick = $true
    $cbRestartHelper.Checked = $script:Settings.RestartHelper
    $cbRestartHelper.Add_Click({
        $script:Settings.RestartHelper = $cbRestartHelper.Checked
        Save-Settings
    })
    $miSettings.DropDownItems.Add($cbRestartHelper) | Out-Null

    $menu.Items.Add($miSettings) | Out-Null

    $menu.Items.Add((New-Object System.Windows.Forms.ToolStripSeparator)) | Out-Null

    # Restart CUA Helper
    $miRestartCua = New-Object System.Windows.Forms.ToolStripMenuItem("Restart CUA Helper")
    $miRestartCua.Add_Click({
        Restart-CuaHelper
        [System.Windows.Forms.MessageBox]::Show("CUA helper restarted.", "BridgeSessions", 0, 64)
    })
    $menu.Items.Add($miRestartCua) | Out-Null

    # Restart Daemon
    $miRestartDaemon = New-Object System.Windows.Forms.ToolStripMenuItem("Restart Daemon")
    $miRestartDaemon.Add_Click({ Restart-Daemon })
    $menu.Items.Add($miRestartDaemon) | Out-Null

    # Open Logs
    $miLogs = New-Object System.Windows.Forms.ToolStripMenuItem("Open Logs")
    $miLogs.Add_Click({
        $logDir = Join-Path $INSTALL_DIR "logs"
        if (-not (Test-Path $logDir)) { $logDir = $CONFIG_DIR }
        if (-not (Test-Path $logDir)) { $logDir = $INSTALL_DIR }
        Start-Process explorer.exe $logDir
    })
    $menu.Items.Add($miLogs) | Out-Null

    $menu.Items.Add((New-Object System.Windows.Forms.ToolStripSeparator)) | Out-Null

    # Quit
    $miQuit = New-Object System.Windows.Forms.ToolStripMenuItem("Quit")
    $miQuit.Add_Click({
        # Clean up CUA helper
        if ($script:CuaHelperProcess -and -not $script:CuaHelperProcess.HasExited) {
            try { $script:CuaHelperProcess.Kill() } catch {}
        }
        $notify.Visible = $false
        $notify.Dispose()
        [System.Windows.Forms.Application]::Exit()
    })
    $menu.Items.Add($miQuit) | Out-Null

    $notify.ContextMenuStrip = $menu

    # -- Timer: poll fleet status + watch CUA helper --------------------------
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = $script:Settings.FleetPollSeconds * 1000
    $timer.Add_Tick({
        Update-FleetStatus
        Watch-CuaHelper
    })

    # Start CUA helper on launch
    Start-CuaHelper

    # Start the poll timer
    $timer.Start()

    # Initial fleet status fetch
    Update-FleetStatus

    Write-Host '-> BridgeSessions tray app running. Right-click the B icon.'

    # Run the application loop (blocks until Application.Exit)
    [System.Windows.Forms.Application]::Run()

    # Cleanup
    $timer.Stop()
    $timer.Dispose()
}

# -- Entry point ---------------------------------------------------------------
# Single-instance: a second tray races Start-CuaHelper and leaves dual helpers.
$mutex = $null
try {
    $created = $false
    $mutex = New-Object System.Threading.Mutex($true, 'Global\BridgeSessionsTray', [ref]$created)
    if (-not $created) {
        Write-Host '-> BridgeSessions tray already running - exiting'
        exit 0
    }
} catch {
    # If mutex fails (session isolation), still start; attach-only limits damage.
}

Start-Tray

if ($mutex) {
    try { $mutex.ReleaseMutex() | Out-Null } catch {}
    $mutex.Dispose()
}
