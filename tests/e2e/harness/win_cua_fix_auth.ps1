#Requires -Version 5.1
# Ensure exactly ONE bridgesessions --cua-helper owns 127.0.0.1:19986
$ErrorActionPreference = "Continue"
$InstallDir = Join-Path $env:LOCALAPPDATA "bridgesessions"
$Bin = Join-Path $InstallDir "bridgesessions.exe"
$ConfigDir = Join-Path $env:USERPROFILE ".bridgesessions"

# Prevent scheduled tasks from racing while we fix helper
foreach ($t in @("BS-CUA-Helper", "BS-Tray")) {
    Disable-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue | Out-Null
}

function Kill-CuaHelpers {
    Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.CommandLine -and ($_.CommandLine -match "cua-helper")) {
            Write-Host ("KILL_HELPER " + $_.ProcessId)
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
    Get-NetTCPConnection -LocalPort 19986 -State Listen -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host ("KILL_PORT19986 " + $_.OwningProcess)
        Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}

Kill-CuaHelpers
# Second pass - anything that respawned
Kill-CuaHelpers

# Mesh daemon must stay (no cua-helper in cmdline)
$mesh = @(Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | Where-Object {
    $_.CommandLine -and ($_.CommandLine -notmatch "cua-helper")
})
if ($mesh.Count -eq 0) {
    $cfg = Join-Path $ConfigDir "config"
    Start-Process -FilePath $Bin -ArgumentList @("--config", $cfg) -WorkingDirectory $InstallDir -WindowStyle Hidden
    Start-Sleep -Seconds 2
    Write-Host "RESTARTED_MESH"
}

Remove-Item (Join-Path $ConfigDir "cua-helper-token") -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

# Kill any OTHER helpers that appeared after us
Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.CommandLine -and ($_.CommandLine -match "cua-helper") -and ($_.ProcessId -ne $p.Id)) {
        Write-Host ("KILL_EXTRA " + $_.ProcessId)
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
# If someone else owns the port, kill them and restart once
$own = Get-NetTCPConnection -LocalPort 19986 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
if ($own -and $own.OwningProcess -ne $p.Id) {
    Write-Host ("STEAL_PORT from " + $own.OwningProcess)
    Stop-Process -Id $own.OwningProcess -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item (Join-Path $ConfigDir "cua-helper-token") -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
}

$own = Get-NetTCPConnection -LocalPort 19986 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
Write-Host ("HELPER_PID=" + $p.Id + " exited=" + $p.HasExited + " listen_pid=" + $(if ($own) { $own.OwningProcess } else { "none" }))

$tokenPath = Join-Path $ConfigDir "cua-helper-token"
if (-not (Test-Path $tokenPath)) { Write-Host "NO_TOKEN"; exit 1 }
$token = [IO.File]::ReadAllText($tokenPath).Trim()
Write-Host ("token_len=" + $token.Length)

$c = New-Object System.Net.Sockets.TcpClient
try {
    $c.Connect("127.0.0.1", 19986)
    $s = $c.GetStream()
    $bytes = [Text.Encoding]::UTF8.GetBytes(($token + ' {"action":0}' + "`n"))
    $s.Write($bytes, 0, $bytes.Length)
    $buf = New-Object byte[] 8192
    $n = $s.Read($buf, 0, $buf.Length)
    $resp = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    Write-Host ("RESP=" + $resp.Trim())
    if ($resp -match '"status"\s*:\s*0') {
        Write-Host "CUA_LOCAL_OK"
        # Re-enable logon task for helper only (single action)
        Enable-ScheduledTask -TaskName "BS-CUA-Helper" -ErrorAction SilentlyContinue | Out-Null
        exit 0
    }
} catch {
    Write-Host ("CONNECT_FAIL " + $_.Exception.Message)
} finally {
    $c.Close()
}
Write-Host "CUA_LOCAL_FAIL"
exit 1
