#pragma once
// Client side of the DGMP protocol: server queries / LAN discovery and the game connection.
#include "protocol.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace net {

bool InitNetwork();  // safe to call repeatedly

struct QueryResult {
    std::string host;   // as given / as answered (LAN)
    uint16_t port = 0;
    int pingMs = -1;    // -1: no answer
    ServerInfo info;
};

// Queries the given servers in parallel and waits up to timeoutMs for answers.
std::vector<QueryResult> QueryServers(const std::vector<std::pair<std::string, uint16_t>>& targets, int timeoutMs);
// Broadcasts a query on the local network (ports kDefaultPort .. +kLanPortSpan-1).
std::vector<QueryResult> ScanLan(int timeoutMs);
// "host", "host:port" or "[v6]:port" -> host + port (port defaults to kDefaultPort).
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port);

struct RemotePlayer {
    uint16_t id = 0;
    std::string nick;
    PlayerState state;
    double stateTime = 0;  // local receive time (seconds)
};

class Client {
public:
    enum class Status { Idle, Connecting, Handshake, Connected, Failed };
    struct Events {
        std::function<void(const std::string& serverName, const std::string& mode)> welcome;
        std::function<void(const std::string& reason)> disconnected;
        std::function<void(const RemotePlayer&)> playerJoin;
        std::function<void(uint16_t id, uint8_t reason)> playerQuit;
        std::function<void(uint32_t rgba, const std::string& text)> message;
        std::function<void(float hours, bool paused)> setTime;
        std::function<void(int weather)> setWeather;
        std::function<void(float x, float y, float z, float yaw)> teleport;
        std::function<void(const std::string& title, const std::string& text, const std::string& image, bool pause, float seconds)> hint;
        std::function<void(uint16_t id, const PlayerState& s)> state;                     // every received snapshot
        std::function<void(uint16_t id, const uint8_t* payload, size_t size)> pose;        // raw PoseFrame payload
        std::function<void(ItemAction action, const std::string& key, int amount)> item;
        std::function<void(BikeAction action, float x, float y, float z, float yaw)> bike;
        std::function<void(NametagMode nametags, uint8_t collisions)> settings;  // collisions: CollisionMask
        std::function<void(uint16_t id, const uint8_t* gear, size_t size)> gear;  // GearList payload
        std::function<void(uint16_t id, const std::string& lookPath)> look;  // id == localId(): wear it
        struct MarkerInfo { float x, y, z; uint8_t category, style; bool route; std::string icon, title, description; };
        std::function<void(uint16_t id, const MarkerInfo* info)> marker;  // info null = removed
        std::function<void(uint16_t id, const std::string* item, int count, float x, float y, float z, float yaw)> worldItem;  // item null = removed
        std::function<void(uint16_t playerId)> corpse;
        std::function<void(uint16_t attacker, float damage, HitZone zone)> damage;  // another player hurt you
        std::function<void(bool on)> freeze;
        // choices: (name, look) to pick from; empty: the full character creator
        // place: x, y, z, yaw where the server put the player to choose (null: where it is)
        std::function<void(bool cancel, const std::string& title, const std::string& families, const std::string& current,
                           const std::vector<std::pair<std::string, std::string>>& choices, const float* place)> customize;
        std::function<void(bool on, const std::vector<uint16_t>& only)> spectate;  // only: players to watch (empty: anyone)
        std::function<void(const std::string& json)> worldEdits;  // whole world.json once all parts arrived
        std::function<void(uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp)> objective;
        std::function<void(bool show, const std::string& title, float percent)> objectiveMeter;
        struct DialogInfo { uint16_t id = 0; DialogStyle style = DialogStyle::MsgBox; std::string title, text, button1, button2, defaultText, headers; std::vector<std::string> items; };
        std::function<void(const DialogInfo& d)> dialog;                           // style Close: close dialog d.id
        std::function<void(const std::string& id, const std::string& json)> hud;  // json "": remove (id "": all)
        struct InteractionInfo { float x, y, z, radius; uint8_t key; std::string text; int world = -1; };
        std::function<void(uint16_t id, const InteractionInfo* info)> interaction;  // info null = removed
        std::function<void(const std::vector<uint8_t>& keys)> watchKeys;
        std::function<void(int world)> setWorld;
        struct ActorInfo { float x, y, z, yaw; int world; std::string look, anim, name; bool solid; };
        std::function<void(uint16_t id, const ActorInfo* info)> actor;  // info null = removed
        struct VehicleInfo { VehicleState state; int world = 0; uint16_t driver = 0xFFFF; bool locked = false, yours = false, marked = false; uint32_t color = 0; std::string icon, model; bool npc = false; std::string npcLook, npcName; uint16_t passenger = 0xFFFF; uint8_t riders = 0; };  // marked: on your map; color RGBA (0: default); model "": the bike; npc: an NPC of the mode drives it
        std::function<void(uint16_t id, const VehicleInfo* info)> vehicle;  // info null = removed
        std::function<void(uint16_t id, int seat)> vehicleAllow;  // seat 0: the driver's
        // The way an NPC drives a vehicle: x,y,z of each point (empty: it stopped), units a second, seconds since it set off.
        std::function<void(uint16_t id, const std::vector<float>& points, float speed, float elapsed, bool loop)> vehicleRoute;
        std::function<void(uint16_t id)> vehicleEject;
        std::function<void(const std::vector<std::pair<std::string, std::string>>& images)> imageList;  // (name, sha256)
        std::function<void(const std::string& name, int part, int parts, const std::string& bytes)> imageData;
        struct IconInfo { float x, y, z, size, distance; int world; uint32_t color; std::string image, text; bool throughWalls = false; };
        std::function<void(uint16_t id, const IconInfo* info)> icon;  // info null = removed
        struct CarryInfo { std::string model; float x, y, z, yaw; int world; uint16_t carrier; uint8_t roll = 0; };  // roll: CarryRoll
        std::function<void(uint16_t id, const CarryInfo* info)> carry;  // info null = removed
    } on;

