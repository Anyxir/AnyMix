$roots = @(
  'C:\Program Files',
  'C:\Program Files (x86)',
  'C:\Users\akram\Downloads',
  'C:\Users\akram\Desktop',
  'C:\Users\akram\Music'
)
foreach ($root in $roots) {
  if (-not (Test-Path -LiteralPath $root)) { continue }
  Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -match 'asio|link|vad'
  } | ForEach-Object { $_.FullName }
}
Write-Output '---- driver store ----'
Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository' -Directory -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match 'vad|asiolink|asio' } |
  ForEach-Object { $_.FullName }
Write-Output '---- sys ----'
Get-ChildItem 'C:\Windows\System32\drivers\asiovadpro.sys','C:\Windows\System32\drivers\asiovadpro.dat' -ErrorAction SilentlyContinue |
  ForEach-Object { $_.FullName + ' ' + $_.Length + ' ' + $_.LastWriteTime }
