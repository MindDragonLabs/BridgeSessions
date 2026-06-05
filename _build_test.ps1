$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$vcpkg = "C:\vcpkg\installed\x64-windows"
$proj = "C:\Users\Shadow\bridgesessions"
$Env:INCLUDE = "$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared;$vcpkg\include;$proj"
$Env:LIB = "$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64;$vcpkg\lib"
$Env:PATH = "$msvc\bin\Hostx64\x64;$Env:PATH"
Set-Location C:\Users\Shadow\bridgesessions
# Build test executable (test includes bridgesessions.cpp directly)
# /MD matches Catch2.lib's runtime library (dynamic CRT)
# test_message.cpp provides its own main() via Catch::Session
& cl /std:c++latest /EHsc /MD /DBS_TESTING /I $vcpkg\include tests\test_message.cpp /Fe:test_message.exe /link /LIBPATH:$vcpkg\lib Catch2.lib 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "test_message.exe BUILD FAILED"
    exit 1
}
Write-Host "BUILD OK"
