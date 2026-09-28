$sync = @(Import-Csv (Join-Path $PSScriptRoot "mix_sync.csv"))
$prod = @(Import-Csv (Join-Path $PSScriptRoot "mix_prod.csv"))
foreach ($pair in @(@("GAME","0","consumedGame"), @("MUSIC","2","consumedMusic"))) {
  $name = $pair[0]; $id = $pair[1]; $col = $pair[2]
  $recv = 0
  foreach ($r in $prod) { if ($r.stream -eq $id) { $recv += [int]$r.framesWritten } }
  $cons = 0
  foreach ($r in $sync) { $cons += [int]$r.$col }
  Write-Output "$name received=$recv consumed=$cons delta=$($recv - $cons)"
}
