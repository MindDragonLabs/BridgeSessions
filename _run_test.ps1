$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions
& .\test_frame_io.exe 2>&1
Write-Host "EXIT: $LASTEXITCODE"
