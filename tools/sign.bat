@echo off
rem sign.bat - code-sign dist\SoundLatch.exe
rem
rem Usage:
rem   build.bat                       (build first)
rem   tools\sign.bat cert.pfx         (sign with a PFX, prompts for password)
rem
rem What this needs:
rem   A *code signing* certificate. A self-signed one only proves the file was not
rem   modified after signing; SmartScreen still shows "unknown publisher" because
rem   there is no CA chain and no reputation. Real distribution needs an OV/EV cert
rem   from a public CA (or Azure Trusted Signing).
rem
rem Timestamping matters: without it the signature expires with the certificate.
setlocal
set PATH=D:\toolchain\sdkx\bin\10.0.28000.0\x64;%PATH%
set SIGNTOOL=signtool.exe
set TSA=http://timestamp.digicert.com

if "%~1"=="" (
  echo usage: tools\sign.bat ^<cert.pfx^> [password]
  exit /b 1
)
if not exist "%~1" (
  echo PFX not found: %~1
  exit /b 1
)
if not exist dist\SoundLatch.exe (
  echo dist\SoundLatch.exe missing - run build.bat first
  exit /b 1
)

%SIGNTOOL% sign /fd SHA256 /td SHA256 /tr %TSA% /f "%~1" /p "%~2" /d "SoundLatch" dist\SoundLatch.exe
if errorlevel 1 (
  echo SIGN FAILED
  exit /b 1
)
copy /y dist\SoundLatch.exe "dist\SoundLatch-1.2.0-x64.exe" >nul
%SIGNTOOL% verify /pa /v dist\SoundLatch.exe
endlocal
