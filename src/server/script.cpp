#include "script.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <enet/enet.h>
#include <windows.h>
#include <bcrypt.h>
#include <cctype>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <functional>

namespace srv {
namespace {
constexpr const char* kPlayerMeta = "dgmp.player";
constexpr double kCallBudget = 3.0;  // seconds one script call may run before it is aborted
const char* kQuitNames[] = {"left", "timeout", "kicked"};

double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double g_deadline = 0;

void BudgetHook(lua_State* L, lua_Debug*) {
    if (g_deadline > 0 && Clock() > g_deadline) luaL_error(L, "script ran longer than %d s (infinite loop?)", static_cast<int>(kCallBudget));
}

std::string IniValue(const std::filesystem::path& file, const char* section, const char* key) {
    wchar_t buf[512] = {};
    std::wstring wkey(key, key + std::strlen(key)), wsec(section, section + std::strlen(section));
    GetPrivateProfileStringW(wsec.c_str(), wkey.c_str(), L"", buf, 512, file.c_str());
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Accepts 0xRRGGBB, 0xRRGGBBAA (when > 0xFFFFFF) or "#RRGGBB".
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
}  // namespace

struct PlayerRef { uint16_t id; double joinedAt; };

struct Api {
    static Script& Self(lua_State* L) { return **static_cast<Script**>(lua_getextraspace(L)); }
    static Server& Srv(lua_State* L) { return Self(L).server_; }

    static void PushPlayer(lua_State* L, Player& p) {
        auto ref = static_cast<PlayerRef*>(lua_newuserdatauv(L, sizeof(PlayerRef), 0));
        ref->id = p.id; ref->joinedAt = p.joinedAt;
        luaL_setmetatable(L, kPlayerMeta);
    }
    // Returns null if the player has left (stale handle) or the argument is not a player.
    static Player* ToPlayer(lua_State* L, int idx) {
        auto ref = static_cast<PlayerRef*>(luaL_testudata(L, idx, kPlayerMeta));
        if (!ref) return nullptr;
        auto p = Srv(L).Find(ref->id);
        return p && p->joinedAt == ref->joinedAt ? p : nullptr;
    }
    static Player& CheckPlayer(lua_State* L, int idx) {
        auto p = ToPlayer(L, idx);
        if (!p) luaL_error(L, luaL_testudata(L, idx, kPlayerMeta) ? "player has left the server" : "player expected");
        return *p;
    }
    static std::string Concat(lua_State* L, int from) {
        std::string out;
        for (int i = from, n = lua_gettop(L); i <= n; ++i) { if (i > from) out += ' '; out += luaL_tolstring(L, i, nullptr); lua_pop(L, 1); }
        return out;
    }

