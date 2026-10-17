@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RESTORE-CONTROL.ps1" %*
if errorlevel 1 (echo RESTORE FAILED.& pause & exit /b 1)
echo Restored previous controller.
