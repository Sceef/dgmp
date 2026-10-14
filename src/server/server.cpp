#include "server.h"
#include "script.h"
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace srv {
using namespace net;
namespace {
Server* g_server = nullptr;
const auto g_epoch = std::chrono::steady_clock::now();
constexpr uint32_t kSystemColor = 0xA9C4E4FF, kErrorColor = 0xE07070FF;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
std::string Address(const ENetAddress& a) {
    char ip[64] = {}; enet_address_get_host_ip(&a, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(a.port);
}
}  // namespace

// Status queries are raw datagrams; answer them before ENet sees them.
int OnRawPacket(ENetHost* host, ENetEvent*) {
    if (!g_server || host->receivedDataLength != 8 || std::memcmp(host->receivedData, net::kQueryMagic, 4) != 0) return 0;
    uint32_t token; std::memcpy(&token, host->receivedData + 4, 4);
    const auto info = g_server->Info();
    const auto reply = net::BuildReply(token, info);
    ENetBuffer b{}; b.data = const_cast<uint8_t*>(reply.data()); b.dataLength = reply.size();
    enet_socket_send(host->socket, &host->receivedAddress, &b, 1);
    return 1;
}

Server::Server() { g_server = this; }
Server::~Server() {
    script_.reset();
    if (host_) enet_host_destroy(host_);
    if (log_) std::fclose(log_);
    g_server = nullptr;
}

double Server::Now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_epoch).count(); }

void Server::Log(const char* fmt, ...) {
    char text[2048];
    va_list a; va_start(a, fmt); std::vsnprintf(text, sizeof(text), fmt, a); va_end(a);
    const std::time_t t = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &t);
    char stamp[16]; std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::printf("[%s] %s\n", stamp, text); std::fflush(stdout);
    if (log_) { std::fprintf(log_, "[%s] %s\n", stamp, text); std::fflush(log_); }
}

bool Server::Start(const Config& cfg, const std::filesystem::path& root) {
    cfg_ = cfg; root_ = root;
    cfg_.maxPlayers = std::clamp(cfg_.maxPlayers, 1, net::kMaxPlayersLimit);
    std::filesystem::create_directories(root_ / "logs");
    _wfopen_s(&log_, (root_ / "logs" / "server.log").c_str(), L"a");
    if (enet_initialize() != 0) { Log("ENet initialisation failed"); return false; }
    ENetAddress address{}; address.host = ENET_HOST_ANY; address.port = cfg_.port;
    if (!cfg_.bind.empty() && enet_address_set_host_ip(&address, cfg_.bind.c_str()) != 0) { Log("Invalid bind address %s", cfg_.bind.c_str()); return false; }
    host_ = enet_host_create(&address, static_cast<size_t>(cfg_.maxPlayers) + 4, net::kChannels, 0, 0);
    if (!host_) { Log("Cannot listen on UDP port %u (already in use?)", cfg_.port); return false; }
    host_->intercept = &OnRawPacket;
    Log("DGMP server \"%s\" listening on UDP %u, %d slots%s", cfg_.name.c_str(), cfg_.port, cfg_.maxPlayers, cfg_.password.empty() ? "" : ", password protected");
    LoadLooks();
    script_ = std::make_unique<Script>(*this);
    if (!script_->Load(cfg_.mode)) Log("Mode \"%s\" failed to load; the server runs without a mode (fix the script, it reloads automatically)", cfg_.mode.c_str());
    return true;
}

void Server::PushConsole(const std::string& line) {
    std::lock_guard<std::mutex> g(consoleLock_); consoleQueue_.push_back(line);
}

