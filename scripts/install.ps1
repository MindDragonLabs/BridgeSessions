# BridgeSessions one-line install (Windows PowerShell)
#
#   irm https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v2.0.9-alpha5/scripts/install.ps1 | iex
#
# Or join a mesh in one command:
#
#   irm ... | iex
#   bridgesessions join <host-addr> <invite-code> --start

$ErrorActionPreference = "Stop"
$TAG = if ($env:BRIDGESESSIONS_TAG) { $env:BRIDGESESSIONS_TAG } else { "v2.0.9-alpha5" }
$BASE = "https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/$TAG/dist"
$INSTALL_DIR = "$env:LOCALAPPDATA\bridgesessions"
$BIN_PATH = "$INSTALL_DIR\bridgesessions.exe"
$VERSION_FILE = "$INSTALL_DIR\.bridgesessions-version"

New-Item -ItemType Directory -Force -Path $INSTALL_DIR | Out-Null

$CURRENT = ""
if (Test-Path $VERSION_FILE) {
    $CURRENT = Get-Content $VERSION_FILE
}

if ($CURRENT -eq $TAG -and (Test-Path $BIN_PATH)) {
    Write-Host "→ bridgesessions $TAG already installed."
} else {
    $URL = "$BASE/bridgesessions-windows-x86_64.exe"
    Write-Host "→ Downloading bridgesessions $TAG for Windows..."
    Invoke-WebRequest -Uri $URL -OutFile $BIN_PATH
    $TAG | Set-Content $VERSION_PATH
}

& $BIN_PATH --version

# Add to PATH permanently if not there
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$INSTALL_DIR*") {
    Write-Host "→ Adding $INSTALL_DIR to PATH..."
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$INSTALL_DIR", "User")
    $env:Path = "$env:Path;$INSTALL_DIR"
    Write-Host "   (restart your shell for PATH to take effect)"
}

Write-Host "→ Ready. To join a mesh, run:"
Write-Host "   bridgesessions join <host-addr> <invite-code> --start"
