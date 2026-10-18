@echo off
setlocal
chcp 65001 >nul
title SoundLock APO 卸载
set CLSID={A5C6D1E0-3B7F-4C21-9E44-9E1D2C0A7781}

net session >nul 2>&1
if errorlevel 1 (
  echo [x] 需要管理员权限：请右键 - 以管理员身份运行
  pause & exit /b 1
)

echo [1/3] 移除端点挂载
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$devs = Get-PnpDevice -Class AudioEndpoint -Status OK;" ^
  "foreach($d in $devs){ $k='HKLM:\SYSTEM\CurrentControlSet\Enum\'+$d.InstanceId+'\Fx\0';" ^
  " if(Test-Path $k){ Remove-ItemProperty -Path $k -Name FxStreamEffectClsid,FxModeEffectClsid,FxEndpointEffectClsid -ErrorAction SilentlyContinue } }"

echo [2/3] 删除 COM 注册
reg delete "HKCR\CLSID\%CLSID%" /f >nul 2>&1

echo [3/3] 删除 DLL
del /f /q "%SystemRoot%\System32\SoundLockApo.dll" >nul 2>&1

echo.
echo 已卸载。重启电脑生效。
pause