void Server::Run() {
    double last = Now(), nextState = 0, nextReloadCheck = 0;
    while (!quit_) {
        ENetEvent e;
        // Up to ~30 Hz loop; service returns early when packets arrive.
        while (enet_host_service(host_, &e, 5) > 0) {
            switch (e.type) {
            case ENET_EVENT_TYPE_CONNECT: OnConnect(e.peer); break;
            case ENET_EVENT_TYPE_DISCONNECT: OnDisconnect(e.peer, e.data == 0 ? QuitTimeout : QuitLeft); break;  // clients disconnect with data=1
            case ENET_EVENT_TYPE_RECEIVE: OnPacket(e.peer, e.packet->data, e.packet->dataLength); enet_packet_destroy(e.packet); break;
            default: break;
            }
        }
        std::vector<std::string> lines;
        { std::lock_guard<std::mutex> g(consoleLock_); lines.swap(consoleQueue_); }
        for (auto& l : lines) HandleConsole(l);

        const double now = Now();
        const float dt = static_cast<float>(now - last);
        if (dt < 1.0f / 30) continue;
        last = now;
        TickBots(dt);
        TickMarkers();
        script_->Tick(dt);
        if (now >= nextState) { nextState = now + 0.05; BroadcastStates(); }
        if (now >= nextReloadCheck) {
            nextReloadCheck = now + 1.0;
            if (script_->ChangedOnDisk()) { Log("Mode scripts changed on disk, reloading"); ReloadMode(); }
        }
    }
    net::Writer bye; bye.Op(net::Msg::Kick); bye.Str("Server is shutting down");
    Send(nullptr, bye, true);
    enet_host_flush(host_);
    for (auto& [id, p] : players_) if (p->peer) enet_peer_disconnect_later(p->peer, QuitKicked);
    // Let the goodbye packets go out before the socket closes.
    ENetEvent e; const double until = Now() + 0.5;
    while (Now() < until && enet_host_service(host_, &e, 10) >= 0) if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
}

void Server::OnConnect(ENetPeer* peer) {
    peer->data = nullptr;
    enet_peer_timeout(peer, 0, 5000, 15000);
    Log("Connection from %s", Address(peer->address).c_str());
}

void Server::OnDisconnect(ENetPeer* peer, uint8_t reason) {
    auto p = static_cast<Player*>(peer->data);
    if (!p) return;
    peer->data = nullptr; p->peer = nullptr;
    RemovePlayer(p, reason);
}

void Server::RemovePlayer(Player* p, uint8_t reason) {
    const uint16_t id = p->id;
    if (p->ready) {
        static const char* why[] = {"left", "timed out", "kicked"};
        Log("%s (id %u) %s", p->nick.c_str(), id, why[std::min<uint8_t>(reason, 2)]);
        script_->PlayerQuit(*p, reason);
        net::Writer w; w.Op(net::Msg::PlayerQuit); w.U16(id); w.U8(reason);
        players_.erase(id);
        Send(nullptr, w, true);
    } else players_.erase(id);
}

uint16_t Server::FreeId() const {
    for (uint16_t id = 0; id < 0xFFFF; ++id) if (!players_.count(id)) return id;
    return 0xFFFF;
}

