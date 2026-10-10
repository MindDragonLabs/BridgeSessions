param(
    [string]$Binary = "./dist/bridgesessions-windows-x86_64.exe",
    [string]$ExpectedVersion = (Get-Content ./VERSION -Raw).Trim()
)

$ErrorActionPreference = "Stop"
$path = (Resolve-Path $Binary).Path
$before = (Get-FileHash $path -Algorithm SHA256).Hash
$protection = Get-MpComputerStatus
if (-not $protection.AntivirusEnabled -or -not $protection.RealTimeProtectionEnabled) {
    throw "Windows release validation requires enabled Defender antivirus and real-time protection"
}
$preferences = Get-MpPreference
if ($preferences.ExclusionPath -or $preferences.ExclusionProcess -or $preferences.ExclusionExtension) {
    throw "Windows release validation requires a runner without Defender scan exclusions"
}
$scanStarted = Get-Date
Start-MpScan -ScanType CustomScan -ScanPath $path
$detections = @(Get-MpThreatDetection | Where-Object {
    $_.InitialDetectionTime -ge $scanStarted -and
    ($_.Resources | Where-Object { $_ -like "*$path*" })
})
if ($detections.Count -gt 0) { throw "Defender detected the Windows release binary" }
if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash -ne $before) {
    throw "Windows release binary was removed or changed during validation"
}
$protection = Get-MpComputerStatus
if (-not $protection.AntivirusEnabled -or -not $protection.RealTimeProtectionEnabled) {
    throw "Defender protection changed during release validation"
}
$reported = & $path --version
if ($LASTEXITCODE -ne 0 -or "$reported".Trim() -ne $ExpectedVersion) {
    throw "Windows release version mismatch: expected $ExpectedVersion, got $reported"
}
& $path --help
if ($LASTEXITCODE -ne 0) { throw "Windows release binary --help failed" }
Write-Output "Windows release validated: version=$ExpectedVersion sha256=$before Defender=enabled"
