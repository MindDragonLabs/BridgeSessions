$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$proj = "C:\Users\Shadow\bridgesessions"
$Env:INCLUDE = "$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared;$vcpkg\include;$proj"
$Env:LIB = "$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64;$vcpkg\lib"
$Env:PATH = "$msvc\bin\Hostx64\x64;$Env:PATH"
Set-Location $proj

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
    Write-Host -NoNewline "Building $($t.Name)... "
    & cl /std:c++latest /EHsc /MD /utf-8 /DBS_TESTING /I $vcpkg\include $($t.File) /Fe:$($t.Name).exe /link /LIBPATH:$vcpkg\lib Catch2.lib libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib 2>&1 | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED" -ForegroundColor Red
        exit 1
    }
}

Write-Host "All builds OK" -ForegroundColor Green
