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
        std::function<void(NametagMode nametags)> settings;
        std::function<void(uint16_t id, const std::string& lookPath)> look;  // id == localId(): wear it
        struct MarkerInfo { float x, y, z; uint8_t category, style; bool route; std::string icon, title, description; };
        std::function<void(uint16_t id, const MarkerInfo* info)> marker;  // info null = removed
        std::function<void(uint16_t id, const std::string* item, int count, float x, float y, float z)> worldItem;  // item null = removed
        std::function<void(uint16_t playerId)> corpse;
        std::function<void(uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp)> objective;
        std::function<void(bool show, const std::string& title, float percent)> objectiveMeter;
    } on;

    ~Client();
    bool Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password);
    void Disconnect();
    void Service(double now);  // pump network; call every frame
    void SendState(const PlayerState& s);
    void SendChat(const std::string& text);
    void SendPose(const std::vector<uint8_t>& payload);  // payload without the op byte
    void SendInventory(const std::vector<std::pair<std::string, int>>& items);
    void SendGameEvent(GameEventType type, const std::string& cls, const std::string& detail, const std::string& id, float x, float y, float z);
    void SendLook(const std::string& lookPath);

    Status status() const { return status_; }
    uint16_t localId() const { return localId_; }
    int pingMs() const;
    const std::map<uint16_t, RemotePlayer>& players() const { return players_; }

private:
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
