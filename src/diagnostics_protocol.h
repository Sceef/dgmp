#pragma once
#include <windows.h>
#include <cstdint>
namespace diagnostics {
struct DumpRequest {
    uint32_t version = 1;
    DWORD process = 0, thread = 0;
    unsigned sequence = 0;
    uintptr_t exceptionPointers = 0; // address in the target process
    wchar_t directory[MAX_PATH]{};
    wchar_t root[MAX_PATH]{};
    DWORD result = 0, error = 0;
};
}
