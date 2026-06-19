# R6.2 — one-command Windows build (wraps _build_bridgesessions.ps1)
param()
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
& powershell -ExecutionPolicy Bypass -NoProfile -File (Join-Path $here "_build_bridgesessions.ps1")
exit $LASTEXITCODE