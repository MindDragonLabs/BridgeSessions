$env:PATH = "C:\vcpkg\installed\x64-windows\bin;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions

$total = 0
$passed = 0
$failed_tests = @()

$tests = @(
    @{Name="test_message"; File="tests/test_message.cpp"},
    @{Name="test_codec"; File="tests/test_codec.cpp"},
    @{Name="test_identity"; File="tests/test_identity.cpp"},
    @{Name="test_ring_buffer"; File="tests/test_ring_buffer.cpp"},
    @{Name="test_osc52"; File="tests/test_osc52.cpp"},
    @{Name="test_config"; File="tests/test_config.cpp"},
    @{Name="test_tls"; File="tests/test_tls.cpp"},
    @{Name="test_frame_io"; File="tests/test_frame_io.cpp"},
    @{Name="test_session"; File="tests/test_session.cpp"},
    @{Name="test_session_registry"; File="tests/test_session_registry.cpp"},
    @{Name="test_mesh"; File="tests/test_mesh.cpp"},
    @{Name="test_relay"; File="tests/test_relay.cpp"}
)

foreach ($t in $tests) {
    $total++
    Write-Host -NoNewline "$($t.Name): "
    try {
        $out = & .\$($t.Name).exe --verbosity quiet 2>&1
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Write-Host "PASS" -ForegroundColor Green
            $passed++
        } else {
            Write-Host "FAIL (exit=$code)" -ForegroundColor Red
            Write-Host $out
            $failed_tests += $t.Name
        }
    } catch {
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        $failed_tests += $t.Name
    }
}

Write-Host ""
Write-Host "Results: $passed / $total passed"
if ($failed_tests.Count -gt 0) {
    Write-Host "Failed: $($failed_tests -join ', ')" -ForegroundColor Red
    exit 1
}
