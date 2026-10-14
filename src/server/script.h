#pragma once
#include "server.h"
#include "../shared/json.h"
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace srv {

// One running game mode: modes/<id>/main.lua in its own sandboxed Lua state.
class Script {
public:
    explicit Script(Server& server);
    ~Script();
    bool Load(const std::string& modeId);
    void Unload();
    const std::string& mode() const { return mode_; }
    const std::string& title() const { return title_; }

    void PlayerJoin(Player& p);
    void PlayerQuit(Player& p, uint8_t reason);
    bool PlayerChat(Player& p, const std::string& text);  // false = cancelled by the mode
    bool Command(Player& p, const std::string& name, const std::string& args);  // false = unknown
    bool Console(const std::string& line);                  // true = handled by the mode
    void Tick(float dt);
    void InventoryChange(Player& p);
    bool PlayerRespawn(Player& p);
    bool PlayerDeath(Player& p);    // true: a handler returned true (the mode handles the lost backpack)  // true: a handler returned true (it placed the player itself)
    void GameEvent(Player& p, const struct GameEvent& e);
    void MarkerEvent(Player& p, uint16_t marker, bool entered);
    void SaveStorage();  // flushes the mode's persistent storage (also done every few seconds)
    bool ChangedOnDisk() const;

private:
    friend struct Api;
    void Fire(const char* event, int nargs, int* cancelResult = nullptr);
    std::filesystem::file_time_type NewestWrite() const;

    Server& server_;
    lua_State* L_ = nullptr;
    std::string mode_, title_;
    std::filesystem::path dir_;
    std::filesystem::file_time_type loadedWrite_{};
    struct Timer { int id; int ref; double due; double every; };
    std::vector<Timer> timers_;
    int nextTimer_ = 1;
    json::Value storage_ = json::Value::Object();
    std::filesystem::path storageFile_;
    bool storageDirty_ = false;
    double nextStorageSave_ = 0;
};

}  // namespace srv
