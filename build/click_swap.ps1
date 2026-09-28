Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ClickSwap {
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
[ClickSwap]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][ClickSwap]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object ClickSwap+RECT
    [void][ClickSwap]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
$rect = New-Object ClickSwap+RECT
[void][ClickSwap]::GetClientRect($hwnd, [ref]$rect)
$w = $rect.Right
$side = 196
if ($w -lt 1180) { $side = 68 }
$mixerTop = 56
$masterW = 100
$gap = 8
$mastersW = 5 * $masterW + 4 * $gap
$shown = 6
$viewW = [Math]::Max(0, $w - $side - $mastersW - 24)
$stripW = [int](($viewW - ($shown - 1) * $gap) / $shown)
$slot = 1
if ($args.Count -gt 0) { $slot = [int]$args[0] }
$x = $side + 8 + $slot * ($stripW + $gap)
$cardRight = $x + $stripW
$badgeRight = $cardRight - 6
$sx = $badgeRight - 40 + 17
$sy = $mixerTop + 6 + 8 + 12
Write-Output "client $w swap click $sx,$sy stripW $stripW"
function Pack([int]$px, [int]$py) { return [IntPtr](($py -shl 16) -bor ($px -band 0xFFFF)) }
[void][ClickSwap]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 150
[void][ClickSwap]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Pack $sx $sy))
Start-Sleep -Milliseconds 40
[void][ClickSwap]::PostMessage($hwnd, 0x0202, [IntPtr]0, (Pack $sx $sy))
Start-Sleep -Milliseconds 200
Write-Output "CLICKED"
