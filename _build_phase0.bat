@echo off
set "INCLUDE=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\shared"
set "LIB=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.28000.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.28000.0\um\x64"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64;%PATH%"
cd /d C:\Users\Shadow\bridgesessions
cl /std:c++latest /EHsc /Fe:bridgesessions.exe bridgesessions.cpp
if %ERRORLEVEL% equ 0 (
    bridgesessions.exe
    echo BUILD SUCCESS
) else (
    echo BUILD FAILED
)
