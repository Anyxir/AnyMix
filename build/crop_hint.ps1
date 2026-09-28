Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile("C:\Users\akram\Downloads\vb\build\mixer_shot.png")
$crop = New-Object System.Drawing.Bitmap 900, 40
$g = [System.Drawing.Graphics]::FromImage($crop)
$src = New-Object System.Drawing.Rectangle 180, 750, 900, 40
$dst = New-Object System.Drawing.Rectangle 0, 0, 900, 40
$g.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
$crop.Save("C:\Users\akram\Downloads\vb\build\hint_crop.png")
$g.Dispose()
$crop.Dispose()
$bmp.Dispose()
Write-Output "hint"
