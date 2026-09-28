Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class MapWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
$hwnd = [IntPtr]::Zero
[MapWin]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][MapWin]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object MapWin+RECT
    [void][MapWin]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
$win = New-Object MapWin+RECT
[void][MapWin]::GetWindowRect($hwnd, [ref]$win)
$pt = New-Object MapWin+POINT
$pt.X = 499
$pt.Y = 82
[void][MapWin]::ClientToScreen($hwnd, [ref]$pt)
$sx = $pt.X - $win.Left
$sy = $pt.Y - $win.Top
$bmp = [System.Drawing.Bitmap]::FromFile("C:\Users\akram\Downloads\vb\build\mixer_shot.png")
Write-Output "shot $($bmp.Width)x$($bmp.Height) pixel $sx,$sy"
for ($dy = -8; $dy -le 8; $dy += 4) {
  $line = ""
  for ($dx = -12; $dx -le 12; $dx += 4) {
    $c = $bmp.GetPixel($sx + $dx, $sy + $dy)
    $line += ("{0:X2}{1:X2}{2:X2} " -f $c.R, $c.G, $c.B)
  }
  Write-Output $line
}
$bmp.Dispose()