void Server::OnPacket(ENetPeer* peer, const uint8_t* data, size_t size) {
    if (size < 1) return;
    net::Reader r(data + 1, size - 1);
    const auto op = static_cast<net::Msg>(data[0]);
    auto p = static_cast<Player*>(peer->data);
    if (op == net::Msg::Hello) { if (!p) OnHello(peer, r); return; }
    if (!p || !p->ready) return;
    switch (op) {
    case net::Msg::State: { auto s = r.State(); if (r.ok() && std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z)) p->state = s; break; }
    case net::Msg::Chat: { auto text = r.Str(net::kChatMax); if (r.ok()) OnChat(*p, Trim(text)); break; }
    case net::Msg::Pose: RelayPose(*p, data + 1, size - 1); break;
    case net::Msg::Look: {
        // The client reports what it actually wears (its own default, or after an order).
        auto path = r.Str(net::kLookPathMax);
        if (!r.ok() || path == p->look || (!path.empty() && path[0] != '/')) break;
        p->look = path;
        net::Writer w; w.Op(net::Msg::PlayerLook); w.U16(p->id); w.Str(path);
        for (auto& [id, other] : players_) if (other.get() != p && other->ready && other->peer) SendTo(other->peer, w, true);
        break;
    }
    case net::Msg::GameEvent: {
        GameEvent e;
        e.type = r.U8(); e.cls = r.Str(128); e.detail = r.Str(net::kItemKeyMax); e.id = r.Str(128);
        e.x = r.F32(); e.y = r.F32(); e.z = r.F32();
        if (!r.ok() || e.type < 1 || e.type > 10 || !std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z)) break;
        // A client reports what its player does; flood guard: 20 events a second.
        const double now = Now();
        if (now - p->eventWindow >= 1.0) { p->eventWindow = now; p->eventCount = 0; }
        if (++p->eventCount > 20) break;
        if (e.type == uint8_t(net::GameEventType::Died)) {
            // The mode may take things away (playerDeath); the backpack is otherwise kept.
            p->inventoryAtDeath = p->inventory;
            // Everyone else leaves a body where they see the player (the player leaves its own).
            net::Writer w; w.Op(net::Msg::Corpse); w.U16(p->id);
            for (auto& [id, other] : players_) if (other.get() != p && other->ready && other->peer) SendTo(other->peer, w, true);
            script_->PlayerDeath(*p);
            Log("%s died", p->nick.c_str());
            break;
        }
        if (e.type == uint8_t(net::GameEventType::Respawned)) {
            // The game reloaded a checkpoint somewhere (the client keeps it covered): the mode picks the
            // place (playerRespawn returning true), otherwise the server spawn.
            Log("%s respawned", p->nick.c_str());
            if (!script_->PlayerRespawn(*p)) Teleport(p, cfg_.spawnX, cfg_.spawnY, cfg_.spawnZ, cfg_.spawnYaw);
            break;
        }
        script_->GameEvent(*p, e);
        break;
    }
    case net::Msg::Inventory: {
        std::map<std::string, int> inv;
        const auto count = r.U16();
        for (int i = 0; i < count && r.ok(); ++i) { auto key = r.Str(net::kItemKeyMax); const auto amount = r.I32(); if (r.ok()) inv[key] = amount; }
        if (!r.ok() || inv == p->inventory) break;
        p->inventory = std::move(inv);
        script_->InventoryChange(*p);
        break;
    }
    default: break;
    }
}

void Server::OnHello(ENetPeer* peer, net::Reader& r) {
    const uint16_t protocol = r.U16();
    const std::string nick = r.Str(96), password = r.Str(128);
    auto reject = [&](const std::string& why) {
        net::Writer w; w.Op(net::Msg::Reject); w.Str(why); SendTo(peer, w, true);
        enet_host_flush(host_);
        enet_peer_disconnect_later(peer, QuitKicked);
        Log("Rejected %s (%s): %s", Address(peer->address).c_str(), nick.c_str(), why.c_str());
    };
    if (!r.ok()) return reject("Malformed handshake");
    if (protocol != net::kProtocolVersion) return reject("Version mismatch: server protocol " + std::to_string(net::kProtocolVersion) + ", yours " + std::to_string(protocol) + ". Update DGMP.");
    if (!net::ValidNick(nick)) return reject("Invalid nickname");
    if (!cfg_.password.empty() && password != cfg_.password) return reject("Wrong password");
    if (FindByName(nick)) return reject("Nickname is already in use on this server");
    if (PlayerCount() >= cfg_.maxPlayers) return reject("Server is full");

    auto owned = std::make_unique<Player>();
    auto p = owned.get();
    p->id = FreeId(); p->nick = nick; p->peer = peer; p->joinedAt = Now(); p->ready = true;
    for (auto& a : cfg_.admins) if (_stricmp(a.c_str(), nick.c_str()) == 0) p->admin = true;
    p->state.x = cfg_.spawnX; p->state.y = cfg_.spawnY; p->state.z = cfg_.spawnZ; p->state.yaw = cfg_.spawnYaw;
    players_[p->id] = std::move(owned);
    peer->data = p;

    net::Writer w; w.Op(net::Msg::Welcome); w.U16(p->id); w.Str(cfg_.name); w.Str(script_->title()); w.U8(static_cast<uint8_t>(cfg_.maxPlayers));
    SendTo(peer, w, true);
    for (auto& [id, other] : players_) {
        if (other.get() == p || !other->ready) continue;
        net::Writer j; j.Op(net::Msg::PlayerJoin); j.U16(other->id); j.Str(other->nick); SendTo(peer, j, true);
        if (!other->look.empty()) { net::Writer l; l.Op(net::Msg::PlayerLook); l.U16(other->id); l.Str(other->look); SendTo(peer, l, true); }
        if (other->peer) { net::Writer n; n.Op(net::Msg::PlayerJoin); n.U16(p->id); n.Str(p->nick); SendTo(other->peer, n, true); }
    }
    { net::Writer t; t.Op(net::Msg::Settings); t.U8(static_cast<uint8_t>(cfg_.nametags)); SendTo(peer, t, true); }
    for (auto& [mid, m] : markers_) if (m.only == 0xFFFF) SendMarker(peer, m);
    for (auto& [iid, item] : items_) if (item.only == 0xFFFF) SendWorldItem(peer, item);
    if (timeHours_ >= 0) { net::Writer t; t.Op(net::Msg::SetTime); t.F32(timeHours_); t.U8(timePaused_); SendTo(peer, t, true); }
    if (weather_ != -2) { net::Writer t; t.Op(net::Msg::SetWeather); t.I32(weather_); SendTo(peer, t, true); }
    Teleport(p, cfg_.spawnX, cfg_.spawnY, cfg_.spawnZ, cfg_.spawnYaw);
    Log("%s (id %u) joined from %s", nick.c_str(), p->id, Address(peer->address).c_str());
    script_->PlayerJoin(*p);
}

