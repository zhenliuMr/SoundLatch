@echo off
setlocal
set PATH=D:\toolchain\mingw64\bin;%PATH%
if not exist dist mkdir dist
echo [1/2] compiling resources...
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat as the system ANSI
rem codepage (ACP=936 / GBK here); non-ASCII bytes in comments break parsing.
rem -c 65001 forces windres to read res\app.rc as UTF-8. Without it windres
rem uses the system ANSI codepage (GBK), so the Chinese in FileDescription
rem ends up mojibake in the exe's file properties.
windres -I res -i res\app.rc -O coff -c 65001 -o res\app.res
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
