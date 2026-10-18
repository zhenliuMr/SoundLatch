@echo off
rem SoundLock APO uninstaller (ASCII only)
title SoundLock APO uninstaller
echo Requesting administrator rights...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0install_apo.ps1','-Uninstall'"
echo.
echo ---- log ----
powershell -NoProfile -Command "Get-Content -Encoding UTF8 '%~dp0install_log.txt'"
echo.
pause
