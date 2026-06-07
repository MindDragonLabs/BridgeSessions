$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions
$out = & .\bridgesessions.exe --version 2>&1
Write-Host $out
$code = $LASTEXITCODE
Write-Host "EXIT: $code"
