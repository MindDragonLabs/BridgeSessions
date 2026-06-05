@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
cd /d C:\SFTP\agent\bridgesessions\build\windows-msvc-debug\bs-protocol
bs-protocol-tests.exe 2>&1
