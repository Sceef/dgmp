@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"build\\" src\client.cpp src\dllmain.cpp src\profile.cpp src\panel.cpp src\ue.cpp src\proxy.cpp /LD /Fe"build\version.dll" /link /DEF:src\version.def /MAP:build\version.map user32.lib gdi32.lib || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"build\\" src\launcher.cpp /Fe"DGMPLauncher.exe" /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib || exit /b 1
echo DGMP BUILD OK
