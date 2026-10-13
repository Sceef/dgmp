// Forwarding stubs for the real System32\version.dll.
// The game imports VERSION.dll, so dropping our build next to DaysGone.exe gets us loaded.
#include <windows.h>

static HMODULE RealVersion()
{
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

template <typename F>
static F Real(const char* name)
{
    return reinterpret_cast<F>(GetProcAddress(RealVersion(), name));
}

#define FWD(ret, name, params, args)                                         \
    extern "C" ret WINAPI Proxy_##name params                                 \
    {                                                                         \
        static auto fn = Real<ret(WINAPI*) params>(#name);                    \
        return fn args;                                                       \
    }

FWD(BOOL, GetFileVersionInfoA, (LPCSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
FWD(BOOL, GetFileVersionInfoW, (LPCWSTR f, DWORD h, DWORD l, LPVOID d), (f, h, l, d))
FWD(BOOL, GetFileVersionInfoByHandle, (DWORD fl, HANDLE f, LPVOID* d, LPDWORD l), (fl, f, d, l))
FWD(BOOL, GetFileVersionInfoExA, (DWORD fl, LPCSTR f, DWORD h, DWORD l, LPVOID d), (fl, f, h, l, d))
FWD(BOOL, GetFileVersionInfoExW, (DWORD fl, LPCWSTR f, DWORD h, DWORD l, LPVOID d), (fl, f, h, l, d))
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR f, LPDWORD h), (f, h))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR f, LPDWORD h), (f, h))
FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h))
FWD(DWORD, VerFindFileA, (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h), (a, b, c, d, e, f, g, h))
FWD(DWORD, VerFindFileW, (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h), (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileA, (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h), (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileW, (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h), (a, b, c, d, e, f, g, h))
FWD(DWORD, VerLanguageNameA, (DWORD w, LPSTR s, DWORD n), (w, s, n))
FWD(DWORD, VerLanguageNameW, (DWORD w, LPWSTR s, DWORD n), (w, s, n))
FWD(BOOL, VerQueryValueA, (LPCVOID b, LPCSTR s, LPVOID* p, PUINT l), (b, s, p, l))
FWD(BOOL, VerQueryValueW, (LPCVOID b, LPCWSTR s, LPVOID* p, PUINT l), (b, s, p, l))
