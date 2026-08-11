#Requires -Version 5.1
$ErrorActionPreference = "Continue"
$InstallDir = Join-Path $env:LOCALAPPDATA "bridgesessions"
$Bin = Join-Path $InstallDir "bridgesessions.exe"
$ConfigDir = Join-Path $env:USERPROFILE ".bridgesessions"

# Kill every bridgesessions that is NOT pure mesh daemon without cua-helper
Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    $cl = $_.CommandLine
    if (-not $cl) { return }
    if ($cl -match "cua-helper") {
        Write-Host ("KILL_HELPER " + $_.ProcessId)
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Seconds 2

# Ensure only one mesh daemon remains
$mesh = Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue
if (-not $mesh) {
    Write-Host "START_MESH"
    $cfg = Join-Path $ConfigDir "config"
    Start-Process -FilePath $Bin -ArgumentList @("--config", $cfg) -WorkingDirectory $InstallDir -WindowStyle Hidden
    Start-Sleep -Seconds 2
}

Remove-Item (Join-Path $ConfigDir "cua-helper-token") -ErrorAction SilentlyContinue
$err = Join-Path $InstallDir "cua-helper.err"
$out = Join-Path $InstallDir "cua-helper.out"
Remove-Item $err, $out -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Bin -ArgumentList "--cua-helper" -WorkingDirectory $InstallDir `
    -RedirectStandardError $err -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
Write-Host ("HELPER_PID=" + $p.Id + " EXITED=" + $p.HasExited)
if (Test-Path $out) { Get-Content $out | ForEach-Object { Write-Host ("OUT " + $_) } }

$tokenPath = Join-Path $ConfigDir "cua-helper-token"
if (-not (Test-Path $tokenPath)) { Write-Host "NO_TOKEN"; exit 1 }
$token = (Get-Content $tokenPath -Raw).Trim()
Write-Host ("token_len=" + $token.Length)

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect("127.0.0.1", 19986)
    $stream = $client.GetStream()
    $msg = $token + ' {"action":0}' + "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
    $stream.Write($bytes, 0, $bytes.Length)
    $buf = New-Object byte[] 8192
    $n = $stream.Read($buf, 0, $buf.Length)
    $resp = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    Write-Host ("RESP=" + $resp)
    if ($resp -match '"status"\s*:\s*0') { Write-Host "CUA_LOCAL_OK" } else { Write-Host "CUA_LOCAL_FAIL" }
} catch {
    Write-Host ("CONNECT_FAIL " + $_.Exception.Message)
} finally {
    $client.Close()
}

Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ("PROC " + $_.ProcessId + " " + $_.SessionId + " " + $_.CommandLine)
}
