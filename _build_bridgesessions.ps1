param()
$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$Env:INCLUDE = "$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared;$vcpkg\include"
$Env:LIB = "$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64;$vcpkg\lib"
$Env:PATH = "$msvc\bin\Hostx64\x64;$vcpkg\bin;$Env:PATH"
Set-Location C:\Users\Shadow\bridgesessions
Write-Host "Building bridgesessions.exe ..."
& cl /std:c++latest /EHsc /MD /utf-8 /I $vcpkg\include bridgesessions.cpp /Fe:bridgesessions.exe /link /LIBPATH:$vcpkg\lib libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib user32.lib shell32.lib CLI11.lib miniupnpc.lib datachannel.lib 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "BUILD OK"
    & .\bridgesessions.exe --version
} else {
    Write-Host "BUILD FAILED (exit code: $LASTEXITCODE)"
    exit 1
}
