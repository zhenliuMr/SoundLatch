#requires -version 7
param([string]$Exe = 'D:\project\PUBG\dist\SoundLatch.exe', [string]$ProcName = 'SoundLatch')

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class WinProbe {
    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }

    public static List<string> Dump(uint pid) {
        var list = new List<string>();
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
            var txt = new StringBuilder(128); GetWindowTextW(h, txt, 128);
            RECT r; GetWindowRect(h, out r);
            list.Add(string.Format("cls={0,-22} vis={1,-5} rect={2},{3} {4}x{5} title='{6}'",
                cls, IsWindowVisible(h), r.L, r.T, r.R - r.L, r.B - r.T, txt));
            return true;
        }, IntPtr.Zero);
        return list;
    }
    public static string Fg() {
        var sb = new StringBuilder(256);
        GetClassNameW(GetForegroundWindow(), sb, 256);
        return sb.ToString();
    }
}
'@

function Show($tag, $pid_) {
    Write-Host "== $tag (pid $pid_) =="
    $w = [WinProbe]::Dump([uint32]$pid_)
    if ($w.Count -eq 0) { Write-Host "  <no top-level windows>" } else { $w | ForEach-Object { Write-Host "  $_" } }
    Write-Host ("  desktop foreground class: " + [WinProbe]::Fg())
}

Get-Process $ProcName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$log = Join-Path $env:TEMP 'soundlatch_theme.log'
$sz0 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

$p1 = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds 2000
Write-Host ("1st launch exited=" + $p1.HasExited + " responding=" + (-not $p1.HasExited -and $p1.Responding))
Show 'after 1st launch' $p1.Id

$p2 = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds 1500
Write-Host ("2nd launch exited=" + $p2.HasExited + " exitcode=" + $(if ($p2.HasExited) { $p2.ExitCode } else { 'n/a' }))
Show 'after 2nd launch' $p1.Id
Start-Sleep -Milliseconds 800
Show 'after 2nd launch +800ms' $p1.Id

Write-Host "--- new log lines ---"
if (Test-Path $log) {
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $fs.Seek($sz0, 'Begin') | Out-Null
    $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::Unicode); Write-Host $sr.ReadToEnd(); $sr.Dispose(); $fs.Dispose()
}
Get-Process $ProcName -ErrorAction SilentlyContinue | Stop-Process -Force
