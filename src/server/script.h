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
    bool PlayerDeath(Player& p, Player* killer);  // true: a handler returned true (the mode handles the lost backpack)
    // A player hit another one: the damage the victim takes (handlers may change it), < 0 = cancelled.
    // The player closed the character creator: its onDone callback and the "customize" event; empty skin: cancelled.
    void CustomizeDone(Player& p, const std::string& skin, const std::string& id);
    float PlayerDamage(Player& victim, Player& attacker, float damage, const char* zone, const std::string& weapon);
    // Whether a player may hurt another one: `allowed` is the server's rule (PvP switch, zones,
    // setPvp); the mode's setPvpRule may answer otherwise (nil keeps it). zone "" when only asked (canDamage).
    bool PvpRule(Player& victim, Player& attacker, bool allowed, const char* zone, const std::string& weapon);
    void ZoneEvent(Player& p, const std::string& zone, bool entered);  // zoneEnter / zoneLeave
    void GameEvent(Player& p, const struct GameEvent& e);
    void MarkerEvent(Player& p, uint16_t marker, bool entered);
    // The player answered a dialog (item -1: none; text: the typed text or the chosen line).
    void DialogResponse(Player& p, uint16_t id, bool ok, int item, const std::string& text);
    void Use(Player& p, uint16_t interaction);  // pressed the key at an interaction
    void KeyPress(Player& p, uint8_t key);       // pressed a key bound with bindKey
    // The player wants on a vehicle: 0 no say (the lock decides), 1 refused, 2 allowed (handlers of vehicleEnter).
    int VehicleEnter(Player& p, uint16_t vehicle, bool allowedByLock, bool passenger);
    void VehicleExit(Player& p, uint16_t vehicle);
    void VehicleArrive(uint16_t vehicle);  // its NPC got to the end of the way (driveVehicle onArrive, vehicleArrive)
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
