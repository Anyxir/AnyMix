$paths = @(
  "C:\Program Files",
  "C:\Program Files (x86)"
)
foreach ($root in $paths) {
  if (Test-Path $root) {
    Get-ChildItem -Path $root -Filter "Cantabile*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
  }
}
Write-Output "---- HKLM ASIO ----"
reg query HKLM\SOFTWARE\ASIO /s 2>&1
Write-Output "---- DLL ----"
$dll = "C:\Users\akram\Downloads\vb\build\AnyMixInsertASIO.dll"
if (Test-Path $dll) { Write-Output "DLL_OK" } else { Write-Output "DLL_MISSING" }
Write-Output "---- selftest ----"
