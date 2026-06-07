$exe = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
$cfg = "C:\Users\Shadow\.bridgesessions\config"
$action = New-ScheduledTaskAction -Execute $exe -Argument "--config $cfg"
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "Shadow" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName "bridgesessions-daemon" -Action $action -Trigger $trigger -Principal $principal -Force
Write-Host "Task installed"
Start-ScheduledTask -TaskName "bridgesessions-daemon"
Write-Host "Daemon started"
Start-Sleep -Seconds 4
Get-ScheduledTask -TaskName "bridgesessions-daemon" | Select-Object TaskName,State