void Server::OnChat(Player& p, const std::string& text) {
    if (text.empty()) return;
    if (text[0] == '/') {
        const auto space = text.find(' ');
        const std::string name = text.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        const std::string args = space == std::string::npos ? "" : Trim(text.substr(space + 1));
        Log("%s: %s", p.nick.c_str(), text.c_str());
        if (!script_->Command(p, name, args)) Message(&p, kErrorColor, "Unknown command: /" + name);
        return;
    }
    if (!script_->PlayerChat(p, text)) return;
    Log("<%s> %s", p.nick.c_str(), text.c_str());
    Message(nullptr, 0xFFFFFFFF, p.nick + ": " + text);
}

void Server::SendTo(ENetPeer* peer, const net::Writer& w, bool reliable) {
    if (!peer) return;
    auto packet = enet_packet_create(w.buf.data(), w.buf.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, reliable ? net::kChanReliable : net::kChanState, packet);
}

void Server::Send(Player* to, const net::Writer& w, bool reliable) {
    if (to) { SendTo(to->peer, w, reliable); return; }
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendTo(p->peer, w, reliable);
}

void Server::BroadcastStates() {
    for (auto& [id, target] : players_) {
        if (!target->peer || !target->ready) continue;
        net::Writer w; w.Op(net::Msg::States);
        const auto countAt = w.buf.size(); w.U8(0);
        uint8_t count = 0;
        for (auto& [oid, p] : players_) {
            if (p.get() == target.get() || !p->ready) continue;
            w.U16(p->id); w.State(p->state); ++count;
        }
        if (!count) continue;
        w.buf[countAt] = count;
        SendTo(target->peer, w, false);
    }
}

