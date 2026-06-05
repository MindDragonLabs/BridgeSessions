$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions
$result = & .\test_session_registry.exe 2>&1
$result
Write-Host "EXIT: $LASTEXITCODE"
