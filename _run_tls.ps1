$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions
$output = & .\test_tls.exe 2>&1
Write-Host $output
Write-Host "EXIT: $LASTEXITCODE"
