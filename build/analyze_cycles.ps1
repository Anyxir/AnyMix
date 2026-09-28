$ErrorActionPreference = "Stop"
$path = "C:\Users\akram\Downloads\vb\build\audio_cycles.csv"
$rows = Import-Csv $path
$n = $rows.Count
Write-Output "ROWS $n"
if ($n -lt 2) { exit 1 }
$dtUs = 0.0
foreach ($r in $rows) { $dtUs += [double]$r.us }
$sec = $dtUs / 1000000.0
Write-Output ("SPAN_S {0:N3}" -f $sec)

for ($s = 0; $s -lt 5; $s++) {
  $push = 0L; $pop = 0L; $reads = 0L
  $multi = 0; $askedOver = 0; $hitZero = 0
  $maxPushGap = 0.0; $maxPopGap = 0.0
  $sincePush = 0.0; $sincePop = 0.0
  $zeroAfterPush = @()
  $hist = @{}
  $sawPush = $false; $sawPop = $false
  foreach ($r in $rows) {
    $us = [double]$r.us
    $p = [int]$r.("p$s")
    $o = [int]$r.("o$s")
    $b = [int]$r.("b$s")
    $a = [int]$r.("a$s")
    $rd = [int]$r.("r$s")
    $asked = [int]$r.asked
    $push += $p; $pop += $o; $reads += $rd
    if ($rd -gt 1) { $multi++ }
    if ($p -gt 0) {
      if (-not $hist.ContainsKey($p)) { $hist[$p] = 0 }
      $hist[$p]++
      if ($sawPush -and $sincePush -gt $maxPushGap) { $maxPushGap = $sincePush }
      $sincePush = 0
      $sawPush = $true
    } else { $sincePush += $us }
    if ($o -gt 0) {
      if ($sawPop -and $sincePop -gt $maxPopGap) { $maxPopGap = $sincePop }
      $sincePop = 0
      $sawPop = $true
    } else { $sincePop += $us }
    if ($asked -gt 0 -and $asked -gt $b -and ($p -gt 0 -or $o -gt 0 -or $b -gt 0)) { $askedOver++ }
    if ($b -gt 0 -and $a -eq 0) {
      $hitZero++
      $zeroAfterPush += $sincePush
    }
  }
  if ($push -eq 0 -and $pop -eq 0) { continue }
  $pushRate = if ($sec -gt 0) { $push / $sec } else { 0 }
  $popRate = if ($sec -gt 0) { $pop / $sec } else { 0 }
  $zeroGapMax = 0.0
  foreach ($g in $zeroAfterPush) { if ($g -gt $zeroGapMax) { $zeroGapMax = $g } }
  Write-Output ("STRIP {0} push={1} pop={2} pushPerS={3:N1} popPerS={4:N1} reads={5} multiReadCycles={6} askedGtFill={7} drainedTo0={8} maxPushGapUs={9:N0} maxPopGapUs={10:N0} maxUsSincePushWhenDrained={11:N0}" -f $s, $push, $pop, $pushRate, $popRate, $reads, $multi, $askedOver, $hitZero, $maxPushGap, $maxPopGap, $zeroGapMax)
  $top = $hist.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 8
  foreach ($h in $top) { Write-Output ("  PACKET {0} count={1}" -f $h.Key, $h.Value) }
}

$mismatch = 0; $both = 0; $freeSum = 0L; $w0 = 0L; $w1 = 0L
foreach ($r in $rows) {
  $a = [int]$r.w0; $b = [int]$r.w1
  $w0 += $a; $w1 += $b
  $freeSum += [int]$r.free
  if ($a -gt 0 -or $b -gt 0) {
    $both++
    if ($a -ne $b) { $mismatch++ }
  }
}
Write-Output ("WASAPI w0={0} w1={1} cyclesBothOrOne={2} mismatch={3} freeAvg={4:N1}" -f $w0, $w1, $both, $mismatch, $(if ($n -gt 0) { $freeSum / $n } else { 0 }))
