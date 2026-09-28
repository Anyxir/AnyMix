$store = "C:\WINDOWS\system32\DriverStore\FileRepository\anymix.inf_amd64_8e30a1f3de10bf6d"
Get-ChildItem $store | ForEach-Object {
  Write-Output ("{0} {1} {2:yyyy-MM-dd HH:mm:ss}" -f $_.Name, $_.Length, $_.LastWriteTime)
}
$inf = Join-Path $store "AnyMix.inf"
if (Test-Path $inf) {
  Select-String -Path $inf -Pattern "DriverVer" | ForEach-Object { Write-Output $_.Line }
}
$built = "C:\Users\akram\Downloads\vb\driver\anymix\Source\Main\x64\Release\AnyMix.sys"
$live = Join-Path $store "AnyMix.sys"
$hb = (Get-FileHash $built -Algorithm SHA256).Hash
$hl = (Get-FileHash $live -Algorithm SHA256).Hash
Write-Output ("built {0}" -f $hb)
Write-Output ("live  {0}" -f $hl)
Write-Output ("same {0}" -f ($hb -eq $hl))
