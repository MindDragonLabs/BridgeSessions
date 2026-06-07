& $env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64;C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64;" + $env:PATH
Set-Location C:\Users\Shadow\bridgesessions
$result = & cl /std:c++latest /EHsc /MD /utf-8 /I C:\vcpkg\installed\x64-windows\include bridgesessions.cpp /Febridgesessions.exe /link /LIBPATH:C:\vcpkg\installed\x64-windows\lib libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib user32.lib shell32.lib CLI11.lib 2>&1
$result
Write-Host "EXIT: $LASTEXITCODE"
