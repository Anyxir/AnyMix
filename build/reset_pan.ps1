Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class ResetPan {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$hwnd = [IntPtr]::Zero
[ResetPan]::EnumWindows({
  param($h, $l)
  $cb = New-Object System.Text.StringBuilder 256
  [void][ResetPan]::GetClassName($h, $cb, 256)
  if ($cb.ToString() -eq "AnyMixWindow") {
    $rect = New-Object ResetPan+RECT
    [void][ResetPan]::GetClientRect($h, [ref]$rect)
    if ($rect.Right -gt 400) { $script:hwnd = $h }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
function Pack([int]$a, [int]$b) { return [IntPtr](($b -shl 16) -bor ($a -band 0xFFFF)) }
[void][ResetPan]::PostMessage($hwnd, 0x0203, [IntPtr]0, (Pack 502 82))
Write-Output "reset"
