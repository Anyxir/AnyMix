Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ClickB {
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
[ClickB]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][ClickB]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object ClickB+RECT
    [void][ClickB]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
$rect = New-Object ClickB+RECT
[void][ClickB]::GetClientRect($hwnd, [ref]$rect)
$w = $rect.Right
$h = $rect.Bottom
$side = 196
if ($w -lt 1180) { $side = 68 }
$topH = 48
$mixerTop = $topH + 8
$mixerH = $h - $topH - 36
$masterW = 100
$gap = 8
$mastersW = 5 * $masterW + 4 * $gap
$shown = 6
$viewW = [Math]::Max(0, $w - $side - $mastersW - 24)
$stripW = [int](($viewW - ($shown - 1) * $gap) / $shown)
$slot = 1
if ($args.Count -gt 0) { $slot = [int]$args[0] }
$x = $side + 8 + $slot * ($stripW + $gap)
$cardH = $mixerH - 8
$appH = 88
$meterTop = $mixerTop + 52
$meterH = [Math]::Max(64, $cardH - 52 - 156 - $appH)
$y = $meterTop + $meterH + 2 + 18 + 24 + 52
$routeGap = 3
$routeH = 20
$routeSpan = [Math]::Max(5, $stripW - 16)
$routeW = [Math]::Max(18, [int](($routeSpan - 4 * $routeGap) / 5))
$bx = $x + 8 + [int]($routeW / 2)
$by = $y + 1 * ($routeH + 3) + [int]($routeH / 2)
Write-Output "client ${w}x${h} B1 click $bx,$by stripW $stripW"
function Pack([int]$px, [int]$py) { return [IntPtr](($py -shl 16) -bor ($px -band 0xFFFF)) }
[void][ClickB]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 150
[void][ClickB]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Pack $bx $by))
Start-Sleep -Milliseconds 40
[void][ClickB]::PostMessage($hwnd, 0x0202, [IntPtr]0, (Pack $bx $by))
Write-Output "CLICKED"
