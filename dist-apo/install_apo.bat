@echo off
rem SoundLock APO installer (ASCII only: no codepage issues)
title SoundLock APO installer
echo Requesting administrator rights...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0install_apo.ps1','-Sign'"
echo.
echo ---- install log ----
powershell -NoProfile -Command "Get-Content -Encoding UTF8 '%~dp0install_log.txt'"
echo.
pause
