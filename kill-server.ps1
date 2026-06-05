$proc = Get-Process bs-server -ErrorAction SilentlyContinue
if ($proc) {
  Stop-Process -Name bs-server -Force
  Write-Host "bs-server killed (PID $($proc.Id))"
  Start-Sleep 2
  $port = netstat -ano | Select-String ':19948'
  if ($port) { Write-Host "WARNING: port still in use" -ForegroundColor Yellow }
  else { Write-Host "port 19948 free" }
} else {
  Write-Host "bs-server was not running"
}
