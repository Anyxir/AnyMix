$objdump = "C:\Users\akram\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\objdump.exe"
& $objdump -p "C:\Users\akram\Downloads\vb\build\AnyMixInsertASIO.dll" | Select-String -Pattern "DllRegister|DllGet|DllCan"
