$msvc = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
$vcpkg = 'C:\vcpkg\installed\x64-windows'
$Env:PATH = "$msvc\bin\Hostx64\x64;$vcpkg\bin;$Env:PATH"

$exe = 'C:\Users\Shadow\bridgesessions\bridgesessions.exe'
$cfg = 'C:\Users\Shadow\.bridgesessions\config'

# Start daemon
$daemon = Start-Process -FilePath $exe -ArgumentList '--config',$cfg -NoNewWindow -PassThru
Start-Sleep -Seconds 4

Write-Host '=== TEST 1: Shadow health -> linux-b ==='
$r1 = Start-Process -FilePath $exe -ArgumentList '--config',$cfg,'health','linux-b' -NoNewWindow -Wait -PassThru -RedirectStandardOutput 'C:\Users\Shadow\h1.txt' -RedirectStandardError 'C:\Users\Shadow\e1.txt'
Get-Content C:\Users\Shadow\h1.txt, C:\Users\Shadow\e1.txt -ErrorAction SilentlyContinue
Write-Host "Health exit: $($r1.ExitCode)"

Write-Host '=== TEST 2: linux-b health -> Shadow ==='
ssh linux-b '/home/agent/bridgesessions/bsmesh --config /home/agent/.bridgesessions/config health shadow'

Write-Host '=== TEST 3: Shadow sessions on linux-b ==='
$r3 = Start-Process -FilePath $exe -ArgumentList '--config',$cfg,'sessions','linux-b' -NoNewWindow -Wait -PassThru -RedirectStandardOutput 'C:\Users\Shadow\s1.txt' -RedirectStandardError 'C:\Users\Shadow\s2.txt'
Get-Content C:\Users\Shadow\s1.txt, C:\Users\Shadow\s2.txt -ErrorAction SilentlyContinue
Write-Host "Sessions exit: $($r3.ExitCode)"

Write-Host '=== TEST 4: Shadow stats ==='
$r4 = Start-Process -FilePath $exe -ArgumentList '--config',$cfg,'stats' -NoNewWindow -Wait -PassThru -RedirectStandardOutput 'C:\Users\Shadow\t1.txt' -RedirectStandardError 'C:\Users\Shadow\t2.txt'
Get-Content C:\Users\Shadow\t1.txt, C:\Users\Shadow\t2.txt -ErrorAction SilentlyContinue

Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue
Write-Host 'Done'
