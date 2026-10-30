#include "diagnostics.h"
#include "diagnostics_protocol.h"
#include "log.h"
#include <psapi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <iterator>

namespace diagnostics {
namespace {
std::atomic<bool> enabled{false};
thread_local const char* phase = "native/background";
HANDLE journal = INVALID_HANDLE_VALUE, request = nullptr, complete = nullptr;
std::atomic_flag busy = ATOMIC_FLAG_INIT;
std::filesystem::path directory;
std::filesystem::path sourceRoot;
EXCEPTION_RECORD record{};
CONTEXT context{};
EXCEPTION_POINTERS pointers{&record, &context};
DumpRequest* shared = nullptr;
HANDLE helperProcess = nullptr;
DWORD faultThread = 0;
unsigned sequence = 0;
void Line(const char* text) {
    if (journal == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(journal, text, DWORD(strlen(text)), &written, nullptr);
    FlushFileBuffers(journal);
}
LONG CALLBACK Exception(EXCEPTION_POINTERS* info) {
    if (!enabled || !shared || !helperProcess || !info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (busy.test_and_set()) return EXCEPTION_CONTINUE_SEARCH;
    // First-chance observations include recoverable engine probes. Label them explicitly,
    // bound overhead, and leave all exception handling to the game/OS.
    static void* seen[8]{};
    bool duplicate = false;
    for (unsigned i = 0; i < sequence && i < 8; ++i) if (seen[i] == info->ExceptionRecord->ExceptionAddress) duplicate = true;
    if (duplicate || sequence >= 8) { busy.clear(); return EXCEPTION_CONTINUE_SEARCH; }
    seen[sequence] = info->ExceptionRecord->ExceptionAddress;
    ++sequence;
    record = *info->ExceptionRecord; record.ExceptionRecord = nullptr;
    context = *info->ContextRecord; faultThread = GetCurrentThreadId();
    char line[512];
    snprintf(line, sizeof(line), "FIRST-CHANCE #%u tick=%llu thread=%lu phase=%s code=%08lX pc=%p access=%llu target=%llX rsp=%llX rbx=%llX rcx=%llX\r\n",
        sequence, GetTickCount64(), faultThread, phase, code, record.ExceptionAddress,
        record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0ull, record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0ull,
        context.Rsp, context.Rbx, context.Rcx);
    Line(line);
    shared->thread=faultThread; shared->sequence=sequence; shared->result=0; shared->error=0;
    ResetEvent(complete); SetEvent(request);
    // A separate process avoids loader/heap locks held by the failing game. If it hangs,
    // keep the capture buffer reserved rather than letting another fault overwrite it.
    HANDLE wait[]{complete,helperProcess};
    const DWORD outcome=WaitForMultipleObjects(2,wait,FALSE,5000);
    snprintf(line,sizeof(line),"dump=%03u success=%lu error=%lu wait=%lu\r\n",sequence,shared->result,shared->error,outcome); Line(line);
    if (outcome == WAIT_OBJECT_0 || outcome == WAIT_OBJECT_0+1) busy.clear();
    return EXCEPTION_CONTINUE_SEARCH;
}
}
bool Enabled() { return enabled; }
void SetEnabled(bool value) { enabled = value; dg::Log("Development diagnostics %s", value ? "enabled" : "disabled"); }
void Phase(const char* literal) { phase = literal; }
void Event(const char* format, ...) {
    if (!enabled) return;
    char text[768]; va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    dg::Log("DEV tid=%lu phase=%s %s", GetCurrentThreadId(), phase, text);
}
void Object(const char* action, const void* address, int index, int serial) { Event("object %s ptr=%p index=%d serial=%d", action, address, index, serial); }
void Heartbeat(double now, int objects, float x, float y, float z) {
    static double next = 0;
    if (!enabled || now < next) return; next = now + 10;
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); DWORD handles = 0;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    Event("heartbeat objects=%d privateMB=%llu workingMB=%llu handles=%lu pos=%.1f,%.1f,%.1f", objects,
        memory.PrivateUsage / (1024ull * 1024), memory.WorkingSetSize / (1024ull * 1024), handles, x, y, z);
}
void Start(const std::filesystem::path& root, bool value) {
    sourceRoot = root;
    SYSTEMTIME time; GetLocalTime(&time);
    wchar_t name[96]; swprintf_s(name, L"%04u%02u%02u-%02u%02u%02u-%lu", time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
    directory = root / L"logs" / L"diagnostics" / name;
    std::error_code ec; std::filesystem::create_directories(directory, ec);
    if (ec) { dg::Log("Cannot create diagnostics directory: %d", ec.value()); return; }
    journal = CreateFileW((directory / L"exceptions.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    // Keep the exact loaded build, so a later rebuild cannot destroy crash symbols.
    HMODULE self = nullptr; wchar_t path[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Start), &self);
    if (self && GetModuleFileNameW(self, path, MAX_PATH)) CopyFileW(path, (directory / L"version.dll").c_str(), TRUE);
    const auto build = root / L"build" / L"version.dll";
    // Map/PDB are archived only when their adjacent DLL is byte-identical to the loaded file.
    auto equal = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        std::ifstream x(a,std::ios::binary), y(b,std::ios::binary);
        return x && y && std::equal(std::istreambuf_iterator<char>(x),std::istreambuf_iterator<char>(),std::istreambuf_iterator<char>(y),std::istreambuf_iterator<char>());
    };
    if (path[0] && equal(path, build)) for (const wchar_t* symbol : {L"version.map",L"version.pdb"}) CopyFileW((root/L"build"/symbol).c_str(),(directory/symbol).c_str(),TRUE);
    const std::wstring ipc=L"Local\\DGMP.Diagnostics."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(DumpRequest),ipc.c_str());
    shared=mapping?static_cast<DumpRequest*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(DumpRequest))):nullptr;
    request=CreateEventW(nullptr,FALSE,FALSE,(ipc+L"-request").c_str());
    complete=CreateEventW(nullptr,TRUE,FALSE,(ipc+L"-complete").c_str());
    if(shared && request && complete && directory.wstring().size()<MAX_PATH) {
        *shared=DumpRequest{}; shared->process=GetCurrentProcessId(); shared->exceptionPointers=reinterpret_cast<uintptr_t>(&pointers);
        wcscpy_s(shared->directory,directory.c_str());
        if(root.wstring().size()<MAX_PATH) wcscpy_s(shared->root,root.c_str());
        auto helper=root/L"build"/L"dgmp-crashdump.exe";
        if(!std::filesystem::exists(helper)) helper=std::filesystem::path(path).parent_path()/L"dgmp-crashdump.exe";
        std::wstring command=L"\""+helper.wstring()+L"\" \""+ipc+L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if(CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,helper.parent_path().c_str(),&startup,&process)) {
            helperProcess=process.hProcess; CloseHandle(process.hThread); AddVectoredExceptionHandler(1,&Exception);
        } else dg::Log("Crash dump helper unavailable, error=%lu; development logs remain enabled",GetLastError());
    }
    enabled = value;
    dg::Log("Development diagnostics=%d folder=%ls (first-chance dumps; maximum 8 per session)", value, directory.c_str());
}
}
