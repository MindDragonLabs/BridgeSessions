$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$env:PATH = "$msvc\bin\Hostx64\x64;$vcpkg\bin;$env:PATH"
Set-Location C:\Users\Shadow\bridgesessions

Write-Host "=== version ==="
.\bridgesessions.exe --version 2>&1
Write-Host "exit: $LASTEXITCODE"

Write-Host "=== help ==="
.\bridgesessions.exe --help 2>&1 | Select-Object -First 5
Write-Host "exit: $LASTEXITCODE"

Write-Host "=== keygen --help ==="
.\bridgesessions.exe keygen --help 2>&1 | Select-Object -First 3
Write-Host "exit: $LASTEXITCODE"

Write-Host "=== authorize (no args) ==="
.\bridgesessions.exe authorize 2>&1
Write-Host "exit: $LASTEXITCODE"

Write-Host "=== peers list ==="
$job = Start-Job { Set-Location C:\Users\Shadow\bridgesessions; .\bridgesessions.exe peers list 2>&1 }
$done = Wait-Job $job -Timeout 8
if (-not $done) { Stop-Job $job -PassThru | Remove-Job -Force; Write-Host "(timed out - ok)" }
else { Receive-Job $job 2>&1; Remove-Job $job -Force }
Write-Host "exit: $LASTEXITCODE"
