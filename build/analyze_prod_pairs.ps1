$rows = @(Import-Csv (Join-Path $PSScriptRoot "mix_prod_both.csv"))
$near = 0
$far = 0
$fillZero = 0
for ($i = 1; $i -lt $rows.Count; $i++) {
  $a = $rows[$i - 1]
  $b = $rows[$i]
  if ([int64]$b.fillBefore -eq 0) { $fillZero++ }
  if ($a.stream -eq $b.stream) { continue }
  $ms = ([int64]$b.qpcWrite - [int64]$a.qpcWrite) / 10000.0
  if ($ms -lt 0.2) { $near++ } else { $far++ }
}
Write-Output "crossStream closeUnder0.2ms=$near separated=$far fillBeforeZero=$fillZero of $($rows.Count - 1)"