    // ---- globals ----
    static int Log(lua_State* L) { Srv(L).Log("[%s] %s", Self(L).mode_.c_str(), Concat(L, 1).c_str()); return 0; }
    static int On(lua_State* L) {
        const char* event = luaL_checkstring(L, 1); luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.events");
        if (lua_getfield(L, -1, event) != LUA_TTABLE) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, -3, event); }
        lua_pushvalue(L, 2); lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2)) + 1);
        return 0;
    }
    static int Command(lua_State* L) {
        std::string name = luaL_checkstring(L, 1); luaL_checktype(L, 2, LUA_TFUNCTION);
        if (!name.empty() && name[0] == '/') name.erase(0, 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.commands");
        lua_pushvalue(L, 2); lua_setfield(L, -2, name.c_str());
        return 0;
    }
    static int Players(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto p : Srv(L).Players()) { PushPlayer(L, *p); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    static int GetPlayer(lua_State* L) {
        Player* p = lua_type(L, 1) == LUA_TNUMBER ? Srv(L).Find(static_cast<uint16_t>(lua_tointeger(L, 1))) : Srv(L).FindByName(luaL_checkstring(L, 1));
        if (p) PushPlayer(L, *p); else lua_pushnil(L);
        return 1;
    }
    static int Broadcast(lua_State* L) { Srv(L).Message(nullptr, ColorArg(L, 2, 0xFFFFFFFF), luaL_checkstring(L, 1)); return 0; }
    static int SetTime(lua_State* L) { Srv(L).SetTime(static_cast<float>(luaL_checknumber(L, 1)), lua_toboolean(L, 2)); return 0; }
    static int SetWeather(lua_State* L) {
        const int id = lua_type(L, 1) == LUA_TSTRING && std::strcmp(lua_tostring(L, 1), "auto") == 0 ? -1 : static_cast<int>(luaL_checkinteger(L, 1));
        Srv(L).SetWeather(id); return 0;
    }
    static int HintAll(lua_State* L) {
        Srv(L).Hint(nullptr, luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_optstring(L, 3, ""), lua_toboolean(L, 4)); return 0;
    }
    // Survival-tip style hint (bottom of the screen): a hint without a title.
    // tip(text [, seconds = 5]): fades out by itself, never blocks input.
    static int TipAll(lua_State* L) { Srv(L).Hint(nullptr, "", luaL_checkstring(L, 1), "", false, float(luaL_optnumber(L, 2, 5))); return 0; }
    static int PTip(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Hint(&p, "", luaL_checkstring(L, 2), "", false, float(luaL_optnumber(L, 3, 5))); return 0; }
    static int Now(lua_State* L) { lua_pushnumber(L, Srv(L).Now()); return 1; }
    static int AddTimer(lua_State* L, bool repeat) {
        const double ms = luaL_checknumber(L, 1); luaL_checktype(L, 2, LUA_TFUNCTION);
        if (repeat && ms < 10) luaL_error(L, "every(): interval must be at least 10 ms");
        auto& s = Self(L);
        lua_pushvalue(L, 2);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        const int id = s.nextTimer_++;
        s.timers_.push_back({id, ref, s.server_.Now() + ms / 1000.0, repeat ? ms / 1000.0 : 0});
        lua_pushinteger(L, id); return 1;
    }
    static int After(lua_State* L) { return AddTimer(L, false); }
    static int Every(lua_State* L) { return AddTimer(L, true); }
    static int CancelTimer(lua_State* L) {
        const auto id = static_cast<int>(luaL_checkinteger(L, 1));
        auto& t = Self(L).timers_;
        for (auto& timer : t) if (timer.id == id) timer.due = -1;  // removed after the current tick
        return 0;
    }
    static int SafeLoad(lua_State* L) {
        // Text chunks only: precompiled bytecode can break out of the sandbox.
        size_t n; const char* code = luaL_checklstring(L, 1, &n);
        const char* name = luaL_optstring(L, 2, "=(load)");
        if (luaL_loadbufferx(L, code, n, name, "t") != LUA_OK) { lua_pushnil(L); lua_insert(L, -2); return 2; }
        return 1;
    }

    // ---- player methods ----
    static int PIndex(lua_State* L) {
        const char* key = luaL_checkstring(L, 2);
        auto ref = static_cast<PlayerRef*>(luaL_checkudata(L, 1, kPlayerMeta));
        if (std::strcmp(key, "id") == 0) { lua_pushinteger(L, ref->id); return 1; }
        if (std::strcmp(key, "valid") == 0) { lua_pushboolean(L, ToPlayer(L, 1) != nullptr); return 1; }
        auto p = ToPlayer(L, 1);
        if (std::strcmp(key, "name") == 0) { if (p) lua_pushstring(L, p->nick.c_str()); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "bot") == 0) { lua_pushboolean(L, p && p->bot); return 1; }
        if (std::strcmp(key, "lang") == 0) { lua_pushstring(L, p ? p->lang.c_str() : "en"); return 1; }
        if (std::strcmp(key, "admin") == 0) { lua_pushboolean(L, p && p->admin); return 1; }
        if (std::strcmp(key, "look") == 0) {
            if (!p || p->look.empty()) { lua_pushnil(L); return 1; }
            const auto dot = p->look.rfind('.'); lua_pushstring(L, p->look.substr(dot == std::string::npos ? 0 : dot + 1).c_str()); return 1;
        }
        if (std::strcmp(key, "hasBike") == 0) { lua_pushboolean(L, p && (p->state.flags & net::StateOwnsBike)); return 1; }
        if (std::strcmp(key, "ping") == 0) { lua_pushinteger(L, p && p->peer ? static_cast<lua_Integer>(p->peer->roundTripTime) : 0); return 1; }
        if (std::strcmp(key, "data") == 0) {
            // Per-player table for mode state; dropped when the player leaves.
            lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.playerdata");
            lua_pushinteger(L, ref->id);
            if (lua_gettable(L, -2) != LUA_TTABLE) {
                lua_pop(L, 1); lua_newtable(L);
                lua_pushinteger(L, ref->id); lua_pushvalue(L, -2); lua_settable(L, -4);
            }
            return 1;
        }
        luaL_getmetafield(L, 1, "methods");
        lua_getfield(L, -1, key);
        return 1;
    }
    static int PEq(lua_State* L) {
        auto a = static_cast<PlayerRef*>(luaL_testudata(L, 1, kPlayerMeta)), b = static_cast<PlayerRef*>(luaL_testudata(L, 2, kPlayerMeta));
        lua_pushboolean(L, a && b && a->id == b->id && a->joinedAt == b->joinedAt); return 1;
    }
    static int PToString(lua_State* L) {
        auto ref = static_cast<PlayerRef*>(luaL_checkudata(L, 1, kPlayerMeta));
        auto p = ToPlayer(L, 1);
        lua_pushfstring(L, "player(%d, %s)", ref->id, p ? p->nick.c_str() : "left"); return 1;
    }
    static int PMessage(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Message(&p, ColorArg(L, 3, 0xFFFFFFFF), luaL_checkstring(L, 2)); return 0; }
    static int PPosition(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        lua_pushnumber(L, p.state.x); lua_pushnumber(L, p.state.y); lua_pushnumber(L, p.state.z); lua_pushnumber(L, p.state.yaw); return 4;
    }
    static int PVelocity(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        lua_pushnumber(L, p.state.vx); lua_pushnumber(L, p.state.vy); lua_pushnumber(L, p.state.vz); return 3;
    }
    static int PSetPosition(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Srv(L).Teleport(&p, static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                        static_cast<float>(luaL_optnumber(L, 5, p.state.yaw)));
        return 0;
    }
    static int PIsOnBike(lua_State* L) { lua_pushboolean(L, CheckPlayer(L, 1).state.flags & 1); return 1; }
    static int PKick(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Kick(&p, luaL_optstring(L, 2, "Kicked")); return 0; }
    static int PHint(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Srv(L).Hint(&p, luaL_checkstring(L, 2), luaL_checkstring(L, 3), luaL_optstring(L, 4, ""), lua_toboolean(L, 5)); return 0;
    }

    // ---- inventory, bike, rights ----
    static uint8_t ToolMask(lua_State* L, int idx) {
        if (!lua_istable(L, idx)) return 0;
        uint8_t mask = 0;
        lua_getfield(L, idx, "knife"); if (lua_toboolean(L, -1)) mask |= net::ToolKnife; lua_pop(L, 1);
        lua_getfield(L, idx, "rock"); if (lua_toboolean(L, -1)) mask |= net::ToolRock; lua_pop(L, 1);
        lua_getfield(L, idx, "taser"); if (lua_toboolean(L, -1)) mask |= net::ToolTaser; lua_pop(L, 1);
        return mask;
    }
    static int PGiveItem(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Item(&p, net::ItemAction::Give, luaL_checkstring(L, 2), int(luaL_optinteger(L, 3, 1))); return 0; }
    static int PTakeItem(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Item(&p, net::ItemAction::Take, luaL_checkstring(L, 2), int(luaL_optinteger(L, 3, 1))); return 0; }
    static int PGiveWeapon(lua_State* L) {
        auto& p = CheckPlayer(L, 1); const char* key = luaL_checkstring(L, 2);
        // giveWeapon(key [, ammo] [, {replace = true}]): replace makes the displaced weapon vanish instead of dropping.
        bool replace = false;
        if (lua_istable(L, 4)) { lua_getfield(L, 4, "replace"); replace = lua_toboolean(L, -1); lua_pop(L, 1); }
        Srv(L).Item(&p, replace ? net::ItemAction::GiveReplace : net::ItemAction::Give, key, 1);
        if (!lua_isnoneornil(L, 3)) Srv(L).Item(&p, net::ItemAction::SetAmmo, key, int(luaL_checkinteger(L, 3)));
        return 0;
    }
    static int PSetAmmo(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Item(&p, net::ItemAction::SetAmmo, luaL_checkstring(L, 2), int(luaL_checkinteger(L, 3))); return 0; }
    static int PTakeWeapons(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Item(&p, net::ItemAction::TakeWeapons, "", ToolMask(L, 2)); return 0; }
    static int PTakeTools(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        const uint8_t mask = lua_istable(L, 2) ? ToolMask(L, 2) : uint8_t(net::ToolKnife | net::ToolRock | net::ToolTaser);
        Srv(L).Item(&p, net::ItemAction::TakeTools, "", mask); return 0;
    }
    static int PClearInventory(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Item(&p, net::ItemAction::Clear, "", 0); return 0; }
    static int PInventory(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        lua_createtable(L, 0, int(p.inventory.size()));
        for (auto& [k, v] : p.inventory) { lua_pushinteger(L, v); lua_setfield(L, -2, k.c_str()); }
        return 1;
    }
    // restoreInventory(inv [, {drop = {"kWeaponID_", ...}}]): clear the backpack and give `inv` back,
    // without the keys starting with one of the `drop` prefixes.
    static int PRestoreInventory(lua_State* L) {
        auto& p = CheckPlayer(L, 1); luaL_checktype(L, 2, LUA_TTABLE);
        std::map<std::string, int> inv;
        lua_pushnil(L);
        while (lua_next(L, 2)) { if (lua_type(L, -2) == LUA_TSTRING) inv[lua_tostring(L, -2)] = int(lua_tointeger(L, -1)); lua_pop(L, 1); }
        std::vector<std::string> drop;
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "drop");
            if (lua_istable(L, -1)) {
                for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; ++i) { drop.push_back(lua_tostring(L, -1)); lua_pop(L, 1); }
                lua_pop(L, 1);  // the value that ended the list
            }
            lua_pop(L, 1);
        }
        Srv(L).RestoreInventory(&p, inv, drop); return 0;
    }
    static int PItemCount(lua_State* L) {
        auto& p = CheckPlayer(L, 1); auto it = p.inventory.find(luaL_checkstring(L, 2));
        lua_pushinteger(L, it == p.inventory.end() ? 0 : it->second); return 1;
    }
    static int PGiveBike(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Srv(L).Bike(&p, net::BikeAction::Give, float(luaL_optnumber(L, 2, 0)), float(luaL_optnumber(L, 3, 0)), float(luaL_optnumber(L, 4, 0)), float(luaL_optnumber(L, 5, p.state.yaw)));
        return 0;
    }
    static int PTakeBike(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Bike(&p, net::BikeAction::Take); return 0; }
    // setLook(name or "/Game/..." path): see docs/looks.md
    static int PSetLook(lua_State* L) {
        auto& p = CheckPlayer(L, 1); const std::string want = luaL_checkstring(L, 2);
        const auto path = Srv(L).ResolveLook(want);
        if (path.empty()) luaL_error(L, "setLook: unknown look '%s' (see docs/looks.md)", want.c_str());
        Srv(L).SetLook(&p, path); return 0;
    }
    static int Looks(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto& n : Srv(L).LookNames(luaL_optstring(L, 1, ""))) { lua_pushstring(L, n.c_str()); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    // createMarker{x=, y=, z=, category="task", style="basic", route=false, radius=3 (m), player=p} -> id
    static int CreateMarker(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        auto num = [&](const char* k, double d) { lua_getfield(L, 1, k); const double v = luaL_optnumber(L, -1, d); lua_pop(L, 1); return v; };
        auto str = [&](const char* k, const char* d) { lua_getfield(L, 1, k); std::string v = luaL_optstring(L, -1, d); lua_pop(L, 1); return v; };
        static const char* categories[] = {"story", "camp", "infestation", "ambush", "nero", "killednpc", "nest", "horde", "campmission", "task"};
        static const char* styles[] = {"basic", "kill", "killarea", "defend", "defendarea", "search", "track", "nokill", "nero"};
        Marker m;
        lua_getfield(L, 1, "x"); m.x = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "y"); m.y = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "z"); m.z = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        m.radius = float(num("radius", 3) * 100.0);
        const auto cat = str("category", "task"), sty = str("style", "basic");
        m.category = 255; for (uint8_t i = 0; i < 10; ++i) if (cat == categories[i]) m.category = i;
        m.style = 255; for (uint8_t i = 0; i < 9; ++i) if (sty == styles[i]) m.style = i;
        if (m.category == 255) luaL_error(L, "createMarker: unknown category '%s'", cat.c_str());
        if (m.style == 255) luaL_error(L, "createMarker: unknown style '%s'", sty.c_str());
        lua_getfield(L, 1, "route"); m.route = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "track"); m.track = lua_isnil(L, -1) || lua_toboolean(L, -1); lua_pop(L, 1);
        m.icon = str("icon", ""); m.title = str("title", ""); m.description = str("description", "");
        lua_getfield(L, 1, "player");
        if (!lua_isnil(L, -1)) m.only = CheckPlayer(L, -1).id;
        lua_pop(L, 1);
        const uint16_t id = Srv(L).AddMarker(m);
        // onEnter / onLeave callbacks of this marker live in the registry until it is removed.
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.markers");
        lua_createtable(L, 0, 2);
        lua_getfield(L, 1, "onEnter"); lua_setfield(L, -2, "onEnter");
        lua_getfield(L, 1, "onLeave"); lua_setfield(L, -2, "onLeave");
        lua_rawseti(L, -2, id);
        lua_pop(L, 1);
        lua_pushinteger(L, id); return 1;
    }
    // spawnItem{item=, count=1, x=, y=, z=, yaw=0, player=}: a pickup at that spot (for one player or everyone).
    // Picking it up reports gameEvent{type="pickup", id="dgmp_item_<id>"}. Returns the id.
    static int SpawnItem(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        WorldItem item;
        lua_getfield(L, 1, "item"); item.key = luaL_checkstring(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "count"); item.count = int(luaL_optinteger(L, -1, 1)); lua_pop(L, 1);
        lua_getfield(L, 1, "x"); item.x = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "y"); item.y = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "z"); item.z = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "yaw"); item.yaw = float(luaL_optnumber(L, -1, 0)); lua_pop(L, 1);
        lua_getfield(L, 1, "player"); if (!lua_isnil(L, -1)) item.only = CheckPlayer(L, -1).id; lua_pop(L, 1);
        lua_pushinteger(L, Srv(L).AddWorldItem(item)); return 1;
    }
    static int DespawnItem(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveWorldItem(uint16_t(luaL_checkinteger(L, 1)))); return 1; }
    static int RemoveMarker(lua_State* L) {
        const auto id = uint16_t(luaL_checkinteger(L, 1));
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.markers"); lua_pushnil(L); lua_rawseti(L, -2, id); lua_pop(L, 1);
        lua_pushboolean(L, Srv(L).RemoveMarker(id)); return 1;
    }
    // objective{title=, text=, state="new", category="story", style="basic"} on one player (or everyone via the global)
    static int ObjectiveImpl(lua_State* L, Player* p, int t) {
        luaL_checktype(L, t, LUA_TTABLE);
        auto str = [&](const char* k, const char* d) { lua_getfield(L, t, k); std::string v = luaL_optstring(L, -1, d); lua_pop(L, 1); return v; };
        static const char* states[] = {"update", "new", "travel", "updatedata", "remove", "complete", "failed"};
        static const char* categories[] = {"story", "camp", "infestation", "ambush", "nero", "killednpc", "nest", "horde", "campmission", "task"};
        static const char* styles[] = {"basic", "kill", "killarea", "defend", "defendarea", "search", "track", "nokill", "nero"};
        auto pick = [&](const char* key, const char* def, const char* const* names, int count) {
            const auto v = str(key, def);
            for (int i = 0; i < count; ++i) if (v == names[i]) return uint8_t(i);
            luaL_error(L, "objective: unknown %s '%s'", key, v.c_str()); return uint8_t(0);
        };
        const uint8_t state = pick("state", "new", states, 7), category = pick("category", "story", categories, 10), style = pick("style", "basic", styles, 9);
        lua_getfield(L, t, "xp"); const int xp = int(luaL_optinteger(L, -1, 0)); lua_pop(L, 1);
        Srv(L).Objective(p, state, category, style, str("title", ""), str("text", ""), xp);
        return 0;
    }
    static int PObjective(lua_State* L) { auto& p = CheckPlayer(L, 1); return ObjectiveImpl(L, &p, 2); }
    static int ObjectiveAll(lua_State* L) { return ObjectiveImpl(L, nullptr, 1); }
    // objectiveMeter(title, percent 0..1) / objectiveMeter() hides it
    static int PObjectiveMeter(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        if (lua_isnoneornil(L, 2)) { Srv(L).ObjectiveMeter(&p, false, "", 0); return 0; }
        Srv(L).ObjectiveMeter(&p, true, luaL_checkstring(L, 2), float(luaL_checknumber(L, 3))); return 0;
    }
    static int PSetAdmin(lua_State* L) { CheckPlayer(L, 1).admin = lua_toboolean(L, 2); return 0; }
    static int SetNametags(lua_State* L) {
        const std::string m = luaL_checkstring(L, 1);
        if (m != "visible" && m != "walls" && m != "hidden") luaL_error(L, "setNametags: expected \"visible\", \"walls\" or \"hidden\"");
        Srv(L).SetNametags(m == "walls" ? net::NametagMode::ThroughWalls : m == "hidden" ? net::NametagMode::Hidden : net::NametagMode::Visible);
        return 0;
    }
    static int Sha256(lua_State* L) {
        size_t n; const char* data = luaL_checklstring(L, 1, &n);
        uint8_t digest[32] = {};
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), ULONG(n), digest, sizeof(digest));
        char hex[65]; for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        lua_pushlstring(L, hex, 64); return 1;
    }

    // ---- persistent storage (data/<mode>/storage.json) ----
    static json::Value ToJson(lua_State* L, int idx, int depth) {
        idx = lua_absindex(L, idx);
        switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN: return json::Value(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER: return json::Value(lua_tonumber(L, idx));
        case LUA_TSTRING: return json::Value(std::string(lua_tostring(L, idx)));
        case LUA_TTABLE: {
            if (depth > 32) luaL_error(L, "storage: table nesting too deep");
            const auto n = lua_rawlen(L, idx);
            bool array = n > 0;
            lua_pushnil(L);
            while (array && lua_next(L, idx)) {
                if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_Unsigned(lua_tointeger(L, -2)) > n) array = false;
                lua_pop(L, 1);
                if (!array) lua_pop(L, 1);
            }
            if (array) {
                json::Value a = json::Value::Array();
                for (lua_Unsigned i = 1; i <= n; ++i) { lua_rawgeti(L, idx, lua_Integer(i)); a.push(ToJson(L, -1, depth + 1)); lua_pop(L, 1); }
                return a;
            }
            json::Value o = json::Value::Object();
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) key = lua_tostring(L, -2);
                else if (lua_type(L, -2) == LUA_TNUMBER) { lua_pushvalue(L, -2); key = lua_tostring(L, -1); lua_pop(L, 1); }
                else luaL_error(L, "storage: table keys must be strings or numbers");
                o.set(key, ToJson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return o;
        }
        case LUA_TNIL: case LUA_TNONE: return json::Value();
        default: luaL_error(L, "storage: cannot store a %s", luaL_typename(L, idx)); return json::Value();
        }
    }
    static void PushJson(lua_State* L, const json::Value& v) {
        switch (v.type()) {
        case json::Value::Type::Null: lua_pushnil(L); break;
        case json::Value::Type::Bool: lua_pushboolean(L, v.asBool()); break;
        case json::Value::Type::Number: {
            const double d = v.asNumber();
            if (d == double(lua_Integer(d))) lua_pushinteger(L, lua_Integer(d)); else lua_pushnumber(L, d);
            break;
        }
        case json::Value::Type::String: lua_pushstring(L, v.asString().c_str()); break;
        case json::Value::Type::Array: {
            lua_createtable(L, int(v.size()), 0);
            for (size_t i = 0; i < v.size(); ++i) { PushJson(L, v[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
            break;
        }
        case json::Value::Type::Object: {
            lua_createtable(L, 0, int(v.size()));
            for (auto& [k, item] : v.items()) { PushJson(L, item); lua_setfield(L, -2, k.c_str()); }
            break;
        }
        }
    }
    // ---- quests (quests/<id>.json, written by the launcher's quest editor) ----
    static bool QuestId(const std::string& id) {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
        return true;
    }
    static int QuestsLoad(lua_State* L) {
        const std::string id = luaL_checkstring(L, 1);
        if (!QuestId(id)) { lua_pushnil(L); return 1; }
        std::ifstream in(Srv(L).root() / "quests" / (id + ".json"), std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        json::Value v;
        if (!in.good() && ss.str().empty()) { lua_pushnil(L); return 1; }
        if (!json::Value::Parse(ss.str(), v) || !v.isObject()) { Srv(L).Log("quests/%s.json is not valid JSON", id.c_str()); lua_pushnil(L); return 1; }
        PushJson(L, v);
        lua_pushstring(L, id.c_str()); lua_setfield(L, -2, "id");
        return 1;
    }
    static int QuestsList(lua_State* L) {
        lua_newtable(L);
        std::error_code ec; lua_Integer n = 0;
        for (auto it = std::filesystem::directory_iterator(Srv(L).root() / "quests", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".json") { lua_pushstring(L, it->path().stem().string().c_str()); lua_rawseti(L, -2, ++n); }
        return 1;
    }
    static int StorageGet(lua_State* L) { PushJson(L, Self(L).storage_[luaL_checkstring(L, 1)]); return 1; }
    static int StorageSet(lua_State* L) {
        auto& s = Self(L); const std::string key = luaL_checkstring(L, 1);
        if (lua_isnoneornil(L, 2)) s.storage_.erase(key); else s.storage_.set(key, ToJson(L, 2, 0));
        s.storageDirty_ = true; return 0;
    }
    static int StorageKeys(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto& [k, v] : Self(L).storage_.items()) { lua_pushstring(L, k.c_str()); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    static int StorageSave(lua_State* L) { Self(L).SaveStorage(); return 0; }
};

Script::Script(Server& server) : server_(server) {}
Script::~Script() { Unload(); }

std::filesystem::file_time_type Script::NewestWrite() const {
    std::filesystem::file_time_type newest{};
    std::error_code ec;
    // The mode and the shared library it requires (modes/_lib).
    for (const auto& root : {dir_, dir_.parent_path() / "_lib"}) {
        ec.clear();
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const auto ext = it->path().extension();
            if (ext == ".lua" || ext == ".ini") newest = std::max(newest, it->last_write_time(ec));
        }
    }
    return newest;
}

bool Script::ChangedOnDisk() const {
    if (dir_.empty()) return false;
    return NewestWrite() != loadedWrite_;
}

bool Script::Load(const std::string& modeId) {
    Unload();
    mode_ = modeId; title_ = modeId;
    const bool safeId = !modeId.empty() && std::all_of(modeId.begin(), modeId.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    dir_ = server_.root() / "modes" / modeId;
    if (!safeId || !std::filesystem::exists(dir_ / "main.lua")) {
        server_.Log("Mode \"%s\" not found (expected modes/%s/main.lua)", modeId.c_str(), modeId.c_str());
        loadedWrite_ = safeId ? NewestWrite() : std::filesystem::file_time_type{};
        return false;
    }
    loadedWrite_ = NewestWrite();
    if (auto name = IniValue(dir_ / "meta.ini", "Mode", "Name"); !name.empty()) title_ = name;

    L_ = luaL_newstate();
    *static_cast<Script**>(lua_getextraspace(L_)) = this;
    // Sandbox: no io, no native modules, no bytecode, os reduced to clock/date.
    for (auto lib : {std::pair<const char*, lua_CFunction>{LUA_GNAME, luaopen_base}, {LUA_LOADLIBNAME, luaopen_package}, {LUA_COLIBNAME, luaopen_coroutine},
                     {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
                     {LUA_OSLIBNAME, luaopen_os}}) { luaL_requiref(L_, lib.first, lib.second, 1); lua_pop(L_, 1); }
    lua_getglobal(L_, "os");
    for (const char* banned : {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"}) { lua_pushnil(L_); lua_setfield(L_, -2, banned); }
    lua_pop(L_, 1);
    for (const char* banned : {"dofile", "loadfile"}) { lua_pushnil(L_); lua_setglobal(L_, banned); }
    lua_pushcfunction(L_, &Api::SafeLoad); lua_setglobal(L_, "load");
    lua_getglobal(L_, "package");
    const auto modeDir = dir_.u8string(), libDir = (server_.root() / "modes" / "_lib").u8string();
    const std::string path = std::string(modeDir.begin(), modeDir.end()) + "\\?.lua;" + std::string(libDir.begin(), libDir.end()) + "\\?.lua";
    lua_pushstring(L_, path.c_str()); lua_setfield(L_, -2, "path");
    lua_pushstring(L_, ""); lua_setfield(L_, -2, "cpath");
    lua_pushnil(L_); lua_setfield(L_, -2, "loadlib");
    lua_getfield(L_, -1, "searchers");  // keep preload + Lua searchers only
    lua_pushnil(L_); lua_rawseti(L_, -2, 4); lua_pushnil(L_); lua_rawseti(L_, -2, 3);
    lua_pop(L_, 2);

    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.events");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.commands");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.playerdata");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.markers");

    luaL_newmetatable(L_, kPlayerMeta);
    lua_pushcfunction(L_, &Api::PIndex); lua_setfield(L_, -2, "__index");
    lua_pushcfunction(L_, &Api::PEq); lua_setfield(L_, -2, "__eq");
    lua_pushcfunction(L_, &Api::PToString); lua_setfield(L_, -2, "__tostring");
    const luaL_Reg methods[] = {{"message", &Api::PMessage}, {"position", &Api::PPosition}, {"velocity", &Api::PVelocity}, {"setPosition", &Api::PSetPosition},
                                {"isOnBike", &Api::PIsOnBike}, {"kick", &Api::PKick}, {"hint", &Api::PHint},
                                {"giveItem", &Api::PGiveItem}, {"takeItem", &Api::PTakeItem}, {"giveWeapon", &Api::PGiveWeapon}, {"setAmmo", &Api::PSetAmmo},
                                {"takeWeapons", &Api::PTakeWeapons}, {"takeTools", &Api::PTakeTools}, {"clearInventory", &Api::PClearInventory},
                                {"inventory", &Api::PInventory}, {"itemCount", &Api::PItemCount}, {"giveBike", &Api::PGiveBike}, {"takeBike", &Api::PTakeBike},
                                {"setAdmin", &Api::PSetAdmin}, {"objective", &Api::PObjective}, {"objectiveMeter", &Api::PObjectiveMeter}, {"setLook", &Api::PSetLook}, {"tip", &Api::PTip}, {"restoreInventory", &Api::PRestoreInventory}, {nullptr, nullptr}};
    luaL_newlib(L_, methods); lua_setfield(L_, -2, "methods");
    lua_pop(L_, 1);

    const luaL_Reg globals[] = {{"print", &Api::Log}, {"log", &Api::Log}, {"on", &Api::On}, {"command", &Api::Command}, {"players", &Api::Players},
                                {"getPlayer", &Api::GetPlayer}, {"broadcast", &Api::Broadcast}, {"setTime", &Api::SetTime}, {"setWeather", &Api::SetWeather},
                                {"hint", &Api::HintAll}, {"now", &Api::Now}, {"after", &Api::After}, {"every", &Api::Every}, {"cancelTimer", &Api::CancelTimer},
                                {"setNametags", &Api::SetNametags}, {"objective", &Api::ObjectiveAll}, {"createMarker", &Api::CreateMarker}, {"removeMarker", &Api::RemoveMarker}, {"spawnItem", &Api::SpawnItem}, {"despawnItem", &Api::DespawnItem}, {"looks", &Api::Looks}, {"tip", &Api::TipAll}, {"sha256", &Api::Sha256}, {nullptr, nullptr}};
    lua_pushglobaltable(L_); luaL_setfuncs(L_, globals, 0); lua_pop(L_, 1);
    const luaL_Reg storage[] = {{"get", &Api::StorageGet}, {"set", &Api::StorageSet}, {"keys", &Api::StorageKeys}, {"save", &Api::StorageSave}, {nullptr, nullptr}};
    luaL_newlib(L_, storage); lua_setglobal(L_, "storage");
    const luaL_Reg quests[] = {{"load", &Api::QuestsLoad}, {"list", &Api::QuestsList}, {nullptr, nullptr}};
    luaL_newlib(L_, quests); lua_setglobal(L_, "quests");

    // Persistent storage survives reloads and restarts: data/<mode>/storage.json
    storageFile_ = server_.root() / "data" / mode_ / "storage.json";
    storage_ = json::Value::Object(); storageDirty_ = false;
    {
        std::ifstream in(storageFile_, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
        json::Value v;
        if (!ss.str().empty() && (!json::Value::Parse(ss.str(), v) || !v.isObject())) server_.Log("[%s] storage.json is corrupt; starting empty (file kept)", mode_.c_str());
        else if (v.isObject()) storage_ = v;
    }

    const auto& c = server_.config();
    lua_newtable(L_);
    lua_pushstring(L_, c.name.c_str()); lua_setfield(L_, -2, "name");
    lua_pushstring(L_, mode_.c_str()); lua_setfield(L_, -2, "mode");
    lua_pushinteger(L_, c.maxPlayers); lua_setfield(L_, -2, "maxPlayers");
    lua_newtable(L_);
    lua_pushnumber(L_, c.spawnX); lua_setfield(L_, -2, "x"); lua_pushnumber(L_, c.spawnY); lua_setfield(L_, -2, "y");
    lua_pushnumber(L_, c.spawnZ); lua_setfield(L_, -2, "z"); lua_pushnumber(L_, c.spawnYaw); lua_setfield(L_, -2, "yaw");
    lua_setfield(L_, -2, "spawn");
    lua_setglobal(L_, "server");

    lua_sethook(L_, &BudgetHook, LUA_MASKCOUNT, 100000);
    const auto mainFile = (dir_ / "main.lua").u8string();
    g_deadline = Clock() + kCallBudget;
    const int status = luaL_loadfilex(L_, reinterpret_cast<const char*>(mainFile.c_str()), "t");
    const int run = status == LUA_OK ? lua_pcall(L_, 0, 0, 0) : status;
    g_deadline = 0;
    if (run != LUA_OK) {
        server_.Log("[%s] load error: %s", mode_.c_str(), lua_tostring(L_, -1));
        lua_close(L_); L_ = nullptr; timers_.clear();
        return false;
    }
    server_.Log("Mode \"%s\" (%s) loaded", title_.c_str(), mode_.c_str());
    Fire("start", 0);
    // Players already on the server (after a reload) are announced again so the mode can rebuild state.
    for (auto p : server_.Players()) PlayerJoin(*p);
    return true;
}

void Script::SaveStorage() {
    if (!storageDirty_ || storageFile_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(storageFile_.parent_path(), ec);
    const auto tmp = storageFile_.wstring() + L".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << storage_.dump(); if (!out) { server_.Log("[%s] cannot write storage", mode_.c_str()); return; } }
    MoveFileExW(tmp.c_str(), storageFile_.c_str(), MOVEFILE_REPLACE_EXISTING);
    storageDirty_ = false;
}

void Script::MarkerEvent(Player& p, uint16_t marker, bool entered) {
    if (!L_) return;
    // The marker's own callback first, then the global event.
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.markers");
    lua_rawgeti(L_, -1, marker);
    if (lua_istable(L_, -1) && lua_getfield(L_, -1, entered ? "onEnter" : "onLeave") == LUA_TFUNCTION) {
        Api::PushPlayer(L_, p); lua_pushinteger(L_, marker);
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L_, 2, 0, 0) != LUA_OK) { server_.Log("[%s] marker %u callback error: %s", mode_.c_str(), marker, lua_tostring(L_, -1)); lua_pop(L_, 1); }
        g_deadline = 0;
    } else lua_pop(L_, 1);
    lua_pop(L_, 2);
    Api::PushPlayer(L_, p); lua_pushinteger(L_, marker);
    Fire(entered ? "markerEnter" : "markerLeave", 2);
}

void Script::GameEvent(Player& p, const struct GameEvent& e) {
    if (!L_) return;
    static const char* types[] = {"", "pickup", "search", "body", "interact", "door", "explosion", "fire", "carry"};
    Api::PushPlayer(L_, p);
    lua_createtable(L_, 0, 7);
    lua_pushstring(L_, types[e.type]); lua_setfield(L_, -2, "type");
    lua_pushstring(L_, e.cls.c_str()); lua_setfield(L_, -2, "class");
    if (!e.detail.empty()) { lua_pushstring(L_, e.detail.c_str()); lua_setfield(L_, -2, "item"); }
    lua_pushstring(L_, e.id.c_str()); lua_setfield(L_, -2, "id");
    lua_pushnumber(L_, e.x); lua_setfield(L_, -2, "x");
    lua_pushnumber(L_, e.y); lua_setfield(L_, -2, "y");
    lua_pushnumber(L_, e.z); lua_setfield(L_, -2, "z");
    Fire("gameEvent", 2);
}

bool Script::PlayerDeath(Player& p) {
    if (!L_) return false;
    int result = 0;
    Api::PushPlayer(L_, p);
    lua_createtable(L_, 0, int(p.inventoryAtDeath.size()));
    for (auto& [k, v] : p.inventoryAtDeath) { lua_pushinteger(L_, v); lua_setfield(L_, -2, k.c_str()); }
    Fire("playerDeath", 2, &result);
    return (result & 2) != 0;
}

bool Script::PlayerRespawn(Player& p) {
    if (!L_) return false;
    int result = 0;
    Api::PushPlayer(L_, p); Fire("playerRespawn", 1, &result);
    return (result & 2) != 0;
}

void Script::InventoryChange(Player& p) {
    if (!L_) return;
    Api::PushPlayer(L_, p); Fire("inventoryChange", 1);
}

void Script::Unload() {
    if (!L_) return;
    Fire("stop", 0);
    SaveStorage();
    for (auto& t : timers_) luaL_unref(L_, LUA_REGISTRYINDEX, t.ref);
    timers_.clear();
    lua_close(L_); L_ = nullptr;
}

// Calls every handler of `event` with the nargs values on top of the stack (they are consumed).
// cancelResult: set to 1 if any handler returned exactly false, 2 if any returned true.
void Script::Fire(const char* event, int nargs, int* cancelResult) {
    if (!L_) return;
    const int base = lua_gettop(L_) - nargs;
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.events");
    if (lua_getfield(L_, -1, event) == LUA_TTABLE) {
        const auto n = static_cast<int>(lua_rawlen(L_, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L_, -1, i);
            for (int a = 1; a <= nargs; ++a) lua_pushvalue(L_, base + a);
            g_deadline = Clock() + kCallBudget;
            if (lua_pcall(L_, nargs, 1, 0) != LUA_OK) {
                server_.Log("[%s] error in %s handler: %s", mode_.c_str(), event, lua_tostring(L_, -1));
            } else if (cancelResult && lua_isboolean(L_, -1)) {
                *cancelResult |= lua_toboolean(L_, -1) ? 2 : 1;
            }
            g_deadline = 0;
            lua_pop(L_, 1);
        }
    }
    lua_settop(L_, base);
}

void Script::PlayerJoin(Player& p) {
    if (!L_) return;
    Api::PushPlayer(L_, p); Fire("playerJoin", 1);
}

void Script::PlayerQuit(Player& p, uint8_t reason) {
    if (!L_) return;
    Api::PushPlayer(L_, p); lua_pushstring(L_, kQuitNames[std::min<uint8_t>(reason, 2)]);
    // The player is still registered while the handlers run, so methods keep working.
    Fire("playerQuit", 2);
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.playerdata");
    lua_pushinteger(L_, p.id); lua_pushnil(L_); lua_settable(L_, -3);
    lua_pop(L_, 1);
}

bool Script::PlayerChat(Player& p, const std::string& text) {
    if (!L_) return true;
    int result = 0;
    Api::PushPlayer(L_, p); lua_pushstring(L_, text.c_str());
    Fire("chat", 2, &result);
    return !(result & 1);
}

bool Script::Command(Player& p, const std::string& name, const std::string& args) {
    if (!L_) return false;
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.commands");
    if (lua_getfield(L_, -1, key.c_str()) != LUA_TFUNCTION) { lua_pop(L_, 2); return false; }
    Api::PushPlayer(L_, p);
    lua_pushstring(L_, args.c_str());
    // Words are passed too: command("tp", function(player, args, x, y, z) ... end)
    int words = 0;
    for (size_t i = 0; i < args.size() && words < 16;) {
        while (i < args.size() && args[i] == ' ') ++i;
        size_t j = i; while (j < args.size() && args[j] != ' ') ++j;
        if (j > i) { lua_pushlstring(L_, args.data() + i, j - i); ++words; }
        i = j;
    }
    g_deadline = Clock() + kCallBudget;
    if (lua_pcall(L_, 2 + words, 0, 0) != LUA_OK) {
        server_.Log("[%s] error in /%s: %s", mode_.c_str(), key.c_str(), lua_tostring(L_, -1));
        lua_pop(L_, 1);
        server_.Message(&p, 0xE07070FF, "Command failed: /" + key);
    }
    g_deadline = 0;
    lua_pop(L_, 1);
    return true;
}

bool Script::Console(const std::string& line) {
    if (!L_) return false;
    int result = 0;
    lua_pushstring(L_, line.c_str());
    Fire("console", 1, &result);
    return result & 2;
}

void Script::Tick(float dt) {
    if (!L_) return;
    const double now = server_.Now();
    if (storageDirty_ && now >= nextStorageSave_) { nextStorageSave_ = now + 5.0; SaveStorage(); }
    // Copy due timers first: callbacks may add or cancel timers.
    std::vector<Timer> due;
    for (auto& t : timers_) if (t.due >= 0 && t.due <= now) due.push_back(t);
    for (auto& t : due) {
        auto live = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& x) { return x.id == t.id; });
        if (live == timers_.end() || live->due < 0) continue;
        if (live->every > 0) live->due = now + live->every; else live->due = -1;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, t.ref);
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK) { server_.Log("[%s] timer error: %s", mode_.c_str(), lua_tostring(L_, -1)); lua_pop(L_, 1); }
        g_deadline = 0;
        if (!L_) return;
    }
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& t) {
        if (t.due >= 0) return false;
        luaL_unref(L_, LUA_REGISTRYINDEX, t.ref); return true;
    }), timers_.end());
    lua_pushnumber(L_, dt);
    Fire("tick", 1);
}

}  // namespace srv
