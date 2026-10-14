// DGMP.exe — player launcher: nickname, server browser (favorites / recent / LAN / direct),
// local server hosting with a live console, the quest editor (quests/*.json) and the editor entry point.
// While the editor game runs, a second window ("DGMP — Квесты", same page with ?view=quests) docks to the game,
// receives picks from editor_picks.jsonl and writes quest_preview.json for the game to draw.
// UI: launcher/ui/*.html in WebView2; this file is the native side of the bridge.
//   DGMP.exe [--editor] [--prepare-only] [--refresh-seed] [--dev]
#include "../shared/json.h"
#include "../shared/netclient.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <wrl.h>
#include "WebView2.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr UINT WM_TO_UI = WM_APP + 1;      // lParam: std::string* JSON for the page, wParam: view index + 1 (0 = all views)
constexpr UINT WM_GAME_EXIT = WM_APP + 2;
constexpr UINT WM_SERVER_EXIT = WM_APP + 3; // wParam: process id of the local server whose console closed
constexpr UINT WM_RUN = WM_APP + 4;         // lParam: std::function<void()>* to run on the UI thread
constexpr UINT_PTR kDockTimer = 1, kPickTimer = 2;  // quest window timers
constexpr int kMaxRecent = 15;
constexpr const char* kQuestTestMode = "questtest";
constexpr int kMainView = 0, kQuestView = 1, kAllViews = -1;

fs::path g_root;
HWND g_window = nullptr;
// [kMainView] = launcher window, [kQuestView] = quest editor window next to the editor game (hwnd null when closed).
struct View {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
} g_views[2];
ComPtr<ICoreWebView2Environment> g_env;
int g_from = kMainView;            // view whose message HandleMessage is processing (UI thread)
bool g_dev = false;
json::Value g_settings = json::Value::Object();
std::atomic<bool> g_english{true};  // UI language: settings "lang" ("en" default, "ru"); set by the page with setLang
std::atomic<bool> g_busy{false};
HANDLE g_game = nullptr;
std::atomic<DWORD> g_gamePid{0};
bool g_gameIsEditor = false;
DWORD g_gameServerPid = 0;         // local server the running game was sent to (0 = remote server or editor)
bool g_questTestStarting = false;  // "Проверить" is waiting for the test server to come up
std::function<void()> g_afterGameExit;  // runs once the game has exited (editor closed for a quest test)
std::ofstream g_log;

// Quest window docking: placed next to the game window when it appears (and while the game settles its
// size for a few seconds), then left where it is; moving the game does not drag it along.
struct Dock {
    bool manual = false;
    ULONGLONG until = 0;  // GetTickCount64 when docking stops (0 = game window not seen yet)
} g_dock;
// editor_picks.jsonl reader state; the game recreates the file on start.
struct PickReader {
    unsigned long long offset = 0;
    DWORD volume = 0, indexHigh = 0, indexLow = 0;
    double lastSeq = -1;
    std::string partial;
} g_picks;
bool g_pickArmed = false;  // editor_pick_mode.json written by us and not yet consumed by the game

struct LocalServer {
    HANDLE process = nullptr, input = nullptr;
    DWORD pid = 0;
    int port = 0;
    std::string password, mode;
} g_server;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr); return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n); return w;
}
// User-visible text in the UI language (thread-safe); "{}" in it is replaced by arg.
std::string L10n(const char* ru, const char* en, const std::string& arg = {}) {
    std::string s = g_english ? en : ru;
    if (const auto at = s.find("{}"); at != std::string::npos) s.replace(at, 2, arg);
    return s;
}
const wchar_t* L10nW(const wchar_t* ru, const wchar_t* en) { return g_english ? en : ru; }
void Log(const std::string& line) {
    if (!g_log.is_open()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char stamp[32]; std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    g_log << stamp << line << '\n'; g_log.flush();
}

// Thread-safe: hands a message to the UI thread, which forwards it to the page(s).
void ToUi(const json::Value& msg, int view = kAllViews) {
    PostMessageW(g_window, WM_TO_UI, static_cast<WPARAM>(view + 1), reinterpret_cast<LPARAM>(new std::string(msg.dump())));
}
// UI thread: answers the view that sent the message being handled.
void Reply(const json::Value& msg) { ToUi(msg, g_from); }
// Thread-safe: runs fn on the UI thread (where g_server and g_settings live).
void RunOnUi(std::function<void()> fn) { PostMessageW(g_window, WM_RUN, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn)))); }
void Status(const std::string& text, bool busy = false, bool error = false) {
    ToUi(json::Value::Object().set("type", "status").set("text", text).set("busy", busy).set("error", error));
    Log(text);
}

