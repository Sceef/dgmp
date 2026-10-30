@echo off
setlocal
where cl >nul 2>nul || (echo Run from an x64 Native Tools Command Prompt for Visual Studio. & exit /b 1)
cd /d "%~dp0.."
if not exist build\editor-tests mkdir build\editor-tests
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tests\editor_storage.cpp /Fo"build\editor-tests\storage.obj" /Fe"build\editor-storage-test.exe" || exit /b 1
build\editor-storage-test.exe || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX src\tools\crashdump.cpp /Fo"build\editor-tests\crashdump.obj" /Fe"build\dgmp-crashdump.exe" /link dbghelp.lib || exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tests\editor_diagnostics.cpp src\diagnostics.cpp /Fo"build\editor-tests\\" /Fe"build\diagnostics-test.exe" /link psapi.lib || exit /b 1
build\diagnostics-test.exe || exit /b 1

