# Run elevated: powershell -ExecutionPolicy Bypass -File configure-firewall.ps1
$ErrorActionPreference = "Continue"
function Ensure-Rule { param([string]$Name,[int]$Port,[string]$Proto="TCP")
  if (Get-NetFirewallRule -DisplayName $Name -EA SilentlyContinue) { Write-Host "OK $Name"; return }
  New-NetFirewallRule -DisplayName $Name -Direction Inbound -Protocol $Proto -LocalPort $Port -Action Allow -Profile Any | Out-Null
  Write-Host "Created $Name $Port"
}
Ensure-Rule "bridgesessions-mesh-19949" 19949
Ensure-Rule "bridgesessions-mesh-cli-19980" 19980
Ensure-Rule "bridgesessions-mesh-udp-19949" 19949 UDP
$exe = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"
if ((Test-Path $exe) -and -not (Get-NetFirewallRule -DisplayName "bridgesessions-exe-inbound" -EA SilentlyContinue)) {
  New-NetFirewallRule -DisplayName "bridgesessions-exe-inbound" -Direction Inbound -Program $exe -Action Allow -Profile Any | Out-Null
}