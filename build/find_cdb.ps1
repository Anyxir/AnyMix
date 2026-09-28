$kits = "C:\Program Files (x86)\Windows Kits\10\Debuggers"
Get-ChildItem $kits -Filter cdb.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
Get-ChildItem "C:\Program Files (x86)\Windows Kits\10" -Filter windbg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 5 -ExpandProperty FullName
