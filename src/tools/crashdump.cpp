// Out-of-process minidump writer: the failing game can hold loader/heap locks.
#include "../diagnostics_protocol.h"
#include <dbghelp.h>
#include <string>
#include <cstdio>
#include <filesystem>
int wmain(int argc, wchar_t** argv) {
    if(argc!=2) return 2;
    // This executable ships beside a version.dll proxy. DbgHelp delay-loads version.dll;
    // pin the OS copy first and forbid application-directory dependency resolution.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!LoadLibraryExW(L"version.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) return 6;
    HMODULE library=LoadLibraryExW(L"dbghelp.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    using DumpFn=BOOL(WINAPI*)(HANDLE,DWORD,HANDLE,MINIDUMP_TYPE,const MINIDUMP_EXCEPTION_INFORMATION*,const MINIDUMP_USER_STREAM_INFORMATION*,const MINIDUMP_CALLBACK_INFORMATION*);
    const auto writeDump=library?reinterpret_cast<DumpFn>(GetProcAddress(library,"MiniDumpWriteDump")):nullptr;
    if(!writeDump) return 7;
    const std::wstring name=argv[1];
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name.c_str());
    if(!mapping) return 3;
    auto data=static_cast<diagnostics::DumpRequest*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(diagnostics::DumpRequest)));
    if(!data || data->version!=1 || !data->process || !data->exceptionPointers) return 4;
    HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,data->process);
    HANDLE request=OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,(name+L"-request").c_str());
    HANDLE complete=OpenEventW(EVENT_MODIFY_STATE,FALSE,(name+L"-complete").c_str());
    if(!process || !request || !complete) return 5;
    HANDLE wait[]{process,request};
    while(WaitForMultipleObjects(2,wait,FALSE,INFINITE)==WAIT_OBJECT_0+1) {
        wchar_t fileName[MAX_PATH]; swprintf_s(fileName,L"%s\\exception-%03u.dmp",data->directory,data->sequence);
        HANDLE file=CreateFileW(fileName,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        data->result=0; data->error=GetLastError();
        if(file!=INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{data->thread,reinterpret_cast<EXCEPTION_POINTERS*>(data->exceptionPointers),TRUE};
            const auto flags=MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory|MiniDumpWithUnloadedModules|MiniDumpWithThreadInfo|MiniDumpWithProcessThreadData);
            data->result=writeDump(process,data->process,file,flags,&info,nullptr,nullptr);
            data->error=data->result?0:GetLastError();
            FlushFileBuffers(file); CloseHandle(file);
        }
        if(data->root[0]) {
            namespace fs=std::filesystem;
            wchar_t artifact[64]; swprintf_s(artifact,L"exception-%03u.log",data->sequence);
            CopyFileW((fs::path(data->root)/L"logs"/L"DGMP.log").c_str(),(fs::path(data->directory)/artifact).c_str(),TRUE);
            swprintf_s(artifact,L"exception-%03u-telemetry.json",data->sequence);
            CopyFileW((fs::path(data->root)/L"telemetry.json").c_str(),(fs::path(data->directory)/artifact).c_str(),TRUE);
        }
        SetEvent(complete);
    }
    CloseHandle(complete); CloseHandle(request); CloseHandle(process);
    UnmapViewOfFile(data); CloseHandle(mapping);
    return 0;
}
