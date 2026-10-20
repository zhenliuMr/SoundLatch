@echo off
cd /d "%~dp0vbcable"
echo Installing VB-Audio Virtual Cable (needs administrator)...
VBCABLE_Setup_x64.exe -i
echo.
echo Done. Restart Windows, then start SoundLatch.exe
pause
