@echo off
setlocal
set "ROOT=%~dp0.."
set "GPP=C:\Users\akram\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
"%GPP%" -std=c++20 -O2 -DUNICODE -D_UNICODE -I"%ROOT%\engine\include" -c "%ROOT%\engine\src\Engine.cpp" -o "%ROOT%\build\Engine.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%\engine\include" -mwindows -municode "%ROOT%\app\src\main.cpp" "%ROOT%\build\Process.o" "%ROOT%\build\Devices.o" "%ROOT%\build\Engine.o" "%ROOT%\build\AnyMix.res" -o "%ROOT%\build\AnyMix.stage.exe" -lole32 -luuid -lwinmm -lavrt -lcomctl32 -loleaut32 -lshell32 -lgdiplus || exit /b 1
"%GPP%" -std=c++20 -O2 -shared -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%\engine\include" "%ROOT%\asio\src\AsioInsert.cpp" -o "%ROOT%\build\AnyMixInsertASIO.stage.dll" -lole32 -luuid -lwinmm -lavrt -Wl,--export-all-symbols || exit /b 1
echo compiled
exit /b 0
