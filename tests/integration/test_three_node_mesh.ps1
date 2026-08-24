# Three-node mesh integration test
# Node 1 seeds Node 2; Node 2 seeds Node 3; Node 3 seeds nothing
# Verifies that after 10 seconds all 3 are running

$ErrorActionPreference = "Stop"
$bridgesessions = "C:\Users\Shadow\bridgesessions\bridgesessions.exe"

$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH

$tmp1 = Join-Path $env:TEMP "bs-test-n1-$PID"
$tmp2 = Join-Path $env:TEMP "bs-test-n2-$PID"
$tmp3 = Join-Path $env:TEMP "bs-test-n3-$PID"
New-Item -ItemType Directory -Force $tmp1, $tmp2, $tmp3 | Out-Null

$bs1 = Join-Path $tmp1 ".bridgesessions"
$bs2 = Join-Path $tmp2 ".bridgesessions"
$bs3 = Join-Path $tmp3 ".bridgesessions"
New-Item -ItemType Directory -Force $bs1, $bs2, $bs3 | Out-Null

$origUp = $env:USERPROFILE

# Generate keys
Write-Host "=== Keygen n1 ==="
$env:USERPROFILE = $tmp1; & $bridgesessions keygen 2>&1 | Write-Host; $env:USERPROFILE = $origUp
if ($LASTEXITCODE -ne 0) { throw "keygen n1 failed" }

Write-Host "=== Keygen n2 ==="
$env:USERPROFILE = $tmp2; & $bridgesessions keygen 2>&1 | Write-Host; $env:USERPROFILE = $origUp
if ($LASTEXITCODE -ne 0) { throw "keygen n2 failed" }

Write-Host "=== Keygen n3 ==="
$env:USERPROFILE = $tmp3; & $bridgesessions keygen 2>&1 | Write-Host; $env:USERPROFILE = $origUp
if ($LASTEXITCODE -ne 0) { throw "keygen n3 failed" }

$pub1 = (Get-Content (Join-Path $bs1 "id_ed25519.pub")).Trim()
$pub2 = (Get-Content (Join-Path $bs2 "id_ed25519.pub")).Trim()
$pub3 = (Get-Content (Join-Path $bs3 "id_ed25519.pub")).Trim()

# Cross-auth
Write-Host "=== Auth n1 trusts n2 ==="
$env:USERPROFILE = $tmp1; & $bridgesessions authorize $pub2 2>&1 | Write-Host; $env:USERPROFILE = $origUp
Write-Host "=== Auth n1 trusts n3 ==="
$env:USERPROFILE = $tmp1; & $bridgesessions authorize $pub3 2>&1 | Write-Host; $env:USERPROFILE = $origUp
Write-Host "=== Auth n2 trusts n1 ==="
$env:USERPROFILE = $tmp2; & $bridgesessions authorize $pub1 2>&1 | Write-Host; $env:USERPROFILE = $origUp
Write-Host "=== Auth n2 trusts n3 ==="
$env:USERPROFILE = $tmp2; & $bridgesessions authorize $pub3 2>&1 | Write-Host; $env:USERPROFILE = $origUp
Write-Host "=== Auth n3 trusts n1 ==="
$env:USERPROFILE = $tmp3; & $bridgesessions authorize $pub1 2>&1 | Write-Host; $env:USERPROFILE = $origUp
Write-Host "=== Auth n3 trusts n2 ==="
$env:USERPROFILE = $tmp3; & $bridgesessions authorize $pub2 2>&1 | Write-Host; $env:USERPROFILE = $origUp

# Configs
@"
node.name n1
node.listen :19962
mesh.gossip_interval_secs 3
mesh.ping_interval_secs 1
seed n2 127.0.0.1:19963
"@ | Out-File -FilePath (Join-Path $tmp1 "config") -Encoding ASCII

@"
node.name n2
node.listen :19963
mesh.gossip_interval_secs 3
mesh.ping_interval_secs 1
seed n3 127.0.0.1:19964
"@ | Out-File -FilePath (Join-Path $tmp2 "config") -Encoding ASCII

@"
node.name n3
node.listen :19964
mesh.gossip_interval_secs 3
mesh.ping_interval_secs 1
"@ | Out-File -FilePath (Join-Path $tmp3 "config") -Encoding ASCII

# Launch
function Start-BSNode {
    param([string]$UserProfile, [string]$ConfigPath, [string]$OutLog, [string]$ErrLog)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $bridgesessions
    $psi.Arguments = "--config `"$ConfigPath`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables["USERPROFILE"] = $UserProfile
    $psi.EnvironmentVariables["PATH"] = $env:PATH

    $outW = [System.IO.StreamWriter]::new($OutLog)
    $errW = [System.IO.StreamWriter]::new($ErrLog)

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($Event.SourceEventArgs.Data) { $Event.MessageData.WriteLine($Event.SourceEventArgs.Data) }
    } -MessageData $outW | Out-Null
    Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($Event.SourceEventArgs.Data) { $Event.MessageData.WriteLine($Event.SourceEventArgs.Data) }
    } -MessageData $errW | Out-Null
    $proc.Start() | Out-Null
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
    return @{ Process = $proc; OutW = $outW; ErrW = $errW }
}

Write-Host "Starting n1 on :19962..."
$n1 = Start-BSNode $tmp1 (Join-Path $tmp1 "config") (Join-Path $tmp1 "stdout.log") (Join-Path $tmp1 "stderr.log")
Write-Host "n1 PID: $($n1.Process.Id)"

Write-Host "Starting n2 on :19963..."
$n2 = Start-BSNode $tmp2 (Join-Path $tmp2 "config") (Join-Path $tmp2 "stdout.log") (Join-Path $tmp2 "stderr.log")
Write-Host "n2 PID: $($n2.Process.Id)"

Write-Host "Starting n3 on :19964..."
$n3 = Start-BSNode $tmp3 (Join-Path $tmp3 "config") (Join-Path $tmp3 "stdout.log") (Join-Path $tmp3 "stderr.log")
Write-Host "n3 PID: $($n3.Process.Id)"

Write-Host "Waiting for mesh discovery (10 seconds)..."
Start-Sleep -Seconds 10

$fail = $false
if ($n1.Process.HasExited) { Write-Host "FAIL: n1 exited (code: $($n1.Process.ExitCode))"; $fail = $true }
if ($n2.Process.HasExited) { Write-Host "FAIL: n2 exited (code: $($n2.Process.ExitCode))"; $fail = $true }
if ($n3.Process.HasExited) { Write-Host "FAIL: n3 exited (code: $($n3.Process.ExitCode))"; $fail = $true }

if (-not $fail) {
    Write-Host "PASS: All 3 nodes running after 10 seconds"
}

# Cleanup
if (-not $n1.Process.HasExited) { $n1.Process.Kill() }
if (-not $n2.Process.HasExited) { $n2.Process.Kill() }
if (-not $n3.Process.HasExited) { $n3.Process.Kill() }
$n1.OutW.Close(); $n1.ErrW.Close()
$n2.OutW.Close(); $n2.ErrW.Close()
$n3.OutW.Close(); $n3.ErrW.Close()

Start-Sleep -Seconds 1
Remove-Item -Recurse -Force $tmp1, $tmp2, $tmp3 -ErrorAction SilentlyContinue

if ($fail) { exit 1 } else { Write-Host "THREE-NODE INTEGRATION TEST PASSED" }
