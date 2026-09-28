param([string]$Label)
$path = Join-Path $PSScriptRoot "mix_prod.csv"
$rows = @(Import-Csv $path)
Write-Output "CASE $Label rows=$($rows.Count)"
if ($rows.Count -eq 0) { return }
$freq = 10000000.0
$by = @{ "0" = @(); "2" = @() }
foreach ($r in $rows) { $by[$r.stream] += $r }
foreach ($id in @("0", "2")) {
  $name = $(if ($id -eq "0") { "GAME" } else { "MUSIC" })
  $list = $by[$id]
  if ($list.Count -eq 0) {
    Write-Output "$name packets=0"
    continue
  }
  $frames = 0
  $sizes = @{}
  foreach ($r in $list) {
    $frames += [int]$r.framesWritten
    $key = "$($r.framesWritten)"
    if (-not $sizes.ContainsKey($key)) { $sizes[$key] = 0 }
    $sizes[$key]++
  }
  $t0 = [int64]$list[0].qpcWrite
  $t1 = [int64]$list[$list.Count - 1].qpcWrite
  $sec = ($t1 - $t0) / $freq
  $rate = 0
  if ($sec -gt 0) { $rate = [math]::Round($frames / $sec, 1) }
  $sizeText = ($sizes.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)x$($_.Value)" }) -join " "
  Write-Output "$name packets=$($list.Count) frames=$frames spanSec=$([math]::Round($sec, 3)) framesPerSec=$rate sizes=$sizeText lastPacket=$($list[$list.Count - 1].packet)"
}
$seq = New-Object System.Collections.Generic.List[string]
$gg = 0; $mm = 0; $gm = 0; $mg = 0
$prev = ""
$gaps = @()
$prevQ = 0
foreach ($r in $rows) {
  $tag = $(if ($r.stream -eq "0") { "G" } else { "M" })
  if ($seq.Count -lt 24) { $seq.Add($tag) }
  if ($prev -eq "G" -and $tag -eq "G") { $gg++ }
  elseif ($prev -eq "M" -and $tag -eq "M") { $mm++ }
  elseif ($prev -eq "G" -and $tag -eq "M") { $gm++ }
  elseif ($prev -eq "M" -and $tag -eq "G") { $mg++ }
  if ($prevQ -ne 0) {
    $gaps += ([int64]$r.qpcWrite - $prevQ) / $freq * 1000.0
  }
  $prev = $tag
  $prevQ = [int64]$r.qpcWrite
}
Write-Output ("order=" + ($seq -join ""))
Write-Output "transitions GG=$gg MM=$mm GM=$gm MG=$mg"
if ($gaps.Count -gt 0) {
  $sorted = $gaps | Sort-Object
  $mid = $sorted[[int]($sorted.Count / 2)]
  Write-Output ("gapMs min=$([math]::Round($sorted[0], 3)) median=$([math]::Round($mid, 3)) max=$([math]::Round($sorted[$sorted.Count - 1], 3))")
}
