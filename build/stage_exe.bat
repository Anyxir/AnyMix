@echo off
setlocal
set "ROOT=%~dp0.."
set "GPP=C:\Users\akram\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
set "WINDRES=C:\Users\akram\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\windres.exe"
"%WINDRES%" "%ROOT%\app\manifest.rc" -O coff -o "%ROOT%\build\manifest.res" || exit /b 1
"%GPP%" -std=c++20 -O2 -DUNICODE -D_UNICODE -I"%ROOT%\engine\include" -c "%ROOT%\engine\src\Engine.cpp" -o "%ROOT%\build\Engine.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%\engine\include" -mwindows -municode "%ROOT%\app\src\main.cpp" "%ROOT%\build\Process.o" "%ROOT%\build\Devices.o" "%ROOT%\build\Engine.o" "%ROOT%\build\AnyMix.res" "%ROOT%\build\manifest.res" -o "%ROOT%\build\AnyMix.stage.exe" -lole32 -luuid -lwinmm -lmfreadwrite -lmfplat -lmfuuid -lavrt -lcomctl32 -lcomdlg32 -loleaut32 -lshell32 -lgdiplus -ld2d1 -ldwrite -ldwmapi || exit /b 1
echo compiled
exit /b 0
