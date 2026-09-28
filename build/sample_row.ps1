Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile("C:\Users\akram\Downloads\vb\public\logo_long.png")
$b = New-Object System.Drawing.Bitmap $img
$y = [int]($b.Height * 0.52)
$w = $b.Width
for ($x = 0; $x -lt $w; $x += 20) {
  $c = $b.GetPixel($x, $y)
  if ($c.A -gt 40) {
    Write-Output ("{0,4} a={1,3} {2:x2}{3:x2}{4:x2}" -f $x, $c.A, $c.R, $c.G, $c.B)
  }
}
