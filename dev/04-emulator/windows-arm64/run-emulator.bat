@echo off
rem 双击即用：把参数原样转给 run-emulator.ps1
rem 例：  run-emulator.bat -WipeData -NoSnapshot
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-emulator.ps1" %*
if errorlevel 1 (
    echo.
    echo [失败] 退出码 %errorlevel%
    pause
)
endlocal