// ---------------- settings ----------------
fs::path SettingsFile() { return g_root / L"profiles" / L"launcher.json"; }
void LoadSettings() {
    std::ifstream in(SettingsFile(), std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (json::Value::Parse(ss.str(), v) && v.isObject()) g_settings = v;
    if (!g_settings["favorites"].isArray()) g_settings.set("favorites", json::Value::Array());
    if (!g_settings["recent"].isArray()) g_settings.set("recent", json::Value::Array());
    if (!g_settings["host"].isObject()) g_settings.set("host", json::Value::Object());
    g_english = g_settings.str("lang") != "ru";
}
void SaveSettings() {
    fs::create_directories(SettingsFile().parent_path());
    const auto tmp = SettingsFile().wstring() + L".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << g_settings.dump(); }
    MoveFileExW(tmp.c_str(), SettingsFile().c_str(), MOVEFILE_REPLACE_EXISTING);
}
void RememberServer(const char* list, const std::string& address, const std::string& name, size_t cap) {
    json::Value updated = json::Value::Array();
    updated.push(json::Value::Object().set("address", address).set("name", name));
    for (auto& e : g_settings[list].elements()) if (e.str("address") != address && updated.size() < cap) updated.push(e);
    g_settings.set(list, updated);
}

// ---------------- environment ----------------
json::Value Modes() {
    json::Value out = json::Value::Array();
    std::error_code ec;
    for (auto& dir : fs::directory_iterator(g_root / L"modes", ec)) {
        const auto id = Utf8(dir.path().filename().wstring());
        if (!dir.is_directory() || id.empty() || id[0] == '_' || !fs::exists(dir.path() / L"main.lua")) continue;
        auto ini = [&](const wchar_t* key) { wchar_t b[512] = {}; GetPrivateProfileStringW(L"Mode", key, L"", b, 512, (dir.path() / L"meta.ini").c_str()); return Utf8(b); };
        auto name = ini(L"Name");
        out.push(json::Value::Object().set("id", id).set("name", name.empty() ? id : name).set("description", ini(L"Description"))
                     .set("author", ini(L"Author")).set("version", ini(L"Version")));
    }
    return out;
}
json::Value LocalAddresses() {
    json::Value out = json::Value::Array();
    ULONG size = 16 * 1024; std::vector<uint8_t> buf(size);
    auto list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) return out;
    for (auto a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[64] = {}; auto sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const auto* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (b[0] == 169 && b[1] == 254) continue;
            out.push(json::Value::Object().set("ip", ip).set("adapter", Utf8(a->FriendlyName)));
        }
    }
    return out;
}
json::Value StateMessage() {
    json::Value s = json::Value::Object();
    s.set("type", "state");
    s.set("nick", g_settings.str("nick"));
    s.set("lang", g_english ? "en" : "ru");
    s.set("favorites", g_settings["favorites"]);
    s.set("recent", g_settings["recent"]);
    s.set("host", g_settings["host"]);
    s.set("modes", Modes());
    s.set("addresses", LocalAddresses());
    s.set("version", "0.6.0");
    s.set("protocol", static_cast<int>(net::kProtocolVersion));
    s.set("gameRunning", g_game != nullptr);
    s.set("gameEditor", g_game != nullptr && g_gameIsEditor);
    s.set("serverRunning", g_server.process != nullptr);
    s.set("serverPort", g_server.port);
    s.set("serverAvailable", fs::exists(g_root / L"dgmp-server.exe"));
    s.set("gameFound", fs::exists(g_root.parent_path() / L"BendGame" / L"Binaries" / L"Win64" / L"DaysGone.exe"));
    return s;
}
json::Value ResultJson(const net::QueryResult& r, const std::string& address) {
    auto v = json::Value::Object();
    v.set("address", address).set("ping", r.pingMs);
    if (r.pingMs >= 0) {
        v.set("name", r.info.name).set("mode", r.info.mode).set("players", r.info.players).set("maxPlayers", r.info.maxPlayers)
            .set("password", r.info.password != 0).set("version", r.info.version).set("protocol", r.info.protocol)
            .set("compatible", r.info.protocol == net::kProtocolVersion);
    }
    return v;
}

// ---------------- processes ----------------
bool RunHidden(std::wstring command, DWORD* exitCode) {
    STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, exitCode); CloseHandle(pi.hProcess);
    return true;
}

bool IsGameRunning() {
    // Another Days Gone (original or DGMP) would share the Steam app and saves.
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) found |= _wcsicmp(e.szExeFile, L"DaysGone.exe") == 0;
    CloseHandle(snap);
    return found;
}

