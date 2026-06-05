@echo off
taskkill /f /im bs-server.exe >nul 2>&1
if %errorlevel% equ 0 (
  echo bs-server killed.
) else (
  echo bs-server was not running.
)
timeout /t 2 /nobreak >nul
netstat -ano | findstr /c:":19948"
if %errorlevel% equ 0 (
  echo WARNING: port still in use. RUN AGAIN in 5s.
) else (
  echo port 19948 free.
)
