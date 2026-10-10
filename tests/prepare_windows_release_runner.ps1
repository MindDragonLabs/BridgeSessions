# GitHub's Server runner images disable Defender and exclude both drives.
# Strengthen only the disposable hosted runner before release validation.
$ErrorActionPreference = "Stop"
if ($env:GITHUB_ACTIONS -ne "true" -or $env:RUNNER_ENVIRONMENT -ne "github-hosted") {
    throw "Defender preparation is restricted to disposable GitHub-hosted runners"
}
Start-Service WinDefend
Set-MpPreference -DisableRealtimeMonitoring $false -DisableBehaviorMonitoring $false `
    -DisableIOAVProtection $false -DisableScriptScanning $false -DisableArchiveScanning $false
$preferences = Get-MpPreference
foreach ($kind in @("ExclusionPath", "ExclusionProcess", "ExclusionExtension")) {
    $entries = @($preferences.$kind | Where-Object { $_ })
    if ($entries.Count -gt 0) {
        $remove = @{}
        $remove[$kind] = $entries
        Remove-MpPreference @remove
    }
}
Update-MpSignature
# Preference changes are asynchronous. Fresh hosted runners can report
# antivirus enabled before real-time protection has finished starting.
$deadline = [DateTime]::UtcNow.AddSeconds(90)
do {
    $protection = Get-MpComputerStatus
    if ($protection.AntivirusEnabled -and $protection.RealTimeProtectionEnabled) {
        break
    }
    Write-Host "Waiting for hosted runner Defender real-time protection..."
    Start-Sleep -Seconds 3
} while ([DateTime]::UtcNow -lt $deadline)
$protection | Select-Object AntivirusEnabled, RealTimeProtectionEnabled, BehaviorMonitorEnabled,
    AntivirusSignatureVersion, AntivirusSignatureLastUpdated | Format-List
if (-not $protection.AntivirusEnabled -or -not $protection.RealTimeProtectionEnabled) {
    throw "Hosted runner Defender protection could not be enabled"
}