// The game's main window: the largest visible top-level window of its process.
HWND GameWindow(DWORD pid) {
    struct Search { DWORD pid; HWND best; long long area; } search{pid, nullptr, 0};
    if (!pid) return nullptr;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(p);
        DWORD owner = 0; GetWindowThreadProcessId(w, &owner);
        RECT r;
        if (owner == s.pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetWindowRect(w, &r)) {
            const long long area = 1LL * (r.right - r.left) * (r.bottom - r.top);
            if (area > s.area) { s.area = area; s.best = w; }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

void OpenQuestWindow();

// Prepares the isolated runtime and starts the game. connect empty = editor.
void LaunchGame(const std::string& connect, const std::string& password, bool refreshSeed) {
    if (g_busy.exchange(true)) return;
    std::thread([connect, password, refreshSeed] {
        auto done = [] { g_busy = false; };
        if (IsGameRunning()) { Status(L10n("Days Gone уже запущена. Сначала закройте её.", "Days Gone is already running. Close it first."), false, true); return done(); }
        Status(L10n("Подготовка файлов игры…", "Preparing game files…"), true);
        const auto script = g_root / L"prepare.ps1";
        DWORD code = 0;
        const std::wstring setup = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"" + (refreshSeed ? L" -RefreshSeed" : L"");
        if (!RunHidden(setup, &code) || code != 0) {
            Status(L10n("Подготовка не удалась. Подробности: logs\\prepare-error.log", "Preparation failed. Details: logs\\prepare-error.log"), false, true);
            return done();
        }
        Status(connect.empty() ? L10n("Запуск редактора…", "Starting the editor…") : L10n("Запуск Days Gone…", "Launching Days Gone…"), true);
        SetEnvironmentVariableW(L"DGMP_ROOT", g_root.c_str());
        SetEnvironmentVariableW(L"SteamAppId", L"1259420");
        SetEnvironmentVariableW(L"SteamGameId", L"1259420");
        SetEnvironmentVariableW(L"DGMP_CONNECT", connect.empty() ? nullptr : Wide(connect).c_str());
        SetEnvironmentVariableW(L"DGMP_NICK", connect.empty() ? nullptr : Wide(g_settings.str("nick")).c_str());
        SetEnvironmentVariableW(L"DGMP_PASSWORD", password.empty() ? nullptr : Wide(password).c_str());
        SetEnvironmentVariableW(L"DGMP_LANG", g_english ? L"en" : L"ru");  // the game reports it to the server
        const auto game = g_root / L"runtime" / L"BendGame" / L"Binaries" / L"Win64" / L"DaysGone.exe";
        std::wstring command = L"\"" + game.wstring() + L"\" -windowed -ResX=1600 -ResY=900 -NoSplash -NoMovies";
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        const bool ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, game.parent_path().c_str(), &si, &pi);
        // The password must not linger in the launcher's environment.
        SetEnvironmentVariableW(L"DGMP_PASSWORD", nullptr);
        if (!ok) { Status(L10n("Не удалось запустить Days Gone. Проверьте, что Steam запущен.", "Couldn't start Days Gone. Make sure Steam is running."), false, true); return done(); }
        CloseHandle(pi.hThread);
        g_game = pi.hProcess;
        g_gamePid = pi.dwProcessId;
        ToUi(json::Value::Object().set("type", "game").set("running", true).set("editor", connect.empty()));
        // The quest editor lives in the game now (editorui); the side window stays available but is not opened.
        Status(connect.empty() ? L10n("Редактор запущен", "Editor running") : L10n("Игра на сервере {}", "Playing on {}", connect));
        done();
        WaitForSingleObject(pi.hProcess, INFINITE);
        PostMessageW(g_window, WM_GAME_EXIT, 0, 0);
    }).detach();
}

// UI thread: forgets the exited local server so a new one can be started.
void ServerGone() {
    DWORD code = 0; GetExitCodeProcess(g_server.process, &code);
    CloseHandle(g_server.process); CloseHandle(g_server.input);
    g_server = {};
    ToUi(json::Value::Object().set("type", "serverExit").set("code", static_cast<int>(code)));
    Log("Local server stopped (code " + std::to_string(code) + ")");
}

void StopServer() {
    if (!g_server.process) return;
    DWORD written;
    WriteFile(g_server.input, "quit\n", 5, &written, nullptr);
    if (WaitForSingleObject(g_server.process, 3000) != WAIT_OBJECT_0) { TerminateProcess(g_server.process, 1); WaitForSingleObject(g_server.process, 2000); }
    ServerGone();
}

// Types a line into the local server console (and echoes it to the console view).
bool ServerCommand(const std::string& text) {
    if (!g_server.input) return false;
    const auto line = text + "\n"; DWORD written;
    if (!WriteFile(g_server.input, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) return false;
    ToUi(json::Value::Object().set("type", "serverLog").set("line", "> " + text));
    return true;
}

// thenPlay: the page joins once the server answers. onReady (instead): runs on the UI thread with the outcome.
// remember = false keeps the host settings (used by the quest test, which runs its own mode).
bool StartServer(const json::Value& cfg, bool thenPlay, bool remember = true, std::function<void(bool)> onReady = nullptr) {
    if (g_server.process) { Status(L10n("Локальный сервер уже запущен", "A local server is already running"), false, true); return false; }
    const auto name = cfg.str("name", "DGMP Server"), mode = cfg.str("mode", "freeroam"), password = cfg.str("password");
    const int port = std::clamp(cfg["port"].asInt(net::kDefaultPort), 1024, 65535);
    const int maxPlayers = std::clamp(cfg["maxPlayers"].asInt(16), 1, net::kMaxPlayersLimit);
    if (remember) {
        g_settings.set("host", json::Value::Object().set("name", name).set("mode", mode).set("port", port).set("maxPlayers", maxPlayers));
        SaveSettings();
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead, outWrite, inRead, inWrite;
    CreatePipe(&outRead, &outWrite, &sa, 0); SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&inRead, &inWrite, &sa, 0); SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inRead; si.hStdOutput = outWrite; si.hStdError = outWrite;
    auto quote = [](const std::wstring& s) { std::wstring q = L"\""; for (auto c : s) { if (c == L'"') q += L'\\'; q += c; } return q + L"\""; };
    std::wstring command = quote((g_root / L"dgmp-server.exe").wstring()) + L" --name " + quote(Wide(name)) + L" --mode " + quote(Wide(mode)) +
                           L" --port " + std::to_wstring(port) + L" --max " + std::to_wstring(maxPlayers);
    if (!password.empty()) command += L" --password " + quote(Wide(password));
    PROCESS_INFORMATION pi{};
    const bool ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g_root.c_str(), &si, &pi);
    CloseHandle(outWrite); CloseHandle(inRead);
    if (!ok) { CloseHandle(outRead); CloseHandle(inWrite); Status(L10n("Не удалось запустить dgmp-server.exe", "Couldn't start dgmp-server.exe"), false, true); return false; }
    CloseHandle(pi.hThread);
    g_server.process = pi.hProcess; g_server.input = inWrite; g_server.pid = pi.dwProcessId;
    g_server.port = port; g_server.password = password; g_server.mode = mode;
    ToUi(json::Value::Object().set("type", "server").set("running", true).set("port", port).set("mode", mode));
    Log("Local server started on port " + std::to_string(port) + ", mode " + mode);

    // Console output reader: forwards lines to the UI; the UI thread cleans up when the console closes.
    std::thread([outRead, pid = pi.dwProcessId] {
        std::string pending; char buf[4096]; DWORD read;
        while (ReadFile(outRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
            pending.append(buf, read);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, nl); pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                ToUi(json::Value::Object().set("type", "serverLog").set("line", line));
            }
        }
        CloseHandle(outRead);
        PostMessageW(g_window, WM_SERVER_EXIT, pid, 0);
    }).detach();

    if (!thenPlay && !onReady) return true;
    // Wait until the server answers status queries, then join it.
    std::thread([port, password, onReady] {
        bool ready = false;
        for (int i = 0; i < 25 && !ready; ++i) {
            auto r = net::QueryServers({{"127.0.0.1", static_cast<uint16_t>(port)}}, 200);
            ready = !r.empty() && r[0].pingMs >= 0;
        }
        if (!ready) Status(L10n("Локальный сервер не запустился, смотрите консоль сервера", "The local server didn't start, check the server console"), false, true);
        if (onReady) RunOnUi([onReady, ready] { onReady(ready); });
        else if (ready) ToUi(json::Value::Object().set("type", "hostReady").set("address", "127.0.0.1:" + std::to_string(port)).set("password", password));
    }).detach();
    return true;
}

// ---------------- quests ----------------
fs::path QuestDir() { std::error_code ec; fs::create_directories(g_root / L"quests", ec); return g_root / L"quests"; }
bool ValidQuestId(const std::string& id) {
    return !id.empty() && id.size() <= 64 &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}
fs::path QuestFile(const std::string& id) { return QuestDir() / (Wide(id) + L".json"); }

bool ReadText(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf(); out = ss.str();
    return true;
}

// Writes via <file>.tmp + rename, so readers (the game) never see half a file.
bool WriteFileAtomic(const fs::path& file, const std::string& text) {
    const auto tmp = fs::path(file.wstring() + L".tmp");
    bool ok;
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << text; ok = static_cast<bool>(out); }
    ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    std::error_code ec;
    if (!ok) fs::remove(tmp, ec);
    return ok;
}

