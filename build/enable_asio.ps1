$log = "C:\AnyProject\Anymix\build\enable_asio.txt"
$targets = @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object {
  $_.FriendlyName -match "ASIOVADPRO" -and $_.Problem -eq "CM_PROB_DISABLED"
})
$ok = 0
$fail = 0
$lines = New-Object System.Collections.Generic.List[string]
foreach ($d in $targets) {
  try {
    Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction Stop
    $ok++
  } catch {
    $fail++
    if ($lines.Count -lt 8) { $lines.Add("$($d.FriendlyName): $($_.Exception.Message)") }
  }
}
$left = @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object {
  $_.FriendlyName -match "ASIOVADPRO" -and $_.Problem -eq "CM_PROB_DISABLED"
}).Count
Set-Content -Path $log -Value @("enabled=$ok failed=$fail stillDisabled=$left")
Add-Content -Path $log -Value $lines
