Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ClickAsio {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$hwnd = [IntPtr]::Zero
[ClickAsio]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][ClickAsio]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object ClickAsio+RECT
    [void][ClickAsio]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
function Pack([int]$a, [int]$b) { return [IntPtr](($b -shl 16) -bor ($a -band 0xFFFF)) }
# ASIO is nav index 5. Item top = 126 + 5*36, height 32.
$y = 126 + 5 * 36 + 16
[void][ClickAsio]::PostMessage($hwnd, 0x0201, [IntPtr]1, (Pack 80 $y))
Start-Sleep -Milliseconds 40
[void][ClickAsio]::PostMessage($hwnd, 0x0202, [IntPtr]0, (Pack 80 $y))
Write-Output "clicked $y"
