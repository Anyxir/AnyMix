@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc "C:\Users\akram\Downloads\vb\build\asio_layout.cpp" /Fe"C:\Users\akram\Downloads\vb\build\asio_layout_msvc.exe"
"C:\Users\akram\Downloads\vb\build\asio_layout_msvc.exe"
