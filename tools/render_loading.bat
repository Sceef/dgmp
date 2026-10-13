@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"build\render_loading.obj" tools\render_loading.cpp /Fe"build\render_loading.exe" /link user32.lib gdi32.lib gdiplus.lib || exit /b 1
build\render_loading.exe
