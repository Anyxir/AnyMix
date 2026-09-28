$path = Join-Path $PSScriptRoot "mix_sync.csv"
$rows = Import-Csv $path
$both = 0
$one = 0
$diff = 0
$emptyOther = 0
$gameOnly = 0
$musicOnly = 0
$maxDelta = 0
$sumDelta = 0
$n = 0
foreach ($r in $rows) {
  $cg = [int]$r.consumedGame
  $cm = [int]$r.consumedMusic
  $ag = [int]$r.availGame
  $am = [int]$r.availMusic
  $d = [int64]$r.deltaUs
  $n++
  if ($d -gt $maxDelta) { $maxDelta = $d }
  $sumDelta += $d
  $gNew = $cg -gt 0
  $mNew = $cm -gt 0
  if ($gNew -and $mNew) { $both++ }
  elseif ($gNew -or $mNew) { $one++ }
  if ($gNew -and -not $mNew) { $gameOnly++ }
  if ($mNew -and -not $gNew) { $musicOnly++ }
  if ($cg -ne $cm) { $diff++ }
  if (($ag -eq 0 -and $cm -gt 0) -or ($am -eq 0 -and $cg -gt 0)) { $emptyOther++ }
}
$avg = 0
if ($n -gt 0) { $avg = [math]::Round($sumDelta / $n, 3) }
Write-Output "cycles=$n both=$both one=$one gameOnly=$gameOnly musicOnly=$musicOnly differentConsume=$diff oneEmptyWhileOtherAdvances=$emptyOther deltaUsAvg=$avg deltaUsMax=$maxDelta"
