@echo off
set PATH=C:\vcpkg\installed\x64-windows\bin;%PATH%
cd /d C:\Users\Shadow\bridgesessions
test_tls.exe 2>&1
echo EXIT_CODE=%ERRORLEVEL%
