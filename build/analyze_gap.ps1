$csv = Import-Csv "C:\Users\akram\Downloads\vb\build\mix_sync.csv"
$allOn = 119285196326L
$paused = 119325168737L
$resumed = 119350069117L
$done = 119410163312L
function Phase($rows, $name) {
  $n = $rows.Count
  if ($n -eq 0) { Write-Output "$name rows=0"; return }
  $asked = $rows | ForEach-Object { [int]$_.asked }
  $holeM = @($rows | Where-Object { [int]$_.consumedMusic -eq 0 -and [int]$_.asked -gt 0 }).Count
  $holeG = @($rows | Where-Object { [int]$_.consumedGame -eq 0 -and [int]$_.asked -gt 0 }).Count
  $both = @($rows | Where-Object { [int]$_.consumedMusic -gt 0 -and [int]$_.consumedGame -gt 0 }).Count
  $minA = ($asked | Measure-Object -Minimum).Minimum
  $maxA = ($asked | Measure-Object -Maximum).Maximum
  $avgA = [math]::Round(($asked | Measure-Object -Average).Average, 1)
  $fills = $rows | ForEach-Object { [int]$_.fillBeforeMusic }
  $minF = ($fills | Measure-Object -Minimum).Minimum
  $maxF = ($fills | Measure-Object -Maximum).Maximum
  $avgF = [math]::Round(($fills | Measure-Object -Average).Average, 1)
  Write-Output "$name rows=$n both=$both holeGame=$holeG holeMusic=$holeM asked min/avg/max=$minA/$avgA/$maxA musicFill min/avg/max=$minF/$avgF/$maxF"
}
$before = $csv | Where-Object { [long]$_.qpcMusic -ge $allOn -and [long]$_.qpcMusic -lt $paused }
$gap = $csv | Where-Object { [long]$_.qpcMusic -ge $paused -and [long]$_.qpcMusic -lt $resumed }
$after = $csv | Where-Object { [long]$_.qpcMusic -ge $resumed -and [long]$_.qpcMusic -lt $done }
Phase $before "before"
Phase $gap "paused"
Phase $after "after"
Write-Output "--- first 15 after resume ---"
$after | Select-Object -First 15 | ForEach-Object {
  "{0} asked={1} game={2}/{3} music={4}/{5} fillM={6}" -f $_.qpcMusic, $_.asked, $_.consumedGame, $_.availGame, $_.consumedMusic, $_.availMusic, $_.fillBeforeMusic
}
Write-Output "--- asked histogram after ---"
$after | Group-Object asked | Sort-Object { [int]$_.Name } | ForEach-Object { "asked=$($_.Name) n=$($_.Count)" }
