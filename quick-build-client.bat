@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
cd /d C:\SFTP\agent\bridgesessions
cmake --build build/windows-msvc-debug --target bs-client 2>&1
