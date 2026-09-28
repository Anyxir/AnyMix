Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile("C:\Users\akram\Downloads\vb\build\mixer_shot.png")
$x = 1280
for ($y = 40; $y -lt $b.Height; $y += 10) {
  $c = $b.GetPixel($x, $y)
  Write-Output ("{0,4} {1,3} {2,3} {3,3}" -f $y, $c.R, $c.G, $c.B)
}
$b.Dispose()
