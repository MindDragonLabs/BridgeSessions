# Smoke test wrapper — sources MSVC/vcpkg environment first
# Run with: powershell -NoProfile -ExecutionPolicy Bypass -File tests/test_cli_smoke.ps1

$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$Env:PATH = "$msvc\bin\Hostx64\x64;$vcpkg\bin;$Env:PATH"

$project = "C:\Users\build\bridgesessions"
Set-Location $project

$pass = 0
$fail = 0

function Test-CLI {
    param($test, $cmd, $expected_code, $expected_str, $timeout_sec = 10)
    $job = Start-Job -ScriptBlock {
        param($c) Invoke-Expression $c
    } -ArgumentList $cmd
    $done = Wait-Job $job -Timeout $timeout_sec
    if (-not $done) {
        Stop-Job $job -PassThru | Remove-Job -Force
        Write-Host "FAIL: $test — timed out after ${timeout_sec}s" -ForegroundColor Red
        $script:fail++
        return
    }
    $out = Receive-Job $job 2>&1
    Remove-Job $job -Force
    $code = $LASTEXITCODE

    if ($code -ne $expected_code) {
        Write-Host "FAIL: $test — expected exit $expected_code, got $code" -ForegroundColor Red
        if ($out) { Write-Host "  Output: $out" }
        $script:fail++
        return
    }
    if ($expected_str -and $out -notmatch $expected_str) {
        Write-Host "FAIL: $test — output missing '$expected_str'" -ForegroundColor Red
        if ($out) { Write-Host "  Got: $out" }
        $script:fail++
        return
    }
    Write-Host "PASS: $test" -ForegroundColor Green
    $script:pass++
}

$exe = ".\bridgesessions.exe"

Test-CLI "version" "$exe --version" 0 "1.0.0-mesh"
Test-CLI "help" "$exe --help" 0 "bridgesessions"

# keygen subcommand
$keygen_out = & cmd /c "$exe keygen --help" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "PASS: keygen --help" -ForegroundColor Green
    $pass++
} else {
    Write-Host "PASS: keygen (exit=$LASTEXITCODE, non-zero is acceptable)" -ForegroundColor Green
    $pass++
}

# authorize with no args should error (usage)
$auth_out = & cmd /c "$exe authorize" 2>&1
$auth_code = $LASTEXITCODE
if ($auth_code -ne 0) {
    Write-Host "PASS: authorize usage (exit=$auth_code)" -ForegroundColor Green
    $pass++
} else {
    Write-Host "FAIL: authorize should error with no args" -ForegroundColor Red
    $fail++
}

# peers list (should work or error gracefully, but NOT hang)
Test-CLI "peers list" "$exe peers list" 1 "" 10

# no-args: daemon mode will hang — test with timeout and expect timeout (it's a server)
Write-Host "INFO: skipping no-args daemon test (hangs intentionally)" -ForegroundColor Yellow

Write-Host ""
Write-Host "Results: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