// Bone poses are heavy (~4.5 KB per frame), so they only go to players close enough to see them.
void Server::RelayPose(Player& from, const uint8_t* payload, size_t size) {
    constexpr size_t kHeader = 1 + 2 + 4 + 2 + 8 + 7 * 4;
    if (size < kHeader || size > kHeader + net::kMaxPoseBones * sizeof(net::QBone)) return;
    uint16_t count; std::memcpy(&count, payload + 7, 2);
    if (size != kHeader + count * sizeof(net::QBone)) return;
    const double now = Now();
    if (now - from.poseWindow > 1.0) { from.poseWindow = now; from.posesInWindow = 0; }
    if (++from.posesInWindow > 40) return;  // body + bike at up to 20 Hz each
    net::Writer w; w.Op(net::Msg::PlayerPose); w.U16(from.id); w.Raw(payload, size);
    constexpr float range2 = net::kPoseRelayRange * net::kPoseRelayRange;
    for (auto& [id, p] : players_) {
        if (p.get() == &from || !p->ready || !p->peer) continue;
        const float dx = p->state.x - from.state.x, dy = p->state.y - from.state.y, dz = p->state.z - from.state.z;
        if (dx * dx + dy * dy + dz * dz > range2) continue;
        enet_peer_send(p->peer, net::kChanPose, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
    }
}

void Server::SendMarker(ENetPeer* peer, const Marker& m) {
    net::Writer w; w.Op(net::Msg::Marker); w.U16(m.id); w.U8(1);
    w.F32(m.x); w.F32(m.y); w.F32(m.z); w.U8(m.category); w.U8(m.style); w.U8(m.route); w.Str(m.icon); w.Str(m.title); w.Str(m.description);
    SendTo(peer, w, true);
}
uint16_t Server::AddMarker(Marker m) {
    if (!m.id) { while (markers_.count(nextMarker_) || !nextMarker_) ++nextMarker_; m.id = nextMarker_++; }
    if (m.only != 0xFFFF) if (auto p = Find(m.only)) m.onlyJoinedAt = p->joinedAt;
    markers_[m.id] = m;
    for (auto& [id, p] : players_) if (p->ready && p->peer && (m.only == 0xFFFF || m.only == id)) SendMarker(p->peer, m);
    return m.id;
}
bool Server::RemoveMarker(uint16_t id) {
    if (!markers_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Marker); w.U16(id); w.U8(2); Send(nullptr, w, true);
    for (auto& [pid, p] : players_) std::erase(p->insideMarkers, id);
    return true;
}
void Server::SendWorldItem(ENetPeer* peer, const WorldItem& item) {
    net::Writer w; w.Op(net::Msg::WorldItem); w.U16(item.id); w.U8(1); w.Str(item.key); w.I32(item.count); w.F32(item.x); w.F32(item.y); w.F32(item.z);
    SendTo(peer, w, true);
}
void Server::RestoreInventory(Player* p, const std::map<std::string, int>& inv, const std::vector<std::string>& drop) {
    Item(p, net::ItemAction::Clear, "", 0);
    for (const auto& [key, count] : inv) {
        bool dropped = false;
        for (const auto& prefix : drop) dropped |= key.rfind(prefix, 0) == 0;
        if (dropped || count <= 0) continue;
        if (key.rfind("kAmmoID_", 0) == 0) Item(p, net::ItemAction::SetAmmo, key, count);
        else if (key.rfind("kWeaponID_", 0) == 0) Item(p, net::ItemAction::GiveReplace, key, count);  // no drops from full slots
        else Item(p, net::ItemAction::Give, key, count);
    }
}
uint16_t Server::AddWorldItem(WorldItem item) {
    while (items_.count(nextItem_) || !nextItem_) ++nextItem_;
    item.id = nextItem_++;
    if (item.only != 0xFFFF) if (auto p = Find(item.only)) item.onlyJoinedAt = p->joinedAt;
    items_[item.id] = item;
    for (auto& [id, p] : players_) if (p->ready && p->peer && (item.only == 0xFFFF || item.only == id)) SendWorldItem(p->peer, item);
    return item.id;
}
bool Server::RemoveWorldItem(uint16_t id) {
    if (!items_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::WorldItem); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
const Marker* Server::FindMarker(uint16_t id) const { auto it = markers_.find(id); return it == markers_.end() ? nullptr : &it->second; }

// Arrival is decided here from the positions players send: no client trust needed beyond that.
void Server::TickMarkers() {
    // Callbacks run after the scan: they add and remove markers (a quest moving to its next step).
    struct Event { uint16_t player, marker; bool inside; };
    std::vector<Event> events;
    std::vector<uint16_t> stale;
    for (auto& [mid, m] : markers_) {
        if (m.only != 0xFFFF) { auto p = Find(m.only); if (!p || p->joinedAt != m.onlyJoinedAt) { stale.push_back(mid); continue; } }
        if (!m.track) continue;
        for (auto p : Players()) {
            if (m.only != 0xFFFF && p->id != m.only) continue;
            const float dx = p->state.x - m.x, dy = p->state.y - m.y, dz = p->state.z - m.z;
            const bool inside = dx * dx + dy * dy + dz * dz <= m.radius * m.radius;
            auto& list = p->insideMarkers;
            const bool was = std::find(list.begin(), list.end(), mid) != list.end();
            if (inside == was) continue;
            if (inside) list.push_back(mid); else std::erase(list, mid);
            events.push_back({p->id, mid, inside});
        }
    }
    for (auto id : stale) RemoveMarker(id);  // personal markers die with their player
    std::vector<uint16_t> staleItems;
    for (auto& [iid, item] : items_) if (item.only != 0xFFFF) { auto p = Find(item.only); if (!p || p->joinedAt != item.onlyJoinedAt) staleItems.push_back(iid); }
    for (auto id : staleItems) RemoveWorldItem(id);
    for (const auto& e : events) {
        auto p = Find(e.player);  // an earlier callback may have kicked the player or removed the marker
        if (p && (!e.inside || FindMarker(e.marker))) script_->MarkerEvent(*p, e.marker, e.inside);
    }
}

void Server::TickBots(float dt) {
    for (auto& [id, p] : players_) {
        if (!p->bot) continue;
        // Walk a 6 m circle around the spawn so clients have something moving to render.
        p->botPhase += dt * 0.25f;
        const float r = 600.f, a = p->botPhase + id;
        p->state.x = cfg_.spawnX + std::cos(a) * r; p->state.y = cfg_.spawnY + std::sin(a) * r; p->state.z = cfg_.spawnZ;
        p->state.vx = -std::sin(a) * r * 0.25f; p->state.vy = std::cos(a) * r * 0.25f; p->state.vz = 0;
        p->state.yaw = (a + 3.14159265f / 2) * 57.2957795f;
        p->state.anim = net::AnimWalk; p->state.flags = 0; p->state.time = Now();
    }
}

// ---------------- API ----------------
Player* Server::Find(uint16_t id) { auto it = players_.find(id); return it != players_.end() && it->second->ready ? it->second.get() : nullptr; }
Player* Server::FindByName(const std::string& nick) {
    for (auto& [id, p] : players_) if (p->ready && _stricmp(p->nick.c_str(), nick.c_str()) == 0) return p.get();
    return nullptr;
}
std::vector<Player*> Server::Players() {
    std::vector<Player*> out;
    for (auto& [id, p] : players_) if (p->ready) out.push_back(p.get());
    return out;
}
net::ServerInfo Server::Info() const {
    net::ServerInfo info;
    info.players = static_cast<uint16_t>(PlayerCount());
    info.maxPlayers = static_cast<uint16_t>(cfg_.maxPlayers);
    info.password = cfg_.password.empty() ? 0 : 1;
    info.name = cfg_.name;
    info.mode = script_ ? script_->title() : cfg_.mode;
    info.version = "0.6.0";
    return info;
}
int Server::PlayerCount() const {
    int n = 0; for (auto& [id, p] : players_) n += p->ready && !p->bot; return n;
}
void Server::Message(Player* to, uint32_t rgba, const std::string& text) {
    net::Writer w; w.Op(net::Msg::Message); w.U32(rgba); w.Str(text.substr(0, 1024)); Send(to, w, true);
}
void Server::SetTime(float hours, bool paused) {
    timeHours_ = std::fmod(std::fmod(hours, 24.f) + 24.f, 24.f); timePaused_ = paused;
    net::Writer w; w.Op(net::Msg::SetTime); w.F32(timeHours_); w.U8(paused); Send(nullptr, w, true);
}
void Server::SetWeather(int id) { weather_ = id; net::Writer w; w.Op(net::Msg::SetWeather); w.I32(id); Send(nullptr, w, true); }
void Server::Teleport(Player* p, float x, float y, float z, float yaw) {
    if (!p) return;
    p->state.x = x; p->state.y = y; p->state.z = z; p->state.yaw = yaw;
    net::Writer w; w.Op(net::Msg::Teleport); w.F32(x); w.F32(y); w.F32(z); w.F32(yaw); Send(p, w, true);
}
void Server::Hint(Player* to, const std::string& title, const std::string& text, const std::string& image, bool pause, float seconds) {
    net::Writer w; w.Op(net::Msg::Hint); w.Str(title); w.Str(text); w.Str(image); w.U8(pause); w.F32(seconds); Send(to, w, true);
}
void Server::Kick(Player* p, const std::string& reason) {
    if (!p) return;
    if (p->bot) { RemovePlayer(p, QuitKicked); return; }
    net::Writer w; w.Op(net::Msg::Kick); w.Str(reason); Send(p, w, true);
    enet_host_flush(host_);
    auto peer = p->peer; peer->data = nullptr; p->peer = nullptr;
    enet_peer_disconnect_later(peer, QuitKicked);
    RemovePlayer(p, QuitKicked);
}
void Server::Item(Player* p, net::ItemAction action, const std::string& key, int amount) {
    if (!p || p->bot) return;
    net::Writer w; w.Op(net::Msg::Item); w.U8(static_cast<uint8_t>(action)); w.Str(key.substr(0, net::kItemKeyMax)); w.I32(amount); Send(p, w, true);
}
void Server::Bike(Player* p, net::BikeAction action, float x, float y, float z, float yaw) {
    if (!p || p->bot) return;
    net::Writer w; w.Op(net::Msg::Bike); w.U8(static_cast<uint8_t>(action)); w.F32(x); w.F32(y); w.F32(z); w.F32(yaw); Send(p, w, true);
}
void Server::LoadLooks() {
    std::ifstream in(root_ / "data" / "looks.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto t1 = line.find('\t'); if (t1 == std::string::npos) continue;
        const auto t2 = line.find('\t', t1 + 1);
        looks_.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1)});
    }
    Log("Character looks catalog: %zu entries", looks_.size());
}
std::string Server::ResolveLook(const std::string& nameOrPath) const {
    if (!nameOrPath.empty() && nameOrPath[0] == '/') return nameOrPath;
    for (auto& [name, path] : looks_) if (_stricmp(name.c_str(), nameOrPath.c_str()) == 0) return path;
    return "";
}
std::vector<std::string> Server::LookNames(const std::string& filter) const {
    std::vector<std::string> out;
    std::string f = filter; for (auto& c : f) c = char(tolower(static_cast<unsigned char>(c)));
    for (auto& [name, path] : looks_) {
        std::string hay = path; for (auto& c : hay) c = char(tolower(static_cast<unsigned char>(c)));
        if (f.empty() || hay.find(f) != std::string::npos) out.push_back(name);
    }
    return out;
}
void Server::SetLook(Player* p, const std::string& path) {
    if (!p) return;
    p->look = path;
    net::Writer w; w.Op(net::Msg::PlayerLook); w.U16(p->id); w.Str(path);
    Send(nullptr, w, true);  // everyone, including the player (who then wears it)
}
void Server::Objective(Player* p, uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp) {
    net::Writer w; w.Op(net::Msg::Objective); w.U8(state); w.U8(category); w.U8(style); w.Str(title); w.Str(text); w.I32(xp); Send(p, w, true);
}
void Server::ObjectiveMeter(Player* p, bool show, const std::string& title, float percent) {
    net::Writer w; w.Op(net::Msg::ObjectiveMeter); w.U8(show); w.Str(title); w.F32(percent); Send(p, w, true);
}
void Server::SetNametags(net::NametagMode mode) {
    cfg_.nametags = mode;
    net::Writer w; w.Op(net::Msg::Settings); w.U8(static_cast<uint8_t>(mode)); Send(nullptr, w, true);
}
Player* Server::AddBot(const std::string& nick) {
    if (FindByName(nick)) return nullptr;
    auto owned = std::make_unique<Player>();
    auto p = owned.get();
    p->id = FreeId(); p->nick = nick; p->bot = true; p->ready = true; p->joinedAt = Now();
    p->state.x = cfg_.spawnX; p->state.y = cfg_.spawnY; p->state.z = cfg_.spawnZ;
    players_[p->id] = std::move(owned);
    net::Writer w; w.Op(net::Msg::PlayerJoin); w.U16(p->id); w.Str(p->nick); Send(nullptr, w, true);
    Log("Bot %s (id %u) added", nick.c_str(), p->id);
    script_->PlayerJoin(*p);
    return p;
}
void Server::RemoveBots() {
    std::vector<Player*> bots;
    for (auto& [id, p] : players_) if (p->bot) bots.push_back(p.get());
    for (auto b : bots) RemovePlayer(b, QuitLeft);
}
bool Server::ReloadMode() {
    const auto mode = script_->mode().empty() ? cfg_.mode : script_->mode();
    script_->Unload();
    // What the old script put in the world goes with it (its callbacks are gone).
    std::vector<uint16_t> ids;
    for (auto& [id, m] : markers_) ids.push_back(id);
    for (auto id : ids) RemoveMarker(id);
    ids.clear();
    for (auto& [id, item] : items_) ids.push_back(id);
    for (auto id : ids) RemoveWorldItem(id);
    for (auto& [id, p] : players_) if (p->ready && p->peer) { Objective(p.get(), 4, 0, 0, "", ""); ObjectiveMeter(p.get(), false, "", 0); }
    if (!script_->Load(mode)) return false;
    return true;
}

