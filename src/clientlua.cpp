#include "clientlua.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "shared/luajson.h"
#include "hud.h"
#include "images.h"
#include "lang.h"
#include "log.h"
#include "mp.h"
#include "mpui.h"
#include "objects.h"
#include "blips.h"
#include "overlay.h"
#include "policy.h"
#include "profile.h"
#include "sounds.h"
#include "webui.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace clientlua {
namespace {
using namespace ue;
namespace fs = std::filesystem;

constexpr size_t kMemoryLimit = 64u * 1024 * 1024;
constexpr double kCallBudget = 0.1;   // seconds one call of a script may run (a frame, a key, a message)
constexpr double kLoadBudget = 2.0;   // main.lua's first run
constexpr int kSendsPerSecond = 60;   // the server drops more

double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::wstring W(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
// 0xRRGGBB, 0xRRGGBBAA (when > 0xFFFFFF) or "#RRGGBB[AA]" -> RGBA.
uint32_t ColorArg(lua_State* L, int idx, uint32_t fallback) {
    if (lua_isnoneornil(L, idx)) return fallback;
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx); if (*s == '#') ++s;
        const auto v = static_cast<uint32_t>(std::strtoul(s, nullptr, 16));
        return std::strlen(s) > 6 ? v : (v << 8) | 0xFF;
    }
    const auto v = static_cast<uint32_t>(luaL_checkinteger(L, idx));
    return v > 0xFFFFFF ? v : (v << 8) | 0xFF;
}
ImU32 Abgr(uint32_t rgba) { return (rgba >> 24) | ((rgba >> 8) & 0xFF00) | ((rgba << 8) & 0xFF0000) | ((rgba & 0xFF) << 24); }

// Key names: A-Z, 0-9, F1-F12 and these.
const std::pair<const char*, int> kKeys[] = {
    {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"TAB", VK_TAB}, {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"ALT", VK_MENU},
    {"BACKSPACE", VK_BACK}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"INSERT", VK_INSERT},
    {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
    {"MOUSE1", VK_LBUTTON}, {"MOUSE2", VK_RBUTTON}, {"MOUSE3", VK_MBUTTON}, {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2},
    {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2}, {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4},
    {"NUM5", VK_NUMPAD5}, {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8}, {"NUM9", VK_NUMPAD9}};
int KeyCode(const std::string& name) {
    std::string k; for (char c : name) k += char(std::toupper(static_cast<unsigned char>(c)));
    if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
    if (k.size() >= 2 && k[0] == 'F') { const int n = std::atoi(k.c_str() + 1); if (n >= 1 && n <= 12 && std::to_string(n) == k.substr(1)) return 0x6F + n; }
    for (auto& [n, vk] : kKeys) if (k == n) return vk;
    return -1;
}
std::string KeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x6F);
    for (auto& [n, code] : kKeys) if (code == vk) return n;
    return std::to_string(vk);
}
bool GameFocused() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }

