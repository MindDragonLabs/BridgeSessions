$Env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $Env:PATH

$exe = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
$cfg = "C:\Users\Shadow\.bridgesessions\config"

# Test 1: health check Shadow -> linux-b
Write-Host "=== TEST 1: Shadow -> linux-b health ==="
$r1 = Start-Process -FilePath $exe -ArgumentList "--config","$cfg","health","linux-b" -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$env:TEMP\t1.txt" -RedirectStandardError "$env:TEMP\t2.txt"
Get-Content "$env:TEMP\t1.txt","$env:TEMP\t2.txt"
Write-Host "Exit: $($r1.ExitCode)"

# Test 2: list sessions on linux-b
Write-Host "=== TEST 2: Shadow sessions linux-b ==="
$r2 = Start-Process -FilePath $exe -ArgumentList "--config","$cfg","sessions","linux-b" -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$env:TEMP\t1.txt" -RedirectStandardError "$env:TEMP\t2.txt"
Get-Content "$env:TEMP\t1.txt","$env:TEMP\t2.txt"
Write-Host "Exit: $($r2.ExitCode)"

# Test 3: stats
Write-Host "=== TEST 3: Shadow stats ==="
$r3 = Start-Process -FilePath $exe -ArgumentList "--config","$cfg","stats" -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$env:TEMP\t1.txt" -RedirectStandardError "$env:TEMP\t2.txt"
Get-Content "$env:TEMP\t1.txt","$env:TEMP\t2.txt"
Write-Host "Exit: $($r3.ExitCode)"

# Test 4: linux-b -> Shadow health (from SSH)
Write-Host "=== TEST 4: linux-b -> Shadow health ==="
ssh linux-b "/home/agent/bridgesessions/bsmesh --config /home/agent/.bridgesessions/config health shadow"
