#include "profile.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>

namespace profile {
namespace {
std::wstring root, saved, original, originalLower;
std::wstring savedLower;
std::atomic<unsigned int> blockedSaveWrites{0};
int patched = 0;
std::wstring Lower(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return value;
}
std::wstring Map(LPCWSTR path) {
    if (!path || originalLower.empty()) return {};
    const std::wstring value(path), lower = Lower(value);
    if (lower.compare(0, originalLower.size(), originalLower) || (lower.size() > originalLower.size() && lower[originalLower.size()] != L'\\')) return {};
    return saved + value.substr(originalLower.size());
}
bool IsGameSave(LPCWSTR path) {
    if(!path || savedLower.empty()) return false;
    wchar_t absolute[32768]{};
    const DWORD length=GetFullPathNameW(path,32768,absolute,nullptr);
    const auto value=Lower(length && length<32768?absolute:path);
    if(value.compare(0,savedLower.size(),savedLower) || value.size()<=savedLower.size() || value[savedLower.size()]!=L'\\') return false;
    const auto relative=value.substr(savedLower.size());
    // The engine synchronously creates its menu-scene snapshot on startup.
    // Rejecting it causes an endless native retry loop before LoadGame runs.
    // It is not player progress and stays inside the isolated DGMP profile.
    const std::wstring menuCache=L"\\savegames\\mainmenu.sav";
    if(relative.size()>=menuCache.size() && relative.compare(relative.size()-menuCache.size(),menuCache.size(),menuCache)==0) return false;
    // Includes temporary save files, so native atomic replace cannot bypass this.
    return relative.find(L"\\savegames\\")!=std::wstring::npos || (relative.size()>=4 && relative.compare(relative.size()-4,4,L".sav")==0);
}
bool RejectSave(LPCWSTR path) {
    if(!IsGameSave(path)) return false;
    const auto count=blockedSaveWrites.fetch_add(1)+1;
    if(count<=8) dg::Log("Native save file mutation blocked (%u): %ls",count,path);
    SetLastError(ERROR_ACCESS_DENIED); return true;
}
#define DECLARE(name) decltype(&::name) o##name = nullptr
DECLARE(CreateFileW); DECLARE(DeleteFileW); DECLARE(FindFirstFileW); DECLARE(FindFirstFileExW);
DECLARE(GetFileAttributesW); DECLARE(GetFileAttributesExW); DECLARE(SetFileAttributesW);
DECLARE(MoveFileW); DECLARE(MoveFileExW); DECLARE(CopyFileW); DECLARE(CreateDirectoryW); DECLARE(RemoveDirectoryW);
#define PATH_ORIG(p,m) ((m).empty() ? (p) : (m).c_str())
HANDLE WINAPI hCreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    auto m=Map(p); const auto target=PATH_ORIG(p,m);
    constexpr DWORD writes=GENERIC_WRITE|GENERIC_ALL|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER;
    if(((a&writes) || c!=OPEN_EXISTING || (f&FILE_FLAG_DELETE_ON_CLOSE)) && RejectSave(target)) {
        if(blockedSaveWrites.load()<=8) dg::Log("  CreateFile access=%08lx disposition=%lu flags=%08lx",a,c,f);
        return INVALID_HANDLE_VALUE;
    }
    return oCreateFileW(target,a,s,sa,c,f,t);
}
BOOL WINAPI hDeleteFileW(LPCWSTR p) { auto m=Map(p); const auto target=PATH_ORIG(p,m); if(RejectSave(target)) return FALSE; return oDeleteFileW(target); }
HANDLE WINAPI hFindFirstFileW(LPCWSTR p, LPWIN32_FIND_DATAW d) { auto m=Map(p); return oFindFirstFileW(PATH_ORIG(p,m),d); }
HANDLE WINAPI hFindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS s, LPVOID f, DWORD flags) { auto m=Map(p); return oFindFirstFileExW(PATH_ORIG(p,m),l,d,s,f,flags); }
DWORD WINAPI hGetFileAttributesW(LPCWSTR p) { auto m=Map(p); return oGetFileAttributesW(PATH_ORIG(p,m)); }
BOOL WINAPI hGetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID d) { auto m=Map(p); return oGetFileAttributesExW(PATH_ORIG(p,m),l,d); }
BOOL WINAPI hSetFileAttributesW(LPCWSTR p, DWORD a) { auto m=Map(p); const auto target=PATH_ORIG(p,m); if(RejectSave(target)) return FALSE; return oSetFileAttributesW(target,a); }
BOOL WINAPI hCreateDirectoryW(LPCWSTR p, LPSECURITY_ATTRIBUTES sa) { auto m=Map(p); return oCreateDirectoryW(PATH_ORIG(p,m),sa); }
BOOL WINAPI hRemoveDirectoryW(LPCWSTR p) { auto m=Map(p); return oRemoveDirectoryW(PATH_ORIG(p,m)); }
BOOL WINAPI hMoveFileW(LPCWSTR a,LPCWSTR b) { auto ma=Map(a),mb=Map(b); if(RejectSave(PATH_ORIG(a,ma)) || RejectSave(PATH_ORIG(b,mb))) return FALSE; return oMoveFileW(PATH_ORIG(a,ma),PATH_ORIG(b,mb)); }
BOOL WINAPI hMoveFileExW(LPCWSTR a,LPCWSTR b,DWORD f) { auto ma=Map(a),mb=Map(b); if(RejectSave(PATH_ORIG(a,ma)) || RejectSave(PATH_ORIG(b,mb))) return FALSE; return oMoveFileExW(PATH_ORIG(a,ma),PATH_ORIG(b,mb),f); }
BOOL WINAPI hCopyFileW(LPCWSTR a,LPCWSTR b,BOOL f) { auto ma=Map(a),mb=Map(b); if(RejectSave(PATH_ORIG(b,mb))) return FALSE; return oCopyFileW(PATH_ORIG(a,ma),PATH_ORIG(b,mb),f); }

