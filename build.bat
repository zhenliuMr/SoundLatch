@echo off
setlocal
set PATH=D:\toolchain\mingw64\bin;%PATH%
if not exist dist mkdir dist
echo [1/2] compiling resources...
windres -I res -i res\app.rc -O coff -o res\app.res
if errorlevel 1 (
  echo RESOURCE FAILED
  exit /b 1
)
echo [2/2] compiling...
g++ -std=c++20 -O2 -municode -mwindows -DUNICODE -D_UNICODE -Wall -Wno-unused-variable ^
    src\main.cpp src\engine.cpp res\app.res -o dist\SoundLock.exe ^
    -lole32 -luuid -lcomctl32 -lpsapi -lavrt -lgdi32 -luser32 -lshlwapi
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
strip dist\SoundLock.exe
for %%F in (dist\SoundLock.exe) do echo OK: %%~fF  %%~zF bytes
endlocal
