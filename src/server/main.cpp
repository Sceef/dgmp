// dgmp-server — standalone DGMP game server (no game client required): dgmp-server.exe on Windows,
// dgmp-server on Linux.
//   dgmp-server [--config server.ini] [--name "My server"] [--mode freeroam] [--port 7777]
//               [--max 16] [--password secret] [--bind 0.0.0.0]
#include "server.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <cstring>
#if __has_include(<execinfo.h>)  // glibc; musl (Alpine) has no backtrace()
#include <execinfo.h>
#define DGMP_BACKTRACE 1
#endif
#include <pthread.h>
#include <unistd.h>
#endif

namespace {
srv::Server* g_server = nullptr;

std::string Ini(const std::filesystem::path& file, const char* key, const char* fallback) { return srv::IniValue(file, "Server", key, fallback); }

#ifdef _WIN32
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
                if (buf[i] == L'\n') { if (g_server) g_server->PushConsole(srv::Utf8(line)); line.clear(); }
                else if (buf[i] != L'\r') line += buf[i];
            }
        } else {
            char buf[1024];
            if (!std::fgets(buf, sizeof(buf), stdin)) return;
            if (g_server) g_server->PushConsole(buf);
        }
    }
}
#else
// Writes the fault and the stack to the server log. Frames show as dgmp-server(+0x...): resolve them
// with `addr2line -f -C -e dgmp-server 0x...` (the default build keeps debug info).
void OnCrash(int sig, siginfo_t* info, void*) {
    if (g_server) {
        g_server->Log("CRASH: signal %d (%s) at %p", sig, strsignal(sig), info->si_addr);
#ifdef DGMP_BACKTRACE
        void* frames[24];
        const int n = backtrace(frames, 24);
        if (char** names = backtrace_symbols(frames, n)) for (int i = 0; i < n; ++i) g_server->Log("  #%d %s", i, names[i]);
#endif
    }
    raise(sig);  // SA_RESETHAND restored the default action: the process ends (with a core dump if enabled)
}

// Ctrl+C, kill and systemctl stop quit like the console command, so clients are told first.
void SignalThread(sigset_t set) {
    for (int sig = 0;;) if (sigwait(&set, &sig) == 0 && g_server) g_server->PushConsole("quit");
}

void InstallSignals() {
    static char altStack[64 * 1024];  // the crash handler still runs after a stack overflow
    stack_t ss{}; ss.ss_sp = altStack; ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction crash{};
    crash.sa_sigaction = &OnCrash; crash.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK; sigemptyset(&crash.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &crash, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);  // started in the background (&): reading stdin then fails instead of stopping the server
    sigset_t quit; sigemptyset(&quit);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaddset(&quit, sig);
    pthread_sigmask(SIG_BLOCK, &quit, nullptr);  // before any other thread starts: only SignalThread receives them
    std::thread(SignalThread, quit).detach();
}

// Lines from stdin (a terminal, tmux/screen or a pipe), UTF-8 as they are.
void ConsoleThread() {
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), stdin)) if (g_server) g_server->PushConsole(buf);
}
#endif