// json::Value keeps keys sorted; quest files are also edited by hand, so fields go in the documented order.
std::vector<std::string> OrderedKeys(const json::Value& obj, const std::string& parentKey) {
    static const std::vector<std::string> quest{"id", "title", "category", "style", "text", "xp", "reward", "steps"};
    static const std::vector<std::string> step{"type", "text", "x", "y", "z", "radius", "seconds", "meter", "event", "count",
                                               "item", "class", "ids", "area", "marker", "route", "spawn"};
    static const std::vector<std::string> row{"item", "count", "x", "y", "z"};
    static const std::vector<std::string> area{"x", "y", "z", "radius"};
    const auto& order = parentKey == "steps" ? step : parentKey == "reward" || parentKey == "spawn" ? row : parentKey == "area" ? area : quest;
    std::vector<std::string> keys;
    for (auto& k : order) if (obj.items().count(k)) keys.push_back(k);
    for (auto& [k, v] : obj.items()) if (std::find(order.begin(), order.end(), k) == order.end()) keys.push_back(k);
    return keys;
}
// Top level: one field per line; arrays of objects (steps, reward): one element per line; deeper values inline.
void WriteQuestJson(std::string& out, const json::Value& v, const std::string& key, int depth) {
    if (v.isObject() && v.size()) {
        const bool lines = depth == 0;
        out += lines ? "{\n" : "{ ";
        bool first = true;
        for (auto& k : OrderedKeys(v, key)) {
            if (!first) out += lines ? ",\n" : ", ";
            first = false;
            if (lines) out += "  ";
            out += json::Value(k).dump() + ": ";
            WriteQuestJson(out, v[k], k, depth + 1);
        }
        out += lines ? "\n}\n" : " }";
    } else if (v.isArray() && v.size()) {
        const bool lines = depth == 1 && v[0].isObject();
        out += lines ? "[\n" : "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += lines ? ",\n" : ", ";
            if (lines) out += "    ";
            WriteQuestJson(out, v[i], key, depth + 1);
        }
        out += lines ? "\n  ]" : "]";
    } else {
        out += v.dump();
    }
}

json::Value QuestList() {
    json::Value list = json::Value::Array();
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(QuestDir(), ec))
        if (f.is_regular_file() && f.path().extension() == L".json") files.push_back(f.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        const auto id = Utf8(f.stem().wstring());
        if (!ValidQuestId(id)) continue;
        std::string text; json::Value q;
        const bool ok = ReadText(f, text) && json::Value::Parse(text, q) && q.isObject();
        list.push(json::Value::Object().set("id", id).set("title", q.str("title")).set("category", q.str("category")).set("broken", !ok));
    }
    return list;
}
void SendQuestList() { ToUi(json::Value::Object().set("type", "questList").set("quests", QuestList())); }

// All item keys from docs/items.md (whitespace-separated inside its ``` blocks).
json::Value ItemKeys() {
    json::Value keys = json::Value::Array();
    std::ifstream in(g_root / L"docs" / L"items.md", std::ios::binary);
    bool code = false;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("```", 0) == 0) { code = !code; continue; }
        if (!code) continue;
        std::istringstream words(line);
        for (std::string w; words >> w;) if (w.size() > 4 && w[0] == 'k' && w.find("ID_") != std::string::npos) keys.push(w);
    }
    return keys;
}

// The game writes telemetry.json about once a second; a file older than 5 s means the game is not running.
// The file may be caught mid-write: the last good read is reused then.
json::Value Telemetry() {
    static json::Value lastGood;
    const auto path = g_root / L"telemetry.json";
    std::error_code ec;
    const auto written = fs::last_write_time(path, ec);
    if (ec || fs::file_time_type::clock::now() - written > std::chrono::seconds(5)) {
        lastGood = json::Value();
        return json::Value::Object().set("type", "telemetry").set("live", false);
    }
    std::string text; json::Value t;
    if (ReadText(path, text) && json::Value::Parse(text, t) && t.isObject() && t["x"].type() == json::Value::Type::Number) {
        const auto& aim = t["aim"];
        lastGood = json::Value::Object().set("type", "telemetry").set("live", true)
                       .set("x", t["x"].asNumber()).set("y", t["y"].asNumber()).set("z", t["z"].asNumber()).set("yaw", t["yaw"].asNumber())
                       .set("aim", aim.isObject() ? aim : json::Value());
    }
    return lastGood.isObject() ? lastGood : json::Value::Object().set("type", "telemetry").set("live", false);
}

void SaveQuest(const json::Value& msg) {
    auto reply = [](bool ok, const std::string& id) { Reply(json::Value::Object().set("type", "questSaved").set("ok", ok).set("id", id)); };
    const auto& quest = msg["quest"];
    const auto id = quest.str("id"), oldId = msg.str("oldId");
    if (!quest.isObject() || !ValidQuestId(id)) { Status(L10n("Неверный id квеста: только a-z, 0-9, _ и -", "Invalid quest id: only a-z, 0-9, _ and -"), false, true); return reply(false, id); }
    std::error_code ec;
    if (id != oldId && fs::exists(QuestFile(id), ec)) { Status(L10n("Квест «{}» уже существует", "Quest \"{}\" already exists", id), false, true); return reply(false, id); }
    std::string text; WriteQuestJson(text, quest, "", 0);
    if (!WriteFileAtomic(QuestFile(id), text)) { Status(L10n("Не удалось записать quests\\{}.json", "Couldn't write quests\\{}.json", id), false, true); return reply(false, id); }
    if (ValidQuestId(oldId) && oldId != id) fs::remove(QuestFile(oldId), ec);
    Status(L10n("Квест сохранён: quests\\{}.json", "Quest saved: quests\\{}.json", id));
    reply(true, id);
    SendQuestList();
}

// ---------------- editor game link (quest window) ----------------
// The game draws the quest being edited (with "selected": step index) from quest_preview.json; no quest = no file.
void WriteQuestPreview(const json::Value& quest) {
    const auto file = g_root / L"quest_preview.json";
    std::error_code ec;
    if (!quest.isObject()) { fs::remove(file, ec); return; }
    std::string text; WriteQuestJson(text, quest, "", 0);
    WriteFileAtomic(file, text);
}

