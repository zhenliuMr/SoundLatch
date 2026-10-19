param([string]$Exe = "D:\project\PUBG\dist\_mt.exe", [string]$Fake = "0.35", [int]$ClickX = -1, [string]$Out = "D:\project\PUBG\tools\_mt.png")
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace SLW {
  public class W {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int EnumWindows(Cb cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int EnumChildWindows(IntPtr parent, Cb cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public delegate bool Cb(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static List<IntPtr> wins = new List<IntPtr>();
    public static IntPtr meter = IntPtr.Zero;
    public static List<string> texts = new List<string>();
    public static bool Find(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == "SoundLatchPopup") wins.Add(h);
      return true;
    }
    public static bool Child(IntPtr h, IntPtr p) {
      StringBuilder c = new StringBuilder(256); GetClassName(h, c, 256);
      StringBuilder t = new StringBuilder(512); GetWindowText(h, t, 512);
      if (c.ToString() == "SLMETER") meter = h;
      texts.Add(t.ToString());
      return true;
    }
  }
}
'@
[void][SLW.W]::SetProcessDPIAware()
Get-Process -Name SoundLatch, _mt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$env:SL_FAKE = $Fake
$p = Start-Process $Exe -PassThru
Start-Sleep -Milliseconds 2200
[SLW.W]::wins.Clear()
[SLW.W]::EnumWindows([SLW.W+Cb]{ param($a,$b) [void][SLW.W]::Find($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
if ([SLW.W]::wins.Count -eq 0) { "no popup"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
$h = [SLW.W]::wins[0]
if ($ClickX -ge 0) {
  [SLW.W]::texts.Clear()
  [SLW.W]::EnumChildWindows($h, [SLW.W+Cb]{ param($a,$b) [void][SLW.W]::Child($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
  $cr = New-Object -TypeName 'SLW.W+RECT'
  [SLW.W]::GetClientRect([SLW.W]::meter, [ref]$cr) | Out-Null
  $lp = [IntPtr]([int](($cr.B - $cr.T) / 2) * 65536 + $ClickX)
  [SLW.W]::PostMessage([SLW.W]::meter, 0x0201, [IntPtr]1, $lp) | Out-Null
  [SLW.W]::PostMessage([SLW.W]::meter, 0x0202, [IntPtr]0, $lp) | Out-Null
  Start-Sleep -Milliseconds 200
  [SLW.W]::texts.Clear()
  [SLW.W]::EnumChildWindows($h, [SLW.W+Cb]{ param($a,$b) [void][SLW.W]::Child($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
  "label -> " + (([SLW.W]::texts | Where-Object { $_ -match '   ' -and $_ -notmatch '%' }) -join ' | ')
}
$r = New-Object -TypeName 'SLW.W+RECT'
[SLW.W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); [SLW.W]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"shot $Out ${w}x${ht} fake=$Fake click=$ClickX"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
