$msvc = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$Env:INCLUDE = "$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared"
$Env:LIB = "$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64"
$Env:PATH = "$msvc\bin\Hostx64\x64;$Env:PATH"
Set-Location C:\Users\Shadow\bridgesessions
& cl /std:c++latest /EHsc /Fe:bridgesessions.exe bridgesessions.cpp
if ($LASTEXITCODE -eq 0) {
    & .\bridgesessions.exe
    Write-Host "BUILD OK"
} else {
    Write-Host "BUILD FAILED"
}