bool Patch(const char* name, void* hook, void** orig) {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto funcs = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(ibn->Name,name)) continue;
            DWORD old;
            if (!VirtualProtect(&funcs->u1.Function,sizeof(void*),PAGE_READWRITE,&old)) return false;
            *orig = reinterpret_cast<void*>(funcs->u1.Function);
            funcs->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&funcs->u1.Function,sizeof(void*),old,&old); return true;
        }
    } return false;
}
}
bool Install() {
    wchar_t buf[32768]{};
    if (!GetEnvironmentVariableW(L"DGMP_ROOT",buf,32768)) return false;
    root = buf;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA",buf,32768)) return false;
    original = std::wstring(buf) + L"\\BendGame\\Saved";
    originalLower = Lower(original);
    saved = root + L"\\profiles\\editor\\BendGame\\Saved";
    savedLower = Lower(saved);
#define PATCH(name) patched += Patch(#name,reinterpret_cast<void*>(&h##name),reinterpret_cast<void**>(&o##name))
    PATCH(CreateFileW); PATCH(DeleteFileW); PATCH(FindFirstFileW); PATCH(FindFirstFileExW);
    PATCH(GetFileAttributesW); PATCH(GetFileAttributesExW); PATCH(SetFileAttributesW);
    PATCH(MoveFileW); PATCH(MoveFileExW); PATCH(CopyFileW); PATCH(CreateDirectoryW); PATCH(RemoveDirectoryW);
    return oCreateFileW && oFindFirstFileW && oGetFileAttributesW;
}
const std::wstring& Root() { return root; }
const std::wstring& Saved() { return saved; }
int PatchedCount() { return patched; }
unsigned int BlockedSaveWrites() { return blockedSaveWrites.load(); }
}
