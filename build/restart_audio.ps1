$log = 'C:\Users\akram\Downloads\vb\build\restart_audio.log'
function Log($m) { Add-Content -LiteralPath $log -Value $m }
Set-Content -LiteralPath $log -Value 'start'
$names = @('Audiosrv', 'AudioEndpointBuilder', 'RtkAudioUniversalService')
try {
  Stop-Service -Name Audiosrv -Force -ErrorAction Stop
  Log 'Audiosrv stopped'
} catch {
  Log ('stop Audiosrv ' + $_.Exception.Message)
}
try {
  Stop-Service -Name AudioEndpointBuilder -Force -ErrorAction Stop
  Log 'AudioEndpointBuilder stopped'
} catch {
  Log ('stop builder ' + $_.Exception.Message)
}
Start-Sleep -Seconds 1
try {
  Start-Service -Name AudioEndpointBuilder -ErrorAction Stop
  Log 'AudioEndpointBuilder started'
} catch {
  Log ('start builder ' + $_.Exception.Message)
}
try {
  Start-Service -Name Audiosrv -ErrorAction Stop
  Log 'Audiosrv started'
} catch {
  Log ('start Audiosrv ' + $_.Exception.Message)
}
try {
  Restart-Service -Name RtkAudioUniversalService -Force -ErrorAction Stop
  Log 'Realtek restarted'
} catch {
  Log ('realtek ' + $_.Exception.Message)
}
foreach ($name in $names) {
  $svc = Get-Service -Name $name
  Log ($svc.Name + ' ' + $svc.Status)
}
Log 'done'
