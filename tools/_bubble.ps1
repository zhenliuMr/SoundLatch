param([int]$Hover = 0, [int]$WaitMs = 1200, [string]$Out = "D:\project\PUBG\tools\_bubble.png")
# Hover=0 -> 第一个问号(g_hQ1)；Hover=1 -> 第二个
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
namespace SLV {
  public class W {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int EnumWindows(Cb cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int EnumChildWindows(IntPtr parent, Cb cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    public delegate bool Cb(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr popup = IntPtr.Zero;
    public static IntPtr main = IntPtr.Zero;
    public static IntPtr bubble = IntPtr.Zero;
    public static bool Find2(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == "SoundLatchBubble" && IsWindowVisible(h)) bubble = h;
      return true;
    }
    public static System.Collections.Generic.List<IntPtr> qs = new System.Collections.Generic.List<IntPtr>();
    public static bool Find(IntPtr h, IntPtr p) {
      StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (sb.ToString() == "SoundLatchWnd") main = h;
      if (sb.ToString() == "SoundLatchPopup" && IsWindowVisible(h)) popup = h;
      return true;
    }
    public static bool Child(IntPtr h, IntPtr p) {
      StringBuilder t = new StringBuilder(64); GetWindowText(h, t, 64);
      if (t.ToString() == "?") qs.Add(h);
      return true;
    }
  }
}
'@
[void][SLV.W]::SetProcessDPIAware()

Get-Process -Name SoundLatch -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Start-Process "D:\project\PUBG\dist\SoundLatch.exe"
Start-Sleep -Seconds 3

# 弹出面板
[SLV.W]::EnumWindows({ param($h,$p) [SLV.W]::Find($h,$p) }, [IntPtr]::Zero) | Out-Null
if ([SLV.W]::main -ne [IntPtr]::Zero) {
  [void][SLV.W]::PostMessage([SLV.W]::main, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_APP+2
  Start-Sleep -Milliseconds 800
  [void][SLV.W]::EnumWindows({ param($h,$p) [SLV.W]::Find($h,$p) }, [IntPtr]::Zero)
}
if ([SLV.W]::popup -eq [IntPtr]::Zero) { "popup not found"; exit 1 }
[SLV.W]::EnumChildWindows([SLV.W]::popup, { param($h,$p) [SLV.W]::Child($h,$p) }, [IntPtr]::Zero) | Out-Null
if ([SLV.W]::qs.Count -lt 2) { "q marks: " + [SLV.W]::qs.Count; exit 1 }

$q = [SLV.W]::qs[$Hover]
$r = New-Object SLV.W+RECT
[void][SLV.W]::GetWindowRect($q, [ref]$r)
$cx = [int](($r.L + $r.R)/2); $cy = [int](($r.T + $r.B)/2)
[void][SLV.W]::SetCursorPos($cx, $cy)
Start-Sleep -Milliseconds $WaitMs

# 截整个屏幕（气泡是独立顶层窗，PrintWindow 抓不到）
Add-Type -AssemblyName System.Drawing
$vw = [SLV.W]::GetSystemMetrics(78); $vh = [SLV.W]::GetSystemMetrics(79)   #虚拟屏幕
$vx = [SLV.W]::GetSystemMetrics(76); $vy = [SLV.W]::GetSystemMetrics(77)
$b = New-Object System.Drawing.Bitmap $vw, $vh
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($vx, $vy, 0, 0, (New-Object System.Drawing.Size $vw, $vh))
$g.Dispose()
# 以气泡窗口为准裁剪；找不到就退回整屏右下角
$cx = $vx; $cy = $vy; $cw = $vw; $ch = $vh
[void][SLV.W]::EnumWindows({ param($h,$p) [SLV.W]::Find2($h,$p) }, [IntPtr]::Zero)
if ([SLV.W]::bubble -ne [IntPtr]::Zero) {
  $br = New-Object SLV.W+RECT
  [void][SLV.W]::GetWindowRect([SLV.W]::bubble, [ref]$br)
  $m = 30
  $cx = [Math]::Max(0, $br.L - $m - $vx); $cy = [Math]::Max(0, $br.T - $m - $vy)
  $cw = [Math]::Min($vw - $cx, ($br.R - $br.L) + 2*$m); $ch = [Math]::Min($vh - $cy, ($br.B - $br.T) + 2*$m)
}
$crop = New-Object System.Drawing.Rectangle $cx, $cy, $cw, $ch
$c = $b.Clone($crop, $b.PixelFormat)
$c.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$c.Dispose(); $b.Dispose()
"saved $Out (cursor $cx,$cy)"