param([string]$Label, [string]$Which)
$root = $PSScriptRoot
$sync = @(Import-Csv (Join-Path $root "mix_sync.csv"))
$prod = @(Import-Csv (Join-Path $root "mix_prod.csv"))
function StreamReport($name, $id, $active) {
  $list = @($prod | Where-Object { $_.stream -eq $id })
  if ($list.Count -eq 0) { Write-Output "$name packets=0"; return }
  $frames = 0; $minS = 999999; $maxS = 0
  foreach ($r in $list) {
    $n = [int]$r.framesWritten
    $frames += $n
    if ($n -lt $minS) { $minS = $n }
    if ($n -gt $maxS) { $maxS = $n }
  }
  $t0 = [int64]$list[0].qpcWrite
  $t1 = [int64]$list[$list.Count - 1].qpcWrite
  $sec = ($t1 - $t0) / 10000000.0
  $rate = 0; if ($sec -gt 0) { $rate = [math]::Round($frames / $sec, 1) }
  $gaps = @(); $burst = [int]$list[0].framesWritten; $maxBurst = $burst
  for ($i = 1; $i -lt $list.Count; $i++) {
    $ms = ([int64]$list[$i].qpcWrite - [int64]$list[$i - 1].qpcWrite) / 10000.0
    if ($ms -lt 0.2) { $burst += [int]$list[$i].framesWritten }
    else { $gaps += $ms; if ($burst -gt $maxBurst) { $maxBurst = $burst }; $burst = [int]$list[$i].framesWritten }
  }
  if ($burst -gt $maxBurst) { $maxBurst = $burst }
  $gs = $gaps | Sort-Object
  $gmin = 0; $gmax = 0; $gavg = 0
  if ($gs.Count -gt 0) {
    $gmin = [math]::Round($gs[0], 3)
    $gmax = [math]::Round($gs[$gs.Count - 1], 3)
    $gavg = [math]::Round((($gaps | Measure-Object -Average).Average), 3)
  }
  $avgS = [math]::Round($frames / $list.Count, 1)
  Write-Output "$name packets=$($list.Count) frames=$frames spanSec=$([math]::Round($sec,3)) framesPerSec=$rate packetMin=$minS packetAvg=$avgS packetMax=$maxS gapMsMin=$gmin gapMsAvg=$gavg gapMsMax=$gmax maxBurstFrames=$maxBurst"
  if (-not $active) { return }
  $rows = @($sync | Where-Object { [int64]$_.qpcGame -gt 0 })
  $askedName = "asked"
  $cons = $(if ($name -eq "GAME") { "consumedGame" } else { "consumedMusic" })
  $fillB = $(if ($name -eq "GAME") { "fillBeforeGame" } else { "fillBeforeMusic" })
  $fillA = $(if ($name -eq "GAME") { "fillAfterGame" } else { "fillAfterMusic" })
  $qpc = $(if ($name -eq "GAME") { "qpcGame" } else { "qpcMusic" })
  $first = -1; $last = -1
  for ($i = 0; $i -lt $rows.Count; $i++) {
    if ([int]$rows[$i].$cons -gt 0) { if ($first -lt 0) { $first = $i }; $last = $i }
  }
  if ($first -lt 0) { Write-Output "$name mixCycles=0"; return }
  $cycles = 0; $full = 0; $zero = 0; $partial = 0; $miss = 0
  $minF = 999999; $maxF = 0; $sumF = 0
  $case1 = 0; $emptyUs = 0
  for ($i = $first; $i -le $last; $i++) {
    $cycles++
    $a = [int]$rows[$i].$askedName
    $c = [int]$rows[$i].$cons
    $fb = [int]$rows[$i].$fillB
    $fa = [int]$rows[$i].$fillA
    if ($fb -lt $minF) { $minF = $fb }
    if ($fb -gt $maxF) { $maxF = $fb }
    $sumF += $fb
    if ($c -ge $a -and $a -gt 0) { $full++ }
    elseif ($c -eq 0) { $zero++; $miss += $a }
    else { $partial++; $miss += ($a - $c) }
    if ($fa -eq 0 -and $i -lt $last) {
      $nxt = [int]$rows[$i + 1].$cons
      $na = [int]$rows[$i + 1].$askedName
      if ($nxt -ge $na -and $na -gt 0) { $case1++ }
      $emptyUs += ([int64]$rows[$i + 1].$qpc - [int64]$rows[$i].$qpc) / 10
    }
  }
  $avgF = [math]::Round($sumF / $cycles, 1)
  $missMs = [math]::Round($miss / 48.0, 1)
  $emptyMs = [math]::Round($emptyUs / 1000.0, 1)
  Write-Output "$name mixCycles=$cycles full=$full zeroAtMix=$zero partial=$partial missingFrames=$miss missingMs=$missMs ringMin=$minF ringAvg=$avgF ringMax=$maxF case1EmptyThenFull=$case1 emptyBetweenMixMs=$emptyMs"
}
Write-Output "CASE $Label"
$wantGame = $Which -ne "music"
$wantMusic = $Which -ne "game"
StreamReport "GAME" "0" $wantGame
StreamReport "MUSIC" "2" $wantMusic
