#pragma once
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <string>
#include <windows.h>

namespace dg {

inline FILE*& LogFile()
{
    static FILE* f = nullptr;
    return f;
}

inline void LogOpen(const char* path)
{
    LogFile() = _fsopen(path, "w", _SH_DENYNO); // readable while the game runs
}

// Last logged message, shown in the debug window.
inline std::string& LastLine()
{
    static std::string s;
    return s;
}

inline void Log(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LastLine() = msg;
    FILE* f = LogFile();
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    fflush(f);
}

} // namespace dg
