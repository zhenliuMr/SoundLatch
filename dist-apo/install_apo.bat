@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
title SoundLock APO 安装

set CLSID={A5C6D1E0-3B7F-4C21-9E44-9E1D2C0A7781}
set DLLDIR=%SystemRoot%\System32

net session >nul 2>&1
if errorlevel 1 (
  echo [x] 需要管理员权限：请右键本文件 - 以管理员身份运行
  pause & exit /b 1
)

echo ============================================================
echo  SoundLock APO 安装
echo  说明：APO 在 Windows 音频引擎内运行，延迟约 1.5 ms，
echo        但会处理该端点上的**所有**声音（不只游戏）。
echo ============================================================
echo.
echo [1/4] 复制 DLL 到 System32
copy /y "%~dp0SoundLockApo.dll" "%DLLDIR%\SoundLockApo.dll" >nul || (echo 复制失败 & pause & exit /b 1)

echo [2/4] 注册 COM 类
reg add "HKCR\CLSID\%CLSID%" /ve /d "SoundLock APO" /f >nul
reg add "HKCR\CLSID\%CLSID%\InprocServer32" /ve /d "%DLLDIR%\SoundLockApo.dll" /f >nul
reg add "HKCR\CLSID\%CLSID%\InprocServer32" /v ThreadingModel /d "Both" /f >nul

echo [3/4] 安装驱动包
pnputil /add-driver "%~dp0SoundLockApo.inf" /install >nul 2>&1
if errorlevel 1 (
  echo     pnputil 未接受该包（未签名）。将仅使用注册表方式挂载。
)

echo [4/4] 挂载到当前默认播放设备
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$cls='%CLSID%';" ^
  "$dev = Get-PnpDevice -Class AudioEndpoint -Status OK | Where-Object { $_.FriendlyName -notmatch 'CABLE|Broadcast|Virtual' } | Select-Object -First 1;" ^
  "if(-not $dev){ Write-Host '    未找到播放端点'; exit }" ^
  "$key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $dev.InstanceId + '\Fx\0';" ^
  "New-Item -Path $key -Force | Out-Null;" ^
  "New-ItemProperty -Path $key -Name FxStreamEffectClsid -Value $cls -PropertyType String -Force | Out-Null;" ^
  "New-ItemProperty -Path $key -Name FxModeEffectClsid   -Value $cls -PropertyType String -Force | Out-Null;" ^
  "New-ItemProperty -Path $key -Name FxEndpointEffectClsid -Value $cls -PropertyType String -Force | Out-Null;" ^
  "Write-Host ('    已挂载到: ' + $dev.FriendlyName)"

echo.
echo 完成。重启电脑后生效。
echo 若声音异常：运行 uninstall_apo.bat 卸载。
echo.
pause
