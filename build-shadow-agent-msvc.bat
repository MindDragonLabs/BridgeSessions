@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cd /d C:\SFTP\agent\bridgesessions
cmake --build build\windows-msvc-debug --target shadow-agent shadow-agent-tests
