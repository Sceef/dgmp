@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\lua mkdir build\lua
if not exist build\enet mkdir build\enet
if not exist build\server mkdir build\server
if not exist build\probe mkdir build\probe
set CXXFLAGS=/nologo /std:c++20 /O2 /MT /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX

rem ---- third-party static libraries (rebuilt only when missing) ----
if not exist build\lua.lib (
  cl /nologo /O2 /MT /W0 /c /Fo"build\lua\\" third_party\lua\src\lapi.c third_party\lua\src\lcode.c third_party\lua\src\lctype.c third_party\lua\src\ldebug.c third_party\lua\src\ldo.c third_party\lua\src\ldump.c third_party\lua\src\lfunc.c third_party\lua\src\lgc.c third_party\lua\src\llex.c third_party\lua\src\lmem.c third_party\lua\src\lobject.c third_party\lua\src\lopcodes.c third_party\lua\src\lparser.c third_party\lua\src\lstate.c third_party\lua\src\lstring.c third_party\lua\src\ltable.c third_party\lua\src\ltm.c third_party\lua\src\lundump.c third_party\lua\src\lvm.c third_party\lua\src\lzio.c third_party\lua\src\lauxlib.c third_party\lua\src\lbaselib.c third_party\lua\src\lcorolib.c third_party\lua\src\ldblib.c third_party\lua\src\liolib.c third_party\lua\src\lmathlib.c third_party\lua\src\loadlib.c third_party\lua\src\loslib.c third_party\lua\src\lstrlib.c third_party\lua\src\ltablib.c third_party\lua\src\lutf8lib.c third_party\lua\src\linit.c || exit /b 1
  lib /nologo /OUT:build\lua.lib build\lua\*.obj || exit /b 1
)
if not exist build\enet.lib (
  cl /nologo /O2 /MT /W0 /c /Ithird_party\enet\include /Fo"build\enet\\" third_party\enet\callbacks.c third_party\enet\compress.c third_party\enet\host.c third_party\enet\list.c third_party\enet\packet.c third_party\enet\peer.c third_party\enet\protocol.c third_party\enet\win32.c || exit /b 1
  lib /nologo /OUT:build\enet.lib build\enet\*.obj || exit /b 1
)

if not exist build\imgui.lib (
  if not exist build\imgui mkdir build\imgui
  cl /nologo /std:c++20 /O2 /MT /W0 /EHsc /c /Ithird_party\imgui /Ithird_party\imguizmo /Fo"build\imgui\\" third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx11.cpp third_party\imgui\backends\imgui_impl_win32.cpp third_party\imguizmo\ImGuizmo.cpp || exit /b 1
  lib /nologo /OUT:build\imgui.lib build\imgui\*.obj || exit /b 1
)

rem ---- out-of-process development crash dump helper ----
cl %CXXFLAGS% src\tools\crashdump.cpp /Fo"build\crashdump.obj" /Fe"build\dgmp-crashdump.exe" /link dbghelp.lib || exit /b 1

rem ---- game client DLL (version.dll proxy) ----
if not exist build\dll mkdir build\dll
cl %CXXFLAGS% /Ithird_party\enet\include /Ithird_party\imgui /Ithird_party\imguizmo /Ithird_party\webview2\build\native\include /Ithird_party\lua\src /Fo"build\dll\\" /Zi /Fd"build\dll\client.pdb" /DDGMP_BUILD src\client.cpp src\diagnostics.cpp src\policy.cpp src\environment.cpp src\devmenu.cpp src\dllmain.cpp src\profile.cpp src\panel.cpp src\ue.cpp src\proxy.cpp src\mp.cpp src\hud.cpp src\devcmd.cpp src\inventory.cpp src\gameevents.cpp src\overlay.cpp src\editorui.cpp src\worldedit.cpp src\skins.cpp src\mpui.cpp src\vehicles.cpp src\carry.cpp src\images.cpp src\audio.cpp src\steam.cpp src\objects.cpp src\playeranim.cpp src\blips.cpp src\sounds.cpp src\zombies.cpp src\webui.cpp src\voice.cpp src\clientlua.cpp src\traps.cpp src\throws.cpp src\shared\netclient.cpp /LD /Fe"build\version.dll" /link /DEF:src\version.def /MAP:build\version.map /DEBUG:FULL /PDB:build\version.pdb /OPT:REF /OPT:ICF build\enet.lib build\lua.lib ws2_32.lib winmm.lib user32.lib gdi32.lib gdiplus.lib build\imgui.lib d3d11.lib dxgi.lib windowscodecs.lib ole32.lib third_party\webview2\build\native\x64\WebView2LoaderStatic.lib windowsapp.lib CoreMessaging.lib shlwapi.lib advapi32.lib shell32.lib || exit /b 1

rem ---- dedicated server ----
cl %CXXFLAGS% /Ithird_party\enet\include /Ithird_party\lua\src /Fo"build\server\\" src\server\main.cpp src\server\server.cpp src\server\script.cpp src\server\platform.cpp src\server\security.cpp src\server\master.cpp src\server\voice.cpp src\server\zombies.cpp src\server\traps.cpp /Zi /Fd"build\server\\" /Fe"dgmp-server.exe" /link /DEBUG /OPT:REF /OPT:ICF build\lua.lib build\enet.lib ws2_32.lib winmm.lib dbghelp.lib winhttp.lib || exit /b 1

rem ---- test client ----
cl %CXXFLAGS% /Ithird_party\enet\include /Fo"build\probe\\" src\tools\probe.cpp src\shared\netclient.cpp /Fe"build\dgmp-probe.exe" /link build\enet.lib ws2_32.lib winmm.lib shell32.lib || exit /b 1

rem ---- player launcher (WebView2 UI in launcher\ui) ----
if not exist build\launcher mkdir build\launcher
cl %CXXFLAGS% /Ithird_party\enet\include /Ithird_party\webview2\build\native\include /Fo"build\launcher\\" src\launcher\main.cpp src\shared\netclient.cpp src\server\platform.cpp /Fe"DGMP.exe" /link /SUBSYSTEM:WINDOWS build\enet.lib third_party\webview2\build\native\x64\WebView2LoaderStatic.lib ws2_32.lib winmm.lib user32.lib shell32.lib ole32.lib advapi32.lib iphlpapi.lib dwmapi.lib version.lib gdi32.lib winhttp.lib || exit /b 1
echo DGMP BUILD OK
