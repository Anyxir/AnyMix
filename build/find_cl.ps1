$root = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
Get-ChildItem -Path $root -Filter cl.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
