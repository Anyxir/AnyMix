Get-PnpDevice | Where-Object { $_.InstanceId -like 'ROOT\MEDIA*' -or $_.FriendlyName -like '*AnyMix*' } | Format-List InstanceId, FriendlyName, Status, Problem, ProblemDescription
Write-Host '--- REG ---'
Get-ItemProperty 'HKLM:\SOFTWARE\AnyMix' -ErrorAction SilentlyContinue | Format-List Step, Status, Topo, Wave, Connect, Sub
Write-Host '--- ENDPOINTS ---'
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*AnyMix*' } | Format-Table FriendlyName, Status -AutoSize
Write-Host '--- BOOT ---'
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime
Write-Host '--- STORE ---'
Get-ChildItem 'C:\WINDOWS\System32\DriverStore\FileRepository' -Directory -Filter 'anymix.inf*' | ForEach-Object {
    $sys = Join-Path $_.FullName 'AnyMix.sys'
    $inf = Join-Path $_.FullName 'AnyMix.inf'
    if (Test-Path $sys) {
        $item = Get-Item $sys
        Write-Host ($item.FullName + ' ' + $item.Length + ' ' + $item.LastWriteTime)
    }
    if (Test-Path $inf) {
        Select-String -Path $inf -Pattern 'DriverVer' | ForEach-Object { Write-Host $_.Line }
    }
}