// Arms a pick: the game takes the next left click in the editor and deletes editor_pick_mode.json. Empty kind = cancel.
void SetPickMode(const std::string& kind) {
    const auto file = g_root / L"editor_pick_mode.json";
    std::error_code ec;
    g_pickArmed = (kind == "point" || kind == "object" || kind == "item") &&
                  WriteFileAtomic(file, json::Value::Object().set("kind", kind).dump());
    if (!g_pickArmed) fs::remove(file, ec);
}

HANDLE OpenPicksFile() {
    return CreateFileW((g_root / L"editor_picks.jsonl").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}
unsigned long long FileSize(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// Starts reading editor_picks.jsonl from its current end: picks of an earlier session are not replayed.
void ResetPickReader() {
    g_picks = {};
    HANDLE f = OpenPicksFile();
    if (f == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(f, &info)) {
        g_picks.volume = info.dwVolumeSerialNumber; g_picks.indexHigh = info.nFileIndexHigh; g_picks.indexLow = info.nFileIndexLow;
        g_picks.offset = FileSize(info);
    }
    CloseHandle(f);
}

// Every ~250 ms: new complete lines of editor_picks.jsonl go to the quest window as {type:"pick", ...}.
// A recreated (other file id) or truncated file is read from the start; seq values already seen are skipped.
void PollPicks() {
    // The game deleted the pick mode file: it took the click (the pick follows, now or next poll) or dropped it.
    // Checked first: the game deletes the file before appending the pick, so the page sees "end" before the pick.
    std::error_code ec;
    if (g_pickArmed && !fs::exists(g_root / L"editor_pick_mode.json", ec)) {
        g_pickArmed = false;
        ToUi(json::Value::Object().set("type", "pickModeEnd"), kQuestView);
    }
    HANDLE f = OpenPicksFile();
    BY_HANDLE_FILE_INFORMATION info{};
    if (f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &info)) {
        const auto size = FileSize(info);
        if (info.dwVolumeSerialNumber != g_picks.volume || info.nFileIndexHigh != g_picks.indexHigh || info.nFileIndexLow != g_picks.indexLow ||
            size < g_picks.offset) {
            g_picks = {};
            g_picks.volume = info.dwVolumeSerialNumber; g_picks.indexHigh = info.nFileIndexHigh; g_picks.indexLow = info.nFileIndexLow;
        }
        if (size > g_picks.offset) {
            LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(g_picks.offset);
            std::string chunk(static_cast<size_t>(std::min<unsigned long long>(size - g_picks.offset, 1 << 20)), '\0');
            DWORD read = 0;
            if (SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
                g_picks.offset += read;
                g_picks.partial.append(chunk.data(), read);
            }
            // Only complete lines: the game may be in the middle of appending one.
            for (size_t nl; (nl = g_picks.partial.find('\n')) != std::string::npos;) {
                json::Value pick;
                const bool ok = json::Value::Parse(g_picks.partial.substr(0, nl), pick) && pick.isObject();
                g_picks.partial.erase(0, nl + 1);
                const double seq = pick["seq"].asNumber(-1);
                if (!ok || (seq >= 0 && seq <= g_picks.lastSeq)) continue;
                g_picks.lastSeq = std::max(g_picks.lastSeq, seq);
                pick.set("type", "pick");
                ToUi(pick, kQuestView);
            }
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

// Asks the game to quit (WM_CLOSE to its window) and terminates it if it is still running after 5 s.
// WM_GAME_EXIT follows as for any game exit.
void CloseGame() {
    if (!g_game) return;
    if (HWND w = GameWindow(g_gamePid)) PostMessageW(w, WM_CLOSE, 0, 0);
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), g_game, GetCurrentProcess(), &h, SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) { TerminateProcess(g_game, 0); return; }
    std::thread([h] {
        if (WaitForSingleObject(h, 5000) == WAIT_TIMEOUT) TerminateProcess(h, 0);
        CloseHandle(h);
    }).detach();
}

// Joins the local server and remembers that the game plays there (so a retest only re-issues the quest).
void LaunchToLocalServer() {
    g_gameServerPid = g_server.pid;
    g_gameIsEditor = false;
    LaunchGame("127.0.0.1:" + std::to_string(g_server.port), g_server.password, false);
}

// "Проверить": a local server in the questtest mode gets `quest <id>`, then the game joins it.
// closeEditor: the page confirmed that a running editor game may be closed first (the quest window stays open).
void TestQuest(const std::string& id, bool closeEditor) {
    std::error_code ec;
    if (!ValidQuestId(id) || !fs::exists(QuestFile(id), ec)) { Status(L10n("Сначала сохраните квест", "Save the quest first"), false, true); return; }
    if (g_busy || g_questTestStarting || g_afterGameExit) { Status(L10n("Подождите, запуск уже идёт…", "Please wait, already launching…"), false, true); return; }
    const bool testServer = g_server.process && g_server.mode == kQuestTestMode;
    if (g_game && g_gameIsEditor) {
        if (!closeEditor) { Status(L10n("Закройте редактор, чтобы проверить квест", "Close the editor to test the quest"), false, true); return; }
        Status(L10n("Закрытие редактора…", "Closing the editor…"), true);
        g_afterGameExit = [id] { TestQuest(id, false); };
        CloseGame();
        return;
    }
    if (g_game) {
        if (testServer && g_gameServerPid == g_server.pid) {
            ServerCommand("quest " + id);
            Status(L10n("Квест выдан заново", "Quest restarted"));
        } else {
            Status(L10n("Игра уже запущена на другом сервере. Закройте её, чтобы проверить квест.",
                        "The game is running on another server. Close it to test the quest."), false, true);
        }
        return;
    }
    if (!net::ValidNick(g_settings.str("nick"))) { Status(L10n("Сначала введите корректный ник", "Enter a valid nickname first"), false, true); return; }
    if (testServer) {
        ServerCommand("quest " + id);
        Status(L10n("Квест «{}» выдан, запуск игры…", "Quest \"{}\" given, launching the game…", id), true);
        LaunchToLocalServer();
        return;
    }
    if (!fs::exists(g_root / L"dgmp-server.exe", ec)) { Status(L10n("dgmp-server.exe не найден рядом с лаунчером", "dgmp-server.exe not found next to the launcher"), false, true); return; }
    if (!fs::exists(g_root / L"modes" / Wide(kQuestTestMode) / L"main.lua", ec)) { Status(L10n("Режим questtest не найден в папке modes", "The questtest mode is missing from the modes folder"), false, true); return; }
    if (g_server.process) { Status(L10n("Остановка сервера (режим {})…", "Stopping the server (mode {})…", g_server.mode), true); StopServer(); }

    // Same port and name as the "Создать сервер" page, but its own mode and no password.
    json::Value cfg = g_settings["host"];
    cfg.set("mode", kQuestTestMode).set("password", "");
    Status(L10n("Проверка квеста «{}»: запуск сервера…", "Testing quest \"{}\": starting the server…", id), true);
    g_questTestStarting = StartServer(cfg, false, false, [id](bool ready) {
        g_questTestStarting = false;
        if (!ready || !g_server.process || g_server.mode != kQuestTestMode) return;
        ServerCommand("quest " + id);
        Status(L10n("Квест «{}» выдан, запуск игры…", "Quest \"{}\" given, launching the game…", id), true);
        LaunchToLocalServer();
    });
}

// ---------------- bridge ----------------
void HandleMessage(const std::string& text, int from) {
    g_from = from;
    json::Value m;
    if (!json::Value::Parse(text, m)) return;
    const auto cmd = m.str("cmd");
    if (cmd == "init") {
        Reply(StateMessage());
    } else if (cmd == "setLang") {
        // Kept in launcher.json; the other window (quest view) follows.
        const auto lang = m.str("lang");
        if (lang != "en" && lang != "ru") return;
        g_english = lang == "en";
        g_settings.set("lang", lang); SaveSettings();
        ToUi(json::Value::Object().set("type", "lang").set("lang", lang));
    } else if (cmd == "setNick") {
        const auto nick = m.str("nick");
        if (!net::ValidNick(nick)) { Reply(json::Value::Object().set("type", "nickResult").set("ok", false)); return; }
        g_settings.set("nick", nick); SaveSettings();
        Reply(json::Value::Object().set("type", "nickResult").set("ok", true).set("nick", nick));
    } else if (cmd == "query") {
        // Each address is queried independently; results stream back in one message per batch.
        std::vector<std::string> addresses;
        for (auto& a : m["addresses"].elements()) if (a.isString()) addresses.push_back(a.asString());
        const auto tag = m.str("tag");
        std::thread([addresses, tag] {
            std::vector<std::pair<std::string, uint16_t>> targets; std::vector<std::string> valid;
            json::Value results = json::Value::Array();
            for (auto& a : addresses) {
                std::string host; uint16_t port;
                if (net::ParseAddress(a, host, port)) { targets.push_back({host, port}); valid.push_back(a); }
                else results.push(json::Value::Object().set("address", a).set("ping", -1).set("invalid", true));
            }
            auto found = net::QueryServers(targets, 1500);
            for (size_t i = 0; i < found.size() && i < valid.size(); ++i) results.push(ResultJson(found[i], valid[i]));
            ToUi(json::Value::Object().set("type", "query").set("tag", tag).set("results", results));
        }).detach();
    } else if (cmd == "lan") {
        std::thread([] {
            json::Value results = json::Value::Array();
            for (auto& r : net::ScanLan(1200)) results.push(ResultJson(r, r.host + ":" + std::to_string(r.port)));
            ToUi(json::Value::Object().set("type", "lan").set("results", results));
        }).detach();
    } else if (cmd == "favorite") {
        const auto address = m.str("address");
        if (m["add"].asBool()) RememberServer("favorites", address, m.str("name"), 200);
        else {
            json::Value kept = json::Value::Array();
            for (auto& e : g_settings["favorites"].elements()) if (e.str("address") != address) kept.push(e);
            g_settings.set("favorites", kept);
        }
        SaveSettings(); ToUi(StateMessage());
    } else if (cmd == "clearRecent") {
        g_settings.set("recent", json::Value::Array()); SaveSettings(); ToUi(StateMessage());
    } else if (cmd == "connect") {
        const auto address = m.str("address");
        std::string host; uint16_t port;
        if (!net::ParseAddress(address, host, port)) { Status(L10n("Неверный адрес сервера", "Invalid server address"), false, true); return; }
        if (!net::ValidNick(g_settings.str("nick"))) { Status(L10n("Сначала введите корректный ник", "Enter a valid nickname first"), false, true); return; }
        if (g_game) { Status(L10n("Игра уже запущена", "The game is already running"), false, true); return; }
        const auto normalized = host + ":" + std::to_string(port);
        RememberServer("recent", normalized, m.str("name"), kMaxRecent); SaveSettings();
        Log("Connecting to " + normalized + " as " + g_settings.str("nick"));
        const bool local = (host == "127.0.0.1" || host == "localhost") && g_server.process && port == g_server.port;
        g_gameServerPid = local ? g_server.pid : 0;
        g_gameIsEditor = false;
        LaunchGame(normalized, m.str("password"), false);
    } else if (cmd == "editor") {
        if (g_game && g_gameIsEditor) { OpenQuestWindow(); return; }  // brings back a closed quest window
        if (g_game) { Status(L10n("Игра уже запущена", "The game is already running"), false, true); return; }
        g_gameServerPid = 0; g_gameIsEditor = true;
        LaunchGame("", "", m["refreshSeed"].asBool());
    } else if (cmd == "host") {
        StartServer(m["config"], m["play"].asBool());
    } else if (cmd == "stopServer") {
        StopServer();
    } else if (cmd == "serverCommand") {
        if (!g_server.input) return;
        const auto line = m.str("line") + "\n"; DWORD written;
        WriteFile(g_server.input, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } else if (cmd == "questList") {
        SendQuestList();
    } else if (cmd == "questLoad") {
        const auto id = m.str("id");
        std::string file; json::Value quest;
        if (!ValidQuestId(id) || !ReadText(QuestFile(id), file)) { Status(L10n("Квест «{}» не найден", "Quest \"{}\" not found", id), false, true); SendQuestList(); return; }
        if (!json::Value::Parse(file, quest) || !quest.isObject()) { Status(L10n("quests\\{}.json: ошибка в JSON", "quests\\{}.json: invalid JSON", id), false, true); return; }
        quest.set("id", id);  // the file name wins over a stale "id" inside
        Reply(json::Value::Object().set("type", "quest").set("id", id).set("quest", quest));
    } else if (cmd == "questSave") {
        SaveQuest(m);
    } else if (cmd == "questDelete") {
        const auto id = m.str("id");
        std::error_code ec;
        if (ValidQuestId(id) && fs::remove(QuestFile(id), ec)) Status(L10n("Квест «{}» удалён", "Quest \"{}\" deleted", id));
        else Status(L10n("Не удалось удалить квест «{}»", "Couldn't delete quest \"{}\"", id), false, true);
        SendQuestList();
    } else if (cmd == "questTest") {
        TestQuest(m.str("id"), m["closeEditor"].asBool());
    } else if (cmd == "questPreview") {
        WriteQuestPreview(m["quest"]);
    } else if (cmd == "pickMode") {
        SetPickMode(m.str("kind"));
    } else if (cmd == "itemKeys") {
        Reply(json::Value::Object().set("type", "itemKeys").set("keys", ItemKeys()));
    } else if (cmd == "telemetry") {
        Reply(Telemetry());
    } else if (cmd == "openFolder") {
        const auto which = m.str("which");
        const auto path = which == "modes" ? g_root / L"modes" : which == "logs" ? g_root / L"logs" : g_root;
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == "copy") {
        const auto w = Wide(m.str("text"));
        if (OpenClipboard(g_window)) {
            EmptyClipboard();
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
                memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
            CloseClipboard();
        }
    }
}

// Creates the WebView of g_views[index] (its hwnd must exist) and opens url in it.
void CreateView(int index, const std::wstring& url) {
    const HWND hwnd = g_views[index].hwnd;
    g_env->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [index, hwnd, url](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
            auto& view = g_views[index];
            if (FAILED(hr) || !controller) { if (index == kMainView) PostQuitMessage(1); return S_OK; }
            if (view.hwnd != hwnd) { controller->Close(); return S_OK; }  // the quest window was closed meanwhile
            view.controller = controller;
            view.controller->get_CoreWebView2(&view.web);
            if (ComPtr<ICoreWebView2Controller2> c2; SUCCEEDED(view.controller.As(&c2))) c2->put_DefaultBackgroundColor({255, 14, 13, 12});
            ComPtr<ICoreWebView2Settings> settings; view.web->get_Settings(&settings);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDevToolsEnabled(g_dev);
            settings->put_AreDefaultContextMenusEnabled(g_dev);
            if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(g_dev);
            RECT r; GetClientRect(hwnd, &r); view.controller->put_Bounds(r);

            ComPtr<ICoreWebView2_3> v3;
            if (FAILED(view.web.As(&v3))) {
                if (index == kMainView) { MessageBoxW(g_window, L10nW(L"Обновите Microsoft Edge WebView2 Runtime.", L"Please update the Microsoft Edge WebView2 Runtime."), L"DGMP", MB_ICONERROR); PostQuitMessage(1); }
                return S_OK;
            }
            v3->SetVirtualHostNameToFolderMapping(L"dgmp.ui", (g_root / L"launcher" / L"ui").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
            EventRegistrationToken token;
            view.web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [index](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR text = nullptr;
                    if (SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text) { HandleMessage(Utf8(text), index); CoTaskMemFree(text); }
                    return S_OK;
                }).Get(), &token);
            // Links to the internet open in the default browser, never inside the launcher.
            view.web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr; args->get_Uri(&uri);
                    if (uri && (wcsncmp(uri, L"https://", 8) == 0)) ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                    if (uri) CoTaskMemFree(uri);
                    args->put_Handled(TRUE); return S_OK;
                }).Get(), &token);
            view.web->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr; args->get_Uri(&uri);
                    if (uri && wcsncmp(uri, L"https://dgmp.ui/", 16) != 0) args->put_Cancel(TRUE);
                    if (uri) CoTaskMemFree(uri);
                    return S_OK;
                }).Get(), &token);
            view.web->Navigate(url.c_str());
            return S_OK;
        }).Get());
}

