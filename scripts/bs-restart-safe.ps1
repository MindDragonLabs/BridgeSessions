# bs-restart-safe.ps1 — robust daemon restart that survives its own transport.
# Fixes audit F5: the old .cmd used `start /MIN` whose quoting silently failed;
# this variant uses a scheduled-task one-shot (survives parent exit, no console
# flash, works from any shell including a dying bs session).
#
# Usage (from any admin/user shell on the Windows peer):
#   powershell -NoProfile -ExecutionPolicy Bypass -File bs-restart-safe.ps1
$ErrorActionPreference = 'Stop'
$exe  = "$env:LOCALAPPDATA\bridgesessions\bridgesessions.exe"
$cfg  = "$env:USERPROFILE\.bridgesessions\config"

if (-not (Test-Path $exe)) { Write-Output "ERROR: binary not found: $exe"; exit 1 }
if (-not (Test-Path $cfg)) { Write-Output "ERROR: config not found: $cfg"; exit 1 }

# 1) Stop existing daemon (graceful first, force after 5s)
$proc = Get-Process bridgesessions -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $exe }
if ($proc) {
    Write-Output "stopping pid(s): $($proc.Id -join ',')"
    $proc | Stop-Process -Force
    Start-Sleep -Seconds 3
}

# 2) Start via a one-shot scheduled task (detached, hidden, no quoting traps).
#    Using the task scheduler means the process outlives THIS shell even when
#    the bs shell transport dies mid-restart (the F5 failure mode).
$action  = New-ScheduledTaskAction -Execute $exe -Argument "--daemon --config `"$cfg`""
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(2)
Register-ScheduledTask -TaskName 'BS-Restart' -Action $action -Trigger $trigger -Force | Out-Null
Write-Output "restart task registered; firing in 2s"

# 3) Wait for the daemon to listen on 19949 (up to 30s)
$deadline = (Get-Date).AddSeconds(30)
$up = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $t = Test-NetConnection -ComputerName 127.0.0.1 -Port 19949 -WarningAction SilentlyContinue
    if ($t.TcpTestSucceeded) { $up = $true; break }
}
Unregister-ScheduledTask -TaskName 'BS-Restart' -Confirm:$false -ErrorAction SilentlyContinue
if ($up) {
    Write-Output "OK daemon listening on 19949"
    & $exe --version
    exit 0
} else {
    Write-Output "ERROR: daemon did not come up in 30s — check %USERPROFILE%\.bridgesessions\bs-mesh.log"
    Get-Content "$env:USERPROFILE\.bridgesessions\bs-mesh.log" -Tail 10
    exit 1
}