// ---------------------------------------------------------------- the mode's files (cache/scripts/<sha256>)
std::map<std::string, std::string> files;  // path -> sha256
struct Partial { std::string hash, bytes; int next = 0; };
std::map<std::string, Partial> arriving;
net::Client* client = nullptr;
fs::path CacheDir() { return fs::path(profile::Root()) / L"cache" / L"scripts"; }
bool SafeHash(const std::string& h) { if (h.size() != 64) return false; for (char c : h) if (!isxdigit(static_cast<unsigned char>(c))) return false; return true; }
bool ReadFile(const std::string& path, std::string& out) {
    auto it = files.find(path);
    if (it == files.end()) return false;
    std::ifstream in(CacheDir() / it->second, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------- the Lua state
lua_State* L = nullptr;
size_t used = 0;          // bytes the state holds
bool limitOn = false;     // the memory limit holds inside protected calls only (outside, a failed allocation would abort)
double deadline = 0;
bool timedOut = false;
std::set<std::string> reported;           // errors already shown (each once)
std::deque<std::pair<std::string, std::string>> early;  // server messages before the scripts started
struct Timer { int id; double due, every; };
std::vector<Timer> timers;
int nextTimer = 1;
std::set<int> boundKeys;
bool keyWasDown[256] = {};
bool keyBlocked[256] = {};  // bindKey(..., {block = true}): the game does not get it while the scripts' keys work
std::set<std::string> uiNames, commandNames;
Frame cur;
double sendWindow = 0; int sendCount = 0;

// Objects and blips only this player has (ids from kLocalBase up: the server's count from 1).
constexpr uint16_t kLocalBase = 0xF000;
struct LocalObject { net::Client::Events::ObjectInfo info; float to[6]{}; };
std::map<uint16_t, LocalObject> localObjects;
std::map<uint16_t, net::Client::Events::BlipInfo> localBlips;
uint16_t nextLocal = kLocalBase;
uint16_t NewLocalId() {
    for (int tries = 0; tries < 0x1000; ++tries) {
        const uint16_t id = nextLocal; nextLocal = nextLocal == 0xFFFE ? kLocalBase : uint16_t(nextLocal + 1);
        if (!localObjects.count(id) && !localBlips.count(id)) return id;
    }
    return 0;
}
// data/props.txt: lower-case mesh name -> asset path (the same catalog the server uses).
std::string ResolveProp(const std::string& nameOrPath) {
    if (nameOrPath.rfind("/Game/", 0) == 0) return nameOrPath;
    static std::map<std::string, std::string> props;
    if (props.empty()) {
        std::ifstream in(fs::path(profile::Root()) / L"data" / L"props.txt");
        for (std::string line; std::getline(in, line);) {
            size_t at = 0; std::string cols[4];
            for (int i = 0; i < 4; ++i) { const auto tab = line.find('\t', at); cols[i] = line.substr(at, tab == std::string::npos ? std::string::npos : tab - at); at = tab == std::string::npos ? line.size() : tab + 1; }
            const auto dot = cols[3].rfind('.');
            if (cols[3].empty() || cols[3][0] != '/' || dot == std::string::npos) continue;
            std::string key = cols[3].substr(dot + 1); for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
            props.emplace(key, cols[3]);
        }
    }
    std::string key = nameOrPath; for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = props.find(key);
    return it == props.end() ? std::string() : it->second;
}

// The scripts' own saved data (storage.*): cache/scriptdata/<server and mode>.json on this computer.
std::string serverKey;
json::Value saved = json::Value::Object();
bool savedDirty = false; double nextSave = 0;
constexpr size_t kStorageMax = 256 * 1024;
fs::path StorageFile() {
    std::string name;
    for (char c : serverKey) name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_';
    return fs::path(profile::Root()) / L"cache" / L"scriptdata" / (name.empty() ? std::string("local") : name).append(".json");
}
void LoadStorage() {
    saved = json::Value::Object(); savedDirty = false;
    std::ifstream in(StorageFile(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json::Value v;
    if (!text.empty() && json::Value::Parse(text, v) && v.isObject()) saved = v;
}
void SaveStorage() {
    if (!savedDirty) return;
    savedDirty = false;
    std::error_code ec; fs::create_directories(StorageFile().parent_path(), ec);
    const fs::path file = StorageFile(), temp = file.wstring() + L".part";
    { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << saved.dump(); }
    fs::rename(temp, file, ec);
}

// What the scripts drew: built on the game thread each frame, drawn by the overlay.
struct Cmd {
    enum Kind : uint8_t { Text, Rect, Line, Circle, Image } kind = Text;
    float x = 0, y = 0, x2 = 0, y2 = 0, size = 16, thick = 1, rounding = 0;
    ImU32 color = IM_COL32_WHITE;
    bool fill = true, shadow = true, large = false;
    int align = 0;  // 0 left, 1 centre, 2 right
    std::string text;
};
std::vector<Cmd> building;
std::mutex drawLock;
std::vector<Cmd> shown;
std::atomic<int> screenW{0}, screenH{0};

void* Alloc(void*, void* ptr, size_t osize, size_t nsize) {
    if (!ptr) osize = 0;
    if (nsize == 0) { std::free(ptr); used -= osize; return nullptr; }
    if (limitOn && nsize > osize && used + (nsize - osize) > kMemoryLimit) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (!p) return nullptr;
    used = used - osize + nsize;
    return p;
}
void Hook(lua_State* s, lua_Debug*) {
    if (deadline > 0 && Clock() > deadline) { timedOut = true; luaL_error(s, "the script ran longer than %d ms (an endless loop?)", int(kCallBudget * 1000)); }
}
int Traceback(lua_State* s) {
    const char* msg = lua_tostring(s, 1);
    luaL_traceback(s, s, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

// An error of a script: the log, the server's console, and once on screen.
void Report(const std::string& what, const std::string& message) {
    dg::Log("client script error (%s): %s", what.c_str(), message.c_str());
    const std::string first = message.substr(0, message.find('\n'));
    if (reported.size() < 50 && reported.insert(first).second) {
        hud::AddMessage(0xE07070FF, W("[client] " + first));
        if (client) client->SendScriptError(what + ": " + message);
    }
}
// Calls the function under its nargs arguments on the stack, protected and time-limited. false: it failed.
bool Run(int nargs, const std::string& what, double budget = kCallBudget) {
    const int base = lua_gettop(L) - nargs;  // where the function is
    lua_pushcfunction(L, Traceback); lua_insert(L, base);
    deadline = Clock() + budget; timedOut = false; limitOn = true;
    const int status = lua_pcall(L, nargs, 0, base);
    limitOn = false; deadline = 0;
    if (status != LUA_OK) Report(what, lua_tostring(L, -1) ? lua_tostring(L, -1) : "error");
    lua_settop(L, base - 1);
    return status == LUA_OK;
}
// The registry table of that name, pushed.
void Registry(const char* name) { lua_getfield(L, LUA_REGISTRYINDEX, name); }

// Calls every handler of on(event, fn) with the nargs values on top of the stack (popped). A handler that ran
// too long is taken off: every frame again would freeze the game.
void Fire(const char* event, int nargs) {
    const int args = lua_gettop(L) - nargs + 1;
    Registry("cl.events");
    if (lua_getfield(L, -1, event) != LUA_TTABLE) { lua_settop(L, args - 1); return; }
    const int list = lua_gettop(L);
    for (lua_Integer i = 1, n = lua_Integer(lua_rawlen(L, list)); i <= n; ++i) {
        if (lua_rawgeti(L, list, i) != LUA_TFUNCTION) { lua_pop(L, 1); continue; }
        for (int a = 0; a < nargs; ++a) lua_pushvalue(L, args + a);
        if (!Run(nargs, std::string("on(\"") + event + "\")") && timedOut) {
            lua_pushboolean(L, false); lua_rawseti(L, list, i);
            hud::AddMessage(0xE07070FF, W(std::string("[client] on(\"") + event + "\") " + lang::Pick("отключён: выполнялся слишком долго", "turned off: it ran too long")));
        }
    }
    lua_settop(L, args - 1);
}
// Calls the function kept in a registry table under that key (string or integer) with the nargs values on top.
bool CallKept(const char* table, const char* key, lua_Integer index, int nargs, const std::string& what) {
    Registry(table);
    const int type = key ? lua_getfield(L, -1, key) : lua_rawgeti(L, -1, index);
    lua_remove(L, -2);
    if (type != LUA_TFUNCTION) { lua_pop(L, 1 + nargs); return false; }
    lua_insert(L, -(nargs + 1));
    Run(nargs, what);
    return true;
}

// ---------------------------------------------------------------- the API
json::Value Data(lua_State* s, int idx) { return lua_isnoneornil(s, idx) ? json::Value() : luajson::ToJson(s, idx); }
float Num(lua_State* s, int table, const char* key, float fallback) {
    lua_getfield(s, table, key);
    const float v = lua_isnil(s, -1) ? fallback : float(luaL_checknumber(s, -1));
    lua_pop(s, 1);
    return v;
}
bool Flag(lua_State* s, int table, const char* key, bool fallback) {
    lua_getfield(s, table, key);
    const bool v = lua_isnil(s, -1) ? fallback : lua_toboolean(s, -1) != 0;
    lua_pop(s, 1);
    return v;
}
void SetNum(lua_State* s, const char* key, double v) { lua_pushnumber(s, v); lua_setfield(s, -2, key); }

struct Api {
    static int Print(lua_State* s) {
        std::string line;
        for (int i = 1, n = lua_gettop(s); i <= n; ++i) { if (i > 1) line += '\t'; line += luaL_tolstring(s, i, nullptr); lua_pop(s, 1); }
        dg::Log("[client] %s", line.c_str());
        return 0;
    }
    // Text chunks only: precompiled bytecode can break out of the sandbox.
    static int Load(lua_State* s) {
        size_t n; const char* code = luaL_checklstring(s, 1, &n);
        const char* name = luaL_optstring(s, 2, "=(load)");
        if (luaL_loadbufferx(s, code, n, name, "t") != LUA_OK) { lua_pushnil(s); lua_insert(s, -2); return 2; }
        return 1;
    }
    // require("ui.speedo") -> client/ui/speedo.lua of the mode, run once.
    static int Require(lua_State* s) {
        std::string name = luaL_checkstring(s, 1);
        Registry("cl.modules");
        if (lua_getfield(s, -1, name.c_str()) != LUA_TNIL) return 1;
        lua_pop(s, 1);
        std::string path = name; std::replace(path.begin(), path.end(), '.', '/'); path += ".lua";
        std::string code;
        if (!ReadFile(path, code)) luaL_error(s, "require: no file client/%s in the mode", path.c_str());
        if (luaL_loadbufferx(s, code.data(), code.size(), ("@" + path).c_str(), "t") != LUA_OK) lua_error(s);
        lua_pushstring(s, name.c_str());
        lua_call(s, 1, 1);
        if (lua_isnil(s, -1)) { lua_pop(s, 1); lua_pushboolean(s, true); }
        lua_pushvalue(s, -1); lua_setfield(s, -3, name.c_str());
        return 1;
    }
    // on("frame", function(dt) end), on("stop", function() end)
    static int On(lua_State* s) {
        const char* event = luaL_checkstring(s, 1); luaL_checktype(s, 2, LUA_TFUNCTION);
        Registry("cl.events");
        if (lua_getfield(s, -1, event) != LUA_TTABLE) { lua_pop(s, 1); lua_newtable(s); lua_pushvalue(s, -1); lua_setfield(s, -3, event); }
        lua_pushvalue(s, 2); lua_rawseti(s, -2, lua_Integer(lua_rawlen(s, -2)) + 1);
        return 0;
    }
    static int Keep(lua_State* s, const char* table, std::set<std::string>* names, bool lower) {
        std::string name = luaL_checkstring(s, 1);
        if (lower) { if (!name.empty() && name[0] == '/') name.erase(0, 1); for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c))); }
        Registry(table);
        if (lua_isnoneornil(s, 2)) { lua_pushnil(s); if (names) names->erase(name); }
        else { luaL_checktype(s, 2, LUA_TFUNCTION); lua_pushvalue(s, 2); if (names) names->insert(name); }
        lua_setfield(s, -2, name.c_str());
        return 0;
    }
    static int OnServer(lua_State* s) { return Keep(s, "cl.server", nullptr, false); }   // onServer(name, fn(data))
    static int OnUi(lua_State* s) { return Keep(s, "cl.ui", &uiNames, false); }          // onUi(name, fn(data))
    static int Command(lua_State* s) { return Keep(s, "cl.commands", &commandNames, true); }  // command(name, fn(args, words...))
    // bindKey("G", function(key) end [, {block = true}]): the key's presses while the player plays; block: the
    // game does not get them then; unbindKey("G").
    static int BindKey(lua_State* s) {
        const std::string name = luaL_checkstring(s, 1);
        const int vk = KeyCode(name);
        if (vk < 0) luaL_error(s, "bindKey: unknown key '%s' (A-Z, 0-9, F1-F12, SPACE, SHIFT, CTRL, ALT, MOUSE1-5, NUM0-9 ...)", name.c_str());
        luaL_checktype(s, 2, LUA_TFUNCTION);
        Registry("cl.keys"); lua_pushvalue(s, 2); lua_rawseti(s, -2, vk);
        boundKeys.insert(vk);
        keyBlocked[vk] = lua_istable(s, 3) && Flag(s, 3, "block", false) && vk > VK_XBUTTON2;  // (mouse buttons are never taken)
        keyWasDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;  // a key held now is no press yet
        return 0;
    }
    static int UnbindKey(lua_State* s) {
        const int vk = KeyCode(luaL_checkstring(s, 1));
        if (vk < 0) return 0;
        Registry("cl.keys"); lua_pushnil(s); lua_rawseti(s, -2, vk);
        boundKeys.erase(vk); keyBlocked[vk] = false;
        return 0;
    }
    static int IsKeyDown(lua_State* s) {
        const int vk = KeyCode(luaL_checkstring(s, 1));
        lua_pushboolean(s, vk >= 0 && GameFocused() && (GetAsyncKeyState(vk) & 0x8000));
        return 1;
    }
    // after(ms, fn) / every(ms, fn) -> id; cancelTimer(id)
    static int AddTimer(lua_State* s, bool repeat) {
        const double ms = luaL_checknumber(s, 1); luaL_checktype(s, 2, LUA_TFUNCTION);
        if (repeat && ms < 1) luaL_error(s, "every: the interval must be at least 1 ms");
        const int id = nextTimer++;
        Registry("cl.timers"); lua_pushvalue(s, 2); lua_rawseti(s, -2, id);
        timers.push_back({id, Clock() + ms / 1000.0, repeat ? ms / 1000.0 : 0});
        lua_pushinteger(s, id); return 1;
    }
    static int After(lua_State* s) { return AddTimer(s, false); }
    static int Every(lua_State* s) { return AddTimer(s, true); }
    static int CancelTimer(lua_State* s) {
        const auto id = int(luaL_checkinteger(s, 1));
        for (auto& t : timers) if (t.id == id) t.due = -1;
        Registry("cl.timers"); lua_pushnil(s); lua_rawseti(s, -2, id);
        return 0;
    }
    static int Now(lua_State* s) { lua_pushnumber(s, Clock()); return 1; }

    // send(name, data) -> true: the server's on("clientMessage", fn(player, name, data)) gets it.
    static int Send(lua_State* s) {
        const std::string name = luaL_checkstring(s, 1);
        if (name.empty() || name.size() > net::kScriptNameMax) luaL_error(s, "send: the name must be 1..%d characters", int(net::kScriptNameMax));
        const std::string json = Data(s, 2).dump();
        if (json.size() > net::kScriptFromClientMax) luaL_error(s, "send: data too large (%d KB at most)", int(net::kScriptFromClientMax / 1024));
        const double now = Clock();
        if (now - sendWindow >= 1.0) { sendWindow = now; sendCount = 0; }
        const bool ok = client && ++sendCount <= kSendsPerSecond;
        if (ok) client->SendScriptMessage(name, json);
        lua_pushboolean(s, ok); return 1;
    }
    // uiSend(name, data): the mode's page gets it (dgmp.on(name, fn)).
    static int UiSend(lua_State* s) {
        const std::string name = luaL_checkstring(s, 1);
        if (name.empty() || name.size() > net::kUiNameMax) luaL_error(s, "uiSend: the name must be 1..%d characters", int(net::kUiNameMax));
        const std::string json = Data(s, 2).dump();
        if (json.size() > net::kUiToPageMax) luaL_error(s, "uiSend: data too large (%d KB at most)", int(net::kUiToPageMax / 1024));
        webui::OnUi(net::UiOp::Event, name, json, false);
        return 0;
    }

    // me() -> {id, name, x, y, z, yaw, health, dead, onBike, world, lang}
    static int Me(lua_State* s) {
        lua_createtable(s, 0, 12);
        lua_pushinteger(s, cur.id); lua_setfield(s, -2, "id");
        lua_pushstring(s, cur.name.c_str()); lua_setfield(s, -2, "name");
        SetNum(s, "x", cur.state.x); SetNum(s, "y", cur.state.y); SetNum(s, "z", cur.state.z); SetNum(s, "yaw", cur.state.yaw);
        SetNum(s, "speed", std::sqrt(cur.state.vx * cur.state.vx + cur.state.vy * cur.state.vy + cur.state.vz * cur.state.vz));
        lua_pushinteger(s, cur.state.health); lua_setfield(s, -2, "health");
        lua_pushboolean(s, cur.dead); lua_setfield(s, -2, "dead");
        lua_pushboolean(s, (cur.state.flags & net::StateOnBike) != 0); lua_setfield(s, -2, "onBike");
        lua_pushinteger(s, cur.world); lua_setfield(s, -2, "world");
        lua_pushstring(s, lang::English() ? "en" : "ru"); lua_setfield(s, -2, "lang");
        return 1;
    }
    // players() -> {{id, name, x, y, z, yaw, health, onBike, dead}, ...}: the other players this game shows
    static int Players(lua_State* s) {
        lua_newtable(s);
        if (!client) return 1;
        int i = 0;
        for (auto& [id, p] : client->players()) {
            if (id == cur.id || (p.state.flags & (net::StateHidden | net::StateOtherWorld))) continue;
            lua_createtable(s, 0, 9);
            lua_pushinteger(s, id); lua_setfield(s, -2, "id");
            lua_pushstring(s, p.nick.c_str()); lua_setfield(s, -2, "name");
            SetNum(s, "x", p.state.x); SetNum(s, "y", p.state.y); SetNum(s, "z", p.state.z); SetNum(s, "yaw", p.state.yaw);
            lua_pushinteger(s, p.state.health); lua_setfield(s, -2, "health");
            lua_pushboolean(s, (p.state.flags & net::StateOnBike) != 0); lua_setfield(s, -2, "onBike");
            lua_pushboolean(s, (p.state.flags & net::StateDead) != 0); lua_setfield(s, -2, "dead");
            lua_rawseti(s, -2, ++i);
        }
        return 1;
    }
    // camera() -> {x, y, z, pitch, yaw, fx, fy, fz (where it looks, length 1)}
    static int Camera(lua_State* s) {
        const float rad = 0.0174532925f, p = cur.camRot.Pitch * rad, y = cur.camRot.Yaw * rad;
        lua_createtable(s, 0, 8);
        SetNum(s, "x", cur.cam.X); SetNum(s, "y", cur.cam.Y); SetNum(s, "z", cur.cam.Z);
        SetNum(s, "pitch", cur.camRot.Pitch); SetNum(s, "yaw", cur.camRot.Yaw);
        SetNum(s, "fx", std::cos(p) * std::cos(y)); SetNum(s, "fy", std::cos(p) * std::sin(y)); SetNum(s, "fz", std::sin(p));
        return 1;
    }
    static int Screen(lua_State* s) { lua_pushinteger(s, screenW.load()); lua_pushinteger(s, screenH.load()); return 2; }
    // worldToScreen(x, y, z) -> sx, sy (pixels) or nil when it is behind the camera
    static bool Project(const FVector& at, float& sx, float& sy) {
        auto f = IsValid(cur.controller) ? Fn(cur.controller, "ProjectWorldLocationToScreen") : nullptr;
        if (!f) return false;
        Params p(f); p.Set("WorldLocation", at); Call(cur.controller, "ProjectWorldLocationToScreen", &p);
        if (!p.GetBool("ReturnValue")) return false;
        const auto v = p.Get<FVector2D>("ScreenLocation"); sx = v.X; sy = v.Y;
        return true;
    }
    static int WorldToScreen(lua_State* s) {
        const FVector at{float(luaL_checknumber(s, 1)), float(luaL_checknumber(s, 2)), float(luaL_checknumber(s, 3))};
        float x, y;
        if (!Project(at, x, y)) return 0;
        lua_pushnumber(s, x); lua_pushnumber(s, y); return 2;
    }
    // raycast(x1, y1, z1, x2, y2, z2) -> {x, y, z, distance, class, player (id: another player's body)} or nil.
    // The other players' bodies do not block the game's traces: they are tested as capsules here.
    static int Raycast(lua_State* s) {
        FVector a{}, b{};
        for (int i = 0; i < 3; ++i) { (&a.X)[i] = float(luaL_checknumber(s, 1 + i)); (&b.X)[i] = float(luaL_checknumber(s, 4 + i)); }
        const FVector d{b.X - a.X, b.Y - a.Y, b.Z - a.Z};
        const float length = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
        float best = 2;  // the nearest hit as a fraction of the segment (> 1: none)
        FVector at{};
        std::string cls;
        uint16_t player = 0;
        static UObject* kismet = nullptr; static UObject* statics = nullptr;
        if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
        if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
        auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, brk = statics ? Fn(statics, "BreakHitResult") : nullptr;
        UObject* context = IsValid(cur.pawn) ? cur.pawn : cur.controller;
        if (trace && brk && IsValid(context) && length > 0) {
            Params t(trace); t.Set("WorldContextObject", context); t.Set("Start", a); t.Set("End", b);
            t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
            Call(kismet, "LineTraceSingle_NEW", &t);
            auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
            if (t.GetBool("ReturnValue") && hitProp && inProp) {
                Params h(brk);
                std::memcpy(static_cast<uint8_t*>(h.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
                Call(statics, "BreakHitResult", &h);
                at = h.Get<FVector>("ImpactPoint");
                const float dx = at.X - a.X, dy = at.Y - a.Y, dz = at.Z - a.Z;
                best = std::sqrt(dx * dx + dy * dy + dz * dz) / length;
                UObject* actor = h.Get<UObject*>("HitActor");
                if (IsValid(actor)) { cls = GetName(actor->Class); player = mp::PlayerOfActor(actor); }
            }
        }
        // A standing body: a capsule 40 units wide from the feet up to 180.
        constexpr float kRadius = 40, kHeight = 180;
        for (const auto& body : mp::RemoteBodies()) {
            if (!body.alive || length <= 0) continue;
            const FVector p{body.pos.X, body.pos.Y, body.pos.Z + kRadius};
            const FVector e{0, 0, kHeight - 2 * kRadius};
            // Closest points of the ray a + d*u and the body's axis p + e*v (u, v in 0..1).
            const FVector r{a.X - p.X, a.Y - p.Y, a.Z - p.Z};
            const float aa = d.X * d.X + d.Y * d.Y + d.Z * d.Z, ee = e.Z * e.Z, ab = d.Z * e.Z, f = e.Z * r.Z, c = d.X * r.X + d.Y * r.Y + d.Z * r.Z;
            const float denom = aa * ee - ab * ab;
            float u = denom > 1e-6f ? std::clamp((ab * f - c * ee) / denom, 0.f, 1.f) : 0.f;
            float v = (ab * u + f) / ee;
            if (v < 0) { v = 0; u = std::clamp(-c / aa, 0.f, 1.f); }
            else if (v > 1) { v = 1; u = std::clamp((ab - c) / aa, 0.f, 1.f); }
            const FVector q{a.X + d.X * u, a.Y + d.Y * u, a.Z + d.Z * u};
            const float gx = q.X - p.X, gy = q.Y - p.Y, gz = q.Z - (p.Z + e.Z * v);
            if (gx * gx + gy * gy + gz * gz > kRadius * kRadius || u >= best) continue;
            best = u; at = q; cls = "player"; player = body.id;
        }
        if (best > 1) return 0;
        lua_createtable(s, 0, 6);
        SetNum(s, "x", at.X); SetNum(s, "y", at.Y); SetNum(s, "z", at.Z);
        SetNum(s, "distance", best * length);
        if (!cls.empty()) { lua_pushstring(s, cls.c_str()); lua_setfield(s, -2, "class"); }
        if (player) { lua_pushinteger(s, player); lua_setfield(s, -2, "player"); }
        return 1;
    }

    // chat(text [, color]): a line in this player's chat only.
    static int Chat(lua_State* s) { hud::AddMessage(ColorArg(s, 2, 0xE0E0E0FF), W(luaL_checkstring(s, 1))); return 0; }
    // hint(title, text [, seconds])
    static int Hint(lua_State* s) {
        const std::wstring title = W(luaL_checkstring(s, 1)), text = W(luaL_optstring(s, 2, ""));
        const float seconds = float(luaL_optnumber(s, 3, 0));
        if (!policy::ShowNativeHint(cur.controller, title, text, "", false, seconds)) hud::ShowHint(title, text, seconds > 0 ? seconds : 7.0);
        return 0;
    }
    // playSound(name or "/Game/..." path [, {x=, y=, z=, volume=1, pitch=1}]): heard by this player only.
    static int PlaySound(lua_State* s) {
        std::string path = luaL_checkstring(s, 1);
        if (path.rfind("/Game/", 0) != 0) {
            static std::map<std::string, std::string> names;  // data/sounds.txt
            if (names.empty()) {
                std::ifstream in(fs::path(profile::Root()) / L"data" / L"sounds.txt");
                for (std::string line; std::getline(in, line);) {
                    const auto tab = line.find('\t');
                    if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
                    std::string key = line.substr(0, tab); for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
                    names[key] = line.substr(tab + 1);
                }
            }
            std::string key = path; for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
            auto it = names.find(key);
            if (it == names.end()) luaL_error(s, "playSound: unknown sound '%s' (data/sounds.txt or a /Game/... path)", path.c_str());
            path = it->second;
        }
        float volume = 1, pitch = 1; FVector at{}; bool placed = false;
        if (lua_istable(s, 2)) {
            volume = Num(s, 2, "volume", 1); pitch = Num(s, 2, "pitch", 1);
            lua_getfield(s, 2, "x"); placed = !lua_isnil(s, -1); lua_pop(s, 1);
            if (placed) at = {Num(s, 2, "x", 0), Num(s, 2, "y", 0), Num(s, 2, "z", 0)};
        }
        sounds::Play(path, placed ? &at : nullptr, volume, pitch);
        return 0;
    }

    // ---- draw.*: shown this frame (call them in on("frame")); x, y in pixels from the top left
    static Cmd& Add(lua_State* s, Cmd::Kind kind) {
        if (building.size() >= 5000) luaL_error(s, "draw: more than 5000 shapes in a frame");
        building.emplace_back(); building.back().kind = kind; return building.back();
    }
    static void TextOptions(lua_State* s, int idx, Cmd& c) {
        if (!lua_istable(s, idx)) return;
        c.size = Num(s, idx, "size", 16); c.shadow = Flag(s, idx, "shadow", true);
        lua_getfield(s, idx, "color"); c.color = Abgr(ColorArg(s, -1, 0xFFFFFFFF)); lua_pop(s, 1);
        lua_getfield(s, idx, "align");
        if (const char* a = lua_tostring(s, -1)) c.align = std::strcmp(a, "center") == 0 ? 1 : std::strcmp(a, "right") == 0 ? 2 : 0;
        lua_pop(s, 1);
        c.large = c.size > 24;
    }
    // draw.text(x, y, text [, {size=16, color=, align="left|center|right", shadow=true}])
    static int DrawText(lua_State* s) {
        const float x = float(luaL_checknumber(s, 1)), y = float(luaL_checknumber(s, 2));
        std::string text = luaL_checkstring(s, 3);
        Cmd& c = Add(s, Cmd::Text); c.x = x; c.y = y; c.text = std::move(text);
        TextOptions(s, 4, c);
        return 0;
    }
    // draw.text3d(x, y, z, text [, {distance=5000 (units), ...as draw.text}]) -> true when on screen
    static int DrawText3d(lua_State* s) {
        const FVector at{float(luaL_checknumber(s, 1)), float(luaL_checknumber(s, 2)), float(luaL_checknumber(s, 3))};
        std::string text = luaL_checkstring(s, 4);
        const float range = lua_istable(s, 5) ? Num(s, 5, "distance", 5000) : 5000;
        const float dx = at.X - cur.cam.X, dy = at.Y - cur.cam.Y, dz = at.Z - cur.cam.Z;
        float x, y;
        if (dx * dx + dy * dy + dz * dz > range * range || !Project(at, x, y)) { lua_pushboolean(s, false); return 1; }
        Cmd& c = Add(s, Cmd::Text); c.x = x; c.y = y; c.text = std::move(text); c.align = 1;  // (centred unless align says otherwise)
        TextOptions(s, 5, c);
        lua_pushboolean(s, true); return 1;
    }
    // draw.rect(x, y, w, h, color [, {fill=true, rounding=0, thickness=1}])
    static int DrawRect(lua_State* s) {
        Cmd& c = Add(s, Cmd::Rect);
        c.x = float(luaL_checknumber(s, 1)); c.y = float(luaL_checknumber(s, 2));
        c.x2 = c.x + float(luaL_checknumber(s, 3)); c.y2 = c.y + float(luaL_checknumber(s, 4));
        c.color = Abgr(ColorArg(s, 5, 0xFFFFFFFF));
        if (lua_istable(s, 6)) { c.fill = Flag(s, 6, "fill", true); c.rounding = Num(s, 6, "rounding", 0); c.thick = Num(s, 6, "thickness", 1); }
        return 0;
    }
    // draw.line(x1, y1, x2, y2, color [, thickness=1])
    static int DrawLine(lua_State* s) {
        Cmd& c = Add(s, Cmd::Line);
        c.x = float(luaL_checknumber(s, 1)); c.y = float(luaL_checknumber(s, 2)); c.x2 = float(luaL_checknumber(s, 3)); c.y2 = float(luaL_checknumber(s, 4));
        c.color = Abgr(ColorArg(s, 5, 0xFFFFFFFF)); c.thick = float(luaL_optnumber(s, 6, 1));
        return 0;
    }
    // draw.circle(x, y, radius, color [, {fill=true, thickness=1}])
    static int DrawCircle(lua_State* s) {
        Cmd& c = Add(s, Cmd::Circle);
        c.x = float(luaL_checknumber(s, 1)); c.y = float(luaL_checknumber(s, 2)); c.size = float(luaL_checknumber(s, 3));
        c.color = Abgr(ColorArg(s, 4, 0xFFFFFFFF));
        if (lua_istable(s, 5)) { c.fill = Flag(s, 5, "fill", true); c.thick = Num(s, 5, "thickness", 1); }
        return 0;
    }
    // draw.image(name, x, y, w, h [, color]): a picture of the mode's images folder
    static int DrawImage(lua_State* s) {
        Cmd& c = Add(s, Cmd::Image);
        c.text = luaL_checkstring(s, 1);
        c.x = float(luaL_checknumber(s, 2)); c.y = float(luaL_checknumber(s, 3));
        c.x2 = c.x + float(luaL_checknumber(s, 4)); c.y2 = c.y + float(luaL_checknumber(s, 5));
        c.color = Abgr(ColorArg(s, 6, 0xFFFFFFFF));
        return 0;
    }
    // ---- objects and blips only this player has (docs/client-lua.md)
    static void ObjectFields(lua_State* s, int idx, net::Client::Events::ObjectInfo& o) {
        o.x = Num(s, idx, "x", o.x); o.y = Num(s, idx, "y", o.y); o.z = Num(s, idx, "z", o.z);
        o.pitch = Num(s, idx, "pitch", o.pitch); o.yaw = Num(s, idx, "yaw", o.yaw); o.roll = Num(s, idx, "roll", o.roll);
        o.scale = std::clamp(Num(s, idx, "scale", o.scale), 0.01f, 100.f); o.collision = Flag(s, idx, "collision", o.collision);
        o.ghost = Flag(s, idx, "ghost", o.ghost); if (o.ghost) o.collision = false;  // a preview: outlined, nothing bumps into it
        for (const char* k : {"model", "asset"}) {
            lua_getfield(s, idx, k);
            if (const char* want = lua_tostring(s, -1)) {
                const std::string path = ResolveProp(want);
                if (path.empty()) luaL_error(s, "createObject: unknown model '%s' (a mesh of data/props.txt or a /Game/... path)", want);
                o.asset = path;
            }
            lua_pop(s, 1);
        }
    }
    // createObject{model= or asset=, x, y, z, pitch, yaw, roll, scale = 1, collision = true, ghost = false} -> id
    static int CreateObject(lua_State* s) {
        luaL_checktype(s, 1, LUA_TTABLE);
        if (localObjects.size() >= 500) luaL_error(s, "createObject: 500 objects at most");
        LocalObject o; ObjectFields(s, 1, o.info);
        if (o.info.asset.empty()) luaL_error(s, "createObject: model or asset is required");
        const uint16_t id = NewLocalId();
        if (!id) luaL_error(s, "createObject: no free id");
        const float at[6] = {o.info.x, o.info.y, o.info.z, o.info.pitch, o.info.yaw, o.info.roll};
        std::copy(at, at + 6, o.to);
        localObjects[id] = o;
        objects::OnObject(id, &o.info);
        lua_pushinteger(s, id); return 1;
    }
    static LocalObject* CheckObject(lua_State* s) {
        auto it = localObjects.find(uint16_t(luaL_checkinteger(s, 1)));
        return it == localObjects.end() ? nullptr : &it->second;
    }
    // setObject(id, {fields}) -> true: there at once (a move stops)
    static int SetObject(lua_State* s) {
        LocalObject* o = CheckObject(s); luaL_checktype(s, 2, LUA_TTABLE);
        if (!o) { lua_pushboolean(s, false); return 1; }
        o->info.x = o->to[0]; o->info.y = o->to[1]; o->info.z = o->to[2]; o->info.pitch = o->to[3]; o->info.yaw = o->to[4]; o->info.roll = o->to[5];
        ObjectFields(s, 2, o->info);
        const float at[6] = {o->info.x, o->info.y, o->info.z, o->info.pitch, o->info.yaw, o->info.roll};
        std::copy(at, at + 6, o->to);
        objects::OnObject(uint16_t(lua_tointeger(s, 1)), &o->info);
        lua_pushboolean(s, true); return 1;
    }
    // moveObject(id, {x, y, z, pitch, yaw, roll, time = seconds | speed = metres a second}) -> seconds
    static int MoveObject(lua_State* s) {
        LocalObject* o = CheckObject(s); luaL_checktype(s, 2, LUA_TTABLE);
        if (!o) { lua_pushnil(s); return 1; }
        float to[6]; std::copy(o->to, o->to + 6, to);
        static const char* names[] = {"x", "y", "z", "pitch", "yaw", "roll"};
        for (int i = 0; i < 6; ++i) to[i] = Num(s, 2, names[i], to[i]);
        float seconds = Num(s, 2, "time", -1);
        if (seconds < 0) {
            const float speed = std::max(0.01f, Num(s, 2, "speed", 2)), dx = to[0] - o->to[0], dy = to[1] - o->to[1], dz = to[2] - o->to[2];
            seconds = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.f / speed;
        }
        std::copy(to, to + 6, o->to);
        objects::OnMove(uint16_t(lua_tointeger(s, 1)), to, seconds, 0);
        lua_pushnumber(s, seconds); return 1;
    }
    static int RemoveObject(lua_State* s) {
        const auto id = uint16_t(luaL_checkinteger(s, 1));
        const bool had = localObjects.erase(id) > 0;
        if (had) objects::OnObject(id, nullptr);
        lua_pushboolean(s, had); return 1;
    }
    static void BlipFields(lua_State* s, int idx, net::Client::Events::BlipInfo& b) {
        b.x = Num(s, idx, "x", b.x); b.y = Num(s, idx, "y", b.y); b.z = Num(s, idx, "z", b.z);
        b.scale = std::clamp(Num(s, idx, "scale", b.scale), 0.1f, 5.f);
        b.map = Flag(s, idx, "map", b.map); b.radar = Flag(s, idx, "radar", b.radar); b.edge = Flag(s, idx, "edge", b.edge);
        lua_getfield(s, idx, "icon"); if (const char* icon = lua_tostring(s, -1)) b.icon = icon; lua_pop(s, 1);
        lua_getfield(s, idx, "color"); if (!lua_isnil(s, -1)) b.color = ColorArg(s, lua_gettop(s), 0xFFFFFFFF); lua_pop(s, 1);
    }
    // createBlip{x, y, z, icon = "MAP_...", color, scale = 1, map = true, radar = true, edge = false} -> id
    static int CreateBlip(lua_State* s) {
        luaL_checktype(s, 1, LUA_TTABLE);
        if (localBlips.size() >= 200) luaL_error(s, "createBlip: 200 blips at most");
        net::Client::Events::BlipInfo b; BlipFields(s, 1, b);
        const uint16_t id = NewLocalId();
        if (!id) luaL_error(s, "createBlip: no free id");
        localBlips[id] = b;
        blips::OnBlip(id, &b);
        lua_pushinteger(s, id); return 1;
    }
    static int SetBlip(lua_State* s) {
        auto it = localBlips.find(uint16_t(luaL_checkinteger(s, 1))); luaL_checktype(s, 2, LUA_TTABLE);
        if (it == localBlips.end()) { lua_pushboolean(s, false); return 1; }
        BlipFields(s, 2, it->second);
        blips::OnBlip(it->first, &it->second);
        lua_pushboolean(s, true); return 1;
    }
    static int RemoveBlip(lua_State* s) {
        const auto id = uint16_t(luaL_checkinteger(s, 1));
        const bool had = localBlips.erase(id) > 0;
        if (had) blips::OnBlip(id, nullptr);
        lua_pushboolean(s, had); return 1;
    }

    // ---- storage.* : saved on this computer for this server and mode (256 KB in all)
    static int StorageGet(lua_State* s) { luajson::PushJson(s, saved[luaL_checkstring(s, 1)]); return 1; }
    static int StorageSet(lua_State* s) {
        const std::string key = luaL_checkstring(s, 1);
        if (key.empty() || key.size() > 128) luaL_error(s, "storage.set: the key must be 1..128 characters");
        json::Value before = saved;
        if (lua_isnoneornil(s, 2)) saved.erase(key); else saved.set(key, luajson::ToJson(s, 2));
        if (saved.dump().size() > kStorageMax) { saved = std::move(before); luaL_error(s, "storage.set: the scripts' storage holds %d KB at most", int(kStorageMax / 1024)); }
        savedDirty = true;
        return 0;
    }
    static int StorageKeys(lua_State* s) {
        lua_newtable(s); int i = 0;
        for (auto& [k, v] : saved.items()) { lua_pushstring(s, k.c_str()); lua_rawseti(s, -2, ++i); }
        return 1;
    }
    static int JsonEncode(lua_State* s) { const std::string t = luajson::ToJson(s, 1).dump(); lua_pushlstring(s, t.data(), t.size()); return 1; }
    static int JsonDecode(lua_State* s) {
        json::Value v;
        if (!json::Value::Parse(luaL_checkstring(s, 1), v)) { lua_pushnil(s); lua_pushstring(s, "invalid JSON"); return 2; }
        luajson::PushJson(s, v); return 1;
    }
};

void Stop() {
    if (L) {
        Fire("stop", 0);
        lua_close(L);
        L = nullptr;
        dg::Log("client scripts stopped");
    }
    for (auto& [id, o] : localObjects) objects::OnObject(id, nullptr);
    for (auto& [id, b] : localBlips) blips::OnBlip(id, nullptr);
    localObjects.clear(); localBlips.clear();
    std::fill(std::begin(keyBlocked), std::end(keyBlocked), false); overlay::SetScriptBlockedKeys(keyBlocked);
    SaveStorage();
    used = 0; timers.clear(); boundKeys.clear(); uiNames.clear(); commandNames.clear(); building.clear();
    std::lock_guard g(drawLock); shown.clear();
}

// Builds the sandbox and runs main.lua.
void Start() {
    std::string code;
    if (!ReadFile("main.lua", code)) { Report("main.lua", lang::Pick("в папке client режима нет main.lua", "the mode's client folder has no main.lua")); files.clear(); return; }
    L = lua_newstate(&Alloc, nullptr);
    if (!L) return;
    lua_atpanic(L, [](lua_State* s) -> int { dg::Log("client scripts: unprotected Lua error: %s", lua_tostring(s, -1)); return 0; });
    // Sandbox: no io, os reduced to time, no package (require reads the mode's files), no debug, no bytecode.
    for (auto lib : {std::pair<const char*, lua_CFunction>{LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                     {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_OSLIBNAME, luaopen_os}}) {
        luaL_requiref(L, lib.first, lib.second, 1); lua_pop(L, 1);
    }
    lua_getglobal(L, "os");
    for (const char* banned : {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"}) { lua_pushnil(L); lua_setfield(L, -2, banned); }
    lua_pop(L, 1);
    lua_getglobal(L, "string"); lua_pushnil(L); lua_setfield(L, -2, "dump"); lua_pop(L, 1);
    for (const char* banned : {"dofile", "loadfile"}) { lua_pushnil(L); lua_setglobal(L, banned); }
    for (const char* table : {"cl.events", "cl.server", "cl.ui", "cl.commands", "cl.keys", "cl.timers", "cl.modules"}) { lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, table); }
    const luaL_Reg globals[] = {{"print", &Api::Print}, {"log", &Api::Print}, {"load", &Api::Load}, {"require", &Api::Require},
                                {"on", &Api::On}, {"onServer", &Api::OnServer}, {"onUi", &Api::OnUi}, {"command", &Api::Command},
                                {"bindKey", &Api::BindKey}, {"unbindKey", &Api::UnbindKey}, {"isKeyDown", &Api::IsKeyDown},
                                {"after", &Api::After}, {"every", &Api::Every}, {"cancelTimer", &Api::CancelTimer}, {"now", &Api::Now},
                                {"send", &Api::Send}, {"uiSend", &Api::UiSend}, {"me", &Api::Me}, {"players", &Api::Players},
                                {"camera", &Api::Camera}, {"screen", &Api::Screen}, {"worldToScreen", &Api::WorldToScreen}, {"raycast", &Api::Raycast},
                                {"chat", &Api::Chat}, {"hint", &Api::Hint}, {"playSound", &Api::PlaySound},
                                {"createObject", &Api::CreateObject}, {"setObject", &Api::SetObject}, {"moveObject", &Api::MoveObject}, {"removeObject", &Api::RemoveObject},
                                {"createBlip", &Api::CreateBlip}, {"setBlip", &Api::SetBlip}, {"removeBlip", &Api::RemoveBlip}, {nullptr, nullptr}};
    lua_pushglobaltable(L); luaL_setfuncs(L, globals, 0); lua_pop(L, 1);
    const luaL_Reg draw[] = {{"text", &Api::DrawText}, {"text3d", &Api::DrawText3d}, {"rect", &Api::DrawRect}, {"line", &Api::DrawLine},
                             {"circle", &Api::DrawCircle}, {"image", &Api::DrawImage}, {nullptr, nullptr}};
    luaL_newlib(L, draw); lua_setglobal(L, "draw");
    const luaL_Reg jsonLib[] = {{"encode", &Api::JsonEncode}, {"decode", &Api::JsonDecode}, {nullptr, nullptr}};
    luaL_newlib(L, jsonLib); lua_setglobal(L, "json");
    const luaL_Reg storageLib[] = {{"get", &Api::StorageGet}, {"set", &Api::StorageSet}, {"keys", &Api::StorageKeys}, {nullptr, nullptr}};
    luaL_newlib(L, storageLib); lua_setglobal(L, "storage");
    LoadStorage();
    lua_sethook(L, &Hook, LUA_MASKCOUNT, 10000);

    if (luaL_loadbufferx(L, code.data(), code.size(), "@main.lua", "t") != LUA_OK) { Report("main.lua", lua_tostring(L, -1)); lua_pop(L, 1); return; }
    dg::Log("client scripts: %zu files, main.lua starts", files.size());
    Run(0, "main.lua", kLoadBudget);
    while (L && !early.empty()) { auto [name, json] = early.front(); early.pop_front(); OnServer(name, json); }
}
bool wanted = false;  // the server's list has files: start when they are all here

}  // namespace

void OnList(const std::vector<std::pair<std::string, std::string>>& list, net::Client& c) {
    client = &c;
    Stop();
    files.clear(); arriving.clear(); reported.clear();
    std::error_code ec;
    for (auto& [path, hash] : list) {
        if (!SafeHash(hash)) continue;
        files[path] = hash;
        if (!fs::exists(CacheDir() / hash, ec)) { c.SendScriptRequest(path); arriving[path] = {hash, {}, 0}; }
    }
    wanted = !files.empty();
    if (wanted) dg::Log("client scripts: %zu files of the mode, %zu to download", files.size(), arriving.size());
}

void OnData(const std::string& path, int part, int parts, const std::string& bytes) {
    auto it = arriving.find(path);
    if (it == arriving.end() || part != it->second.next || parts <= 0) return;
    it->second.bytes += bytes; ++it->second.next;
    if (it->second.next < parts) return;
    std::error_code ec; fs::create_directories(CacheDir(), ec);
    const fs::path file = CacheDir() / it->second.hash, temp = file.wstring() + L".part";
    { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << it->second.bytes; }
    fs::rename(temp, file, ec);
    arriving.erase(it);
}

void OnServer(const std::string& name, const std::string& json) {
    if (!L) { if (wanted && early.size() < 256) early.emplace_back(name, json); return; }
    json::Value v;
    if (!json::Value::Parse(json, v)) return;
    luajson::PushJson(L, v);
    CallKept("cl.server", name.c_str(), 0, 1, "onServer(\"" + name + "\")");
}

void Reset() { Stop(); files.clear(); arriving.clear(); early.clear(); wanted = false; client = nullptr; }
void SetServer(const std::string& key) { serverKey = key; }

void Update(const Frame& frame, net::Client& c) {
    client = &c;
    cur = frame;
    if (!L && wanted && arriving.empty()) { wanted = false; Start(); }
    if (!L) return;
    building.clear();
    const double now = Clock();
    // Timers due (one added meanwhile waits for the next frame).
    for (size_t i = 0, n = timers.size(); i < n; ++i) {
        if (timers[i].due < 0 || now < timers[i].due) continue;
        const Timer t = timers[i];
        if (t.every > 0) timers[i].due = now + t.every; else timers[i].due = -1;
        if (!CallKept("cl.timers", nullptr, t.id, 0, "timer") || (t.every <= 0)) { Registry("cl.timers"); lua_pushnil(L); lua_rawseti(L, -2, t.id); lua_pop(L, 1); }
        if (timedOut) for (auto& other : timers) if (other.id == t.id) other.due = -1;  // a repeating one that ran too long stops
    }
    std::erase_if(timers, [](const Timer& t) { return t.due < 0; });
    // Key presses (edges) while the player plays; a key held when that ends is no new press after.
    const bool keys = frame.canAct && GameFocused() && !overlay::CursorMode() && !mpui::DialogOpen() && !webui::Focused();
    { static const bool none[256] = {}; overlay::SetScriptBlockedKeys(keys ? keyBlocked : none); }  // (chat and dialogs keep every key)
    if (savedDirty && now >= nextSave) { nextSave = now + 5; SaveStorage(); }
    for (const int vk : std::vector<int>(boundKeys.begin(), boundKeys.end())) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !keyWasDown[vk] && keys) { lua_pushstring(L, KeyName(vk).c_str()); CallKept("cl.keys", nullptr, vk, 1, "bindKey(\"" + KeyName(vk) + "\")"); }
        keyWasDown[vk] = down;
    }
    lua_pushnumber(L, frame.dt);
    Fire("frame", 1);
    std::lock_guard g(drawLock);
    shown = frame.hudShown ? building : std::vector<Cmd>{};
}

bool OnChat(const std::string& line) {
    if (!L || line.size() < 2 || line[0] != '/') return false;
    const auto space = line.find(' ');
    std::string name = line.substr(1, space == std::string::npos ? std::string::npos : space - 1);
    for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (!commandNames.count(name)) return false;
    const std::string args = space == std::string::npos ? std::string() : line.substr(space + 1);
    lua_pushlstring(L, args.data(), args.size());
    int words = 0;  // command("tp", function(args, x, y, z) end)
    for (size_t i = 0; i < args.size() && words < 16;) {
        while (i < args.size() && args[i] == ' ') ++i;
        size_t j = i; while (j < args.size() && args[j] != ' ') ++j;
        if (j > i) { lua_pushlstring(L, args.data() + i, j - i); ++words; }
        i = j;
    }
    CallKept("cl.commands", name.c_str(), 0, 1 + words, "/" + name);
    return true;
}

bool OnPageMessage(const std::string& name, const std::string& json) {
    if (!L || !uiNames.count(name)) return false;
    json::Value v;
    if (json::Value::Parse(json, v)) luajson::PushJson(L, v); else lua_pushnil(L);
    CallKept("cl.ui", name.c_str(), 0, 1, "onUi(\"" + name + "\")");
    return true;
}

void Draw() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    screenW = int(size.x); screenH = int(size.y);
    std::lock_guard g(drawLock);
    if (shown.empty()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* small = ImGui::GetFont();
    ImFont* large = static_cast<ImFont*>(overlay::LargeFont());
    for (const Cmd& c : shown) switch (c.kind) {
    case Cmd::Text: {
        ImFont* font = c.large && large ? large : small;
        const ImVec2 box = font->CalcTextSizeA(c.size, FLT_MAX, 0, c.text.c_str());
        const float x = c.align == 1 ? c.x - box.x / 2 : c.align == 2 ? c.x - box.x : c.x;
        if (c.shadow) dl->AddText(font, c.size, ImVec2(x + 1, c.y + 1), IM_COL32(0, 0, 0, (c.color >> 24) * 3 / 4), c.text.c_str());
        dl->AddText(font, c.size, ImVec2(x, c.y), c.color, c.text.c_str());
        break;
    }
    case Cmd::Rect:
        if (c.fill) dl->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.x2, c.y2), c.color, c.rounding);
        else dl->AddRect(ImVec2(c.x, c.y), ImVec2(c.x2, c.y2), c.color, c.rounding, 0, c.thick);
        break;
    case Cmd::Line: dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x2, c.y2), c.color, c.thick); break;
    case Cmd::Circle:
        if (c.fill) dl->AddCircleFilled(ImVec2(c.x, c.y), c.size, c.color);
        else dl->AddCircle(ImVec2(c.x, c.y), c.size, c.color, 0, c.thick);
        break;
    case Cmd::Image: {
        const images::View v = images::Get(c.text);
        if (v.texture) dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(v.texture)), ImVec2(c.x, c.y), ImVec2(c.x2, c.y2), ImVec2(0, 0), ImVec2(1, 1), c.color);
        break;
    }
    }
}

}  // namespace clientlua
