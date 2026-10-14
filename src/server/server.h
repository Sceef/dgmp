#pragma once
#include "../shared/protocol.h"
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace srv {

struct Config {
    std::string name = "DGMP Server";
    std::string mode = "freeroam";
    std::string password;
    std::string bind;          // empty = all interfaces
    uint16_t port = net::kDefaultPort;
    int maxPlayers = 16;
    float spawnX = -85462.8f, spawnY = -301759.8f, spawnZ = -5179.7f, spawnYaw = 0;
    net::NametagMode nametags = net::NametagMode::Visible;
    std::vector<std::string> admins;  // nicknames (case-insensitive)
};

// A quest marker shown on the map, the minimap and in the world (the game's own waypoint actor).
struct Marker {
    uint16_t id = 0;
    float x = 0, y = 0, z = 0, radius = 300;
    uint8_t category = 9, style = 0;  // EObjectiveCategories (9 = Task) / EObjectiveVisualType (0 = Basic)
    bool route = false;
    bool track = true;                // false: shown only, no enter/leave events
    std::string icon;                 // Texture2D asset path for map and world icon ("" = by category)
    std::string title, description;   // map tooltip
    uint16_t only = 0xFFFF;           // player id it is shown to; 0xFFFF = everyone
    double onlyJoinedAt = 0;          // identity of that player
};

// Something a player did in the world, as reported by its client (Msg::GameEvent).
struct GameEvent {
    uint8_t type = 0;               // net::GameEventType
    std::string cls, detail, id;
    float x = 0, y = 0, z = 0;
};

// A pickup placed in the world by the mode (quest items); each client spawns its own copy.
struct WorldItem {
    uint16_t id = 0;
    std::string key;
    int count = 1;
    float x = 0, y = 0, z = 0;
    uint16_t only = 0xFFFF;           // player id it is shown to; 0xFFFF = everyone
    double onlyJoinedAt = 0;
};

struct Player {
    uint16_t id = 0;
    std::string nick;
    _ENetPeer* peer = nullptr;  // null for bots
    net::PlayerState state;
    bool bot = false;
    bool ready = false;         // received Hello and was welcomed
    double joinedAt = 0;
    float botPhase = 0;
    double poseWindow = 0; int posesInWindow = 0;  // relay rate limit
    std::map<std::string, int> inventory;          // last snapshot reported by the client
    std::string look;                              // look asset path the player wears
    std::vector<uint16_t> insideMarkers;           // markers whose radius the player is in
    bool admin = false;
    double eventWindow = 0; int eventCount = 0;    // game event rate limit
    std::map<std::string, int> inventoryAtDeath;   // snapshot when the player died
};

class Script;

class Server {
public:
    Server();
    ~Server();
    bool Start(const Config& cfg, const std::filesystem::path& root);
    void Run();                 // returns when Quit() was requested
    void Quit() { quit_ = true; }
    void PushConsole(const std::string& line);  // thread-safe; executed on the server thread

    // ---- API used by the console and by Lua ----
    const Config& config() const { return cfg_; }
    const std::filesystem::path& root() const { return root_; }
    Player* Find(uint16_t id);
    Player* FindByName(const std::string& nick);
    std::vector<Player*> Players();
    int PlayerCount() const;
    net::ServerInfo Info() const;  // answer to status queries
    double Now() const;

    void Message(Player* to, uint32_t rgba, const std::string& text);  // to=null: everyone
    void SetTime(float hours, bool paused);
    void SetWeather(int id);
    void Teleport(Player* p, float x, float y, float z, float yaw);
    void Hint(Player* to, const std::string& title, const std::string& text, const std::string& image, bool pause, float seconds = 0);
    void Kick(Player* p, const std::string& reason);
    void Item(Player* p, net::ItemAction action, const std::string& key, int amount);
    void Bike(Player* p, net::BikeAction action, float x = 0, float y = 0, float z = 0, float yaw = 0);
    void SetNametags(net::NametagMode mode);
    void Objective(Player* p, uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp = 0);
    void ObjectiveMeter(Player* p, bool show, const std::string& title, float percent);
    uint16_t AddMarker(Marker m);
    bool RemoveMarker(uint16_t id);
    const Marker* FindMarker(uint16_t id) const;
    uint16_t AddWorldItem(WorldItem item);
    bool RemoveWorldItem(uint16_t id);
    // Clears the backpack and gives `inv` back, skipping keys that start with one of `drop`.
    void RestoreInventory(Player* p, const std::map<std::string, int>& inv, const std::vector<std::string>& drop);
    void SetLook(Player* p, const std::string& path);   // order: the player wears it, everyone sees it
    std::string ResolveLook(const std::string& nameOrPath) const;  // short name from data/looks.txt or full path; "" if unknown
    std::vector<std::string> LookNames(const std::string& filter) const;
    Player* AddBot(const std::string& nick);
    void RemoveBots();
    bool ReloadMode();

    void Log(const char* fmt, ...);

private:
    void HandleConsole(const std::string& line);
    void OnConnect(_ENetPeer* peer);
    void OnDisconnect(_ENetPeer* peer, uint8_t reason);
    void OnPacket(_ENetPeer* peer, const uint8_t* data, size_t size);
    void OnHello(_ENetPeer* peer, net::Reader& r);
    void OnChat(Player& p, const std::string& text);
    void Send(Player* to, const net::Writer& w, bool reliable);  // to=null: every ready client
    void SendTo(_ENetPeer* peer, const net::Writer& w, bool reliable);
    void BroadcastStates();
    void RelayPose(Player& from, const uint8_t* payload, size_t size);
    void TickBots(float dt);
    void SendMarker(_ENetPeer* peer, const Marker& m);
    void TickMarkers();
    std::map<uint16_t, Marker> markers_;
    std::map<uint16_t, WorldItem> items_;
    uint16_t nextItem_ = 1;
    void SendWorldItem(_ENetPeer* peer, const WorldItem& item);
    uint16_t nextMarker_ = 1;
    void RemovePlayer(Player* p, uint8_t reason);
    uint16_t FreeId() const;

    Config cfg_;
    std::filesystem::path root_;
    _ENetHost* host_ = nullptr;
    std::map<uint16_t, std::unique_ptr<Player>> players_;
    std::unique_ptr<Script> script_;
    std::vector<std::pair<std::string, std::string>> looks_;  // name, asset path
    void LoadLooks();
    std::atomic<bool> quit_{false};
    std::mutex consoleLock_;
    std::vector<std::string> consoleQueue_;
    float timeHours_ = -1; bool timePaused_ = false; int weather_ = -2;  // -1 auto, -2 never set
    FILE* log_ = nullptr;
};

}  // namespace srv
