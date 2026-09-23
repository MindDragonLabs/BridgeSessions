$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$installerPath = Join-Path $repoRoot "scripts/install.ps1"
$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $installerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) {
    $parseErrors | ForEach-Object { Write-Error $_.Message }
    throw "Windows installer has PowerShell parse errors."
}

$source = [System.IO.File]::ReadAllText($installerPath)
$remoteDownload = $source.IndexOf(
    'Invoke-WebRequest -Uri $URL -OutFile $TMP_PATH', [StringComparison]::Ordinal)
$localStage = $source.IndexOf(
    'Copy-Item -LiteralPath $stagedBinary -Destination $TMP_PATH', [StringComparison]::Ordinal)
$hashVerify = $source.IndexOf(
    '$actual = (Get-FileHash $TMP_PATH -Algorithm SHA256).Hash.ToLower()', [StringComparison]::Ordinal)
$hashReject = $source.IndexOf(
    'if ($actual -ne $expected)', [StringComparison]::Ordinal)
$versionVerify = $source.IndexOf(
    '$downloadedVersion = (& $TMP_PATH --version', [StringComparison]::Ordinal)
$versionReject = $source.IndexOf(
    'if ($downloadedVersion -ne $TAG.TrimStart(''v''))', [StringComparison]::Ordinal)
$stopDaemon = $source.IndexOf(
    'Stop-Process -Id $p.ProcessId', [StringComparison]::Ordinal)
$replaceBinary = $source.IndexOf(
    'Move-Item $TMP_PATH $BIN_PATH -Force', [StringComparison]::Ordinal)

if ($remoteDownload -lt 0 -or $localStage -lt 0 -or $hashVerify -lt 0 -or
    $hashReject -lt 0 -or $versionVerify -lt 0 -or $versionReject -lt 0 -or
    $stopDaemon -lt 0 -or $replaceBinary -lt 0) {
    throw "Installer contract missing a required download/stage, validation, stop, or replace operation."
}
$downloadOrStage = [Math]::Min($remoteDownload, $localStage)
if (-not ($downloadOrStage -lt $hashVerify -and $hashVerify -lt $hashReject -and
          $hashReject -lt $versionVerify -and $versionVerify -lt $versionReject -and
          $versionReject -lt $stopDaemon -and $stopDaemon -lt $replaceBinary)) {
    throw "Installer ordering invalid: stage/download, hash-verify, version-verify, stop daemon, then replace binary."
}

Write-Host "PASS: install.ps1 parses and stages/hash-verifies/version-verifies the binary before stopping the daemon; replacement follows shutdown."
