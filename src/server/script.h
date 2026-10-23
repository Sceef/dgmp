#pragma once
#include "server.h"
#include "../shared/json.h"
#include <filesystem>
#include <memory>
#include <mutex>
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

    // Someone is about to join (after the password, bans and the Steam check): playerConnecting handlers
    // may refuse (false, or a text: the reason shown to it). steamId 0: not confirmed by Steam.
    bool PlayerConnecting(const std::string& nick, uint64_t steamId, const std::string& ip, std::string& reason);
    // Anti-cheat strike (docs/API.md "Античит"): true when a handler returned true (the mode deals with it, no kick).
    bool Cheat(Player& p, const char* kind, const std::string& detail, int strikes);
    // The player has more of a protected item than the server gave: true when a handler accepts it.
    bool InventoryViolation(Player& p, const std::string& key, int have, int allowed, bool pickedUp);
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
    void UiMessage(Player& p, const std::string& name, const std::string& json);  // the mode's page: dgmp.send(name, data)
    void ClientMessage(Player& p, const std::string& name, const std::string& json);  // the mode's client script: send(name, data)
    void Use(Player& p, uint16_t interaction);  // pressed the key at an interaction
    void KeyPress(Player& p, uint8_t key);       // pressed a key bound with bindKey
    // The player wants on a vehicle: 0 no say (the lock decides), 1 refused, 2 allowed (handlers of vehicleEnter).
    int VehicleEnter(Player& p, uint16_t vehicle, bool allowedByLock, bool passenger);
    void VehicleExit(Player& p, uint16_t vehicle);
    // Boxes (docs/API.md "Переноска"): the player asks to pick one up (false: a handler refused), or it left
    // its hands at x, y, z (reason "put": it asks to set it down, false refuses; "lost", "death", "quit": told after).
    bool CarryPick(Player& p, uint16_t carry);
    bool CarryDrop(Player& p, uint16_t carry, float x, float y, float z, const char* reason);
    void VehicleArrive(uint16_t vehicle);  // its NPC got to the end of the way (driveVehicle onArrive, vehicleArrive)
    void ObjectMoved(uint16_t object);     // moveObject got there: its onArrive and the objectMoved event
    void ZombieDeath(uint16_t zombie, Player* killer);  // zombieDeath (killer null: nobody hurt it)
    void HordeDeath(uint16_t horde);                    // hordeDeath: the last zombie of a horde died
    void AnimEnd(Player& p, const char* reason);
    // [F] at a door of the mode: 0 no say (its lock decides), 1 refused, 2 allowed even when locked.
    int DoorUse(Player& p, const std::string& door, const char* side);
    void PlayerShot(Player& p, const std::string& weaponClass, float x, float y, float z);  // playerShot (at most twice a second per player)  // the animation of playAnim is over: "done", "moved"
    // A player started (on) or stopped talking with the talk / radio key (how "talk" / "radio"): false at
    // the start when a handler refused it (nobody hears this talk).
    bool Voice(Player& p, bool on, const char* how);
    void SaveStorage();  // flushes the mode's persistent storage (also done every few seconds)
    bool ChangedOnDisk() const;

private:
    friend struct Api;
    void Fire(const char* event, int nargs, int* cancelResult = nullptr, std::string* textResult = nullptr);
    std::filesystem::file_time_type NewestWrite() const;
    void StartJobs(const std::string& list);  // meta.ini [Mode] Jobs=all / Jobs=id,id

    Server& server_;
    lua_State* L_ = nullptr;
    std::string mode_, title_;
    std::filesystem::path dir_;
    std::filesystem::file_time_type loadedWrite_{};
    bool watchQuests_ = false;  // quests.watch(): quests/*.json count as the mode's files (a saved job reloads it)
    struct Timer { int id; int ref; double due; double every; };
    std::vector<Timer> timers_;
    int nextTimer_ = 1;
    // http.request: answers from worker threads, handed to their callbacks in Tick. A new load gets a new
    // box; threads of the old one finish into theirs.
    struct HttpBox {
        struct Done { int id; bool ok; int status; std::string body, error; };
        std::mutex lock;
        std::vector<Done> done;
    };
    std::shared_ptr<HttpBox> http_;
    int nextHttp_ = 1, httpInFlight_ = 0;
    void TickHttp();
    json::Value storage_ = json::Value::Object();
    std::filesystem::path storageFile_;
    bool storageDirty_ = false;
    double nextStorageSave_ = 0;
};

}  // namespace srv
