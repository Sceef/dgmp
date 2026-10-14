#pragma once
#include <windows.h>
#include <cwchar>
// Language of the player-facing texts the DLL draws itself: DGMP_LANG from the launcher, English by default.
namespace lang {
inline bool English() {
    static const bool english = [] {
        wchar_t v[16] = L"";
        const DWORD n = GetEnvironmentVariableW(L"DGMP_LANG", v, 16);
        return !(n && n < 16 && _wcsnicmp(v, L"ru", 2) == 0);
    }();
    return english;
}
inline const wchar_t* Pick(const wchar_t* ru, const wchar_t* en) { return English() ? en : ru; }
inline const char* Pick(const char* ru, const char* en) { return English() ? en : ru; }
}