    ~Client();
    bool Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password);
    void Disconnect();
    void Service(double now);  // pump network; call every frame
    void SendState(const PlayerState& s);
    void SendChat(const std::string& text);
    void SendPose(const std::vector<uint8_t>& payload);  // payload without the op byte
    void SendInventory(const std::vector<std::pair<std::string, int>>& items);
    void SendHit(uint16_t victim, float damage, HitZone zone, const std::string& weapon);
    void SendGameEvent(GameEventType type, const std::string& cls, const std::string& detail, const std::string& id, float x, float y, float z);
    void SendLook(const std::string& lookPath);
    void SendGear(const std::vector<uint8_t>& gear);  // GearList payload
    void SendCustomizeDone(bool ok, const std::string& skin);
    void SendDialogResponse(uint16_t id, bool ok, int item, const std::string& text);
    void SendUse(uint16_t interaction);
    void SendKey(uint8_t virtualKey);
    void SendImageRequest(const std::string& name);
    void SendVehicle(Msg op, uint16_t id, const VehicleState* state = nullptr);  // VehicleEnter / VehicleExit / VehicleState
    void SendCarry(uint16_t id, CarryOp op, float x = 0, float y = 0, float z = 0, float yaw = 0);

    Status status() const { return status_; }
    uint16_t localId() const { return localId_; }
    int pingMs() const;
    const std::map<uint16_t, RemotePlayer>& players() const { return players_; }

private:
    std::string worldEditsBuffer_;  // Msg::WorldEdits parts
    void OnPacket(const uint8_t* data, size_t size, double now);
    void Fail(const std::string& reason);
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    Status status_ = Status::Idle;
    std::string nick_, password_, rejectReason_;
    uint16_t localId_ = 0xFFFF;
    double connectStarted_ = 0;
    std::map<uint16_t, RemotePlayer> players_;
};

}  // namespace net
