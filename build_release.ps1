$msvc='C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231'
$sdk='C:\Program Files (x86)\Windows Kits\10'
$vcpkg='C:\vcpkg\installed\x64-windows'
$proj='C:\Users\Shadow\bridgesessions'
$env:INCLUDE="$msvc\include;$sdk\Include\10.0.28000.0\ucrt;$sdk\Include\10.0.28000.0\um;$sdk\Include\10.0.28000.0\shared;$vcpkg\include;$proj"
$env:LIB="$msvc\lib\x64;$sdk\Lib\10.0.28000.0\ucrt\x64;$sdk\Lib\10.0.28000.0\um\x64;$vcpkg\lib"
$env:PATH="$msvc\bin\Hostx64\x64;$vcpkg\bin;$env:PATH"
Set-Location $proj
cl /std:c++latest /EHsc /MD /utf-8 /DBS_NO_NAT /DBS_NO_WEBRTC /DBS_NO_DHT /I $vcpkg\include bridgesessions.cpp /Febridgesessions.exe /link /LIBPATH:$vcpkg\lib libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib spdlog.lib
exit $LASTEXITCODE
