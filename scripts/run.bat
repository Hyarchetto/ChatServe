@echo off
cd /d "%~dp0.."
echo === Build ChatServe ===
wsl -d Ubuntu ./build.sh
if %ERRORLEVEL% EQU 0 (
    echo.
    echo === Start Server ===
    wsl -d Ubuntu ./build/server/chat_server
) else (
    echo Build failed!
    pause
    exit /b %ERRORLEVEL%
)
pause
