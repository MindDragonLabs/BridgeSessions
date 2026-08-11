#Requires -Version 5.1
$ErrorActionPreference = "Continue"
$InstallDir = Join-Path $env:LOCALAPPDATA "bridgesessions"
$Bin = Join-Path $InstallDir "bridgesessions.exe"
$ConfigDir = Join-Path $env:USERPROFILE ".bridgesessions"

Disable-ScheduledTask -TaskName "BS-CUA-Helper" -ErrorAction SilentlyContinue | Out-Null
Disable-ScheduledTask -TaskName "bridgesessions" -ErrorAction SilentlyContinue | Out-Null

function Stop-ByPort([int]$port) {
    Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host ("KILL_PORT " + $port + " pid=" + $_.OwningProcess)
        Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue
    }
}

# Kill helpers and anything on 19986
Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" | ForEach-Object {
    if ($_.CommandLine -match "cua-helper") {
        Write-Host ("KILL_HELPER " + $_.ProcessId)
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Stop-ByPort 19986
Start-Sleep -Seconds 2

# Ensure mesh daemon still running (no cua-helper flag)
$mesh = Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" | Where-Object {
    $_.CommandLine -and ($_.CommandLine -notmatch "cua-helper")
}
if (-not $mesh) {
    $cfg = Join-Path $ConfigDir "config"
    Start-Process -FilePath $Bin -ArgumentList @("--config", $cfg) -WorkingDirectory $InstallDir -WindowStyle Hidden
    Start-Sleep -Seconds 2
    Write-Host "RESTARTED_MESH"
}

Remove-Item (Join-Path $ConfigDir "cua-helper-token") -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
Write-Host ("HELPER_PID=" + $p.Id + " exited=" + $p.HasExited)

$listeners = @(Get-NetTCPConnection -LocalPort 19986 -State Listen -ErrorAction SilentlyContinue)
foreach ($l in $listeners) { Write-Host ("LISTEN pid=" + $l.OwningProcess) }
if ($listeners.Count -ne 1 -or $listeners[0].OwningProcess -ne $p.Id) {
    Write-Host ("WARN listener mismatch helper=" + $p.Id)
}

$tokenPath = Join-Path $ConfigDir "cua-helper-token"
if (-not (Test-Path $tokenPath)) { Write-Host "NO_TOKEN"; exit 1 }
$token = [IO.File]::ReadAllText($tokenPath).Trim()
Write-Host ("token_prefix=" + $token.Substring(0, [Math]::Min(8, $token.Length)) + " len=" + $token.Length)

$c = New-Object System.Net.Sockets.TcpClient
$c.Connect("127.0.0.1", 19986)
$s = $c.GetStream()
$payload = $token + ' {"action":0}' + "`n"
$bytes = [Text.Encoding]::UTF8.GetBytes($payload)
$s.Write($bytes, 0, $bytes.Length)
$buf = New-Object byte[] 8192
$n = $s.Read($buf, 0, $buf.Length)
$resp = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
Write-Host ("RESP=" + $resp.Trim())
$c.Close()

if ($resp -match '"status"\s*:\s*0') {
    Write-Host "CUA_LOCAL_OK"
    # re-enable mesh task only
    Enable-ScheduledTask -TaskName "bridgesessions" -ErrorAction SilentlyContinue | Out-Null
    exit 0
}
Write-Host "CUA_LOCAL_FAIL"
exit 1
