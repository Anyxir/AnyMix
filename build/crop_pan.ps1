Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile("C:\Users\akram\Downloads\vb\build\mixer_shot.png")
$crop = New-Object System.Drawing.Bitmap 900, 80
$g = [System.Drawing.Graphics]::FromImage($crop)
$src = New-Object System.Drawing.Rectangle 190, 55, 900, 80
$dst = New-Object System.Drawing.Rectangle 0, 0, 900, 80
$g.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
$crop.Save("C:\Users\akram\Downloads\vb\build\pan_crop.png")
$g.Dispose()
$crop.Dispose()
$bmp.Dispose()
Write-Output "pan"
