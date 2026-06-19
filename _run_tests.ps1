# _run_tests.ps1 — Build and run the full bridgesessions test suite
#
# Usage:
#   powershell -File _run_tests.ps1                   # build + run all
#   powershell -File _run_tests.ps1 -Filter "[tls]"   # run only tls-tagged tests
#   powershell -File _run_tests.ps1 -BuildOnly         # build only, no run
#   powershell -File _run_tests.ps1 -Suite test_tls    # build + run one suite
#   powershell -File _run_tests.ps1 -NoBuild           # skip build, run existing exes
#
# Exit codes: 0 = all pass, 1 = any build or test failure

param(
    [string]$Filter   = "",   # Catch2 tag/name filter e.g. "[tls]" or "[r4]"
    [string]$Suite    = "",   # Run only this suite by name
    [switch]$BuildOnly,
    [switch]$NoBuild
)

# ── Environment ────────────────────────────────────────────────────────
$msvc  = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk   = "C:\Program Files (x86)\Windows Kits\10"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$proj  = "C:\Users\Shadow\bridgesessions"
$testHome = Join-Path $env:TEMP "bridgesessions-test-home-$(Get-Random)"

$Env:INCLUDE = "$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared;$vcpkg\include;$proj"
$Env:LIB     = "$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64;$vcpkg\lib"
$Env:PATH    = "$msvc\bin\Hostx64\x64;$vcpkg\bin;$Env:PATH"
Set-Location $proj

# ── Test Registry ──────────────────────────────────────────────────────
# Layer: unit = no sockets/PTY, integration = real TLS/PTY/sockets
$allTests = @(
    # protocol layer — unit
    [PSCustomObject]@{ Name="test_message";               File="tests/test_message.cpp";               Layer="unit";        Tags="message codec" },
    [PSCustomObject]@{ Name="test_codec";                 File="tests/test_codec.cpp";                 Layer="unit";        Tags="codec compression sha256 image" },
    [PSCustomObject]@{ Name="test_frame_io";              File="tests/test_frame_io.cpp";              Layer="unit";        Tags="framing wire" },
    [PSCustomObject]@{ Name="test_osc52";                 File="tests/test_osc52.cpp";                 Layer="unit";        Tags="clipboard osc52" },
    [PSCustomObject]@{ Name="test_ring_buffer";           File="tests/test_ring_buffer.cpp";           Layer="unit";        Tags="scrollback ring thread" },
    # identity + config — unit
    [PSCustomObject]@{ Name="test_identity";              File="tests/test_identity.cpp";              Layer="unit";        Tags="identity keygen ed25519" },
    [PSCustomObject]@{ Name="test_config";                File="tests/test_config.cpp";                Layer="unit";        Tags="config parse save" },
    # TLS + auth — integration
    [PSCustomObject]@{ Name="test_tls";                   File="tests/test_tls.cpp";                   Layer="integration"; Tags="tls auth tofu mtls" },
    [PSCustomObject]@{ Name="test_tls_reliability";       File="tests/test_tls_reliability.cpp";       Layer="integration"; Tags="tls timeout error r1 r2" },
    [PSCustomObject]@{ Name="test_authorized_keys_reload";File="tests/test_authorized_keys_reload.cpp";Layer="integration"; Tags="auth hotreload r4" },
    # session lifecycle — integration
    [PSCustomObject]@{ Name="test_session";               File="tests/test_session.cpp";               Layer="integration"; Tags="session conpty pty" },
    [PSCustomObject]@{ Name="test_session_registry";      File="tests/test_session_registry.cpp";      Layer="integration"; Tags="session registry state" },
    [PSCustomObject]@{ Name="test_relay";                 File="tests/test_relay.cpp";                 Layer="integration"; Tags="relay attach detach keystroke resize signal" },
    [PSCustomObject]@{ Name="test_multi_attach";          File="tests/test_multi_attach.cpp";          Layer="integration"; Tags="multi_attach peer_ids v1.1" },
    # mesh — integration
    [PSCustomObject]@{ Name="test_mesh";                  File="tests/test_mesh.cpp";                  Layer="integration"; Tags="mesh hello gossip connect" },
    [PSCustomObject]@{ Name="test_mesh_reliability";      File="tests/test_mesh_reliability.cpp";      Layer="integration"; Tags="mesh pong gossip duplicate reuseaddr r3 r5" }
)

$tests = if ($Suite) { $allTests | Where-Object { $_.Name -eq $Suite } } else { $allTests }
if ($Suite -and !$tests) { Write-Host "ERROR: no suite named '$Suite'" -ForegroundColor Red; exit 1 }

$clFlags = "/std:c++latest /EHsc /MD /utf-8 /DBS_TESTING /DBS_NO_NAT /DBS_NO_WEBRTC /DBS_NO_DHT /I $vcpkg\include"
$clLibs  = "Catch2.lib libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib"

# ── Header ─────────────────────────────────────────────────────────────
Write-Host ""
Write-Host ("=" * 62) -ForegroundColor Cyan
Write-Host "  bridgesessions TEST SUITE   $(Get-Date -Format 'yyyy-MM-dd HH:mm')" -ForegroundColor Cyan
Write-Host ("=" * 62) -ForegroundColor Cyan
Write-Host ""

$results    = [System.Collections.Generic.List[PSCustomObject]]::new()
$buildFail  = 0
$suiteFail  = 0
$totalPass  = 0
$totalFail  = 0

