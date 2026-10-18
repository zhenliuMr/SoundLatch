@echo off
rem Sign the driver package and enable test signing (needs admin + reboot).
title SoundLock APO - prepare test signing

set "PS=pwsh.exe"
where pwsh.exe >nul 2>&1 || set "PS=powershell.exe"

%PS% -NoProfile -ExecutionPolicy Bypass -Command "Start-Process '%PS%' -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0prepare_testsign.ps1'"
echo.
echo ---- log ----
%PS% -NoProfile -Command "Get-Content -Encoding UTF8 '%~dp0prepare_log.txt'"
echo.
timeout /t 5 >nul
