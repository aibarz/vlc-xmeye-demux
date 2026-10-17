@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0INSTALL-CONTROL.ps1" %*
if errorlevel 1 (echo INSTALL FAILED.& pause & exit /b 1)
echo Installed. Open VLC normally.
