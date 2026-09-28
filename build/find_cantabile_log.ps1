$roots = @(
  "$env:USERPROFILE\Documents",
  "$env:APPDATA",
  "$env:LOCALAPPDATA"
)
foreach ($r in $roots) {
  Get-ChildItem $r -Filter "*Cantabile*" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
}
Get-ChildItem "$env:USERPROFILE\Documents" -Recurse -Filter "*.log" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "Cantabile" } | Select-Object -First 20 -ExpandProperty FullName
