param([string]$Out = "D:\project\PUBG\tools\_verify.png")
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace SLV {
  public class W3 {
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
    public static bool FindPopup(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == "SoundLatchPopup") wins.Add(h);
      return true;
    }
    public static bool ScanChild(IntPtr h, IntPtr p) {
      StringBuilder c = new StringBuilder(256); GetClassName(h, c, 256);
      StringBuilder t = new StringBuilder(512); GetWindowText(h, t, 512);
      if (c.ToString() == "SLMETER") meter = h;
      if (c.ToString() == "Static" || c.ToString() == "STATIC") texts.Add(t.ToString());
      return true;
    }
  }
}
'@
[void][SLV.W3]::SetProcessDPIAware()

function Get-Popup {
  [SLV.W3]::wins.Clear()
  [SLV.W3]::EnumWindows([SLV.W3+Cb]{ param($a,$b) [void][SLV.W3]::FindPopup($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
  if ([SLV.W3]::wins.Count -eq 0) { return [IntPtr]::Zero }
  return [SLV.W3]::wins[0]
}
function Scan($h) {
  [SLV.W3]::meter = [IntPtr]::Zero
  [SLV.W3]::texts.Clear()
  [SLV.W3]::EnumChildWindows($h, [SLV.W3+Cb]{ param($a,$b) [void][SLV.W3]::ScanChild($a,$b); return $true }, [IntPtr]::Zero) | Out-Null
}
function CeilText {
  $c = [SLV.W3]::texts | Where-Object { $_.Length -gt 4 -and $_ -match '   ' }
  if ($c) { return ($c[-1]) } else { return "<none>" }
}
function Shot($h, $path) {
  $r = New-Object -TypeName 'SLV.W3+RECT'
  [SLV.W3]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [SLV.W3]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "shot $path ${w}x${ht}"
}

Get-Process -Name SoundLatch -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$p = Start-Process "D:\project\PUBG\dist\SoundLatch.exe" -PassThru
Start-Sleep -Milliseconds 2000
$h = Get-Popup
if ($h -eq [IntPtr]::Zero) { "no popup"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Scan $h
"startup ceil = $(CeilText)"
"meter = $([SLV.W3]::meter)"

$cr = New-Object -TypeName 'SLV.W3+RECT'
[SLV.W3]::GetClientRect([SLV.W3]::meter, [ref]$cr) | Out-Null
$mw = $cr.R - $cr.L; $mh = $cr.B - $cr.T
"meter client ${mw}x${mh}"

foreach ($frac in 0.0, 0.25, 0.5, 0.9, 1.0) {
  $x = [int]($mw * $frac); if ($x -ge $mw) { $x = $mw - 2 }
  $lp = [IntPtr]([int]($mh/2) * 65536 + $x)
  [SLV.W3]::PostMessage([SLV.W3]::meter, 0x0201, [IntPtr]1, $lp) | Out-Null
  [SLV.W3]::PostMessage([SLV.W3]::meter, 0x0202, [IntPtr]0, $lp) | Out-Null
  Start-Sleep -Milliseconds 150
  Scan $h
  "  click x=$x ({0:P0}) -> {1}" -f $frac, (CeilText)
}
Shot $h $Out
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
