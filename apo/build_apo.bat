@echo off
setlocal
set PATH=D:\toolchain\mingw64\bin;%PATH%
if not exist dist-apo mkdir dist-apo
echo [1/2] building SoundLockApo.dll ...
g++ -std=c++20 -O2 -shared -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN ^
    apo\src\SoundLockApo.cpp -o dist-apo\SoundLockApo.dll ^
    -static-libgcc -static-libstdc++ -lole32 -luuid
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
strip dist-apo\SoundLockApo.dll
echo [2/2] building SoundLockApoCtl.exe ...
g++ -std=c++20 -O2 -municode -mwindows -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN ^
    apo\src\apo_ui.cpp -o dist-apo\SoundLockApoCtl.exe ^
    -static-libgcc -static-libstdc++ -lole32 -luuid -lcomctl32 -ladvapi32
if errorlevel 1 (
  echo UI BUILD FAILED
  exit /b 1
)
strip dist-apo\SoundLockApoCtl.exe
for %%F in (dist-apo\SoundLockApo.dll dist-apo\SoundLockApoCtl.exe) do echo OK: %%~fF  %%~zF bytes
endlocal
