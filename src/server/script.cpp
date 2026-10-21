#include "script.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "platform.h"
#include <enet/enet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
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
        if (std::strcmp(key, "health") == 0) { lua_pushinteger(L, p ? p->state.health : 0); return 1; }
        if (std::strcmp(key, "spectating") == 0) { lua_pushboolean(L, p && p->spectating); return 1; }
        if (std::strcmp(key, "world") == 0) { lua_pushinteger(L, p ? p->world : 0); return 1; }
        if (std::strcmp(key, "vehicle") == 0) { if (p && (p->vehicle || p->passengerOf)) lua_pushinteger(L, p->vehicle ? p->vehicle : p->passengerOf); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "money") == 0) { lua_pushinteger(L, p ? lua_Integer(p->money) : 0); return 1; }
        if (std::strcmp(key, "carrying") == 0) { if (p && p->carrying) lua_pushinteger(L, p->carrying); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "passenger") == 0) { lua_pushboolean(L, p && p->passengerOf); return 1; }
        if (std::strcmp(key, "skin") == 0) { if (p && !p->skin.empty()) lua_pushstring(L, p->skin.c_str()); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "uniform") == 0) { if (p && !p->uniform.empty()) lua_pushstring(L, p->uniform.c_str()); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "admin") == 0) { lua_pushboolean(L, p && p->admin); return 1; }
        if (std::strcmp(key, "anim") == 0) {
            if (!p || p->anim.empty()) { lua_pushnil(L); return 1; }
            const auto dot = p->anim.rfind('.'); lua_pushstring(L, p->anim.substr(dot == std::string::npos ? 0 : dot + 1).c_str()); return 1;
        }
        if (std::strcmp(key, "steamId") == 0) { if (p && p->steamId) lua_pushstring(L, std::to_string(p->steamId).c_str()); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "verified") == 0) { lua_pushboolean(L, p && p->steamId); return 1; }
        if (std::strcmp(key, "ip") == 0) { lua_pushstring(L, p ? p->ip.c_str() : ""); return 1; }
        if (std::strcmp(key, "pvp") == 0) { if (p && p->pvp >= 0) lua_pushboolean(L, p->pvp); else lua_pushnil(L); return 1; }
        if (std::strcmp(key, "zone") == 0) {  // the zone deciding its PvP (a PvE one first), or nil
            const Zone* z = p ? Srv(L).RuleZone(*p) : nullptr;
            if (z) lua_pushstring(L, z->name.c_str()); else lua_pushnil(L);
            return 1;
        }
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
    static int PDamage(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Player* attacker = lua_isnoneornil(L, 3) ? nullptr : &CheckPlayer(L, 3);
        Srv(L).Damage(&p, float(luaL_checknumber(L, 2)), attacker);
        return 0;
    }
    // playSound(name or path [, {x, y, z, volume = 1, pitch = 1, range = 100 (m), world, player}]); player:playSound(name [, opts])
    static int PlaySoundImpl(lua_State* L, Player* to, int optIdx) {
        char want[256]; std::snprintf(want, sizeof(want), "%s", luaL_checkstring(L, optIdx - 1));
        float at[3] = {0, 0, 0}; bool threeD = false; double volume = 1, pitch = 1, range = 100; int world = -1;
        if (lua_istable(L, optIdx)) {
            lua_getfield(L, optIdx, "x");
            if (!lua_isnil(L, -1)) {
                threeD = true; at[0] = float(luaL_checknumber(L, -1));
                lua_getfield(L, optIdx, "y"); at[1] = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
                lua_getfield(L, optIdx, "z"); at[2] = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
            }
            lua_pop(L, 1);
            lua_getfield(L, optIdx, "volume"); volume = luaL_optnumber(L, -1, 1); lua_pop(L, 1);
            lua_getfield(L, optIdx, "pitch"); pitch = luaL_optnumber(L, -1, 1); lua_pop(L, 1);
            lua_getfield(L, optIdx, "range"); range = luaL_optnumber(L, -1, 100); lua_pop(L, 1);
            lua_getfield(L, optIdx, "world"); world = int(luaL_optinteger(L, -1, -1)); lua_pop(L, 1);
            lua_getfield(L, optIdx, "player"); if (!lua_isnil(L, -1) && !to) to = &CheckPlayer(L, lua_gettop(L)); lua_pop(L, 1);
        }
        const std::string path = Srv(L).ResolveSound(want);
        if (path.empty()) { char msg[512]; std::snprintf(msg, sizeof(msg), "playSound: unknown sound '%s' (a name of data/sounds.txt or a path)", want); lua_pushstring(L, msg); return lua_error(L); }
        Srv(L).PlaySound(to, path, threeD ? at : nullptr, float(volume), float(pitch), float(range * 100), world);
        return 0;
    }
    static int PlaySoundAll(lua_State* L) { return PlaySoundImpl(L, nullptr, 2); }
    static int PPlaySound(lua_State* L) { auto& p = CheckPlayer(L, 1); return PlaySoundImpl(L, &p, 3); }
    // playAnim(name or path [, {loop = true, freeze = true, stopOnMove = true, rate = 1}]) / stopAnim()
    static int PPlayAnim(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        char want[256]; std::snprintf(want, sizeof(want), "%s", luaL_checkstring(L, 2));
        bool loop = true, freeze = true, stopOnMove = true; double rate = 1;
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "loop"); if (!lua_isnil(L, -1)) loop = lua_toboolean(L, -1); lua_pop(L, 1);
            lua_getfield(L, 3, "freeze"); if (!lua_isnil(L, -1)) freeze = lua_toboolean(L, -1); lua_pop(L, 1);
            lua_getfield(L, 3, "stopOnMove"); if (!lua_isnil(L, -1)) stopOnMove = lua_toboolean(L, -1); lua_pop(L, 1);
            lua_getfield(L, 3, "rate"); rate = luaL_optnumber(L, -1, 1); lua_pop(L, 1);
        }
        const std::string path = Srv(L).ResolveAnim(want);
        if (path.empty()) { char msg[512]; std::snprintf(msg, sizeof(msg), "playAnim: unknown animation '%s' (a name of data/anims.txt or a path)", want); lua_pushstring(L, msg); return lua_error(L); }
        Srv(L).PlayAnim(&p, path, uint8_t((loop ? net::AnimLoop : 0) | (freeze ? net::AnimFreeze : 0) | (stopOnMove ? net::AnimStopOnMove : 0)), float(rate));
        return 0;
    }
    static int PStopAnim(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).PlayAnim(&p, "", 0, 1); return 0; }
    // setHealth(percent): 0..100 of the maximum; 0 kills. heal(): full health. kill(): dies (playerDeath).
    static int PSetHealth(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetHealth(&p, float(luaL_checknumber(L, 2))); return 0; }
    static int PHeal(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetHealth(&p, float(luaL_optnumber(L, 2, 100))); return 0; }
    static int PKill(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetHealth(&p, 0); return 0; }
    static int PKick(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Kick(&p, luaL_optstring(L, 2, "Kicked")); return 0; }
    // player:setTime(hours [, paused]) / player:setWeather(id or "auto"): only this player's sky.
    static int PSetTime(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetPlayerTime(&p, float(luaL_checknumber(L, 2)), lua_toboolean(L, 3)); return 0; }
    static int PSetWeather(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        const int id = lua_type(L, 2) == LUA_TSTRING && std::strcmp(lua_tostring(L, 2), "auto") == 0 ? -1 : static_cast<int>(luaL_checkinteger(L, 2));
        Srv(L).SetPlayerWeather(&p, id); return 0;
    }
    // player:ban([reason [, minutes]]): by its Steam account and address; minutes nil or 0: for ever.
    static int PBan(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Srv(L).BanPlayer(&p, luaL_optstring(L, 2, ""), int(luaL_optinteger(L, 3, 0)), "mode " + Self(L).mode());
        return 0;
    }
    // ban(target [, minutes [, reason]]): a player, SteamID, address or nickname (an online player
    // with that account, address or name is kicked).
    static int BanGlobal(lua_State* L) {
        const int minutes = int(luaL_optinteger(L, 2, 0));
        const std::string reason = luaL_optstring(L, 3, "");
        auto& s = Srv(L);
        const std::string by = "mode " + Self(L).mode();
        if (luaL_testudata(L, 1, kPlayerMeta)) { s.BanPlayer(&CheckPlayer(L, 1), reason, minutes, by); return 0; }
        const std::string who = luaL_checkstring(L, 1);
        uint64_t sid = 0;
        const bool isId = ParseSteamId(who, sid), isIp = !isId && LooksLikeIp(who);
        for (auto p : s.Players())
            if ((isId && (p->steamId == sid || p->claimedSteamId == sid)) || (isIp && p->ip == who) || (!isId && !isIp && StrICmp(p->nick.c_str(), who.c_str()) == 0)) {
                s.BanPlayer(p, reason, minutes, by); return 0;
            }
        Ban b; b.reason = reason; b.by = by;
        b.until = minutes > 0 ? int64_t(std::time(nullptr)) + int64_t(minutes) * 60 : 0;
        if (isId) b.steamId = who; else if (isIp) b.ip = who; else b.nick = who;
        s.AddBan(b);
        return 0;
    }
    static int Unban(lua_State* L) { lua_pushinteger(L, Srv(L).Unban(luaL_checkstring(L, 1))); return 1; }
    static int Bans(lua_State* L) {
        const auto& list = Srv(L).Bans();
        lua_createtable(L, int(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i) {
            const Ban& b = list[i];
            lua_createtable(L, 0, 7);
            if (!b.steamId.empty()) { lua_pushstring(L, b.steamId.c_str()); lua_setfield(L, -2, "steamId"); }
            if (!b.ip.empty()) { lua_pushstring(L, b.ip.c_str()); lua_setfield(L, -2, "ip"); }
            if (!b.nick.empty()) { lua_pushstring(L, b.nick.c_str()); lua_setfield(L, -2, "name"); }
            lua_pushstring(L, b.reason.c_str()); lua_setfield(L, -2, "reason");
            lua_pushstring(L, b.by.c_str()); lua_setfield(L, -2, "by");
            lua_pushinteger(L, lua_Integer(b.at)); lua_setfield(L, -2, "at");
            lua_pushinteger(L, lua_Integer(b.until)); lua_setfield(L, -2, "expires");
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    }
    // protectItems({"kWeaponID_", "kAmmoID_9mm"}): key prefixes only the server hands out; nil or {}: none.
    static int ProtectItems(lua_State* L) {
        std::vector<std::string> list;
        if (lua_istable(L, 1)) for (lua_Integer i = 1; lua_rawgeti(L, 1, i) == LUA_TSTRING; ++i) { list.push_back(lua_tostring(L, -1)); lua_pop(L, 1); }
        else if (lua_isstring(L, 1)) list.push_back(lua_tostring(L, 1));
        Srv(L).ProtectItems(list);
        return 0;
    }
    static int ProtectedItems(lua_State* L) {
        const auto& list = Srv(L).ProtectedItems();
        lua_createtable(L, int(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i) { lua_pushstring(L, list[i].c_str()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
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
    // putOnBike(x, y, z, yaw): the player's bike to that spot and the player onto it.
    static int PPutOnBike(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        const float x = float(luaL_checknumber(L, 2)), y = float(luaL_checknumber(L, 3)), z = float(luaL_checknumber(L, 4)), yaw = float(luaL_optnumber(L, 5, p.state.yaw));
        const float side = (yaw + 90.f) * 0.0174532925f;  // stand next to it until seated
        Srv(L).Teleport(&p, x + std::cos(side) * 150.f, y + std::sin(side) * 150.f, z + 100.f, yaw);
        Srv(L).Bike(&p, net::BikeAction::Mount, x, y, z, yaw);
        return 0;
    }
    // spectate(on [, {players or ids}])
    static int PSpectate(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        const bool on = lua_isnone(L, 2) || lua_toboolean(L, 2);
        std::vector<uint16_t> only;
        if (lua_istable(L, 3)) {
            const auto n = lua_rawlen(L, 3);
            for (lua_Unsigned i = 1; i <= n; ++i) {
                lua_rawgeti(L, 3, lua_Integer(i));
                if (lua_isnumber(L, -1)) only.push_back(uint16_t(lua_tointeger(L, -1)));
                else if (lua_istable(L, -1) || lua_isuserdata(L, -1)) { Player* o = &CheckPlayer(L, -1); only.push_back(o->id); }
                lua_pop(L, 1);
            }
        }
        Srv(L).Spectate(&p, on, only);
        return 0;
    }
    // Skins (docs/API.md): player.skin / player.uniform; setSkin / setUniform take an id of the mode's
    // skins.json (made in the editor), a look name or path, a "skin:" string, or nil;
    // customize{title=, cancel=true, list="skins"|"uniforms"|{ids}, uniform=, full=, families=, place=, back=true,
    //           onDone=function(player, look, id) end}; place: nil = the mode's "wardrobe" point, false = here, {x,y,z,yaw};
    // skinList("skins"|"uniforms") -> {{id=, name=, nameRu=, look=}, ...}
    static std::string LookArg(lua_State* L, int i, const char* fn, const char* firstList) {
        if (lua_isnoneornil(L, i)) return {};
        const std::string want = luaL_checkstring(L, i);
        if (want.rfind("skin:", 0) == 0) { if (want.size() >= net::kLookPathMax) luaL_error(L, "%s: skin too long", fn); return want; }
        for (const char* kind : {firstList, std::strcmp(firstList, "skins") == 0 ? "uniforms" : "skins"})
            if (auto c = Srv(L).FindSkin(kind, want)) return c->look;
        const auto path = Srv(L).ResolveLook(want);
        if (path.empty()) luaL_error(L, "%s: unknown look '%s' (see docs/looks.md)", fn, want.c_str());
        return path;
    }
    static int PSetSkin(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetSkin(&p, LookArg(L, 2, "setSkin", "skins")); return 0; }
    static int PSetUniform(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetUniform(&p, LookArg(L, 2, "setUniform", "uniforms")); return 0; }
    static int SkinList(lua_State* L) {
        const std::string kind = luaL_optstring(L, 1, "skins");
        const auto& list = Srv(L).SkinList(kind);
        lua_createtable(L, int(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i) {
            lua_createtable(L, 0, 4);
            lua_pushstring(L, list[i].id.c_str()); lua_setfield(L, -2, "id");
            lua_pushstring(L, (list[i].en.empty() ? list[i].id : list[i].en).c_str()); lua_setfield(L, -2, "name");
            lua_pushstring(L, (list[i].ru.empty() ? list[i].en : list[i].ru).c_str()); lua_setfield(L, -2, "nameRu");
            lua_pushstring(L, list[i].look.c_str()); lua_setfield(L, -2, "look");
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    }
    static int PCustomize(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        std::string title, families; bool cancel = true;
        // The mode's start skins by default; the full creator when the mode has none (or full=true).
        std::string listName = "skins"; std::vector<std::string> ids; bool full = false, uniformSet = false, uniform = false, back = true;
        Server::Spot place = Srv(L).Wardrobe();
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.customize");
        if (lua_istable(L, -1)) { lua_pushnil(L); lua_rawseti(L, -2, p.id); }  // an older onDone is not called
        lua_pop(L, 1);
        if (lua_istable(L, 2)) {
            if (lua_getfield(L, 2, "full") == LUA_TBOOLEAN) full = lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "uniform") == LUA_TBOOLEAN) { uniformSet = true; uniform = lua_toboolean(L, -1); }
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "back") == LUA_TBOOLEAN) back = lua_toboolean(L, -1);
            lua_pop(L, 1);
            const int pt = lua_getfield(L, 2, "place");
            if (pt == LUA_TBOOLEAN && !lua_toboolean(L, -1)) place = {};
            else if (pt == LUA_TTABLE) {
                auto num = [&](const char* k) { lua_getfield(L, -1, k); const float v = float(lua_tonumber(L, -1)); lua_pop(L, 1); return v; };
                place = {true, num("x"), num("y"), num("z"), num("yaw")};
            } else if (pt != LUA_TNIL && pt != LUA_TBOOLEAN) luaL_error(L, "customize: place must be a point {x, y, z, yaw} or false");
            lua_pop(L, 1);
            const int t = lua_getfield(L, 2, "list");
            if (t == LUA_TSTRING) listName = lua_tostring(L, -1);
            else if (t == LUA_TTABLE) { listName.clear(); for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; ++i) { ids.push_back(lua_tostring(L, -1)); lua_pop(L, 1); } lua_pop(L, 1); }
            else if (t != LUA_TNIL) luaL_error(L, "customize: list must be \"skins\", \"uniforms\" or a table of ids");
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "title") == LUA_TSTRING) title = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "cancel") == LUA_TBOOLEAN) cancel = lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (lua_getfield(L, 2, "families") == LUA_TTABLE) {
                for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; ++i) { if (!families.empty()) families += ","; families += lua_tostring(L, -1); lua_pop(L, 1); }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            // onDone is kept until the player closes the creator.
            lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.customize");
            if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, LUA_REGISTRYINDEX, "dgmp.customize"); }
            lua_getfield(L, 2, "onDone");
            lua_rawseti(L, -2, p.id);
            lua_pop(L, 1);
        }
        std::vector<const Server::SkinChoice*> choices;
        if (!full) {
            if (!listName.empty()) {
                if (listName != "skins" && listName != "uniforms") luaL_error(L, "customize: unknown list '%s' (\"skins\" or \"uniforms\")", listName.c_str());
                for (auto& c : Srv(L).SkinList(listName)) choices.push_back(&c);
                if (!uniformSet) uniform = listName == "uniforms";
            } else {
                for (auto& id : ids) {
                    const Server::SkinChoice* c = Srv(L).FindSkin("skins", id);
                    if (!c) { c = Srv(L).FindSkin("uniforms", id); if (c && !uniformSet) uniform = true; }
                    if (!c) luaL_error(L, "customize: no skin '%s' in the mode's skins.json", id.c_str());
                    choices.push_back(c);
                }
            }
            if (choices.empty() && listName == "uniforms") luaL_error(L, "customize: the mode has no uniforms (editor: Characters)");
        }
        Srv(L).Customize(&p, cancel, title, families, choices, uniform, place, back);
        return 0;
    }
    // ---------------------------------------------------------------- dialogs, HUD, interactions, keys
    // player:dialog{style="msgbox"|"input"|"password"|"list", title=, text=, items={...}, headers={...},
    //               button1="OK", button2=, default=, onResponse=function(player, ok, item, text) end} -> id
    static std::string Joined(lua_State* L, int idx, char sep) {  // a string, or a table of strings joined
        if (lua_type(L, idx) == LUA_TSTRING || lua_type(L, idx) == LUA_TNUMBER) return lua_tostring(L, idx);
        std::string out;
        if (lua_istable(L, idx)) for (lua_Integer i = 1;; ++i) {
            if (lua_rawgeti(L, idx, i) == LUA_TNIL) { lua_pop(L, 1); break; }
            if (i > 1) out += sep;
            const char* s = luaL_tolstring(L, -1, nullptr); out += s; lua_pop(L, 2);
        }
        return out;
    }
    static int PDialog(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        auto str = [&](const char* k, const std::string& d) { lua_getfield(L, 2, k); std::string v = lua_isnil(L, -1) ? d : std::string(luaL_checkstring(L, -1)); lua_pop(L, 1); return v; };
        std::vector<std::string> items;
        if (lua_getfield(L, 2, "items") == LUA_TTABLE) {
            for (lua_Integer i = 1;; ++i) {
                if (lua_rawgeti(L, -1, i) == LUA_TNIL) { lua_pop(L, 1); break; }
                items.push_back(Joined(L, lua_gettop(L), '\t')); lua_pop(L, 1);
                if (items.size() > net::kDialogItemsMax) luaL_error(L, "dialog: at most %d items", int(net::kDialogItemsMax));
            }
        }
        lua_pop(L, 1);
        std::string headers;
        if (lua_getfield(L, 2, "headers") != LUA_TNIL) headers = Joined(L, lua_gettop(L), '\t');
        lua_pop(L, 1);
        const std::string styleName = str("style", items.empty() ? "msgbox" : "list");
        net::DialogStyle style;
        if (styleName == "msgbox") style = net::DialogStyle::MsgBox;
        else if (styleName == "input") style = net::DialogStyle::Input;
        else if (styleName == "password") style = net::DialogStyle::Password;
        else if (styleName == "list") style = net::DialogStyle::List;
        else return luaL_error(L, "dialog: unknown style '%s' (msgbox, input, password, list)", styleName.c_str());
        if (style == net::DialogStyle::List && items.empty()) luaL_error(L, "dialog: a list needs items");
        const bool ru = p.lang == "ru";
        const std::string button1 = str("button1", "OK");
        const std::string button2 = str("button2", style == net::DialogStyle::MsgBox ? "" : (ru ? "Отмена" : "Cancel"));
        const uint16_t id = Srv(L).ShowDialog(&p, style, str("title", ""), str("text", ""), button1, button2, str("default", ""), headers, items);
        // onResponse is kept until the player answers (a newer dialog replaces it).
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.dialogs");
        lua_getfield(L, 2, "onResponse"); lua_rawseti(L, -2, p.id);
        lua_pop(L, 1);
        lua_pushinteger(L, id); return 1;
    }
    static int PCloseDialog(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        Srv(L).CloseDialog(&p);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.dialogs"); lua_pushnil(L); lua_rawseti(L, -2, p.id); lua_pop(L, 1);
        return 0;
    }
    // player:hud(id, {text=, x=, y=, ...}) shows or changes an element; player:hud(id) removes it.
    static int PHud(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        const std::string id = luaL_checkstring(L, 2);
        if (id.empty() || id.size() > 64) luaL_error(L, "hud: the id must be 1..64 characters");
        if (lua_isnoneornil(L, 3)) { Srv(L).SetHud(&p, id, ""); return 0; }
        luaL_checktype(L, 3, LUA_TTABLE);
        const std::string json = ToJson(L, 3, 0).dump();
        if (json.size() > 4096) luaL_error(L, "hud: element too large");
        Srv(L).SetHud(&p, id, json);
        return 0;
    }
    static int PClearHud(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetHud(&p, "", ""); Srv(L).ShowMoney(&p); return 0; }  // (the money stays)
    // Key names: "A".."Z", "0".."9", "F1".."F12".
    static int KeyCode(const std::string& name) {
        std::string k; for (char c : name) k += char(std::toupper(static_cast<unsigned char>(c)));
        if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
        if (k.size() >= 2 && k[0] == 'F') { const int n = std::atoi(k.c_str() + 1); if (n >= 1 && n <= 12 && std::to_string(n) == k.substr(1)) return 0x6F + n; }
        return -1;
    }
    static std::string KeyName(uint8_t vk) {
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
        if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x6F);
        return std::to_string(vk);
    }
    // createInteraction{x=, y=, z=, radius=2 (m), text=, key="F", onUse=function(player, id) end} -> id
    // (F: E mounts the bike; multiplayer clients move the flashlight from F to L)
    static int CreateInteraction(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        Interaction in;
        lua_getfield(L, 1, "x"); in.x = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "y"); in.y = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "z"); in.z = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, 1, "radius"); in.radius = float(luaL_optnumber(L, -1, 2) * 100.0); lua_pop(L, 1);
        lua_getfield(L, 1, "text"); in.text = luaL_optstring(L, -1, ""); lua_pop(L, 1);
        lua_getfield(L, 1, "key"); const std::string key = luaL_optstring(L, -1, "F"); lua_pop(L, 1);
        const int vk = KeyCode(key);
        if (vk < 0) luaL_error(L, "createInteraction: unknown key '%s' (A-Z, 0-9, F1-F12)", key.c_str());
        in.key = uint8_t(vk);
        lua_getfield(L, 1, "world"); in.world = lua_isnil(L, -1) ? -1 : int32_t(luaL_checkinteger(L, -1)); lua_pop(L, 1);
        const uint16_t id = Srv(L).AddInteraction(in);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.interactions");
        lua_getfield(L, 1, "onUse"); lua_rawseti(L, -2, id);
        lua_pop(L, 1);
        lua_pushinteger(L, id); return 1;
    }
    static int RemoveInteraction(lua_State* L) {
        const auto id = uint16_t(luaL_checkinteger(L, 1));
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.interactions"); lua_pushnil(L); lua_rawseti(L, -2, id); lua_pop(L, 1);
        lua_pushboolean(L, Srv(L).RemoveInteraction(id)); return 1;
    }
    // bindKey("Y", function(player, key) end): the players' presses of that key (outside chat and menus).
    static int BindKey(lua_State* L) {
        const std::string name = luaL_checkstring(L, 1);
        const int vk = KeyCode(name);
        if (vk < 0) luaL_error(L, "bindKey: unknown key '%s' (A-Z, 0-9, F1-F12)", name.c_str());
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.keys"); lua_pushvalue(L, 2); lua_rawseti(L, -2, vk); lua_pop(L, 1);
        Srv(L).WatchKey(uint8_t(vk), true);
        return 0;
    }
    static int UnbindKey(lua_State* L) {
        const int vk = KeyCode(luaL_checkstring(L, 1));
        if (vk < 0) return 0;
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.keys"); lua_pushnil(L); lua_rawseti(L, -2, vk); lua_pop(L, 1);
        Srv(L).WatchKey(uint8_t(vk), false);
        return 0;
    }
    // ---------------------------------------------------------------- actors and virtual worlds
    // Fields of createActor / setActor from the table at idx into `a` (only those present).
    static void ActorFields(lua_State* L, int idx, Actor& a, const char* fn) {
        auto num = [&](const char* k, float& out) { if (lua_getfield(L, idx, k) != LUA_TNIL) out = float(luaL_checknumber(L, -1)); lua_pop(L, 1); };
        num("x", a.x); num("y", a.y); num("z", a.z); num("yaw", a.yaw);
        if (lua_getfield(L, idx, "world") != LUA_TNIL) a.world = int32_t(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "look") != LUA_TNIL) a.look = LookArg(L, lua_gettop(L), fn, "skins");
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "anim") != LUA_TNIL) {
            const std::string want = luaL_checkstring(L, -1);
            a.anim = Srv(L).ResolveAnim(want);
            if (a.anim.empty()) luaL_error(L, "%s: unknown animation '%s' (data/anims.txt)", fn, want.c_str());
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "name") != LUA_TNIL) a.name = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "solid") != LUA_TNIL) a.solid = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    // createActor{x=, y=, z=, yaw=0, look=, anim="idle", name=, solid=true, world=} -> id
    static int CreateActor(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        for (const char* k : {"x", "y", "z"}) { lua_getfield(L, 1, k); luaL_checknumber(L, -1); lua_pop(L, 1); }
        Actor a;
        a.look = Srv(L).ResolveLook("deacon_standard");
        a.anim = Srv(L).ResolveAnim("idle");
        ActorFields(L, 1, a, "createActor");
        lua_pushinteger(L, Srv(L).AddActor(a)); return 1;
    }
    // setActor(id, {fields to change})
    static int SetActor(lua_State* L) {
        const auto id = uint16_t(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        const Actor* old = Srv(L).FindActor(id);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Actor a = *old;
        ActorFields(L, 2, a, "setActor");
        lua_pushboolean(L, Srv(L).UpdateActor(a)); return 1;
    }
    static void PushActor(lua_State* L, const Actor& a) {
        lua_createtable(L, 0, 10);
        lua_pushinteger(L, a.id); lua_setfield(L, -2, "id");
        lua_pushstring(L, a.key.c_str()); lua_setfield(L, -2, "key");
        lua_pushstring(L, a.group.c_str()); lua_setfield(L, -2, "group");
        lua_pushnumber(L, a.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, a.y); lua_setfield(L, -2, "y");
        lua_pushnumber(L, a.z); lua_setfield(L, -2, "z");
        lua_pushnumber(L, a.yaw); lua_setfield(L, -2, "yaw");
        lua_pushstring(L, a.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushinteger(L, a.world); lua_setfield(L, -2, "world");
        lua_pushboolean(L, 1); lua_setfield(L, -2, "placed");
    }
    // getActor("key"): an NPC placed in the editor ({id, key, group, x, y, z, yaw, name, world}) or nil.
    static int GetActor(lua_State* L) {
        const std::string key = luaL_checkstring(L, 1);
        for (auto& [id, a] : Srv(L).Actors()) if (a.key == key) { PushActor(L, a); return 1; }
        lua_pushnil(L); return 1;
    }
    // worldActors(): every NPC placed in the editor, by key order.
    static int WorldActors(lua_State* L) {
        std::vector<const Actor*> list;
        for (auto& [id, a] : Srv(L).Actors()) if (!a.key.empty()) list.push_back(&a);
        std::sort(list.begin(), list.end(), [](const Actor* a, const Actor* b) { return a->key < b->key; });
        lua_createtable(L, int(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i) { PushActor(L, *list[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
    static int RemoveActor(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveActor(uint16_t(luaL_checkinteger(L, 1)))); return 1; }
    static int PSetWorld(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetWorld(&p, int32_t(luaL_checkinteger(L, 2))); return 0; }
    // ---------------------------------------------------------------- vehicles
    // Fields of createVehicle / setVehicle from the table at idx (only those present).
    static void VehicleFields(lua_State* L, int idx, Vehicle& v) {
        auto num = [&](const char* k, float& out) { if (lua_getfield(L, idx, k) != LUA_TNIL) out = float(luaL_checknumber(L, -1)); lua_pop(L, 1); };
        num("x", v.state.x); num("y", v.state.y); num("z", v.state.z); num("yaw", v.state.yaw); num("fuel", v.state.fuel); num("health", v.state.health);
        v.state.fuel = std::clamp(v.state.fuel, 0.f, 1.f); v.state.health = std::clamp(v.state.health, 0.f, 1.f);
        if (lua_getfield(L, idx, "world") != LUA_TNIL) v.world = int32_t(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "locked") != LUA_TNIL) v.locked = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "blip") != LUA_TNIL) {
            const std::string b = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "all" : "none") : luaL_checkstring(L, -1);
            if (b == "none") v.blip = 0; else if (b == "owner") v.blip = 1; else if (b == "all") v.blip = 2;
            else luaL_error(L, "vehicle blip must be \"owner\", \"all\" or \"none\"");
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "blipColor") != LUA_TNIL) {
            const std::string c = luaL_checkstring(L, -1);
            char* end = nullptr;
            const unsigned long rgb = c.size() == 7 && c[0] == '#' ? std::strtoul(c.c_str() + 1, &end, 16) : 0;
            if (!end || end != c.c_str() + 7) luaL_error(L, "blipColor must be \"#RRGGBB\"");
            v.blipColor = uint32_t(rgb << 8) | 0xFF;
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "blipIcon") != LUA_TNIL) {
            v.blipIcon = luaL_checkstring(L, -1);
            for (char ch : v.blipIcon) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) luaL_error(L, "blipIcon: a MAP_* texture name");
            if (v.blipIcon.size() > 60) luaL_error(L, "blipIcon: too long");
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "model") != LUA_TNIL) {
            v.model = luaL_checkstring(L, -1);
            if (v.model == "bike") v.model.clear();
            if (!v.model.empty() && v.model != "golfcart" && v.model != "dumptruck" && v.model != "militarytruck" && v.model != "helicopter")
                luaL_error(L, "vehicle model must be \"bike\", \"golfcart\", \"dumptruck\", \"militarytruck\" or \"helicopter\"");
        }
        lua_pop(L, 1);
        // npc = {look=, name=} (or true): an NPC of the mode drives it; false: nobody.
        if (const int n = lua_getfield(L, idx, "npc"); n == LUA_TTABLE) {
            v.npc = true;
            lua_getfield(L, -1, "look"); v.npcLook = LookArg(L, lua_gettop(L), "vehicle npc", "skins"); lua_pop(L, 1);
            lua_getfield(L, -1, "name"); v.npcName = luaL_optstring(L, -1, ""); lua_pop(L, 1);
        } else if (n != LUA_TNIL) {
            v.npc = lua_toboolean(L, -1);
            if (!v.npc) { v.npcLook.clear(); v.npcName.clear(); v.route.clear(); }
        }
        lua_pop(L, 1);
        const int t = lua_getfield(L, idx, "owner");
        if (t == LUA_TSTRING) v.owner = Server::NickKey(lua_tostring(L, -1));
        else if (t == LUA_TBOOLEAN && !lua_toboolean(L, -1)) v.owner.clear();
        else if (t != LUA_TNIL) v.owner = Server::NickKey(CheckPlayer(L, lua_gettop(L)).nick);
        lua_pop(L, 1);
    }
    // createVehicle{x=, y=, z=, yaw=0, model="bike", npc={look=, name=}, owner=player|nick, locked=false, fuel=1, health=1, world=0} -> id
    static int CreateVehicle(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        for (const char* k : {"x", "y", "z"}) { lua_getfield(L, 1, k); luaL_checknumber(L, -1); lua_pop(L, 1); }
        Vehicle v;
        VehicleFields(L, 1, v);
        v.state.z += 60.f;  // z on the ground: the bike stands on its wheels just above
        lua_pushinteger(L, Srv(L).AddVehicle(v)); return 1;
    }
    static int SetVehicle(lua_State* L) {
        Vehicle* old = Srv(L).FindVehicle(uint16_t(luaL_checkinteger(L, 1)));
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Vehicle v = *old;
        VehicleFields(L, 2, v);
        lua_getfield(L, 2, "z"); if (!lua_isnil(L, -1)) v.state.z += 60.f; lua_pop(L, 1);
        lua_pushboolean(L, Srv(L).UpdateVehicle(v)); return 1;
    }
    static int RemoveVehicle(lua_State* L) {
        const uint16_t id = uint16_t(luaL_checkinteger(L, 1));
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.routes"); lua_pushnil(L); lua_rawseti(L, -2, id); lua_pop(L, 1);
        lua_pushboolean(L, Srv(L).RemoveVehicle(id)); return 1;
    }
    // driveVehicle(id, {points={{x=,y=,z=}, ...} (or {x,y,z}), speed=12 (m/s), loop=false, onArrive=function(id) end}) -> ok
    // Its NPC drives it from where it is through the points; a new way replaces the old one.
    static int DriveVehicle(lua_State* L) {
        const uint16_t id = uint16_t(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        std::vector<float> pts;
        lua_getfield(L, 2, "points"); luaL_checktype(L, -1, LUA_TTABLE);
        const lua_Integer n = luaL_len(L, -1);
        if (n < 1 || n > 500) luaL_error(L, "driveVehicle: 1 to 500 points");
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i); luaL_checktype(L, -1, LUA_TTABLE);
            const char* keys[3] = {"x", "y", "z"};
            for (int k = 0; k < 3; ++k) {
                if (lua_getfield(L, -1, keys[k]) == LUA_TNIL) { lua_pop(L, 1); lua_rawgeti(L, -1, k + 1); }
                pts.push_back(float(luaL_checknumber(L, -1)) + (k == 2 ? 60.f : 0.f));  // z on the ground, like createVehicle
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "speed"); const float speed = float(luaL_optnumber(L, -1, 12) * 100.0); lua_pop(L, 1);
        lua_getfield(L, 2, "loop"); const bool loop = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "altitude"); const float altitude = float(luaL_optnumber(L, -1, 0) * 100.0); lua_pop(L, 1);
        const bool ok = Srv(L).DriveVehicle(id, pts, speed, loop, altitude);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.routes");
        if (ok) lua_getfield(L, 2, "onArrive"); else lua_pushnil(L);
        lua_rawseti(L, -2, id); lua_pop(L, 1);
        lua_pushboolean(L, ok); return 1;
    }
    static int StopVehicle(lua_State* L) {
        const uint16_t id = uint16_t(luaL_checkinteger(L, 1));
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.routes"); lua_pushnil(L); lua_rawseti(L, -2, id); lua_pop(L, 1);
        lua_pushboolean(L, Srv(L).StopVehicle(id)); return 1;
    }
    static void PushVehicle(lua_State* L, const Vehicle& v) {
        lua_createtable(L, 0, 12);
        lua_pushinteger(L, v.id); lua_setfield(L, -2, "id");
        lua_pushnumber(L, v.state.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, v.state.y); lua_setfield(L, -2, "y");
        lua_pushnumber(L, v.state.z - 60.f); lua_setfield(L, -2, "z");
        lua_pushnumber(L, v.state.yaw); lua_setfield(L, -2, "yaw");
        lua_pushnumber(L, v.state.fuel); lua_setfield(L, -2, "fuel");
        lua_pushnumber(L, v.state.health); lua_setfield(L, -2, "health");
        lua_pushinteger(L, v.world); lua_setfield(L, -2, "world");
        lua_pushboolean(L, v.locked); lua_setfield(L, -2, "locked");
        lua_pushstring(L, v.blip == 0 ? "none" : v.blip == 2 ? "all" : "owner"); lua_setfield(L, -2, "blip");
        lua_pushstring(L, v.model.empty() ? "bike" : v.model.c_str()); lua_setfield(L, -2, "model");
        if (!v.key.empty()) { lua_pushstring(L, v.key.c_str()); lua_setfield(L, -2, "key"); }
        lua_pushboolean(L, v.npc); lua_setfield(L, -2, "npc");
        lua_pushboolean(L, !v.route.empty()); lua_setfield(L, -2, "driving");
        if (v.owner.empty()) lua_pushnil(L); else lua_pushstring(L, v.owner.c_str());
        lua_setfield(L, -2, "owner");
        if (Player* d = Srv(L).Find(v.driver)) PushPlayer(L, *d); else lua_pushnil(L);
        lua_setfield(L, -2, "driver");
        if (Player* q = Srv(L).Find(v.passenger)) PushPlayer(L, *q); else lua_pushnil(L);
        lua_setfield(L, -2, "passenger");
        lua_createtable(L, 0, 0);  // a helicopter's passengers
        int n = 0;
        for (uint16_t r : v.riders) if (Player* q = Srv(L).Find(r)) { PushPlayer(L, *q); lua_rawseti(L, -2, ++n); }
        lua_setfield(L, -2, "passengers");
    }
    // vehicleInfo(id) -> {id, x, y, z, yaw, fuel, health, world, locked, owner (lower-case nick or nil), driver (player or nil)} or nil
    static int VehicleInfo(lua_State* L) {
        Vehicle* v = Srv(L).FindVehicle(uint16_t(luaL_checkinteger(L, 1)));
        if (v) PushVehicle(L, *v); else lua_pushnil(L);
        return 1;
    }
    static int VehiclesList(lua_State* L) {
        const auto& all = Srv(L).Vehicles();
        lua_createtable(L, int(all.size()), 0);
        lua_Integer i = 0;
        for (auto& [id, v] : all) { PushVehicle(L, v); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    // player:putInVehicle(id [, passenger]): at the wheel, or behind whoever drives it.
    static int PPutInVehicle(lua_State* L) { auto& p = CheckPlayer(L, 1); lua_pushboolean(L, Srv(L).PutInVehicle(&p, uint16_t(luaL_checkinteger(L, 2)), lua_toboolean(L, 3))); return 1; }
    static int PMarkVehicle(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).MarkVehicle(&p, uint16_t(luaL_checkinteger(L, 2)), lua_isnone(L, 3) || lua_toboolean(L, 3)); return 0; }
    static int PExitVehicle(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).EjectFromVehicle(&p); return 0; }
    // ---------------------------------------------------------------- icons in the world
    static uint32_t RgbaArg(lua_State* L, int idx, const char* fn) {  // "#RRGGBB" or "#RRGGBBAA"
        const std::string c = luaL_checkstring(L, idx);
        char* end = nullptr;
        const unsigned long v = (c.size() == 7 || c.size() == 9) && c[0] == '#' ? std::strtoul(c.c_str() + 1, &end, 16) : 0;
        if (!end || end != c.c_str() + c.size()) luaL_error(L, "%s: colour must be \"#RRGGBB\" or \"#RRGGBBAA\"", fn);
        return c.size() == 7 ? uint32_t(v << 8) | 0xFF : uint32_t(v);
    }
    static void IconFields(lua_State* L, int idx, Icon& i, const char* fn) {
        auto num = [&](const char* k, float& out, float scale) { if (lua_getfield(L, idx, k) != LUA_TNIL) out = float(luaL_checknumber(L, -1)) * scale; lua_pop(L, 1); };
        num("x", i.x, 1); num("y", i.y, 1); num("z", i.z, 1); num("size", i.size, 1); num("distance", i.distance, 100);
        if (lua_getfield(L, idx, "world") != LUA_TNIL) i.world = int32_t(luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "color") != LUA_TNIL) i.color = RgbaArg(L, lua_gettop(L), fn);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "text") != LUA_TNIL) i.text = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "throughWalls") != LUA_TNIL) i.throughWalls = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "image") != LUA_TNIL) {
            i.image = luaL_checkstring(L, -1);
            if (!Srv(L).HasImage(i.image)) luaL_error(L, "%s: no image '%s' in the mode's images folder", fn, i.image.c_str());
        }
        lua_pop(L, 1);
    }
    // createIcon{x=, y=, z=, image=, size=48 (pixels), distance=50 (m), color="#RRGGBB", text=, world=, throughWalls=false} -> id
    static int CreateIcon(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        for (const char* k : {"x", "y", "z", "image"}) { lua_getfield(L, 1, k); if (lua_isnil(L, -1)) luaL_error(L, "createIcon: %s is required", k); lua_pop(L, 1); }
        Icon i;
        IconFields(L, 1, i, "createIcon");
        lua_pushinteger(L, Srv(L).AddIcon(i)); return 1;
    }
    static int SetIcon(lua_State* L) {
        Icon* old = Srv(L).FindIcon(uint16_t(luaL_checkinteger(L, 1)));
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Icon i = *old;
        IconFields(L, 2, i, "setIcon");
        lua_pushboolean(L, Srv(L).UpdateIcon(i)); return 1;
    }
    // ---- doors, vehicles and loot of world.json (docs/API.md "Входы", "Транспорт", "Лут") ----
    static void PushDoorEnd(lua_State* L, const float* at, int world) {
        lua_createtable(L, 0, 5);
        lua_pushnumber(L, at[0]); lua_setfield(L, -2, "x"); lua_pushnumber(L, at[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, at[2]); lua_setfield(L, -2, "z"); lua_pushnumber(L, at[3]); lua_setfield(L, -2, "yaw");
        lua_pushinteger(L, world); lua_setfield(L, -2, "world");
    }
    static void PushDoor(lua_State* L, const Door& d) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, d.key.c_str()); lua_setfield(L, -2, "key");
        lua_pushstring(L, d.text.c_str()); lua_setfield(L, -2, "text");
        lua_pushboolean(L, d.locked); lua_setfield(L, -2, "locked");
        PushDoorEnd(L, d.a, d.worldA); lua_setfield(L, -2, "a");
        PushDoorEnd(L, d.b, d.worldB); lua_setfield(L, -2, "b");
    }
    static int GetDoor(lua_State* L) {
        const Door* d = Srv(L).FindDoor(luaL_checkstring(L, 1));
        if (!d) { lua_pushnil(L); return 1; }
        PushDoor(L, *d); return 1;
    }
    static int DoorsList(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto& [key, d] : Srv(L).Doors()) { PushDoor(L, d); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    // setDoor(key, {locked = bool, text = "..."})
    static int SetDoor(lua_State* L) {
        const Door* old = Srv(L).FindDoor(luaL_checkstring(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Door d = *old;
        lua_getfield(L, 2, "locked"); if (!lua_isnil(L, -1)) d.locked = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "text"); if (lua_isstring(L, -1)) d.text = lua_tostring(L, -1); lua_pop(L, 1);
        Srv(L).UpdateDoor(d);
        lua_pushboolean(L, 1); return 1;
    }
    // getVehicle("key" or id) -> the same table as vehicleInfo, or nil
    static int GetVehicleByKey(lua_State* L) {
        Vehicle* v = lua_isinteger(L, 1) ? Srv(L).FindVehicle(uint16_t(lua_tointeger(L, 1))) : Srv(L).FindVehicle(std::string(luaL_checkstring(L, 1)));
        if (!v) { lua_pushnil(L); return 1; }
        lua_settop(L, 0); lua_pushinteger(L, v->id);
        return VehicleInfo(L);
    }
    static int LootList(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto& l : Srv(L).LootList()) {
            lua_createtable(L, 0, 9);
            lua_pushstring(L, l.key.c_str()); lua_setfield(L, -2, "key");
            lua_pushstring(L, l.item.c_str()); lua_setfield(L, -2, "item");
            lua_pushinteger(L, l.count); lua_setfield(L, -2, "count");
            lua_pushstring(L, l.tag.c_str()); lua_setfield(L, -2, "tag");
            lua_pushnumber(L, l.x); lua_setfield(L, -2, "x"); lua_pushnumber(L, l.y); lua_setfield(L, -2, "y"); lua_pushnumber(L, l.z); lua_setfield(L, -2, "z");
            if (l.itemId) { lua_pushinteger(L, l.itemId); lua_setfield(L, -2, "itemId"); }
            lua_pushboolean(L, l.itemId != 0); lua_setfield(L, -2, "present");
            lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    // ---- blips (docs/API.md "Значки на карте") ----
    // createBlip{x, y, z, icon = "MAP_player_marker", color, scale = 1, map = true, radar = true, edge = false, world, player} -> id
    static void BlipFields(lua_State* L, int idx, Blip& b) {
        auto num = [&](const char* k, float& v) { lua_getfield(L, idx, k); if (!lua_isnil(L, -1)) v = float(luaL_checknumber(L, -1)); lua_pop(L, 1); };
        auto flag = [&](const char* k, bool& v) { lua_getfield(L, idx, k); if (!lua_isnil(L, -1)) v = lua_toboolean(L, -1); lua_pop(L, 1); };
        num("x", b.x); num("y", b.y); num("z", b.z); num("scale", b.scale);
        flag("map", b.map); flag("radar", b.radar); flag("edge", b.edge);
        lua_getfield(L, idx, "icon"); if (lua_isstring(L, -1)) b.icon = lua_tostring(L, -1); lua_pop(L, 1);
        lua_getfield(L, idx, "world"); if (!lua_isnil(L, -1)) b.world = int32_t(luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, idx, "color"); if (!lua_isnil(L, -1)) { b.color = ColorArg(L, lua_gettop(L), 0xFFFFFFFF); } lua_pop(L, 1);
        lua_getfield(L, idx, "player"); if (!lua_isnil(L, -1)) b.only = CheckPlayer(L, lua_gettop(L)).id; lua_pop(L, 1);
        b.scale = std::clamp(b.scale, 0.1f, 5.f);
    }
    static int CreateBlip(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        Blip b; BlipFields(L, 1, b);
        lua_pushinteger(L, Srv(L).AddBlip(b)); return 1;
    }
    static int SetBlip(lua_State* L) {
        Blip* old = Srv(L).FindBlip(uint16_t(luaL_checkinteger(L, 1)));
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Blip b = *old; BlipFields(L, 2, b);
        lua_pushboolean(L, Srv(L).UpdateBlip(b)); return 1;
    }
    static int RemoveBlip(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveBlip(uint16_t(luaL_checkinteger(L, 1)))); return 1; }

    // ---- objects (docs/API.md "Объекты") ----
    // Fields of createObject / setObject: model (a mesh name of data/props.txt) or asset (a /Game/ path),
    // x, y, z, pitch, yaw, roll, scale, world, collision.
    static void ObjectFields(lua_State* L, int idx, Object& o, const char* fn) {
        auto num = [&](const char* k, float& v) { lua_getfield(L, idx, k); if (!lua_isnil(L, -1)) v = float(luaL_checknumber(L, -1)); lua_pop(L, 1); };
        num("x", o.x); num("y", o.y); num("z", o.z); num("pitch", o.pitch); num("yaw", o.yaw); num("roll", o.roll); num("scale", o.scale);
        lua_getfield(L, idx, "world"); if (!lua_isnil(L, -1)) o.world = int32_t(luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_getfield(L, idx, "collision"); if (!lua_isnil(L, -1)) o.collision = lua_toboolean(L, -1); lua_pop(L, 1);
        char want[256] = "";
        for (const char* k : {"model", "asset"}) {
            lua_getfield(L, idx, k);
            if (lua_isstring(L, -1)) std::snprintf(want, sizeof(want), "%s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        if (want[0]) {
            const std::string path = Srv(L).ResolveProp(want);
            if (path.empty()) { char msg[512]; std::snprintf(msg, sizeof(msg), "%s: unknown model '%s' (a mesh name from data/props.txt or a /Game/ path)", fn, want); lua_pushstring(L, msg); lua_error(L); }
            o.asset = path;
        }
        o.scale = std::clamp(o.scale, 0.01f, 100.f);
    }
    static void PushObject(lua_State* L, const Object& o, double now) {
        float at[6]; o.At(now, at);
        lua_createtable(L, 0, 12);
        lua_pushinteger(L, o.id); lua_setfield(L, -2, "id");
        lua_pushstring(L, o.asset.c_str()); lua_setfield(L, -2, "asset");
        static const char* names[] = {"x", "y", "z", "pitch", "yaw", "roll"};
        for (int i = 0; i < 6; ++i) { lua_pushnumber(L, at[i]); lua_setfield(L, -2, names[i]); }
        lua_pushnumber(L, o.scale); lua_setfield(L, -2, "scale");
        lua_pushinteger(L, o.world); lua_setfield(L, -2, "world");
        lua_pushboolean(L, o.collision); lua_setfield(L, -2, "collision");
        lua_pushboolean(L, o.moving); lua_setfield(L, -2, "moving");
    }
    static int CreateObject(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        Object o;
        ObjectFields(L, 1, o, "createObject");
        if (o.asset.empty()) return luaL_error(L, "createObject: model or asset is required");
        lua_pushinteger(L, Srv(L).AddObject(o)); return 1;
    }
    static int SetObject(lua_State* L) {
        Object* old = Srv(L).FindObject(uint16_t(luaL_checkinteger(L, 1)));
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        Object o = *old;
        float at[6]; o.At(Srv(L).Now(), at);  // a moving one stops where it is, unless the table says where
        o.x = at[0]; o.y = at[1]; o.z = at[2]; o.pitch = at[3]; o.yaw = at[4]; o.roll = at[5];
        ObjectFields(L, 2, o, "setObject");
        lua_pushboolean(L, Srv(L).UpdateObject(o)); return 1;
    }
    // moveObject(id, {x, y, z, pitch, yaw, roll, time = seconds | speed = metres a second, onArrive = fn(id)}) -> seconds
    static int MoveObject(lua_State* L) {
        const auto id = uint16_t(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        Object* o = Srv(L).FindObject(id);
        if (!o) { lua_pushnil(L); return 1; }
        float at[6]; o->At(Srv(L).Now(), at);
        float to[6] = {at[0], at[1], at[2], at[3], at[4], at[5]};
        static const char* names[] = {"x", "y", "z", "pitch", "yaw", "roll"};
        for (int i = 0; i < 6; ++i) { lua_getfield(L, 2, names[i]); if (!lua_isnil(L, -1)) to[i] = float(luaL_checknumber(L, -1)); lua_pop(L, 1); }
        lua_getfield(L, 2, "time"); double seconds = luaL_optnumber(L, -1, -1); lua_pop(L, 1);
        if (seconds < 0) {
            lua_getfield(L, 2, "speed"); const double speed = luaL_optnumber(L, -1, 2); lua_pop(L, 1);  // m/s
            const double dist = std::sqrt(double(to[0] - at[0]) * (to[0] - at[0]) + double(to[1] - at[1]) * (to[1] - at[1]) + double(to[2] - at[2]) * (to[2] - at[2])) / 100.0;
            seconds = speed > 0 ? dist / speed : 0;
            if (dist < 0.01) seconds = 1;  // only turning: a second
        }
        seconds = std::min(seconds, 3600.0);
        lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.objmoves");
        lua_getfield(L, 2, "onArrive");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_pushnil(L); }
        lua_rawseti(L, -2, id);
        lua_pop(L, 1);
        Srv(L).MoveObject(id, to, seconds);
        lua_pushnumber(L, seconds); return 1;
    }
    static int StopObject(lua_State* L) {
        Object* o = Srv(L).FindObject(uint16_t(luaL_checkinteger(L, 1)));
        if (!o) { lua_pushboolean(L, 0); return 1; }
        Object copy = *o;
        float at[6]; copy.At(Srv(L).Now(), at);
        copy.x = at[0]; copy.y = at[1]; copy.z = at[2]; copy.pitch = at[3]; copy.yaw = at[4]; copy.roll = at[5];
        lua_pushboolean(L, Srv(L).UpdateObject(copy)); return 1;
    }
    static int RemoveObject(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveObject(uint16_t(luaL_checkinteger(L, 1)))); return 1; }
    static int GetObject(lua_State* L) {
        Object* o = Srv(L).FindObject(uint16_t(luaL_checkinteger(L, 1)));
        if (!o) { lua_pushnil(L); return 1; }
        PushObject(L, *o, Srv(L).Now()); return 1;
    }
    static int ObjectsList(lua_State* L) {
        lua_newtable(L); int i = 0;
        for (auto& [id, o] : Srv(L).Objects()) { PushObject(L, o, Srv(L).Now()); lua_rawseti(L, -2, ++i); }
        return 1;
    }
    static int RemoveIcon(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveIcon(uint16_t(luaL_checkinteger(L, 1)))); return 1; }
    static int PFreeze(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).Freeze(&p, lua_isnone(L, 2) || lua_toboolean(L, 2)); return 0; }
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
    // ---------------------------------------------------------------- boxes to carry
    // Fields of createCarry / setCarry, read before any C++ object exists (Lua errors jump over destructors).
    struct CarryArgs { bool pos = false, yaw = false, world = false; float x = 0, y = 0, z = 0, a = 0; int world_ = -1; const char *model = nullptr, *tag = nullptr, *key = nullptr; int roll = -1; };
    static void ReadCarryArgs(lua_State* L, int idx, CarryArgs& c, const char* fn) {
        if (lua_getfield(L, idx, "model") != LUA_TNIL) {
            c.model = luaL_checkstring(L, -1);
            const bool known = Carry::KnownModel(c.model);
            if (!known) luaL_error(L, "%s: unknown model \"%s\" (%s)", fn, c.model, Carry::ModelList());
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "x") != LUA_TNIL) {
            c.pos = true; c.x = float(luaL_checknumber(L, -1));
            lua_getfield(L, idx, "y"); c.y = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
            lua_getfield(L, idx, "z"); c.z = float(luaL_checknumber(L, -1)); lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "yaw") != LUA_TNIL) { c.yaw = true; c.a = float(luaL_checknumber(L, -1)); }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "world") != LUA_TNIL) { c.world = true; c.world_ = int(luaL_checkinteger(L, -1)); }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "tag") != LUA_TNIL) c.tag = luaL_checkstring(L, -1);  // the table keeps the string alive
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "key") != LUA_TNIL) c.key = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "roll") != LUA_TNIL) {  // a dodge roll with it: "keep", "drop", "break"
            const char* r = luaL_checkstring(L, -1);
            c.roll = std::strcmp(r, "keep") == 0 ? 0 : std::strcmp(r, "drop") == 0 ? 1 : std::strcmp(r, "break") == 0 ? 2 : -1;
            if (c.roll < 0) luaL_error(L, "%s: roll must be \"keep\", \"drop\" or \"break\"", fn);
        }
        lua_pop(L, 1);
    }
    static void ApplyCarryArgs(const CarryArgs& a, Carry& c) {
        if (a.model) c.model = a.model;
        if (a.pos) { c.x = a.x; c.y = a.y; c.z = a.z; c.carrier = 0xFFFF; }  // placed: out of the hands
        if (a.yaw) c.yaw = a.a;
        if (a.world) c.world = a.world_;
        if (a.tag) c.tag = a.tag;
        if (a.key) c.key = a.key;
        if (a.roll >= 0) c.roll = net::CarryRoll(a.roll);
    }
    static void PushCarry(lua_State* L, const Carry& c) {
        lua_createtable(L, 0, 10);
        Player* holder = c.carrier != 0xFFFF ? Srv(L).Find(c.carrier) : nullptr;
        lua_pushinteger(L, c.id); lua_setfield(L, -2, "id");
        lua_pushstring(L, c.model.c_str()); lua_setfield(L, -2, "model");
        // Carried: where its player is (on the ground under it).
        lua_pushnumber(L, holder ? holder->state.x : c.x); lua_setfield(L, -2, "x");
        lua_pushnumber(L, holder ? holder->state.y : c.y); lua_setfield(L, -2, "y");
        lua_pushnumber(L, holder ? holder->state.z - 90.f : c.z); lua_setfield(L, -2, "z");
        lua_pushnumber(L, c.yaw); lua_setfield(L, -2, "yaw");
        lua_pushinteger(L, c.world); lua_setfield(L, -2, "world");
        lua_pushstring(L, c.tag.c_str()); lua_setfield(L, -2, "tag");
        if (!c.key.empty()) { lua_pushstring(L, c.key.c_str()); lua_setfield(L, -2, "key"); }
        lua_pushstring(L, c.roll == net::CarryRoll::Drop ? "drop" : c.roll == net::CarryRoll::Break ? "break" : "keep"); lua_setfield(L, -2, "roll");
        if (holder) { PushPlayer(L, *holder); lua_setfield(L, -2, "carrier"); }
    }
    // createCarry{model="crate", x=, y=, z=, yaw=0, world=, tag=, key=, roll="keep"} -> id: a box on the ground there
    static int CreateCarry(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        CarryArgs a; ReadCarryArgs(L, 1, a, "createCarry");
        if (!a.pos) luaL_error(L, "createCarry: x, y, z expected");
        if (a.key && *a.key && Srv(L).FindCarry(std::string(a.key))) luaL_error(L, "createCarry: key \"%s\" is taken", a.key);
        Carry c; ApplyCarryArgs(a, c);
        lua_pushinteger(L, Srv(L).AddCarry(c)); return 1;
    }
    // setCarry(id, {x=, y=, z=, yaw=, world=, tag=, model=}) -> ok: a position takes it out of the hands
    static int SetCarry(lua_State* L) {
        const auto id = uint16_t(luaL_checkinteger(L, 1));
        luaL_checktype(L, 2, LUA_TTABLE);
        CarryArgs a; ReadCarryArgs(L, 2, a, "setCarry");
        const Carry* old = Srv(L).FindCarry(id);
        if (!old) { lua_pushboolean(L, 0); return 1; }
        if (a.key && *a.key && Srv(L).FindCarry(std::string(a.key)) && Srv(L).FindCarry(std::string(a.key)) != old) luaL_error(L, "setCarry: key \"%s\" is taken", a.key);
        Carry c = *old; ApplyCarryArgs(a, c);
        lua_pushboolean(L, Srv(L).UpdateCarry(c)); return 1;
    }
    static int RemoveCarry(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveCarry(uint16_t(luaL_checkinteger(L, 1)))); return 1; }
    // getCarry(id or "key") -> {id, key, model, x, y, z, yaw, world, tag, carrier} or nil
    static int GetCarry(lua_State* L) {
        const Carry* c = lua_type(L, 1) == LUA_TSTRING ? Srv(L).FindCarry(std::string(lua_tostring(L, 1))) : Srv(L).FindCarry(uint16_t(luaL_checkinteger(L, 1)));
        if (c) PushCarry(L, *c); else lua_pushnil(L);
        return 1;
    }
    // carries([tag]) -> every box (of that tag), by id
    static int Carries(lua_State* L) {
        const char* tag = luaL_optstring(L, 1, nullptr);
        lua_newtable(L);
        int n = 0;
        for (auto& [id, c] : Srv(L).Carries()) if (!tag || c.tag == tag) { PushCarry(L, c); lua_rawseti(L, -2, ++n); }
        return 1;
    }
    // ---------------------------------------------------------------- money
    // player:giveMoney(amount) -> the new amount (negative: takes); player:setMoney(amount)
    static int PGiveMoney(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetMoney(&p, p.money + luaL_checkinteger(L, 2)); lua_pushinteger(L, lua_Integer(p.money)); return 1; }
    static int PSetMoney(lua_State* L) { auto& p = CheckPlayer(L, 1); Srv(L).SetMoney(&p, luaL_checkinteger(L, 2)); return 0; }
    // player:giveCarry(id) -> ok: into its hands (from wherever it is; false: it holds another one)
    static int PGiveCarry(lua_State* L) { auto& p = CheckPlayer(L, 1); lua_pushboolean(L, Srv(L).GiveCarry(&p, uint16_t(luaL_checkinteger(L, 2)))); return 1; }
    // player:dropCarry([x, y, z, yaw]) -> id or nil: out of its hands, onto that spot or where it stands
    static int PDropCarry(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        float at[4] = {};
        const bool spot = !lua_isnoneornil(L, 2);
        if (spot) { at[0] = float(luaL_checknumber(L, 2)); at[1] = float(luaL_checknumber(L, 3)); at[2] = float(luaL_checknumber(L, 4)); at[3] = float(luaL_optnumber(L, 5, p.state.yaw)); }
        const uint16_t id = Srv(L).DropCarry(&p, spot ? at : nullptr);
        if (id) lua_pushinteger(L, id); else lua_pushnil(L);
        return 1;
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
    // setCollisions(true / false) or setCollisions({players = bool, bikes = bool})
    static int SetCollisions(lua_State* L) {
        uint8_t mask = Srv(L).Collisions();
        if (lua_isboolean(L, 1)) mask = lua_toboolean(L, 1) ? (net::CollidePlayers | net::CollideBikes) : 0;
        else {
            luaL_checktype(L, 1, LUA_TTABLE);
            if (lua_getfield(L, 1, "players") != LUA_TNIL) mask = lua_toboolean(L, -1) ? (mask | net::CollidePlayers) : (mask & ~net::CollidePlayers);
            if (lua_getfield(L, 1, "bikes") != LUA_TNIL) mask = lua_toboolean(L, -1) ? (mask | net::CollideBikes) : (mask & ~net::CollideBikes);
            lua_pop(L, 2);
        }
        Srv(L).SetCollisions(mask);
        return 0;
    }
    static int SetNametags(lua_State* L) {
        const std::string m = luaL_checkstring(L, 1);
        if (m != "visible" && m != "walls" && m != "hidden") luaL_error(L, "setNametags: expected \"visible\", \"walls\" or \"hidden\"");
        Srv(L).SetNametags(m == "walls" ? net::NametagMode::ThroughWalls : m == "hidden" ? net::NametagMode::Hidden : net::NametagMode::Visible);
        return 0;
    }
    // ---- http.request / http.get / http.post: asynchronous, the callback runs on the server thread ----
    // http.request{url, method = "GET", body, type (Content-Type), timeout = 10}, callback(ok, status, body, error)
    static int HttpStart(lua_State* L, const std::string& method, const std::string& url, const std::string& body, const std::string& type, int timeout, int callbackIdx) {
        auto& s = Self(L);
        if (!s.server_.config().http) return luaL_error(L, "http is off on this server (server.ini Http=0)");
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return luaL_error(L, "http: the url must start with http:// or https://");
        if (body.size() > (1u << 20)) return luaL_error(L, "http: the body is larger than 1 MB");
        if (s.httpInFlight_ >= 16) return luaL_error(L, "http: 16 requests are already running");
        const int id = s.nextHttp_++;
        if (callbackIdx) {
            luaL_checktype(L, callbackIdx, LUA_TFUNCTION);
            lua_getfield(L, LUA_REGISTRYINDEX, "dgmp.http");
            lua_pushvalue(L, callbackIdx); lua_rawseti(L, -2, id);
            lua_pop(L, 1);
        }
        ++s.httpInFlight_;
        std::thread([box = s.http_, id, method, url, body, type, timeout] {
            Script::HttpBox::Done d{id, false, 0, {}, {}};
            d.ok = HttpRequest(method, url, body, type, d.body, d.status, d.error, timeout);
            std::lock_guard<std::mutex> g(box->lock);
            box->done.push_back(std::move(d));
        }).detach();
        lua_pushinteger(L, id); return 1;
    }
    static int HttpRequestLua(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        auto field = [&](const char* name, const char* def) {
            lua_getfield(L, 1, name);
            std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
            lua_pop(L, 1); return v;
        };
        const std::string url = field("url", ""), method = field("method", "GET"), body = field("body", ""), type = field("type", "");
        lua_getfield(L, 1, "timeout"); const int timeout = int(luaL_optinteger(L, -1, 10)); lua_pop(L, 1);
        std::string m = method; for (auto& c : m) c = char(std::toupper(static_cast<unsigned char>(c)));
        return HttpStart(L, m, url, body, type, std::clamp(timeout, 1, 60), lua_isnoneornil(L, 2) ? 0 : 2);
    }
    static int HttpGetLua(lua_State* L) { return HttpStart(L, "GET", luaL_checkstring(L, 1), "", "", 10, lua_isnoneornil(L, 2) ? 0 : 2); }
    // http.post(url, body [, callback [, contentType]]): a table body is sent as JSON.
    static int HttpPostLua(lua_State* L) {
        std::string body, type = luaL_optstring(L, 4, "");
        if (lua_istable(L, 2)) { body = ToJson(L, 2, 0).dump(); if (type.empty()) type = "application/json"; }
        else body = luaL_checkstring(L, 2);
        if (type.empty()) type = !body.empty() && (body[0] == '{' || body[0] == '[') ? "application/json" : "application/x-www-form-urlencoded";
        return HttpStart(L, "POST", luaL_checkstring(L, 1), body, type, 10, lua_isnoneornil(L, 3) ? 0 : 3);
    }
    // json.encode(value) -> text; json.decode(text) -> value or nil, error
    static int JsonEncode(lua_State* L) {
        const std::string text = ToJson(L, 1, 0).dump();
        lua_pushlstring(L, text.data(), text.size()); return 1;
    }
    static int JsonDecode(lua_State* L) {
        json::Value v;
        if (!json::Value::Parse(luaL_checkstring(L, 1), v)) { lua_pushnil(L); lua_pushstring(L, "invalid JSON"); return 2; }
        PushJson(L, v); return 1;
    }
    // passwordHash(password) -> "pbkdf2-sha256$iterations$salt$hash"; passwordCheck(password, hash) -> bool
    static int PasswordHashLua(lua_State* L) {
        size_t n; const char* pw = luaL_checklstring(L, 1, &n);
        const std::string h = PasswordHash(std::string(pw, n));
        lua_pushlstring(L, h.data(), h.size()); return 1;
    }
    static int PasswordCheckLua(lua_State* L) {
        size_t n; const char* pw = luaL_checklstring(L, 1, &n);
        lua_pushboolean(L, PasswordCheck(std::string(pw, n), luaL_checkstring(L, 2))); return 1;
    }
    static int Sha256(lua_State* L) {
        size_t n; const char* data = luaL_checklstring(L, 1, &n);
        const auto hex = Sha256Hex(data, n);
        lua_pushlstring(L, hex.data(), hex.size()); return 1;
    }

    // ---- PvP and zones (docs/API.md "PvP и зоны") ----
    static void PushZone(lua_State* L, const Zone& z) { PushJson(L, Server::ZoneJson(z)); }
    static int SetPvp(lua_State* L) { luaL_checktype(L, 1, LUA_TBOOLEAN); Srv(L).SetPvp(lua_toboolean(L, 1)); return 0; }
    static int GetPvp(lua_State* L) { lua_pushboolean(L, Srv(L).Pvp()); return 1; }
    // setPvpRule(function(victim, attacker, info) return true / false / nil end), or setPvpRule(nil)
    static int SetPvpRule(lua_State* L) {
        if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_settop(L, 1); lua_setfield(L, LUA_REGISTRYINDEX, "dgmp.pvprule");
        return 0;
    }
    static int CanDamage(lua_State* L) {
        auto& victim = CheckPlayer(L, 1); auto& attacker = CheckPlayer(L, 2);
        lua_pushboolean(L, &victim != &attacker && Self(L).PvpRule(victim, attacker, Srv(L).CanHurt(victim, attacker), "", ""));
        return 1;
    }
    static int Zones(lua_State* L) {
        const auto& list = Srv(L).Zones();
        lua_createtable(L, int(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i) { PushZone(L, list[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
    static int GetZone(lua_State* L) {
        if (const Zone* z = Srv(L).FindZone(luaL_checkstring(L, 1))) PushZone(L, *z); else lua_pushnil(L);
        return 1;
    }
    // createZone({name, type = "pve"|"pvp", x, y, z, radius | width + length, yaw, height, ...}) -> name
    // Lua errors jump over C++ frames: they are raised only once the strings and zones are gone.
    static int CreateZone(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        for (const char* k : {"x", "y", "z"}) { lua_getfield(L, 1, k); if (!lua_isnumber(L, -1)) return luaL_error(L, "createZone: %s is required", k); lua_pop(L, 1); }
        char error[160] = "";
        {
            Zone z; std::string e;
            const json::Value v = ToJson(L, 1, 0);
            if (!v["name"].isString()) for (int n = 1;; ++n) if (!Srv(L).FindZone("zone" + std::to_string(n))) { z.name = "zone" + std::to_string(n); break; }
            if (!Server::ZoneFromJson(v, z, e)) snprintf(error, sizeof error, "%s", e.c_str());
            else { Srv(L).SetZone(z); lua_pushstring(L, z.name.c_str()); }
        }
        if (error[0]) return luaL_error(L, "createZone: %s", error);
        return 1;
    }
    // setZone(name, {enabled = false, type = "pvp", radius = 40, ...}): changes only what is given
    static int SetZone(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TTABLE);
        char error[160] = "";
        {
            Zone* found = Srv(L).FindZone(name);
            if (!found) { lua_pushboolean(L, 0); return 1; }
            Zone z = *found; std::string e;
            json::Value v = ToJson(L, 2, 0); v.erase("name");  // renaming would break the players' zone lists
            if (!Server::ZoneFromJson(v, z, e)) snprintf(error, sizeof error, "%s", e.c_str());
            else { Srv(L).SetZone(z); lua_pushboolean(L, 1); }
        }
        if (error[0]) return luaL_error(L, "setZone: %s", error);
        return 1;
    }
    static int RemoveZone(lua_State* L) { lua_pushboolean(L, Srv(L).RemoveZone(luaL_checkstring(L, 1))); return 1; }
    // zonesAt(x, y, z [, world]) -> names of the enabled zones there
    static int ZonesAt(lua_State* L) {
        const float x = float(luaL_checknumber(L, 1)), y = float(luaL_checknumber(L, 2)), zz = float(luaL_checknumber(L, 3));
        const bool anyWorld = lua_isnoneornil(L, 4); const auto world = int32_t(luaL_optinteger(L, 4, 0));
        lua_newtable(L); int n = 0;
        for (auto& z : Srv(L).Zones())
            if (z.enabled && (anyWorld || z.world == -1 || z.world == world) && z.Contains(x, y, zz)) { lua_pushstring(L, z.name.c_str()); lua_rawseti(L, -2, ++n); }
        return 1;
    }
    // player:setPvp(true / false / nil): always / never / as the rules say
    static int PSetPvp(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        p.pvp = lua_isnoneornil(L, 2) ? int8_t(-1) : int8_t(lua_toboolean(L, 2) ? 1 : 0);
        return 0;
    }
    static int PZones(lua_State* L) {
        auto& p = CheckPlayer(L, 1);
        lua_createtable(L, int(p.zones.size()), 0);
        for (size_t i = 0; i < p.zones.size(); ++i) { lua_pushstring(L, p.zones[i].c_str()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
    static int PInZone(lua_State* L) {
        auto& p = CheckPlayer(L, 1); const std::string name = luaL_checkstring(L, 2);
        lua_pushboolean(L, std::find(p.zones.begin(), p.zones.end(), name) != p.zones.end()); return 1;
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
            if (array) lua_pushnil(L);  // (only then: a nil left on the stack broke the caller's lua_next for nested tables)
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
    // ---- quests (quests/<id>.json, written by the in-game quest editor) ----
    static bool QuestId(const std::string& id) {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
        return true;
    }
    // The logic objects placed in the editor for this mode (modes/<mode>/world.json "logic"), read
    // fresh: { name = {x, y, z, yaw, radius} or {{...}, {...}} }.
    static int WorldLogic(lua_State* L) {
        std::ifstream in(Srv(L).root() / "modes" / Self(L).mode_ / "world.json", std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        json::Value v;
        if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject() || !v["logic"].isObject()) { lua_newtable(L); return 1; }
        PushJson(L, v["logic"]);
        return 1;
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
    // quests.watch(): from now on a change of quests/*.json reloads the mode (jobs read their files once).
    static int QuestsWatch(lua_State* L) {
        auto& s = Self(L);
        if (!s.watchQuests_) { s.watchQuests_ = true; s.loadedWrite_ = s.NewestWrite(); }
        return 0;
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
    // min(), not {}: libstdc++'s file clock counts from 2174, so real file times are below zero there.
    auto newest = std::filesystem::file_time_type::min();
    std::error_code ec;
    // The mode, the shared library it requires (modes/_lib) and the server's scripts (scripts/).
    for (const auto& root : {dir_, dir_.parent_path() / "_lib", server_.root() / "scripts"}) {
        ec.clear();
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const auto ext = it->path().extension();
            if (ext == ".lua" || ext == ".ini" || ext == ".json") newest = std::max(newest, it->last_write_time(ec));  // world.json: the editor saved
        }
    }
    if (watchQuests_) {
        ec.clear();
        for (auto it = std::filesystem::directory_iterator(server_.root() / "quests", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".json") newest = std::max(newest, it->last_write_time(ec));
    }
    return newest;
}

bool Script::ChangedOnDisk() const {
    if (dir_.empty()) return false;
    return NewestWrite() != loadedWrite_;
}

bool Script::Load(const std::string& modeId) {
    Unload();
    mode_ = modeId; title_ = modeId; watchQuests_ = false;
    server_.ProtectItems({});  // each mode says what it protects
    const bool safeId = !modeId.empty() && std::all_of(modeId.begin(), modeId.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    dir_ = server_.root() / "modes" / modeId;
    if (!safeId || !std::filesystem::exists(dir_ / "main.lua")) {
        server_.Log("Mode \"%s\" not found (expected modes/%s/main.lua)", modeId.c_str(), modeId.c_str());
        loadedWrite_ = safeId ? NewestWrite() : std::filesystem::file_time_type::min();
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
    // <mode>\?.lua;<_lib>\?.lua with the system's separator ('/' on Linux).
    const auto modePattern = (dir_ / "?.lua").u8string(), libPattern = (server_.root() / "modes" / "_lib" / "?.lua").u8string();
    const std::string path = std::string(modePattern.begin(), modePattern.end()) + ";" + std::string(libPattern.begin(), libPattern.end());
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
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.dialogs");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.interactions");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.keys");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.routes");

    luaL_newmetatable(L_, kPlayerMeta);
    lua_pushcfunction(L_, &Api::PIndex); lua_setfield(L_, -2, "__index");
    lua_pushcfunction(L_, &Api::PEq); lua_setfield(L_, -2, "__eq");
    lua_pushcfunction(L_, &Api::PToString); lua_setfield(L_, -2, "__tostring");
    const luaL_Reg methods[] = {{"message", &Api::PMessage}, {"position", &Api::PPosition}, {"velocity", &Api::PVelocity}, {"setPosition", &Api::PSetPosition},
                                {"isOnBike", &Api::PIsOnBike}, {"kick", &Api::PKick}, {"ban", &Api::PBan}, {"setHealth", &Api::PSetHealth}, {"heal", &Api::PHeal}, {"kill", &Api::PKill}, {"setTime", &Api::PSetTime}, {"setWeather", &Api::PSetWeather}, {"playAnim", &Api::PPlayAnim}, {"stopAnim", &Api::PStopAnim}, {"playSound", &Api::PPlaySound}, {"damage", &Api::PDamage}, {"hint", &Api::PHint},
                                {"giveItem", &Api::PGiveItem}, {"takeItem", &Api::PTakeItem}, {"giveWeapon", &Api::PGiveWeapon}, {"setAmmo", &Api::PSetAmmo},
                                {"takeWeapons", &Api::PTakeWeapons}, {"takeTools", &Api::PTakeTools}, {"clearInventory", &Api::PClearInventory},
                                {"inventory", &Api::PInventory}, {"itemCount", &Api::PItemCount}, {"giveBike", &Api::PGiveBike}, {"takeBike", &Api::PTakeBike}, {"putOnBike", &Api::PPutOnBike}, {"freeze", &Api::PFreeze}, {"spectate", &Api::PSpectate}, {"setSkin", &Api::PSetSkin}, {"dialog", &Api::PDialog}, {"putInVehicle", &Api::PPutInVehicle}, {"markVehicle", &Api::PMarkVehicle}, {"exitVehicle", &Api::PExitVehicle}, {"setWorld", &Api::PSetWorld}, {"closeDialog", &Api::PCloseDialog}, {"hud", &Api::PHud}, {"clearHud", &Api::PClearHud}, {"setUniform", &Api::PSetUniform}, {"customize", &Api::PCustomize},
                                {"setAdmin", &Api::PSetAdmin}, {"objective", &Api::PObjective}, {"objectiveMeter", &Api::PObjectiveMeter}, {"setLook", &Api::PSetLook}, {"tip", &Api::PTip}, {"restoreInventory", &Api::PRestoreInventory},
                                {"setPvp", &Api::PSetPvp}, {"zones", &Api::PZones}, {"inZone", &Api::PInZone},
                                {"giveCarry", &Api::PGiveCarry}, {"dropCarry", &Api::PDropCarry}, {"giveMoney", &Api::PGiveMoney}, {"setMoney", &Api::PSetMoney}, {nullptr, nullptr}};
    luaL_newlib(L_, methods); lua_setfield(L_, -2, "methods");
    lua_pop(L_, 1);

    const luaL_Reg globals[] = {{"print", &Api::Log}, {"log", &Api::Log}, {"on", &Api::On}, {"command", &Api::Command}, {"players", &Api::Players},
                                {"getPlayer", &Api::GetPlayer}, {"broadcast", &Api::Broadcast}, {"setTime", &Api::SetTime}, {"setWeather", &Api::SetWeather},
                                {"hint", &Api::HintAll}, {"now", &Api::Now}, {"createInteraction", &Api::CreateInteraction}, {"createActor", &Api::CreateActor}, {"createIcon", &Api::CreateIcon}, {"setIcon", &Api::SetIcon}, {"removeIcon", &Api::RemoveIcon}, {"createVehicle", &Api::CreateVehicle}, {"driveVehicle", &Api::DriveVehicle}, {"stopVehicle", &Api::StopVehicle}, {"setVehicle", &Api::SetVehicle}, {"removeVehicle", &Api::RemoveVehicle}, {"vehicleInfo", &Api::VehicleInfo}, {"vehicles", &Api::VehiclesList}, {"getActor", &Api::GetActor}, {"worldActors", &Api::WorldActors}, {"setActor", &Api::SetActor}, {"removeActor", &Api::RemoveActor}, {"removeInteraction", &Api::RemoveInteraction}, {"bindKey", &Api::BindKey}, {"unbindKey", &Api::UnbindKey}, {"skinList", &Api::SkinList}, {"after", &Api::After}, {"every", &Api::Every}, {"cancelTimer", &Api::CancelTimer},
                                {"setNametags", &Api::SetNametags}, {"worldLogic", &Api::WorldLogic}, {"setCollisions", &Api::SetCollisions}, {"objective", &Api::ObjectiveAll}, {"createMarker", &Api::CreateMarker}, {"removeMarker", &Api::RemoveMarker}, {"spawnItem", &Api::SpawnItem}, {"despawnItem", &Api::DespawnItem}, {"looks", &Api::Looks}, {"tip", &Api::TipAll}, {"sha256", &Api::Sha256}, {"passwordHash", &Api::PasswordHashLua}, {"passwordCheck", &Api::PasswordCheckLua},
                                {"setPvp", &Api::SetPvp}, {"pvp", &Api::GetPvp}, {"setPvpRule", &Api::SetPvpRule}, {"canDamage", &Api::CanDamage}, {"zones", &Api::Zones}, {"getZone", &Api::GetZone},
                                {"createZone", &Api::CreateZone}, {"setZone", &Api::SetZone}, {"removeZone", &Api::RemoveZone}, {"zonesAt", &Api::ZonesAt},
                                {"createCarry", &Api::CreateCarry}, {"setCarry", &Api::SetCarry}, {"removeCarry", &Api::RemoveCarry}, {"getCarry", &Api::GetCarry}, {"carries", &Api::Carries},
                                {"playSound", &Api::PlaySoundAll},
                                {"getDoor", &Api::GetDoor}, {"doors", &Api::DoorsList}, {"setDoor", &Api::SetDoor}, {"getVehicle", &Api::GetVehicleByKey}, {"loot", &Api::LootList},
                                {"createBlip", &Api::CreateBlip}, {"setBlip", &Api::SetBlip}, {"removeBlip", &Api::RemoveBlip},
                                {"createObject", &Api::CreateObject}, {"setObject", &Api::SetObject}, {"moveObject", &Api::MoveObject}, {"stopObject", &Api::StopObject},
                                {"removeObject", &Api::RemoveObject}, {"getObject", &Api::GetObject}, {"objects", &Api::ObjectsList},
                                {"ban", &Api::BanGlobal}, {"unban", &Api::Unban}, {"bans", &Api::Bans}, {"protectItems", &Api::ProtectItems}, {"protectedItems", &Api::ProtectedItems}, {nullptr, nullptr}};
    lua_pushglobaltable(L_); luaL_setfuncs(L_, globals, 0); lua_pop(L_, 1);
    const luaL_Reg storage[] = {{"get", &Api::StorageGet}, {"set", &Api::StorageSet}, {"keys", &Api::StorageKeys}, {"save", &Api::StorageSave}, {nullptr, nullptr}};
    luaL_newlib(L_, storage); lua_setglobal(L_, "storage");
    const luaL_Reg quests[] = {{"load", &Api::QuestsLoad}, {"list", &Api::QuestsList}, {"watch", &Api::QuestsWatch}, {nullptr, nullptr}};
    luaL_newlib(L_, quests); lua_setglobal(L_, "quests");
    const luaL_Reg http[] = {{"request", &Api::HttpRequestLua}, {"get", &Api::HttpGetLua}, {"post", &Api::HttpPostLua}, {nullptr, nullptr}};
    luaL_newlib(L_, http); lua_setglobal(L_, "http");
    const luaL_Reg jsonLib[] = {{"encode", &Api::JsonEncode}, {"decode", &Api::JsonDecode}, {nullptr, nullptr}};
    luaL_newlib(L_, jsonLib); lua_setglobal(L_, "json");
    http_ = std::make_shared<HttpBox>(); httpInFlight_ = 0;
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.http");
    lua_newtable(L_); lua_setfield(L_, LUA_REGISTRYINDEX, "dgmp.objmoves");

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
    // The server's own scripts first (server.ini Scripts=): admin tools, logs, anti-cheat rules that every
    // mode gets, like SA-MP filterscripts. They share the mode's state; an error skips only that script.
    for (const auto& name : server_.config().scripts) {
        const bool safe = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
        const auto file = (server_.root() / "scripts" / (name + ".lua")).u8string();
        g_deadline = Clock() + kCallBudget;
        const int st = safe ? luaL_loadfilex(L_, reinterpret_cast<const char*>(file.c_str()), "t") : LUA_ERRFILE;
        const int ok = st == LUA_OK ? lua_pcall(L_, 0, 0, 0) : st;
        g_deadline = 0;
        if (ok != LUA_OK) { server_.Log("[script %s] load error: %s", name.c_str(), safe ? lua_tostring(L_, -1) : "bad name"); if (safe) lua_pop(L_, 1); }
        else server_.Log("Script %s loaded", name.c_str());
    }
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
    if (auto jobs = IniValue(dir_ / "meta.ini", "Mode", "Jobs"); !jobs.empty()) StartJobs(jobs);
    Fire("start", 0);
    // Players already on the server (after a reload) are announced again so the mode can rebuild state.
    for (auto p : server_.Players()) PlayerJoin(*p);
    return true;
}

// meta.ini [Mode] Jobs=all starts every job file of quests/ (modes/_lib/jobs.lua), Jobs=a,b only those:
// the same as require("jobs") / Quest.runJob(id) in main.lua, for modes without code for it.
void Script::StartJobs(const std::string& list) {
    std::string chunk;
    if (list == "all" || list == "*" || list == "1") chunk = "require(\"jobs\")";
    else {
        chunk = "local Quest = require(\"quest\") for _, id in ipairs({";
        for (size_t start = 0; start <= list.size();) {
            const size_t end = std::min(list.find(',', start), list.size());
            std::string id = list.substr(start, end - start);
            id.erase(0, id.find_first_not_of(" \t")); id.erase(id.find_last_not_of(" \t") + 1);
            if (Api::QuestId(id)) chunk += "\"" + id + "\",";
            else if (!id.empty()) server_.Log("[%s] meta.ini Jobs: bad job id \"%s\"", mode_.c_str(), id.c_str());
            start = end + 1;
        }
        chunk += "}) do if not Quest.runJob(id) then log(\"meta.ini Jobs: no quests/\" .. id .. \".json\") end end";
    }
    g_deadline = Clock() + kCallBudget;
    if (luaL_loadbufferx(L_, chunk.data(), chunk.size(), "=meta.ini Jobs", "t") != LUA_OK || lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        server_.Log("[%s] meta.ini Jobs: %s", mode_.c_str(), lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    g_deadline = 0;
}

void Script::SaveStorage() {
    if (!storageDirty_ || storageFile_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(storageFile_.parent_path(), ec);
    auto tmp = storageFile_; tmp += ".tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << storage_.dump(); if (!out) { server_.Log("[%s] cannot write storage", mode_.c_str()); return; } }
    std::filesystem::rename(tmp, storageFile_, ec);  // replaces the old file in one step (MoveFileEx / rename)
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

// A callback kept in a registry table under `key`, called with the player and what is on the stack.
static bool CallKept(lua_State* L, const char* table, lua_Integer key, bool once, Player& p, int nargs, Server& server, const std::string& mode, const char* what);

void Script::DialogResponse(Player& p, uint16_t id, bool ok, int item, const std::string& text) {
    if (!L_) return;
    auto args = [&] {
        lua_pushboolean(L_, ok);
        if (item >= 0) lua_pushinteger(L_, item + 1); else lua_pushnil(L_);
        lua_pushlstring(L_, text.data(), text.size());
    };
    args(); CallKept(L_, "dgmp.dialogs", p.id, true, p, 3, server_, mode_, "dialog onResponse");
    Api::PushPlayer(L_, p); lua_pushinteger(L_, id); args();
    Fire("dialogResponse", 5);
}
int Script::VehicleEnter(Player& p, uint16_t vehicle, bool allowedByLock, bool passenger) {
    if (!L_) return 0;
    Api::PushPlayer(L_, p); lua_pushinteger(L_, vehicle); lua_pushboolean(L_, allowedByLock); lua_pushboolean(L_, passenger);
    int result = 0;
    Fire("vehicleEnter", 4, &result);
    return (result & 1) ? 1 : (result & 2) ? 2 : 0;
}
bool Script::CarryPick(Player& p, uint16_t carry) {
    if (!L_) return true;
    Api::PushPlayer(L_, p); lua_pushinteger(L_, carry);
    int result = 0;
    Fire("carryPick", 2, &result);
    return !(result & 1);
}
bool Script::CarryDrop(Player& p, uint16_t carry, float x, float y, float z, const char* reason) {
    if (!L_) return true;
    Api::PushPlayer(L_, p); lua_pushinteger(L_, carry);
    lua_createtable(L_, 0, 4);
    lua_pushnumber(L_, x); lua_setfield(L_, -2, "x");
    lua_pushnumber(L_, y); lua_setfield(L_, -2, "y");
    lua_pushnumber(L_, z); lua_setfield(L_, -2, "z");
    lua_pushstring(L_, reason); lua_setfield(L_, -2, "reason");
    int result = 0;
    Fire("carryDrop", 3, &result);
    return !(result & 1);
}
void Script::ObjectMoved(uint16_t object) {
    if (!L_) return;
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.objmoves");
    lua_rawgeti(L_, -1, object);
    lua_pushnil(L_); lua_rawseti(L_, -3, object);
    if (lua_isfunction(L_, -1)) {
        lua_pushinteger(L_, object);
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) { server_.Log("[%s] onArrive error: %s", mode_.c_str(), lua_tostring(L_, -1)); lua_pop(L_, 1); }
        g_deadline = 0;
    } else lua_pop(L_, 1);
    lua_pop(L_, 1);
    lua_pushinteger(L_, object); Fire("objectMoved", 1);
}

void Script::PlayerShot(Player& p, const std::string& weaponClass, float x, float y, float z) {
    if (!L_) return;
    Api::PushPlayer(L_, p);
    lua_createtable(L_, 0, 4);
    lua_pushstring(L_, weaponClass.c_str()); lua_setfield(L_, -2, "weapon");
    lua_pushnumber(L_, x); lua_setfield(L_, -2, "x");
    lua_pushnumber(L_, y); lua_setfield(L_, -2, "y");
    lua_pushnumber(L_, z); lua_setfield(L_, -2, "z");
    Fire("playerShot", 2);
}

int Script::DoorUse(Player& p, const std::string& door, const char* side) {
    if (!L_) return 0;
    Api::PushPlayer(L_, p); lua_pushstring(L_, door.c_str()); lua_pushstring(L_, side);
    int result = 0;
    Fire("door", 3, &result);
    return (result & 1) ? 1 : (result & 2) ? 2 : 0;
}

void Script::AnimEnd(Player& p, const char* reason) {
    if (!L_) return;
    Api::PushPlayer(L_, p); lua_pushstring(L_, reason); Fire("animEnd", 2);
}

void Script::VehicleExit(Player& p, uint16_t vehicle) {
    if (!L_) return;
    Api::PushPlayer(L_, p); lua_pushinteger(L_, vehicle);
    Fire("vehicleExit", 2);
}
void Script::VehicleArrive(uint16_t vehicle) {
    if (!L_) return;
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.routes");
    lua_rawgeti(L_, -1, vehicle);
    lua_pushnil(L_); lua_rawseti(L_, -3, vehicle);  // once
    if (lua_isfunction(L_, -1)) {
        lua_pushinteger(L_, vehicle);
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) { server_.Log("[%s] driveVehicle onArrive error: %s", mode_.c_str(), lua_tostring(L_, -1)); lua_pop(L_, 1); }
        g_deadline = 0;
    } else lua_pop(L_, 1);
    lua_pop(L_, 1);
    lua_pushinteger(L_, vehicle);
    Fire("vehicleArrive", 1);
}
void Script::Use(Player& p, uint16_t interaction) {
    if (!L_) return;
    lua_pushinteger(L_, interaction); CallKept(L_, "dgmp.interactions", interaction, false, p, 1, server_, mode_, "interaction onUse");
    Api::PushPlayer(L_, p); lua_pushinteger(L_, interaction);
    Fire("use", 2);
}
void Script::KeyPress(Player& p, uint8_t key) {
    if (!L_) return;
    const std::string name = Api::KeyName(key);
    lua_pushstring(L_, name.c_str()); CallKept(L_, "dgmp.keys", key, false, p, 1, server_, mode_, "bindKey");
    Api::PushPlayer(L_, p); lua_pushstring(L_, name.c_str());
    Fire("key", 2);
}
static bool CallKept(lua_State* L, const char* table, lua_Integer key, bool once, Player& p, int nargs, Server& server, const std::string& mode, const char* what) {
    // Stack: nargs values (consumed). The callback gets the player first.
    const int base = lua_gettop(L) - nargs;
    lua_getfield(L, LUA_REGISTRYINDEX, table);
    lua_rawgeti(L, -1, key);
    if (once) { lua_pushnil(L); lua_rawseti(L, -3, key); }
    bool called = false;
    if (lua_isfunction(L, -1)) {
        Api::PushPlayer(L, p);
        for (int a = 1; a <= nargs; ++a) lua_pushvalue(L, base + a);
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L, nargs + 1, 0, 0) != LUA_OK) server.Log("[%s] %s error: %s", mode.c_str(), what, lua_tostring(L, -1));
        g_deadline = 0;
        called = true;
    }
    lua_settop(L, base);
    return called;
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

bool Script::PlayerDeath(Player& p, Player* killer) {
    if (!L_) return false;
    int result = 0;
    Api::PushPlayer(L_, p);
    lua_createtable(L_, 0, int(p.inventoryAtDeath.size()));
    for (auto& [k, v] : p.inventoryAtDeath) { lua_pushinteger(L_, v); lua_setfield(L_, -2, k.c_str()); }
    if (killer) Api::PushPlayer(L_, *killer); else lua_pushnil(L_);
    Fire("playerDeath", 3, &result);
    return (result & 2) != 0;
}

// Handlers run in order; each sees the damage the previous ones left. A handler returns false to
// cancel the hit, a number to change the damage, nothing to keep it.
void Script::CustomizeDone(Player& p, const std::string& skin, const std::string& id) {
    if (!L_) return;
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.customize");
    if (lua_istable(L_, -1)) {
        lua_rawgeti(L_, -1, p.id);
        lua_pushnil(L_); lua_rawseti(L_, -3, p.id);  // once
        if (lua_isfunction(L_, -1)) {
            Api::PushPlayer(L_, p);
            if (skin.empty()) lua_pushnil(L_); else lua_pushstring(L_, skin.c_str());
            if (id.empty()) lua_pushnil(L_); else lua_pushstring(L_, id.c_str());
            g_deadline = Clock() + kCallBudget;
            if (lua_pcall(L_, 3, 0, 0) != LUA_OK) { server_.Log("[%s] customize onDone error: %s", mode_.c_str(), lua_tostring(L_, -1)); lua_pop(L_, 1); }
            g_deadline = 0;
        } else lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    Api::PushPlayer(L_, p);
    if (skin.empty()) lua_pushnil(L_); else lua_pushstring(L_, skin.c_str());
    if (id.empty()) lua_pushnil(L_); else lua_pushstring(L_, id.c_str());
    Fire("customize", 3);
}

float Script::PlayerDamage(Player& victim, Player& attacker, float damage, const char* zone, const std::string& weapon) {
    if (!L_) return damage;
    const int base = lua_gettop(L_);
    lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.events");
    if (lua_getfield(L_, -1, "playerDamage") == LUA_TTABLE) {
        const auto n = static_cast<int>(lua_rawlen(L_, -1));
        for (int i = 1; i <= n && damage >= 0; ++i) {
            lua_rawgeti(L_, -1, i);
            Api::PushPlayer(L_, victim); Api::PushPlayer(L_, attacker); lua_pushnumber(L_, damage);
            lua_createtable(L_, 0, 2);
            lua_pushstring(L_, zone); lua_setfield(L_, -2, "zone");
            lua_pushstring(L_, weapon.c_str()); lua_setfield(L_, -2, "weapon");
            g_deadline = Clock() + kCallBudget;
            if (lua_pcall(L_, 4, 1, 0) != LUA_OK) server_.Log("[%s] error in playerDamage handler: %s", mode_.c_str(), lua_tostring(L_, -1));
            else if (lua_isboolean(L_, -1) && !lua_toboolean(L_, -1)) damage = -1;
            else if (lua_isnumber(L_, -1)) damage = std::max(0.f, float(lua_tonumber(L_, -1)));
            g_deadline = 0;
            lua_pop(L_, 1);
        }
    }
    lua_settop(L_, base);
    return damage;
}

bool Script::PvpRule(Player& victim, Player& attacker, bool allowed, const char* zone, const std::string& weapon) {
    if (!L_) return allowed;
    const int base = lua_gettop(L_);
    if (lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.pvprule") != LUA_TFUNCTION) { lua_settop(L_, base); return allowed; }
    Api::PushPlayer(L_, victim); Api::PushPlayer(L_, attacker);
    lua_createtable(L_, 0, 5);
    lua_pushboolean(L_, allowed); lua_setfield(L_, -2, "allowed");
    if (zone[0]) { lua_pushstring(L_, zone); lua_setfield(L_, -2, "zone"); lua_pushstring(L_, weapon.c_str()); lua_setfield(L_, -2, "weapon"); }
    if (const Zone* z = server_.RuleZone(victim)) { lua_pushstring(L_, z->name.c_str()); lua_setfield(L_, -2, "victimZone"); }
    if (const Zone* z = server_.RuleZone(attacker)) { lua_pushstring(L_, z->name.c_str()); lua_setfield(L_, -2, "attackerZone"); }
    const double outer = g_deadline;  // canDamage() calls it from inside another handler
    g_deadline = Clock() + kCallBudget;
    if (lua_pcall(L_, 3, 1, 0) != LUA_OK) server_.Log("[%s] error in the PvP rule: %s", mode_.c_str(), lua_tostring(L_, -1));
    else if (lua_isboolean(L_, -1)) allowed = lua_toboolean(L_, -1);
    g_deadline = outer;
    lua_settop(L_, base);
    return allowed;
}

void Script::ZoneEvent(Player& p, const std::string& zone, bool entered) {
    if (!L_) return;
    Api::PushPlayer(L_, p);
    if (const Zone* z = server_.FindZone(zone)) Api::PushZone(L_, *z);
    else { lua_createtable(L_, 0, 1); lua_pushstring(L_, zone.c_str()); lua_setfield(L_, -2, "name"); }  // removed meanwhile
    Fire(entered ? "zoneEnter" : "zoneLeave", 2);
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
// textResult: the first text a handler returned (a string also counts as false for cancelResult).
void Script::Fire(const char* event, int nargs, int* cancelResult, std::string* textResult) {
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
            } else if (textResult && lua_type(L_, -1) == LUA_TSTRING) {
                if (textResult->empty()) *textResult = lua_tostring(L_, -1);
                if (cancelResult) *cancelResult |= 1;
            }
            g_deadline = 0;
            lua_pop(L_, 1);
        }
    }
    lua_settop(L_, base);
}

bool Script::PlayerConnecting(const std::string& nick, uint64_t steamId, const std::string& ip, std::string& reason) {
    if (!L_) return true;
    lua_createtable(L_, 0, 4);
    lua_pushstring(L_, nick.c_str()); lua_setfield(L_, -2, "name");
    if (steamId) { lua_pushstring(L_, std::to_string(steamId).c_str()); lua_setfield(L_, -2, "steamId"); }
    lua_pushboolean(L_, steamId != 0); lua_setfield(L_, -2, "verified");
    lua_pushstring(L_, ip.c_str()); lua_setfield(L_, -2, "ip");
    int result = 0;
    Fire("playerConnecting", 1, &result, &reason);
    return !(result & 1);
}

bool Script::Cheat(Player& p, const char* kind, const std::string& detail, int strikes) {
    if (!L_) return false;
    Api::PushPlayer(L_, p); lua_pushstring(L_, kind);
    lua_createtable(L_, 0, 2);
    lua_pushstring(L_, detail.c_str()); lua_setfield(L_, -2, "detail");
    lua_pushinteger(L_, strikes); lua_setfield(L_, -2, "strikes");
    int result = 0;
    Fire("cheat", 3, &result);
    return (result & 2) != 0;
}

bool Script::InventoryViolation(Player& p, const std::string& key, int have, int allowed, bool pickedUp) {
    if (!L_) return false;
    Api::PushPlayer(L_, p);
    lua_createtable(L_, 0, 4);
    lua_pushstring(L_, key.c_str()); lua_setfield(L_, -2, "item");
    lua_pushinteger(L_, have); lua_setfield(L_, -2, "have");
    lua_pushinteger(L_, allowed); lua_setfield(L_, -2, "allowed");
    lua_pushboolean(L_, pickedUp); lua_setfield(L_, -2, "pickedUp");
    int result = 0;
    Fire("inventoryViolation", 2, &result);
    return (result & 2) != 0;
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

// Finished http requests: their callbacks (ok, status, body, error).
void Script::TickHttp() {
    if (!http_) return;
    std::vector<HttpBox::Done> done;
    { std::lock_guard<std::mutex> g(http_->lock); done.swap(http_->done); }
    for (auto& d : done) {
        if (!L_) return;
        --httpInFlight_;
        lua_getfield(L_, LUA_REGISTRYINDEX, "dgmp.http");
        lua_rawgeti(L_, -1, d.id);
        lua_pushnil(L_); lua_rawseti(L_, -3, d.id);
        if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); continue; }
        lua_pushboolean(L_, d.ok); lua_pushinteger(L_, d.status); lua_pushlstring(L_, d.body.data(), d.body.size());
        if (d.error.empty()) lua_pushnil(L_); else lua_pushstring(L_, d.error.c_str());
        g_deadline = Clock() + kCallBudget;
        if (lua_pcall(L_, 4, 0, 0) != LUA_OK) { server_.Log("[%s] http callback error: %s", mode_.c_str(), lua_tostring(L_, -1)); lua_pop(L_, 1); }
        g_deadline = 0;
        lua_pop(L_, 1);  // dgmp.http
    }
}

void Script::Tick(float dt) {
    if (!L_) return;
    const double now = server_.Now();
    TickHttp();
    if (!L_) return;
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
