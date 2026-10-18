@echo off
rem SoundLock APO uninstaller (ASCII only)
title SoundLock APO uninstaller

set "PS=pwsh.exe"
where pwsh.exe >nul 2>&1 || set "PS=powershell.exe"
echo Using: %PS%
echo Requesting administrator rights...

%PS% -NoProfile -ExecutionPolicy Bypass -Command "Start-Process '%PS%' -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0install_apo.ps1','-Uninstall'"
echo.
echo ---- log ----
%PS% -NoProfile -Command "Get-Content -Encoding UTF8 '%~dp0install_log.txt'"
echo.
timeout /t 5 >nul
