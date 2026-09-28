$paths = @(
  "$env:LOCALAPPDATA\CrashDumps",
  "$env:LOCALAPPDATA\Microsoft\Windows\WER\ReportArchive",
  "$env:PROGRAMDATA\Microsoft\Windows\WER\ReportArchive"
)
foreach ($p in $paths) {
  if (Test-Path $p) {
    Write-Output "DIR $p"
    Get-ChildItem $p -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 8 | ForEach-Object { $_.FullName + " " + $_.LastWriteTime }
  }
}
Get-Process Cantabile -ErrorAction SilentlyContinue | ForEach-Object { "PID $($_.Id) $($_.Path)" }
