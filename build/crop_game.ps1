Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile("C:\Users\akram\Downloads\vb\build\mixer_shot.png")
$crop = New-Object System.Drawing.Bitmap 180, 70
$g = [System.Drawing.Graphics]::FromImage($crop)
$src = New-Object System.Drawing.Rectangle 430, 70, 180, 70
$dst = New-Object System.Drawing.Rectangle 0, 0, 180, 70
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
$crop.Save("C:\Users\akram\Downloads\vb\build\game_badge.png")
$g.Dispose()
$crop.Dispose()
$bmp.Dispose()
Write-Output "game"
