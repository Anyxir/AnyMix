@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /O2 /Fe:C:\Users\akram\Downloads\vb\build\asio_host.exe C:\Users\akram\Downloads\vb\build\asio_host.cpp ole32.lib uuid.lib user32.lib
if errorlevel 1 exit /b 1
C:\Users\akram\Downloads\vb\build\asio_host.exe
