#Requires -Version 5.1
$ErrorActionPreference = "Continue"
$tokenPath = Join-Path $env:USERPROFILE ".bridgesessions\cua-helper-token"
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
    Write-Host ("RESP=" + [Text.Encoding]::UTF8.GetString($buf, 0, $n))
} catch {
    Write-Host ("CONNECT_FAIL " + $_.Exception.Message)
} finally {
    $client.Close()
}
