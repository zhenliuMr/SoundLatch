param([string]$Out = "D:\project\PUBG\tools\_help.png")
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace SLH2 {
  public class W {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int EnumWindows(Cb cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int EnumChildWindows(IntPtr parent, Cb cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    public delegate bool Cb(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr mainw = IntPtr.Zero, popup = IntPtr.Zero;
    public static List<IntPtr> kids = new List<IntPtr>();
    public static List<string> texts = new List<string>();
    public static bool Find(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      string c = sb.ToString();
      if (c == "SoundLatchWnd") mainw = h;
      if (c == "SoundLatchPopup") popup = h;
      return true;
    }
    public static bool Child(IntPtr h, IntPtr p) {
      StringBuilder t = new StringBuilder(512); GetWindowText(h, t, 512);
      kids.Add(h); texts.Add(t.ToString());
      return true;
    }
  }
}
'@
[void][SLH2.W]::SetProcessDPIAware()
Get-Process -Name SoundLatch -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$p = Start-Process "D:\project\PUBG\dist\SoundLatch.exe" -PassThru
Start-Sleep -Milliseconds 2200
[SLH2.W]::EnumWindows([SLH2.W+Cb]{ param($a,$b) [void][SLH2.W]::Find($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
[void][SLH2.W]::PostMessage([SLH2.W]::mainw, 0x8001, [IntPtr]1, [IntPtr]0x0202)
Start-Sleep -Milliseconds 500
[SLH2.W]::kids.Clear(); [SLH2.W]::texts.Clear()
[SLH2.W]::EnumChildWindows([SLH2.W]::popup, [SLH2.W+Cb]{ param($a,$b) [void][SLH2.W]::Child($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
$q = ([SLH2.W]::texts | Where-Object { $_ -eq '?' }).Count
"question marks = $q"
$r = New-Object -TypeName 'SLH2.W+RECT'
[void][SLH2.W]::GetWindowRect([SLH2.W]::popup, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); [void][SLH2.W]::PrintWindow([SLH2.W]::popup, $hdc, 2); $g.ReleaseHdc($hdc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
"shot $Out"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
