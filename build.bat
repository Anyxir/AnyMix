@echo off
setlocal
set "ROOT=%~dp0"
set "GPP="
for /f "delims=" %%I in ('where g++ 2^>nul') do if not defined GPP set "GPP=%%I"
if not defined GPP (
  for /f "delims=" %%I in ('dir /s /b "%LOCALAPPDATA%\Microsoft\WinGet\Packages\g++.exe" 2^>nul') do if not defined GPP set "GPP=%%I"
)
if not defined GPP (
  echo g++ was not found. Install WinLibs and reopen the shell.
  exit /b 1
)
echo Using %GPP%
if not exist "%ROOT%build" mkdir "%ROOT%build"
"%GPP%" -std=c++20 -O2 -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -c "%ROOT%engine\src\Process.cpp" -o "%ROOT%build\Process.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -c "%ROOT%engine\src\Devices.cpp" -o "%ROOT%build\Devices.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -c "%ROOT%engine\src\Engine.cpp" -o "%ROOT%build\Engine.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -fno-devirtualize -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -c "%ROOT%engine\src\VstHost.cpp" -o "%ROOT%build\VstHost.o" || exit /b 1
"%GPP%" -std=c++20 -O2 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -municode "%ROOT%app\src\selftest.cpp" "%ROOT%app\src\testsupport.cpp" "%ROOT%app\src\test_gain.cpp" "%ROOT%app\src\test_engine.cpp" "%ROOT%app\src\test_uimath.cpp" "%ROOT%app\src\test_meter.cpp" "%ROOT%build\Process.o" "%ROOT%build\Devices.o" "%ROOT%build\Engine.o" "%ROOT%build\VstHost.o" -o "%ROOT%build\anymix_test.exe" -lole32 -luuid -lwinmm -lavrt || exit /b 1
for %%G in ("%GPP%") do set "MINGW=%%~dpG"
"%MINGW%windres.exe" -O coff -i "%ROOT%app\AnyMix.rc" -o "%ROOT%build\AnyMix.res" || exit /b 1
"%GPP%" -std=c++20 -O2 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%engine\include" -mwindows -municode "%ROOT%app\src\main.cpp" "%ROOT%build\Process.o" "%ROOT%build\Devices.o" "%ROOT%build\Engine.o" "%ROOT%build\VstHost.o" "%ROOT%build\AnyMix.res" -o "%ROOT%build\AnyMix.next.exe" -lole32 -luuid -lwinmm -lmfreadwrite -lmfplat -lmfuuid -lavrt -lcomctl32 -lcomdlg32 -loleaut32 -lshell32 -lgdiplus -ld2d1 -ldwrite -ldwmapi || exit /b 1
move /y "%ROOT%build\AnyMix.next.exe" "%ROOT%build\AnyMix.exe" >nul
if errorlevel 1 (
  if exist "%ROOT%build\AnyMix.old.exe" del /f /q "%ROOT%build\AnyMix.old.exe"
  ren "%ROOT%build\AnyMix.exe" AnyMix.old.exe
  move /y "%ROOT%build\AnyMix.next.exe" "%ROOT%build\AnyMix.exe" >nul || exit /b 1
  del /f /q "%ROOT%build\AnyMix.old.exe" 2>nul
)
"%GPP%" -std=c++20 -O2 -shared -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -I"%ROOT%engine\include" "%ROOT%asio\src\AsioInsert.cpp" -o "%ROOT%build\AnyMixInsertASIO.next.dll" -lole32 -luuid -lwinmm -lavrt "-Wl,--export-all-symbols" || exit /b 1
move /y "%ROOT%build\AnyMixInsertASIO.next.dll" "%ROOT%build\AnyMixInsertASIO.dll" >nul
if errorlevel 1 (
  del /f /q "%ROOT%build\AnyMixInsertASIO.next.dll" 2>nul
  echo ASIO DLL is in use. The previous DLL was left in place.
)
echo Build ok
exit /b 0