void InitWebView() {
    const auto userData = g_root / L"profiles" / L"webview";
    fs::create_directories(userData);
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                MessageBoxW(g_window, L10nW(L"Нужен Microsoft Edge WebView2 Runtime.\nУстановите его: https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                                            L"Microsoft Edge WebView2 Runtime is required.\nInstall it from: https://go.microsoft.com/fwlink/p/?LinkId=2124703"),
                            L"DGMP", MB_ICONERROR);
                PostQuitMessage(1); return S_OK;
            }
            g_env = env;
            CreateView(kMainView, L"https://dgmp.ui/index.html");

            return S_OK;
        }).Get());
}

// ---------------- quest window ----------------
// Puts the quest window at the right edge of the game window: beside it when the screen has room, otherwise
// over its right part. Only at the start (see Dock); after that it stays where it is.
void DockQuestWindow() {
    const HWND hwnd = g_views[kQuestView].hwnd, game = GameWindow(g_gamePid);
    if (!hwnd || !game || IsIconic(game)) return;
    RECT g;
    if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &g, sizeof(g)))) GetWindowRect(game, &g);
    if (g_dock.manual) return;
    if (!g_dock.until) g_dock.until = GetTickCount64() + 15000;
    else if (GetTickCount64() > g_dock.until) return;
    const int width = MulDiv(420, GetDpiForWindow(hwnd), 96);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromRect(&g, MONITOR_DEFAULTTONEAREST), &mi);
    RECT t{g.right, g.top, g.right + width, g.bottom};
    if (t.right > mi.rcWork.right) { t.left = g.right - width; t.right = g.right; }
    // The window rect includes invisible resize borders; line up the visible frame instead.
    RECT own, frame; GetWindowRect(hwnd, &own);
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        t.left -= frame.left - own.left; t.top -= frame.top - own.top; t.right += own.right - frame.right; t.bottom += own.bottom - frame.bottom;
    }
    if (!EqualRect(&t, &own)) SetWindowPos(hwnd, HWND_TOPMOST, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE);
}

