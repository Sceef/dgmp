#include <windows.h>
#include "profile.h"
void DGMPInitThread();
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Install file redirection before the game can read or write its normal profile.
        if (!profile::Install()) {
            MessageBoxW(nullptr,L"DGMP must be started through DGMPLauncher.exe. Profile isolation failed; the game will close.",L"DGMP",MB_ICONERROR);
            ExitProcess(21);
        }
        HANDLE t = CreateThread(nullptr,0,[](LPVOID)->DWORD { DGMPInitThread(); return 0; },nullptr,0,nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
