$paths = @(
  "C:\Windows\System32\drivers\AnyMix.sys",
  "C:\Users\akram\Downloads\vb\driver\anymix\Source\Main\x64\Release\AnyMix.sys"
)
foreach ($p in $paths) {
  if (Test-Path $p) {
    $i = Get-Item $p
    $v = $i.VersionInfo
    Write-Output ("FILE {0}" -f $p)
    Write-Output ("  size {0} written {1:yyyy-MM-dd HH:mm:ss}" -f $i.Length, $i.LastWriteTime)
    Write-Output ("  file {0} product {1}" -f $v.FileVersion, $v.ProductVersion)
  } else {
    Write-Output ("MISSING {0}" -f $p)
  }
}
Get-CimInstance Win32_SystemDriver | Where-Object { $_.Name -match "AnyMix" } | ForEach-Object {
  Write-Output ("SERVICE {0} state {1} path {2}" -f $_.Name, $_.State, $_.PathName)
}
