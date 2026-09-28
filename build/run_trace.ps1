$ErrorActionPreference = "Stop"
Set-Location "C:\Users\akram\Downloads\vb"
& ".\build\list_sessions.exe" mute | Out-Null
Remove-Item "build\audio_diagnostics.txt","build\audio_cycles.csv" -ErrorAction SilentlyContinue
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path "build\AnyMix.exe").Path
$psi.WorkingDirectory = (Resolve-Path "build").Path
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["ANYMIX_PREBUFFER"] = "2048"
$p = [Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 2
if ($p.HasExited) { Write-Output "FAIL exit=$($p.ExitCode)"; exit 1 }
Write-Output "RUNNING pid=$($p.Id)"
& ".\build\measure_rates.exe" play 30 | Out-File -Encoding utf8 "build\play_trace.txt"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 800
& ".\build\list_sessions.exe" unmute | Out-Null
Write-Output "STOPPED"
Get-Item "build\audio_cycles.csv" | Select-Object Length, FullName | Format-List
