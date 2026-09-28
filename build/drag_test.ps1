Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DragWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr Hwnd;
}
"@

[DragWin]::EnumWindows({
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  $cb = New-Object System.Text.StringBuilder 256
  [void][DragWin]::GetWindowText($h, $sb, 256)
  [void][DragWin]::GetClassName($h, $cb, 256)
  $title = $sb.ToString()
  $class = $cb.ToString()
  if ($title -like "*AnyMix*" -or $class -eq "AnyMixWindow") {
    $rect = New-Object DragWin+RECT
    [void][DragWin]::GetClientRect($h, [ref]$rect)
    Write-Output ("WIN {0} class={1} title={2} client={3}x{4}" -f $h, $class, $title, $rect.Right, $rect.Bottom)
    if ($class -eq "AnyMixWindow" -and $rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null

if (-not $hwnd) {
  Write-Output "NOWINDOW"
  exit 1
}
$rect = New-Object DragWin+RECT
[void][DragWin]::GetClientRect($hwnd, [ref]$rect)
$w = $rect.Right
$h = $rect.Bottom
$side = 196
if ($w -lt 1180) { $side = 68 }
$masterW = 100
$gap = 8
$mastersW = 5 * $masterW + 4 * $gap
$viewW = [Math]::Max(0, $w - $side - $mastersW - 24)
$stripW = [Math]::Max(1, [int](($viewW - 6 * $gap) / 7))
function SlotX([int]$slot) {
  return $side + 8 + $slot * ($stripW + $gap) + [int]($stripW / 2)
}
$appY = $h - 120 + 9
$musicX = SlotX 3
$chatX = SlotX 2
Write-Output "HWND=$hwnd client=${w}x${h} music=$musicX,$appY chat=$chatX,$appY"
function Pack([int]$x, [int]$y) {
  return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
}
[void][DragWin]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 200
[void][DragWin]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Pack $musicX $appY))
Start-Sleep -Milliseconds 80
[void][DragWin]::PostMessage($hwnd, 0x0200, [IntPtr]1, (Pack $chatX $appY))
Start-Sleep -Milliseconds 80
[void][DragWin]::PostMessage($hwnd, 0x0202, [IntPtr]0, (Pack $chatX $appY))
Start-Sleep -Milliseconds 500
$script:dialog = $false
[DragWin]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][DragWin]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "#32770") {
    $sb = New-Object System.Text.StringBuilder 256
    [void][DragWin]::GetWindowText($h, $sb, 256)
    if ($sb.ToString() -eq "AnyMix") { $script:dialog = $true }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
if ($script:dialog) { Write-Output "DIALOG" } else { Write-Output "NODIALOG" }