// Settings from server.ini, then the command line (UTF-8 on both systems).
int Run(const std::vector<std::string>& args, const std::filesystem::path& root) {
    auto ini = root / "server.ini";
    for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == "--config") ini = std::filesystem::absolute(srv::Utf8Path(args[i + 1]));

    srv::Config cfg;
    cfg.name = Ini(ini, "Name", "DGMP Server");
    cfg.mode = Ini(ini, "Mode", "freeroam");
    cfg.password = Ini(ini, "Password", "");
    cfg.bind = Ini(ini, "Bind", "");
    cfg.port = static_cast<uint16_t>(std::atoi(Ini(ini, "Port", "7777").c_str()));
    cfg.maxPlayers = std::atoi(Ini(ini, "MaxPlayers", "16").c_str());
    const auto tags = Ini(ini, "Nametags", "visible");
    cfg.nametags = tags == "walls" ? net::NametagMode::ThroughWalls : tags == "hidden" ? net::NametagMode::Hidden : net::NametagMode::Visible;
    cfg.saveWeapons = Ini(ini, "StartWeapons", "none") == "save";
    const auto coll = Ini(ini, "Collisions", "off");
    cfg.collisions = coll == "all" ? net::CollidePlayers | net::CollideBikes : coll == "players" ? net::CollidePlayers : coll == "bikes" ? net::CollideBikes : 0;
    for (std::string list = Ini(ini, "Admins", ""), item; !list.empty();) {
        const auto comma = list.find(','); item = list.substr(0, comma);
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) cfg.admins.push_back(item);
        list = comma == std::string::npos ? "" : list.substr(comma + 1);
    }
    std::string auth = Ini(ini, "SteamAuth", "optional");
    for (auto& ch : auth) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    cfg.steamAuth = auth == "off" ? srv::Config::SteamAuth::Off : auth == "required" ? srv::Config::SteamAuth::Required : srv::Config::SteamAuth::Optional;
    cfg.steamApiKey = Ini(ini, "SteamApiKey", "");
    if (auto app = Ini(ini, "SteamAppId", ""); !app.empty()) cfg.steamAppId = static_cast<uint32_t>(std::strtoul(app.c_str(), nullptr, 10));
    cfg.reserveNicks = Ini(ini, "ReserveNicks", "1") != "0";
    cfg.localAdmin = Ini(ini, "LocalAdmin", "1") != "0";
    cfg.rconPassword = Ini(ini, "RconPassword", "");
    cfg.maxHitDamage = static_cast<float>(std::atof(Ini(ini, "MaxHitDamage", "500").c_str()));
    cfg.meleeRange = static_cast<float>(std::atof(Ini(ini, "MeleeRange", "6").c_str())) * 100.f;
    cfg.maxHitsPerSecond = std::max(1, std::atoi(Ini(ini, "MaxHitsPerSecond", "30").c_str()));
    cfg.cheatKick = std::atoi(Ini(ini, "CheatKick", "10").c_str());
    if (auto x = Ini(ini, "SpawnX", ""); !x.empty()) {
        cfg.spawnX = static_cast<float>(std::atof(x.c_str()));
        cfg.spawnY = static_cast<float>(std::atof(Ini(ini, "SpawnY", "0").c_str()));
        cfg.spawnZ = static_cast<float>(std::atof(Ini(ini, "SpawnZ", "0").c_str()));
        cfg.spawnYaw = static_cast<float>(std::atof(Ini(ini, "SpawnYaw", "0").c_str()));
    }
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string k = args[i], v = args[i + 1];
        if (k == "--name") cfg.name = v;
        else if (k == "--mode") cfg.mode = v;
        else if (k == "--port") cfg.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "--max") cfg.maxPlayers = std::atoi(v.c_str());
        else if (k == "--password") cfg.password = v;
        else if (k == "--bind") cfg.bind = v;
        else continue;
        ++i;
    }
    if (cfg.port == 0) cfg.port = net::kDefaultPort;

#ifdef _WIN32
    SetConsoleTitleW((L"DGMP Server - " + std::to_wstring(cfg.port)).c_str());
#else
    if (isatty(STDOUT_FILENO)) { std::printf("\033]0;DGMP Server - %u\007", cfg.port); std::fflush(stdout); }
#endif
    srv::Server server;
    g_server = &server;
#ifdef _WIN32
    SetConsoleCtrlHandler(&OnCtrl, TRUE);
    SetUnhandledExceptionFilter(&OnCrash);
#else
    InstallSignals();
#endif
    if (!server.Start(cfg, root)) { g_server = nullptr; return 1; }
    server.Log("Type help for console commands");
    std::thread(ConsoleThread).detach();
    server.Run();
    g_server = nullptr;
    return 0;
}
// The console thread sits in fgets on stdin; a normal exit flushes every stream and waits for stdin's
// lock forever (a server fed through a pipe never quit). The server is already shut down here.
int Exit(int code) {
    std::fflush(stdout); std::fflush(stderr);
    std::_Exit(code);
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(srv::Utf8(argv[i]));
    return Exit(Run(args, srv::ExecutableDir()));
}
#else
int main(int argc, char** argv) { return Exit(Run(std::vector<std::string>(argv, argv + argc), srv::ExecutableDir())); }
#endif
