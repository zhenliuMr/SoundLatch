@echo off
rem SoundLock APO installer (ASCII only: no codepage issues)
rem Prefers PowerShell 7 (pwsh); falls back to Windows PowerShell 5.1.
title SoundLock APO installer

set "PS=pwsh.exe"
where pwsh.exe >nul 2>&1 || set "PS=powershell.exe"
echo Using: %PS%
echo Requesting administrator rights...

%PS% -NoProfile -ExecutionPolicy Bypass -Command "Start-Process '%PS%' -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0install_apo.ps1','-Sign'"
echo.
echo ---- install log ----
%PS% -NoProfile -Command "Get-Content -Encoding UTF8 '%~dp0install_log.txt'"
echo.
pause
