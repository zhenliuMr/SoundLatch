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
    -static -static-libgcc -static-libstdc++ ^
    src\main.cpp src\engine.cpp res\app.res -o dist\SoundLatch.exe ^
    -lole32 -luuid -lcomctl32 -lpsapi -lavrt -lgdi32 -luser32 -lshlwapi -ldwmapi -luxtheme -ladvapi32
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
strip dist\SoundLatch.exe
for %%F in (dist\SoundLatch.exe) do echo OK: %%~fF  %%~zF bytes
endlocal