void Server::HandleConsole(const std::string& raw) {
    const std::string line = Trim(raw);
    if (line.empty()) return;
    std::istringstream in(line); std::string cmd; in >> cmd;
    std::string rest; std::getline(in, rest); rest = Trim(rest);
    if (cmd == "help") {
        Log("Commands: status | say <text> | kick <id|nick> [reason] | bot [count] | bots off | reload | mode <id> | time <hours> | weather <id|auto> | quit");
    } else if (cmd == "status" || cmd == "players") {
        Log("%s | mode %s | %d/%d players", cfg_.name.c_str(), script_->title().c_str(), PlayerCount(), cfg_.maxPlayers);
        for (auto p : Players()) {
            const int ping = p->peer ? static_cast<int>(p->peer->roundTripTime) : 0;
            Log("  %3u  %-24s %s  ping %d  pos %.0f %.0f %.0f", p->id, p->nick.c_str(), p->bot ? "bot " : "    ", ping, p->state.x, p->state.y, p->state.z);
        }
    } else if (cmd == "say") {
        Message(nullptr, kSystemColor, "[Server] " + rest); Log("[Server] %s", rest.c_str());
    } else if (cmd == "kick") {
        std::istringstream a(rest); std::string who; a >> who; std::string reason; std::getline(a, reason); reason = Trim(reason);
        Player* p = FindByName(who);
        if (!p && !who.empty() && std::all_of(who.begin(), who.end(), ::isdigit)) p = Find(static_cast<uint16_t>(std::stoi(who)));
        if (!p) Log("No such player: %s", who.c_str()); else Kick(p, reason.empty() ? "Kicked by server" : reason);
    } else if (cmd == "bot") {
        if (rest == "off") { RemoveBots(); return; }
        const int n = rest.empty() ? 1 : std::clamp(std::atoi(rest.c_str()), 1, 16);
        for (int i = 0, made = 0; made < n && i < 100; ++i) if (AddBot("Bot_" + std::to_string(i + 1))) ++made;
    } else if (cmd == "bots" && rest == "off") {
        RemoveBots();
    } else if (cmd == "reload") {
        Log(ReloadMode() ? "Mode reloaded" : "Mode reload failed");
    } else if (cmd == "mode") {
        script_->Unload();
        if (script_->Load(rest)) { cfg_.mode = rest; Log("Mode switched to %s", rest.c_str()); }
        else { Log("Cannot load mode %s, restoring %s", rest.c_str(), cfg_.mode.c_str()); script_->Load(cfg_.mode); }
    } else if (cmd == "time") {
        SetTime(static_cast<float>(std::atof(rest.c_str())), false); Log("Time set to %.2f", timeHours_);
    } else if (cmd == "weather") {
        SetWeather(rest == "auto" ? -1 : std::atoi(rest.c_str())); Log("Weather set to %d", weather_);
    } else if (cmd == "quit" || cmd == "exit" || cmd == "stop") {
        Log("Shutting down"); quit_ = true;
    } else if (!script_->Console(line)) {
        Log("Unknown command \"%s\". Type help.", cmd.c_str());
    }
}

}  // namespace srv
