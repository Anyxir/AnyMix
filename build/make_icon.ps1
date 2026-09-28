Add-Type -AssemblyName System.Drawing
$png = "C:\Users\akram\Downloads\vb\public\logo.png"
$ico = "C:\Users\akram\Downloads\vb\app\logo.ico"
$src = [System.Drawing.Image]::FromFile($png)
$sizes = @(16, 32, 48, 256)
$images = New-Object System.Collections.Generic.List[byte[]]
foreach ($s in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($src, 0, 0, $s, $s)
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $images.Add($ms.ToArray())
  $bmp.Dispose()
  $ms.Dispose()
}
$src.Dispose()
$fs = [System.IO.File]::Open($ico, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
for ($i = 0; $i -lt $images.Count; $i++) {
  $s = $sizes[$i]
  $dim = [byte]$(if ($s -ge 256) { 0 } else { $s })
  $bw.Write($dim)
  $bw.Write($dim)
  $bw.Write([byte]0)
  $bw.Write([byte]0)
  $bw.Write([uint16]1)
  $bw.Write([uint16]32)
  $bw.Write([uint32]$images[$i].Length)
  $bw.Write([uint32]$offset)
  $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Close()
$fs.Close()
Write-Output "icon $((Get-Item $ico).Length) bytes"
