#pragma once
// Lua values <-> json::Value, shared by the server's Lua and the client scripts (include lua.h first).
// Tables with keys 1..n become arrays, other tables objects; functions and userdata are errors.
#include "json.h"
#include <string>

namespace luajson {

inline json::Value ToJson(lua_State* L, int idx, int depth = 0) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return json::Value(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER: return json::Value(lua_tonumber(L, idx));
    case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, idx, &n); return json::Value(std::string(s, n)); }
    case LUA_TTABLE: {
        if (depth > 32) luaL_error(L, "table nesting too deep");
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
            else luaL_error(L, "table keys must be strings or numbers");
            o.set(key, ToJson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return o;
    }
    case LUA_TNIL: case LUA_TNONE: return json::Value();
    default: luaL_error(L, "cannot turn a %s into data", luaL_typename(L, idx)); return json::Value();
    }
}

inline void PushJson(lua_State* L, const json::Value& v) {
    switch (v.type()) {
    case json::Value::Type::Null: lua_pushnil(L); break;
    case json::Value::Type::Bool: lua_pushboolean(L, v.asBool()); break;
    case json::Value::Type::Number: {
        const double d = v.asNumber();
        if (d == double(lua_Integer(d))) lua_pushinteger(L, lua_Integer(d)); else lua_pushnumber(L, d);
        break;
    }
    case json::Value::Type::String: lua_pushlstring(L, v.asString().data(), v.asString().size()); break;
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

}  // namespace luajson
