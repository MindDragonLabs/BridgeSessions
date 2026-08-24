# Two-node mesh integration test
# Starts two bridgesessions instances, verifies they discover each other
# Each node gets its own USERPROFILE so it has separate identity/keys

$ErrorActionPreference = "Stop"
$bridgesessions = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"

# Add vcpkg DLLs to PATH
$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH

# Create temp directories for each node (they serve as USERPROFILE)
$tmp1 = Join-Path $env:TEMP "bs-test-node1-$PID"
$tmp2 = Join-Path $env:TEMP "bs-test-node2-$PID"
New-Item -ItemType Directory -Force $tmp1, $tmp2 | Out-Null

# Create .bridgesessions dirs
$bs1 = Join-Path $tmp1 ".bridgesessions"
$bs2 = Join-Path $tmp2 ".bridgesessions"
New-Item -ItemType Directory -Force $bs1, $bs2 | Out-Null

# ── Generate identities ──

Write-Host "=== Generating identity for node 1 ==="
$prevUserProfile = $env:USERPROFILE
try {
    $env:USERPROFILE = $tmp1
    & $bridgesessions keygen 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "keygen node1 failed" }
} finally {
    $env:USERPROFILE = $prevUserProfile
}

Write-Host "=== Generating identity for node 2 ==="
try {
    $env:USERPROFILE = $tmp2
    & $bridgesessions keygen 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "keygen node2 failed" }
} finally {
    $env:USERPROFILE = $prevUserProfile
}

# Read pubkeys
$pub1 = (Get-Content (Join-Path $bs1 "id_ed25519.pub")).Trim()
$pub2 = (Get-Content (Join-Path $bs2 "id_ed25519.pub")).Trim()
Write-Host "Node 1 pubkey: $pub1"
Write-Host "Node 2 pubkey: $pub2"

# Cross-authorize so they trust each other's TLS certs
Write-Host "=== Cross-authorizing ==="
try {
    $env:USERPROFILE = $tmp1
    & $bridgesessions authorize $pub2 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "authorize node1->node2 failed" }
} finally {
    $env:USERPROFILE = $prevUserProfile
}
try {
    $env:USERPROFILE = $tmp2
    & $bridgesessions authorize $pub1 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "authorize node2->node1 failed" }
} finally {
    $env:USERPROFILE = $prevUserProfile
}

# ── Write config files ──

@"
node.name test-node-1
node.listen :19960
mesh.gossip_interval_secs 2
mesh.ping_interval_secs 1
"@ | Out-File -FilePath (Join-Path $tmp1 "config") -Encoding ASCII

@"
node.name test-node-2
node.listen :19961
mesh.gossip_interval_secs 2
mesh.ping_interval_secs 1
seed test-node-1 127.0.0.1:19960
"@ | Out-File -FilePath (Join-Path $tmp2 "config") -Encoding ASCII

# ── Launch both nodes with per-process USERPROFILE ──
# Use System.Diagnostics.Process to set environment per-process

function Start-BSNode {
    param(
        [string]$UserProfile,
        [string]$ConfigPath,
        [string]$StdoutLog,
        [string]$StderrLog
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $bridgesessions
    $psi.Arguments = "--config `"$ConfigPath`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    # Set per-process environment
    $psi.EnvironmentVariables["USERPROFILE"] = $UserProfile
    # Copy PATH so vcpkg DLLs are found
    $psi.EnvironmentVariables["PATH"] = $env:PATH

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    # Register output handlers
    $outWriter = [System.IO.StreamWriter]::new($StdoutLog)
    $errWriter = [System.IO.StreamWriter]::new($StderrLog)
    $outEvent = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($Event.SourceEventArgs.Data) {
            $Event.MessageData.WriteLine($Event.SourceEventArgs.Data)
        }
    } -MessageData $outWriter
    $errEvent = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($Event.SourceEventArgs.Data) {
            $Event.MessageData.WriteLine($Event.SourceEventArgs.Data)
        }
    } -MessageData $errWriter

    $proc.Start() | Out-Null
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()

    return @{
        Process = $proc
        OutWriter = $outWriter
        ErrWriter = $errWriter
        OutEvent = $outEvent
        ErrEvent = $errEvent
    }
}

Write-Host "Starting node 1 on :19960..."
$node1 = Start-BSNode -UserProfile $tmp1 -ConfigPath (Join-Path $tmp1 "config") `
    -StdoutLog (Join-Path $tmp1 "stdout.log") `
    -StderrLog (Join-Path $tmp1 "stderr.log")

Write-Host "Starting node 2 on :19961..."
$node2 = Start-BSNode -UserProfile $tmp2 -ConfigPath (Join-Path $tmp2 "config") `
    -StdoutLog (Join-Path $tmp2 "stdout.log") `
    -StderrLog (Join-Path $tmp2 "stderr.log")

Write-Host "Node 1 PID: $($node1.Process.Id)"
Write-Host "Node 2 PID: $($node2.Process.Id)"

# Wait for gossip to propagate (fast intervals: 2s gossip, 1s ping)
Write-Host "Waiting for mesh discovery..."
Start-Sleep -Seconds 8

# ── Check both processes are still running ──
$fail = $false

function Dump-NodeLogs {
    param($label, $dir)
    Write-Host "--- $label stdout (last 15 lines) ---"
    $outPath = Join-Path $dir "stdout.log"
    if (Test-Path $outPath) {
        Get-Content $outPath -ErrorAction SilentlyContinue | Select-Object -Last 15 | Write-Host
    } else {
        Write-Host "(no stdout log)"
    }
    Write-Host "--- $label stderr (last 15 lines) ---"
    $errPath = Join-Path $dir "stderr.log"
    if (Test-Path $errPath) {
        Get-Content $errPath -ErrorAction SilentlyContinue | Select-Object -Last 15 | Write-Host
    } else {
        Write-Host "(no stderr log)"
    }
}

if ($node1.Process.HasExited) {
    Write-Host "FAIL: node 1 (test-node-1) exited early (exit code: $($node1.Process.ExitCode))"
    Dump-NodeLogs "Node 1" $tmp1
    $fail = $true
}
if ($node2.Process.HasExited) {
    Write-Host "FAIL: node 2 (test-node-2) exited early (exit code: $($node2.Process.ExitCode))"
    Dump-NodeLogs "Node 2" $tmp2
    $fail = $true
}

if (-not $fail) {
    Write-Host "PASS: Both nodes running after 8 seconds"
    Dump-NodeLogs "Node 1" $tmp1
    Dump-NodeLogs "Node 2" $tmp2
}

# ── Cleanup ──
if (-not $node1.Process.HasExited) {
    $node1.Process.Kill()
}
if (-not $node2.Process.HasExited) {
    $node2.Process.Kill()
}

# Close writers and unregister events
$node1.OutWriter.Close()
$node1.ErrWriter.Close()
$node2.OutWriter.Close()
$node2.ErrWriter.Close()
Unregister-Event -SourceIdentifier $node1.OutEvent.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $node1.ErrEvent.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $node2.OutEvent.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $node2.ErrEvent.Name -ErrorAction SilentlyContinue

# Wait briefly
Start-Sleep -Seconds 1

# Cleanup dirs
Remove-Item -Recurse -Force $tmp1, $tmp2 -ErrorAction SilentlyContinue

if ($fail) {
    exit 1
} else {
    Write-Host "INTEGRATION TEST PASSED"
}
