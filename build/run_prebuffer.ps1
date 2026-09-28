$ErrorActionPreference = "Stop"
Set-Location "C:\Users\akram\Downloads\vb"

function RunOne([string]$n) {
  Remove-Item "build\audio_diagnostics.txt" -ErrorAction SilentlyContinue
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = (Resolve-Path "build\AnyMix.exe").Path
  $psi.WorkingDirectory = (Resolve-Path "build").Path
  $psi.UseShellExecute = $false
  $psi.EnvironmentVariables["ANYMIX_PREBUFFER"] = $n
  $p = [Diagnostics.Process]::Start($psi)
  Start-Sleep -Seconds 2
  if ($p.HasExited) {
    Write-Output "FAIL prebuffer=$n exit=$($p.ExitCode)"
    return
  }
  Write-Output "RUNNING prebuffer=$n pid=$($p.Id)"
  & ".\build\measure_rates.exe" play 30 | Out-File -Encoding utf8 "build\play_$n.txt"
  if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force
  }
  Start-Sleep -Milliseconds 400
  Copy-Item "build\audio_diagnostics.txt" "build\diag_$n.txt" -Force
  Write-Output "SAVED prebuffer=$n"
}

foreach ($n in @("512", "1024", "2048")) {
  RunOne $n
}
& ".\build\list_sessions.exe" unmute | Out-File -Encoding utf8 "build\sessions_unmute.txt"
Write-Output "DONE"
