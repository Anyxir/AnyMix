Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile("C:\Users\akram\Downloads\vb\public\logo_long.png")
$b = New-Object System.Drawing.Bitmap $img
Write-Output ("size {0} x {1}" -f $b.Width, $b.Height)
$stepX = [Math]::Max(1, [int]($b.Width / 16))
$stepY = [Math]::Max(1, [int]($b.Height / 8))
for ($y = 0; $y -lt $b.Height; $y += $stepY) {
  $line = ""
  for ($x = 0; $x -lt $b.Width; $x += $stepX) {
    $c = $b.GetPixel($x, $y)
    $line += ("{0:x2}{1:x2}{2:x2}.{3:x2} " -f $c.R, $c.G, $c.B, $c.A)
  }
  Write-Output $line
}
Get-ChildItem C:\Windows\Fonts -File | ForEach-Object { $_.Name } | Select-String -Pattern "segoe|bahn|mont|popp|nuni|outfit|inter|gothic|calibri|candara|arial"
