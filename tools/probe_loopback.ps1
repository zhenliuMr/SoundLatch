#requires -version 7
param([string]$Exe = 'D:\project\PUBG\dist\SoundLatch.exe', [int]$Times = 1, [int]$GapMs = 1500)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class W {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);

    public const uint BM_CLICK = 0x00F5, BM_GETCHECK = 0x00F0;

    public static IntPtr Find(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            var sb = new StringBuilder(128); GetClassNameW(h, sb, 128);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static List<string> Children(IntPtr parent) {
        var L = new List<string>();
        EnumChildWindows(parent, (h, p) => {
            var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
            var t = new StringBuilder(64); GetWindowTextW(h, t, 64);
            L.Add(string.Format("{0}|{1}|{2}", h, cls, t));
            return true;
        }, IntPtr.Zero);
        return L;
    }

    public static IntPtr ChildByText(IntPtr parent, string text) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, p) => {
            var t = new StringBuilder(64); GetWindowTextW(h, t, 64);
            if (t.ToString() == text) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

Get-Process SoundLatch -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$log = Join-Path $env:TEMP 'soundlatch_theme.log'
$sz0 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds 1500
$pop = [W]::Find([uint32]$p.Id, 'SoundLatchPopup')
if ($pop -eq [IntPtr]::Zero) { Write-Host '未找到面板窗口'; exit 1 }
Write-Host ("panel=0x{0:x}" -f $pop.ToInt64())
[W]::Children($pop) | Where-Object { $_ -match '\|Button\|' } | ForEach-Object { Write-Host "  $_" }

$cb = [W]::ChildByText($pop, '回环捕获')
if ($cb -eq [IntPtr]::Zero) { Write-Host '未找到「回环捕获」复选框'; exit 1 }
Write-Host ("checkbox=0x{0:x}  checked={1}" -f $cb.ToInt64(), [W]::SendMessageW($cb, [W]::BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero))

for ($i = 1; $i -le $Times; $i++) {
    [void][W]::SendMessageW($cb, [W]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    $st = [W]::SendMessageW($cb, [W]::BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host ("click #{0}  checked={1}" -f $i, $st)
    if ($i -lt $Times) { Start-Sleep -Milliseconds $GapMs }
}
Write-Host 'clicked 回环捕获, waiting...'
Start-Sleep -Milliseconds $GapMs

$p.Refresh()
if ($p.HasExited) {
    Write-Host ("*** 进程已退出  exitcode=0x{0:X8} ({1})" -f $p.ExitCode, $p.ExitCode)
} else {
    Write-Host ("进程仍在运行  responding={0}" -f $p.Responding)
    $p | Stop-Process -Force
}

Write-Host '--- 新增日志 ---'
if (Test-Path $log) {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $fs.Seek($sz0, 'Begin') | Out-Null
    $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::Unicode); Write-Host $sr.ReadToEnd(); $sr.Dispose(); $fs.Dispose()
}
