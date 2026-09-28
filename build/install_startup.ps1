$ErrorActionPreference = 'Stop'
$exe = 'C:\Users\akram\Downloads\vb\build\AnyMix.exe'
if (-not (Test-Path -LiteralPath $exe)) {
  throw "AnyMix.exe introuvable"
}
$action = New-ScheduledTaskAction -Execute $exe
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$trigger.Delay = 'PT0S'
$settings = New-ScheduledTaskSettingsSet `
  -AllowStartIfOnBatteries `
  -DontStopIfGoingOnBatteries `
  -DontStopOnIdleEnd `
  -StartWhenAvailable `
  -Priority 4 `
  -MultipleInstances IgnoreNew `
  -ExecutionTimeLimit ([TimeSpan]::Zero)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'AnyMix' -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
$info = Get-ScheduledTask -TaskName 'AnyMix'
$info.Triggers | ForEach-Object {
  'trigger=' + $_.CimClass.CimClassName + ' user=' + $_.UserId + ' delay=' + $_.Delay
}
'execute=' + $info.Actions[0].Execute
'priority=' + $info.Settings.Priority
'limit=' + $info.Settings.ExecutionTimeLimit
'ok'
