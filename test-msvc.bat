@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
cd /d C:\SFTP\agent\bridgesessions
ctest --test-dir build/windows-msvc-debug --output-on-failure -R bs-protocol 2>&1
