Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DragPan {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$hwnd = [IntPtr]::Zero
[DragPan]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][DragPan]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object DragPan+RECT
    [void][DragPan]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
$rect = New-Object DragPan+RECT
[void][DragPan]::GetClientRect($hwnd, [ref]$rect)
$w = $rect.Right
$side = 196
if ($w -lt 1180) { $side = 68 }
$mixerTop = 56
$gap = 8
$mastersW = 5 * 100 + 4 * $gap
$shown = 6
$viewW = [Math]::Max(0, $w - $side - $mastersW - 24)
$stripW = [int](($viewW - ($shown - 1) * $gap) / $shown)
$slot = 1
$x = $side + 8 + $slot * ($stripW + $gap)
$badgeRight = $x + $stripW - 6
$px = $badgeRight - 34
$py = $mixerTop + 6 + 6
$cx = $px + 14
$cy = $py + 14
$lx = $px + 2
function Pack([int]$a, [int]$b) { return [IntPtr](($b -shl 16) -bor ($a -band 0xFFFF)) }
[void][DragPan]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 120
[void][DragPan]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Pack $cx $cy))
Start-Sleep -Milliseconds 40
[void][DragPan]::PostMessage($hwnd, 0x0200, [IntPtr]1, (Pack $lx $cy))
Start-Sleep -Milliseconds 40
[void][DragPan]::PostMessage($hwnd, 0x0202, [IntPtr]0, (Pack $lx $cy))
Write-Output "drag $cx,$cy -> $lx,$cy stripW $stripW"
