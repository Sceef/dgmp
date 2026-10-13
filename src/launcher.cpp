#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;

static bool Run(std::wstring command, const fs::path& cwd, bool hidden, DWORD* exitCode = nullptr) {
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr, cwd.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    if (exitCode) { WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, exitCode); }
    CloseHandle(pi.hProcess); return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t exe[32768]; GetModuleFileNameW(nullptr, exe, 32768);
    const auto root = fs::path(exe).parent_path();
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool prepareOnly = false, refresh = false;
    for (int i = 1; i < argc; ++i) { prepareOnly |= std::wstring(argv[i]) == L"--prepare-only"; refresh |= std::wstring(argv[i]) == L"--refresh-seed"; }
    LocalFree(argv);
    fs::create_directories(root / L"logs");
    std::ofstream log(root / L"logs" / L"launcher.log", std::ios::app);
    DWORD code = 0;
    const auto script = root / L"prepare.ps1";
    const std::wstring setup = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"" + (refresh ? L" -RefreshSeed" : L"");
    if (!Run(setup, root, true, &code) || code != 0) {
        log << "Preparation failed, exit " << code << '\n';
        MessageBoxW(nullptr, L"DGMP preparation failed. Close Days Gone and run dgmp\\prepare.ps1 in PowerShell to see the reason.", L"DGMP", MB_ICONERROR); return 1;
    }
    if (prepareOnly) { log << "Preparation OK\n"; return 0; }
    SetEnvironmentVariableW(L"DGMP_ROOT", root.c_str());
    SetEnvironmentVariableW(L"SteamAppId", L"1259420");
    SetEnvironmentVariableW(L"SteamGameId", L"1259420");
    const auto game = root / L"runtime" / L"BendGame" / L"Binaries" / L"Win64" / L"DaysGone.exe";
    const std::wstring command = L"\"" + game.wstring() + L"\" -windowed -ResX=1600 -ResY=900 -NoSplash";
    if (!Run(command, game.parent_path(), false)) {
        log << "Game launch failed " << GetLastError() << '\n';
        MessageBoxW(nullptr, L"Cannot start Days Gone. Make sure Steam is running.", L"DGMP", MB_ICONERROR); return 2;
    }
    log << "Isolated game launched\n"; return 0;
}