void OpenQuestWindow() {
    auto& view = g_views[kQuestView];
    if (view.hwnd) { ShowWindow(view.hwnd, SW_SHOWNOACTIVATE); return; }
    if (!g_env) return;  // InitWebView opens it once WebView2 is ready
    const UINT dpi = GetDpiForWindow(g_window);
    view.hwnd = CreateWindowExW(WS_EX_TOPMOST, L"DGMPQuests", L10nW(L"DGMP — Квесты", L"DGMP — Quests"), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(420, dpi, 96), MulDiv(820, dpi, 96), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!view.hwnd) return;
    BOOL dark = TRUE; DwmSetWindowAttribute(view.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    g_dock = {};
    ResetPickReader();
    DockQuestWindow();
    ShowWindow(view.hwnd, SW_SHOWNOACTIVATE);
    CreateView(kQuestView, L"https://dgmp.ui/index.html?view=quests");
    SetTimer(view.hwnd, kDockTimer, 300, nullptr);
    SetTimer(view.hwnd, kPickTimer, 250, nullptr);
}

void CloseQuestWindow() { if (g_views[kQuestView].hwnd) DestroyWindow(g_views[kQuestView].hwnd); }

LRESULT CALLBACK QuestWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto& view = g_views[kQuestView];
    switch (msg) {
    case WM_SIZE:
        if (view.controller) { RECT r; GetClientRect(hwnd, &r); view.controller->put_Bounds(r); }
        return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd);
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {MulDiv(340, dpi, 96), MulDiv(400, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_EXITSIZEMOVE:  // the user dragged or resized the window
        g_dock.manual = true;
        return 0;
    case WM_TIMER:
        if (wp == kDockTimer) DockQuestWindow();
        else if (wp == kPickTimer) PollPicks();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kDockTimer); KillTimer(hwnd, kPickTimer);
        if (view.controller) view.controller->Close();
        view = {};
        SetPickMode("");
        WriteQuestPreview(json::Value());
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Requests from the in-game editor (editorui writes editor_request.json): "Проверить" in the quest
// editor asks the launcher to run the quest test (it closes the editor and starts server + game).
constexpr UINT_PTR kRequestTimer = 77;
void PollEditorRequest() {
    const fs::path file = g_root / L"editor_request.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    std::ifstream in(file, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); in.close();
    fs::remove(file, ec);
    json::Value req;
    if (!json::Value::Parse(ss.str(), req) || !req.isObject()) return;
    if (req.str("cmd") == "questTest") TestQuest(req.str("id"), true);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == kRequestTimer) PollEditorRequest();
        return 0;
    case WM_SIZE:
        if (g_views[kMainView].controller) { RECT r; GetClientRect(hwnd, &r); g_views[kMainView].controller->put_Bounds(r); }
        return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd);
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {MulDiv(900, dpi, 96), MulDiv(600, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TO_UI: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        const auto json = Wide(*text);
        for (int i = 0; i < 2; ++i)
            if (g_views[i].web && (wp == 0 || static_cast<int>(wp) == i + 1)) g_views[i].web->PostWebMessageAsJson(json.c_str());
        return 0;
    }
    case WM_RUN: {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
        (*fn)();
        return 0;
    }
    case WM_SERVER_EXIT:
        // Ignored when the server was already stopped (and maybe replaced) by StopServer.
        if (g_server.process && g_server.pid == static_cast<DWORD>(wp)) { WaitForSingleObject(g_server.process, 2000); ServerGone(); }
        return 0;
    case WM_GAME_EXIT: {
        if (g_game) { CloseHandle(g_game); g_game = nullptr; }
        g_gamePid = 0; g_gameServerPid = 0; g_gameIsEditor = false;
        ToUi(json::Value::Object().set("type", "game").set("running", false));
        // The editor was closed for a quest test: the quest window stays and docks to the test game.
        if (auto next = std::move(g_afterGameExit)) { g_afterGameExit = nullptr; next(); return 0; }
        CloseQuestWindow();
        Status(L10n("Игра закрыта", "Game closed"));
        ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_CLOSE:
        if (g_server.process && WaitForSingleObject(g_server.process, 0) == WAIT_TIMEOUT &&
            MessageBoxW(hwnd, L10nW(L"Локальный сервер ещё работает. Остановить его и выйти?", L"The local server is still running. Stop it and quit?"),
                        L"DGMP", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        StopServer();
        CloseQuestWindow();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_root = fs::path(exe).parent_path();
    fs::create_directories(g_root / L"logs");
    g_log.open(g_root / L"logs" / L"launcher.log", std::ios::app);

    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool editor = false, prepareOnly = false, refresh = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        editor |= a == L"--editor"; prepareOnly |= a == L"--prepare-only"; refresh |= a == L"--refresh-seed"; g_dev |= a == L"--dev";
    }
    LocalFree(argv);
    LoadSettings();

    if (prepareOnly) {
        DWORD code = 1;
        RunHidden(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + (g_root / L"prepare.ps1").wstring() + L"\"" + (refresh ? L" -RefreshSeed" : L""), &code);
        Log(code == 0 ? "Preparation OK" : "Preparation failed");
        return static_cast<int>(code);
    }

    // Single instance: bring the running launcher forward instead.
    CreateMutexW(nullptr, TRUE, L"DGMP.Launcher.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"DGMPLauncher", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = instance; wc.lpszClassName = L"DGMPLauncher";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (ANSI macro without UNICODE)
    wc.hbrBackground = CreateSolidBrush(RGB(14, 13, 12));
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    wc.lpfnWndProc = QuestWndProc; wc.lpszClassName = L"DGMPQuests";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(1180, dpi, 96), h = MulDiv(740, dpi, 96);
    g_window = CreateWindowExW(0, L"DGMPLauncher", L"DGMP", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, nullptr);
    // Dark title bar to match the page.
    BOOL dark = TRUE; DwmSetWindowAttribute(g_window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    ShowWindow(g_window, show);
    g_views[kMainView].hwnd = g_window;
    InitWebView();
    SetTimer(g_window, kRequestTimer, 500, nullptr);
    if (editor) { g_gameIsEditor = true; LaunchGame("", "", refresh); }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    for (auto& view : g_views) view = {};
    g_env.Reset();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