$prevUserProfile = $env:USERPROFILE
$prevHome = $env:HOME
New-Item -ItemType Directory -Force -Path $testHome | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $testHome ".bridgesessions") | Out-Null
$env:USERPROFILE = $testHome
$env:HOME = $testHome

foreach ($t in $tests) {
    $exe = "$proj\$($t.Name).exe"

    # ── Build ─────────────────────────────────────────────────────────
    if (!$NoBuild) {
        Write-Host -NoNewline "  build  $($t.Name.PadRight(34))"
        $buildOut = Invoke-Expression "cl $clFlags $($t.File) /Fe:$($t.Name).exe /link /LIBPATH:$vcpkg\lib $clLibs 2>&1"
        if ($LASTEXITCODE -ne 0) {
            Write-Host " FAIL" -ForegroundColor Red
            $buildOut -split "`n" | Where-Object { $_ -match "error C\d+|fatal error" } |
                Select-Object -First 4 | ForEach-Object { Write-Host "           $_" -ForegroundColor DarkRed }
            $results.Add([PSCustomObject]@{ Name=$t.Name; Layer=$t.Layer; Tags=$t.Tags; Build="FAIL"; Pass=0; Fail=0; Status="BUILD_FAIL" })
            $buildFail++
            continue
        }
        Write-Host " ok" -ForegroundColor DarkGreen
    }

    if ($BuildOnly) { continue }

    if (!(Test-Path $exe)) {
        Write-Host "  run    $($t.Name.PadRight(34)) MISSING" -ForegroundColor Yellow
        $results.Add([PSCustomObject]@{ Name=$t.Name; Layer=$t.Layer; Tags=$t.Tags; Build="?"; Pass=0; Fail=0; Status="MISSING" })
        continue
    }

    # ── Run ───────────────────────────────────────────────────────────
    Write-Host -NoNewline "  run    $($t.Name.PadRight(34))"
    $runArgs = @("--reporter", "compact", "--colour-mode", "none")
    if ($Filter) { $runArgs += $Filter }

    $runOut  = & $exe @runArgs 2>&1 | Out-String
    $exitVal = $LASTEXITCODE

    $pass = 0; $fail = 0
    # Catch2 v3 summary: "test cases: N | P passed | F failed"
    if ($runOut -match "test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed") {
        $pass = [int]$Matches[2]; $fail = [int]$Matches[3]
    } elseif ($runOut -match "All tests passed.*?(\d+) assert") {
        $pass = [int]$Matches[1]; $fail = 0
    } elseif ($runOut -match "(\d+) test case[s]?,\s*(\d+) assertion[s]?,\s*(\d+) failure[s]?") {
        $pass = [int]$Matches[1] - [int]$Matches[3]; $fail = [int]$Matches[3]
    }
    $totalPass += $pass; $totalFail += $fail

    $status = if ($exitVal -eq 0) { "PASS" } else { "FAIL" }
    if ($exitVal -ne 0) { $suiteFail++ }

    $col = if ($status -eq "PASS") { "Green" } else { "Red" }
    Write-Host (" $status  ($pass/$([int]$pass+$fail))") -ForegroundColor $col

    if ($status -eq "FAIL") {
        $runOut -split "`n" | Where-Object { $_ -match "FAILED" } |
            Select-Object -First 8 | ForEach-Object { Write-Host "           $_" -ForegroundColor Red }
    }

    $results.Add([PSCustomObject]@{ Name=$t.Name; Layer=$t.Layer; Tags=$t.Tags; Build="ok"; Pass=$pass; Fail=$fail; Status=$status })
}

if ($BuildOnly) {
    Write-Host ""; Write-Host "  Build-only complete." -ForegroundColor Yellow
    $env:USERPROFILE = $prevUserProfile
    if ($prevHome) { $env:HOME = $prevHome } else { Remove-Item Env:HOME -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $testHome -ErrorAction SilentlyContinue
    if ($buildFail -gt 0) { exit 1 } else { exit 0 }
}

# ── Summary ────────────────────────────────────────────────────────────
Write-Host ""
Write-Host ("=" * 62) -ForegroundColor Cyan
Write-Host "  RESULTS" -ForegroundColor Cyan
Write-Host ("=" * 62) -ForegroundColor Cyan
$results | Format-Table -AutoSize @{L="Suite";E={$_.Name}}, Layer, @{L="Pass";E={$_.Pass}}, @{L="Fail";E={$_.Fail}}, Status

$pct = if (($totalPass+$totalFail) -gt 0) { [int](100*$totalPass/($totalPass+$totalFail)) } else { 0 }
Write-Host "  Assertions : $totalPass passed, $totalFail failed  ($pct%)"
Write-Host "  Suites     : $($results.Count) run, $suiteFail failed, $buildFail build errors"
Write-Host ""

if ($buildFail -eq 0 -and $suiteFail -eq 0) {
    Write-Host "  ALL PASS" -ForegroundColor Green
    $env:USERPROFILE = $prevUserProfile
    if ($prevHome) { $env:HOME = $prevHome } else { Remove-Item Env:HOME -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $testHome -ErrorAction SilentlyContinue
    exit 0
} else {
    Write-Host "  FAILED" -ForegroundColor Red
    $env:USERPROFILE = $prevUserProfile
    if ($prevHome) { $env:HOME = $prevHome } else { Remove-Item Env:HOME -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $testHome -ErrorAction SilentlyContinue
    exit 1
}
