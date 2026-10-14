// dgmp-server.exe — standalone DGMP game server (no game client required).
//   dgmp-server.exe [--config server.ini] [--name "My server"] [--mode freeroam] [--port 7777]
//                   [--max 16] [--password secret] [--bind 0.0.0.0]
#include "server.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <string>
#include <thread>

namespace {
srv::Server* g_server = nullptr;

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Ini(const std::filesystem::path& file, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"Server", key, fallback, buf, 512, file.c_str());
    return buf;
}

// Writes the fault and a symbolised stack to the server log (needs dgmp-server.pdb next to the exe).
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    if (!g_server) return EXCEPTION_CONTINUE_SEARCH;
    const auto process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    g_server->Log("CRASH: exception 0x%08lX at %p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip; frame.AddrFrame.Offset = ctx.Rbp; frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto sym = reinterpret_cast<SYMBOL_INFO*>(buf); sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
        DWORD64 disp = 0; DWORD lineDisp = 0; IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
        const bool hasSym = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym);
        const bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line);
        g_server->Log("  #%d %s %s:%lu", i, hasSym ? sym->Name : "?", hasLine ? line.FileName : "", hasLine ? line.LineNumber : 0);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI OnCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_server) g_server->PushConsole("quit");
        if (type == CTRL_CLOSE_EVENT) Sleep(2000);  // let the server notify clients before the console closes
        return TRUE;
    }
    return FALSE;
}

// Reads console lines as UTF-16 so Cyrillic text in `say` survives; falls back to a pipe.
void ConsoleThread() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    const bool console = GetConsoleMode(in, &mode);
    std::wstring line;
    for (;;) {
        if (console) {
            wchar_t buf[512]; DWORD read = 0;
            if (!ReadConsoleW(in, buf, 511, &read, nullptr) || read == 0) return;
            for (DWORD i = 0; i < read; ++i) {
                if (buf[i] == L'\n') { if (g_server) g_server->PushConsole(Utf8(line)); line.clear(); }
                else if (buf[i] != L'\r') line += buf[i];
            }
        } else {
            char buf[1024];
            if (!std::fgets(buf, sizeof(buf), stdin)) return;
            if (g_server) g_server->PushConsole(buf);
        }
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto root = std::filesystem::path(exe).parent_path();

    auto ini = root / L"server.ini";
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == L"--config") ini = std::filesystem::absolute(argv[i + 1]);

    srv::Config cfg;
    cfg.name = Utf8(Ini(ini, L"Name", L"DGMP Server"));
    cfg.mode = Utf8(Ini(ini, L"Mode", L"freeroam"));
    cfg.password = Utf8(Ini(ini, L"Password", L""));
    cfg.bind = Utf8(Ini(ini, L"Bind", L""));
    cfg.port = static_cast<uint16_t>(_wtoi(Ini(ini, L"Port", L"7777").c_str()));
    cfg.maxPlayers = _wtoi(Ini(ini, L"MaxPlayers", L"16").c_str());
    const auto tags = Utf8(Ini(ini, L"Nametags", L"visible"));
    cfg.nametags = tags == "walls" ? net::NametagMode::ThroughWalls : tags == "hidden" ? net::NametagMode::Hidden : net::NametagMode::Visible;
    for (std::string list = Utf8(Ini(ini, L"Admins", L"")), item; !list.empty();) {
        const auto comma = list.find(','); item = list.substr(0, comma);
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) cfg.admins.push_back(item);
        list = comma == std::string::npos ? "" : list.substr(comma + 1);
    }
    if (auto x = Ini(ini, L"SpawnX", L""); !x.empty()) {
        cfg.spawnX = static_cast<float>(_wtof(x.c_str()));
        cfg.spawnY = static_cast<float>(_wtof(Ini(ini, L"SpawnY", L"0").c_str()));
        cfg.spawnZ = static_cast<float>(_wtof(Ini(ini, L"SpawnZ", L"0").c_str()));
        cfg.spawnYaw = static_cast<float>(_wtof(Ini(ini, L"SpawnYaw", L"0").c_str()));
    }
    for (int i = 1; i + 1 < argc; ++i) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--name") cfg.name = Utf8(v);
        else if (k == L"--mode") cfg.mode = Utf8(v);
        else if (k == L"--port") cfg.port = static_cast<uint16_t>(_wtoi(v.c_str()));
        else if (k == L"--max") cfg.maxPlayers = _wtoi(v.c_str());
        else if (k == L"--password") cfg.password = Utf8(v);
        else if (k == L"--bind") cfg.bind = Utf8(v);
        else continue;
        ++i;
    }
    if (cfg.port == 0) cfg.port = net::kDefaultPort;

    SetConsoleTitleW((L"DGMP Server - " + std::to_wstring(cfg.port)).c_str());
    srv::Server server;
    g_server = &server;
    SetConsoleCtrlHandler(&OnCtrl, TRUE);
    SetUnhandledExceptionFilter(&OnCrash);
    if (!server.Start(cfg, root)) { g_server = nullptr; return 1; }
    server.Log("Type help for console commands");
    std::thread(ConsoleThread).detach();
    server.Run();
    g_server = nullptr;
    return 0;
}
