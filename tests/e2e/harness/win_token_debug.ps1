#Requires -Version 5.1
Get-CimInstance Win32_Process -Filter "Name='bridgesessions.exe'" | Format-List ProcessId,SessionId,ParentProcessId,CommandLine
Get-ScheduledTask | Where-Object { $_.TaskName -match "BS|bridge|cua" } | Format-Table TaskName,State
$p = Join-Path $env:USERPROFILE ".bridgesessions\cua-helper-token"
$b = [IO.File]::ReadAllBytes($p)
Write-Host ("token_bytes_len=" + $b.Length)
Write-Host ("token_hex=" + [BitConverter]::ToString($b[0..([Math]::Min(16,$b.Length-1))]))
$t = [Text.Encoding]::UTF8.GetString($b).Trim()
Write-Host ("token_str_len=" + $t.Length)
# Compare what helper might have - try both raw and trim
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect("127.0.0.1", 19986)
$stream = $client.GetStream()
foreach ($variant in @($t, $t + "`n", [Text.Encoding]::UTF8.GetString($b))) {
    $msg = $variant.TrimEnd("`r","`n") + ' {"action":0}' + "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
    try {
        $c2 = New-Object System.Net.Sockets.TcpClient
        $c2.Connect("127.0.0.1", 19986)
        $s2 = $c2.GetStream()
        $s2.Write($bytes, 0, $bytes.Length)
        $buf = New-Object byte[] 4096
        $n = $s2.Read($buf, 0, $buf.Length)
        Write-Host ("TRY len=" + $variant.Length + " RESP=" + [Text.Encoding]::UTF8.GetString($buf, 0, $n))
        $c2.Close()
    } catch {
        Write-Host ("TRY fail " + $_.Exception.Message)
    }
}
$client.Close()
