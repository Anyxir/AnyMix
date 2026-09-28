$roots = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render',
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
)
foreach ($root in $roots) {
    Write-Host "=== $root ==="
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        $props = Join-Path $_.PSPath 'Properties'
        $name = (Get-ItemProperty $props -ErrorAction SilentlyContinue).'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
        $desc = (Get-ItemProperty $props -ErrorAction SilentlyContinue).'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
        if ($name -match 'AnyMix' -or $desc -match 'AnyMix') {
            Write-Host ($name + ' | ' + $desc)
        }
    }
}
