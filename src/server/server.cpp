#include "server.h"
#include "../shared/json.h"
#include "platform.h"
#include "script.h"
#include <enet/enet.h>
#include <algorithm>
#include <cctype>
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
    const std::time_t t = std::time(nullptr); const std::tm tm = LocalTime(t);
    char stamp[16]; std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::printf("[%s] %s\n", stamp, text); std::fflush(stdout);
    if (log_) { std::fprintf(log_, "[%s] %s\n", stamp, text); std::fflush(log_); }
}

bool Server::Start(const Config& cfg, const std::filesystem::path& root) {
    cfg_ = cfg; root_ = root;
    cfg_.maxPlayers = std::clamp(cfg_.maxPlayers, 1, net::kMaxPlayersLimit);
    std::filesystem::create_directories(root_ / "logs");
    log_ = OpenAppend(root_ / "logs" / "server.log");
    if (enet_initialize() != 0) { Log("ENet initialisation failed"); return false; }
    ENetAddress address{}; address.host = ENET_HOST_ANY; address.port = cfg_.port;
    if (!cfg_.bind.empty() && enet_address_set_host_ip(&address, cfg_.bind.c_str()) != 0) { Log("Invalid bind address %s", cfg_.bind.c_str()); return false; }
    host_ = enet_host_create(&address, static_cast<size_t>(cfg_.maxPlayers) + 4, net::kChannels, 0, 0);
    if (!host_) { Log("Cannot listen on UDP port %u (already in use?)", cfg_.port); return false; }
    host_->intercept = &OnRawPacket;
    Log("DGMP server \"%s\" listening on UDP %u, %d slots%s", cfg_.name.c_str(), cfg_.port, cfg_.maxPlayers, cfg_.password.empty() ? "" : ", password protected");
    LoadLooks();
    LoadAnims();
    LoadSkins();
    script_ = std::make_unique<Script>(*this);
    LoadEnvironment(cfg_.mode);
    if (script_->Load(cfg_.mode)) LoadWorldEdits();
    else Log("Mode \"%s\" failed to load; the server runs without a mode (fix the script, it reloads automatically)", cfg_.mode.c_str());
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
        TickVehicles();
        if (now >= nextZoneTick_) { nextZoneTick_ = now + 0.1; TickZones(); }
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
        LeaveVehicle(p, nullptr);  // its vehicle stays where it rode it
        if (p->carrying) LoseCarry(*p, p->state.x, p->state.y, p->state.z - 90.f, p->state.yaw, "quit");  // its box falls where it stood
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
        if (!r.ok() || path == p->look || (!path.empty() && path[0] != '/' && path.rfind("skin:", 0) != 0)) break;
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
            if (p->carrying) LoseCarry(*p, p->state.x, p->state.y, p->state.z - 90.f, p->state.yaw, "death");
            // Everyone else leaves a body where they see the player (the player leaves its own).
            net::Writer w; w.Op(net::Msg::Corpse); w.U16(p->id);
            for (auto& [id, other] : players_) if (other.get() != p && other->ready && other->peer) SendTo(other->peer, w, true);
            // Killed by a player: the last one who hurt it in the 15 seconds before.
            Player* killer = p->lastHitAt > 0 && now - p->lastHitAt < 15 ? Find(p->lastAttacker) : nullptr;
            p->lastAttacker = 0xFFFF; p->lastHitAt = 0;
            script_->PlayerDeath(*p, killer);
            if (killer) Log("%s was killed by %s", p->nick.c_str(), killer->nick.c_str()); else Log("%s died", p->nick.c_str());
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
    case net::Msg::DialogResponse: {
        const uint16_t id = r.U16(); bool ok = r.U8() != 0; const int item = r.Get<int16_t>(); const auto text = r.Str(256);
        if (!r.ok() || !id || id != p->dialogId) break;  // not the dialog it has open
        const auto style = p->dialogStyle; const auto items = std::move(p->dialogItems);
        p->dialogId = 0; p->dialogItems.clear();
        int index = -1; std::string answer;
        if (style == net::DialogStyle::List) {
            if (item >= 0 && item < int(items.size())) { index = item; answer = items[size_t(item)]; }
            else if (ok) break;  // a list answered with nothing chosen
        } else if (style == net::DialogStyle::Input || style == net::DialogStyle::Password) answer = text;
        script_->DialogResponse(*p, id, ok, index, answer);
        break;
    }
    case net::Msg::Use: {
        const uint16_t id = r.U16();
        auto it = interactions_.find(id);
        if (!r.ok() || it == interactions_.end()) break;
        const Interaction& in = it->second;
        if (in.world >= 0 && in.world != p->world) break;
        const float dx = p->state.x - in.x, dy = p->state.y - in.y, dz = p->state.z - in.z;
        if (dx * dx + dy * dy > (in.radius + 300.f) * (in.radius + 300.f) || std::fabs(dz) > 500.f) break;  // not there
        script_->Use(*p, id);
        break;
    }
    case net::Msg::VehicleEnter: {
        const uint16_t id = r.U16();
        Vehicle* v = FindVehicle(id);
        if (!r.ok() || !v) break;
        // E in a standing helicopter: out of it.
        if (p->passengerOf == id && v->Helicopter()) { if (v->route.empty()) EjectFromVehicle(p); break; }
        if (p->vehicle || p->passengerOf) break;
        const float dx = p->state.x - v->state.x, dy = p->state.y - v->state.y;
        const float reach = v->Helicopter() ? 1500.f : 800.f;
        if (dx * dx + dy * dy > reach * reach || v->world != p->world) break;  // not at it
        if (v->Helicopter()) {  // into a seat, while it stands
            if (!v->route.empty()) break;
            const bool full = v->riders.size() >= kHelicopterSeats && std::count(v->riders.begin(), v->riders.end(), uint16_t(0xFFFF)) == 0;
            if (full) Message(p, 0xE07070FF, p->lang == "ru" ? "Мест нет" : "No seats");
            else PutInVehicle(p, id, true);  // (the mode may refuse it: vehicleEnter)
            break;
        }
        // Someone drives it (a player or its NPC): behind them, if that seat is free; the mode may refuse it.
        if (v->driver != 0xFFFF || v->npc) {
            if (v->passenger != 0xFFFF) { Message(p, 0xE07070FF, p->lang == "ru" ? "Занято" : "Taken"); break; }
            if (script_->VehicleEnter(*p, id, true, true) == 1) break;
            PutInVehicle(p, id, true);
            break;
        }
        // Locked: only its owner; the mode decides in the end (vehicleEnter: false refuses, true allows).
        bool allowed = !v->locked || (!v->owner.empty() && v->owner == NickKey(p->nick));
        const int verdict = script_->VehicleEnter(*p, id, allowed, false);
        if (verdict == 1) allowed = false; else if (verdict == 2) allowed = true;
        if (!allowed) {
            if (verdict == 0) Message(p, 0xE07070FF, p->lang == "ru" ? "Заперто" : "Locked");
            break;
        }
        PutInVehicle(p, id);
        break;
    }
    case net::Msg::VehicleExit:
    case net::Msg::VehicleState: {
        const uint16_t id = r.U16(); const net::VehicleState s = r.Vehicle();
        if (r.ok() && op == net::Msg::VehicleExit && p->passengerOf == id) { LeaveVehicle(p, nullptr); break; }  // got off from behind
        if (!r.ok() || !p->vehicle || p->vehicle != id || !std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z)) break;
        if (Vehicle* v = FindVehicle(id)) {
            v->state = s; v->state.fuel = std::clamp(s.fuel, 0.f, 1.f); v->state.health = std::clamp(s.health, 0.f, 1.f);
            if (op == net::Msg::VehicleExit) LeaveVehicle(p, &v->state);
        }
        break;
    }
    case net::Msg::ImageRequest: {
        const auto name = r.Str(128);
        if (!r.ok()) break;
        for (auto& img : images_) if (img.name == name) {
            constexpr size_t kChunk = 60000;
            const size_t parts = std::max<size_t>(1, (img.data.size() + kChunk - 1) / kChunk);
            for (size_t i = 0; i < parts; ++i) {
                net::Writer w; w.Op(net::Msg::ImageData); w.Str(img.name); w.U16(uint16_t(i)); w.U16(uint16_t(parts)); w.Str(img.data.substr(i * kChunk, kChunk));
                Send(p, w, true);
            }
            break;
        }
        break;
    }
    case net::Msg::CarryAction: OnCarryAction(*p, r); break;
    case net::Msg::Key: {
        const uint8_t key = r.U8();
        if (r.ok() && watchedKeys_.count(key)) script_->KeyPress(*p, key);
        break;
    }
    case net::Msg::CustomizeDone: {
        const bool ok = r.U8() != 0; const auto skin = r.Str(net::kLookPathMax);
        if (!r.ok()) break;
        // A picker: one of the skins it showed; the creator: a skin it made.
        const auto offered = std::move(p->offered); p->offered.clear();
        const bool valid = ok && skin.size() < net::kLookPathMax &&
            (offered.empty() ? skin.rfind("skin:", 0) == 0 : std::find(offered.begin(), offered.end(), skin) != offered.end());
        const bool uniform = !offered.empty() && p->offeredUniform;
        if (valid) { if (uniform) SetUniform(p, skin); else SetSkin(p, skin); }
        std::string id;
        if (valid) for (const char* kind : {uniform ? "uniforms" : "skins", uniform ? "skins" : "uniforms"}) {
            for (auto& c : SkinList(kind)) if (c.look == skin) { id = c.id; break; }
            if (!id.empty()) break;
        }
        if (p->inWardrobe) {  // back where it was before choosing (the script may send it elsewhere)
            p->inWardrobe = false;
            if (p->wardrobeReturn) Teleport(p, p->backX, p->backY, p->backZ, p->backYaw);
        }
        script_->CustomizeDone(*p, valid ? skin : std::string(), id);
        break;
    }
    case net::Msg::Gear: {
        // Relayed as it is: only the receiving clients read it.
        if (size - 1 > net::kGearMax) break;
        p->gear.assign(reinterpret_cast<const char*>(data + 1), size - 1);
        net::Writer w; w.Op(net::Msg::PlayerGear); w.U16(p->id); w.Raw(data + 1, size - 1);
        for (auto& [id, other] : players_) if (other.get() != p && other->ready && other->peer) SendTo(other->peer, w, true);
        break;
    }
    case net::Msg::Hit: {
        // The shooter's client saw its bullet hit another player's body; the victim's client applies it.
        const auto victimId = r.U16(); float damage = r.F32(); const auto zone = r.U8(); const auto weapon = r.Str(net::kItemKeyMax);
        Player* victim = Find(victimId);
        if (!r.ok() || !victim || victim == p || zone > 3 || !std::isfinite(damage) || damage <= 0 || damage > 1000) break;
        if (Apart(*victim, *p)) break;  // another virtual world or choosing a skin: not there
        const double now = Now();
        if (now - p->hitWindow >= 1.0) { p->hitWindow = now; p->hitCount = 0; }
        if (++p->hitCount > 30) break;
        const float dx = victim->state.x - p->state.x, dy = victim->state.y - p->state.y, dz = victim->state.z - p->state.z;
        if (dx * dx + dy * dy + dz * dz > 400.f * 100 * 400.f * 100) break;  // further than 400 m: not a real hit
        static const char* zones[] = {"body", "head", "arm", "leg"};
        static const float scale[] = {1.f, 2.f, 0.75f, 0.75f};  // before the mode's playerDamage handlers
        // PvP switch, PvE/PvP zones, the players' overrides; then the mode's setPvpRule has the last word.
        if (!script_->PvpRule(*victim, *p, CanHurt(*victim, *p), zones[zone], weapon)) break;
        damage = script_->PlayerDamage(*victim, *p, damage * scale[zone], zones[zone], weapon);
        if (damage <= 0) break;
        victim->lastAttacker = p->id; victim->lastHitAt = now;
        if (victim->bot) { HurtBot(victim, damage, p); break; }
        if (!victim->peer) break;
        net::Writer w; w.Op(net::Msg::Damage); w.U16(p->id); w.F32(damage); w.U8(zone);
        SendTo(victim->peer, w, true);
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
    std::string lang = r.left() >= 2 ? r.Str(16) : std::string("en");
    if (lang.empty()) lang = "en";
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
    p->id = FreeId(); p->nick = nick; p->peer = peer; p->joinedAt = Now(); p->ready = true; p->lang = lang;
    for (auto& a : cfg_.admins) if (StrICmp(a.c_str(), nick.c_str()) == 0) p->admin = true;
    p->state.x = cfg_.spawnX; p->state.y = cfg_.spawnY; p->state.z = cfg_.spawnZ; p->state.yaw = cfg_.spawnYaw;
    players_[p->id] = std::move(owned);
    peer->data = p;

    net::Writer w; w.Op(net::Msg::Welcome); w.U16(p->id); w.Str(cfg_.name); w.Str(script_->title()); w.U8(static_cast<uint8_t>(cfg_.maxPlayers));
    SendTo(peer, w, true);
    for (auto& [id, other] : players_) {
        if (other.get() == p || !other->ready) continue;
        net::Writer j; j.Op(net::Msg::PlayerJoin); j.U16(other->id); j.Str(other->nick); SendTo(peer, j, true);
        if (!other->look.empty()) { net::Writer l; l.Op(net::Msg::PlayerLook); l.U16(other->id); l.Str(other->look); SendTo(peer, l, true); }
        if (!other->gear.empty()) { net::Writer g; g.Op(net::Msg::PlayerGear); g.U16(other->id); g.Raw(other->gear.data(), other->gear.size()); SendTo(peer, g, true); }
        if (other->peer) { net::Writer n; n.Op(net::Msg::PlayerJoin); n.U16(p->id); n.Str(p->nick); SendTo(other->peer, n, true); }
    }
    { net::Writer t; t.Op(net::Msg::Settings); t.U8(static_cast<uint8_t>(cfg_.nametags)); t.U8(cfg_.collisions); SendTo(peer, t, true); }
    for (auto& [mid, m] : markers_) if (m.only == 0xFFFF) SendMarker(peer, m);
    for (auto& [iid, item] : items_) if (item.only == 0xFFFF) SendWorldItem(peer, item);
    for (auto& [iid, in] : interactions_) SendInteraction(peer, in);
    for (auto& [aid, a] : actors_) SendActor(peer, a);
    for (auto& [cid, c] : carries_) SendCarry(p, c);
    for (auto& [vid, v] : vehicles_) { SendVehicle(p, v); if (!v.route.empty()) SendRoute(p, v); }
    SendImageList(p);
    for (auto& [iid, ic] : icons_) SendIcon(peer, ic);
    if (!watchedKeys_.empty()) SendWatchedKeys(p);
    if (timeHours_ >= 0) { net::Writer t; t.Op(net::Msg::SetTime); t.F32(timeHours_); t.U8(timePaused_); SendTo(peer, t, true); }
    if (weather_ != -2) { net::Writer t; t.Op(net::Msg::SetWeather); t.I32(weather_); SendTo(peer, t, true); }
    SendWorldEdits(peer);
    Teleport(p, cfg_.spawnX, cfg_.spawnY, cfg_.spawnZ, cfg_.spawnYaw);
    // The shared world save comes with the story's weapons: everyone starts with only the knife, rock
    // and taser, and the mode hands out the rest (playerJoin runs after this).
    if (!cfg_.saveWeapons) Item(p, net::ItemAction::TakeWeapons, "", 0);
    // Nor the bike of the save: a mode adds the vehicles it wants (createVehicle, or giveBike).
    Bike(p, net::BikeAction::Take);
    // The skin it made before, on this server.
    std::string key = nick; for (auto& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (auto s = skins_.find(key); s != skins_.end()) { p->skin = s->second; WearEffective(p); }
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
            net::PlayerState s = p->state;
            if (Apart(*p, *target)) s.flags |= net::StateHidden | net::StateOtherWorld;
            w.U16(p->id); w.State(s); ++count;
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
        if (p.get() == &from || !p->ready || !p->peer || Apart(*p, from)) continue;
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
// The mode's edits of the game world (removed/moved objects, added props): every client applies them.
void Server::LoadEnvironment(const std::string& mode) {
    LoadModeSkins(mode);
    LoadImages(mode);
    LoadWorldActors(mode);
    LoadCarries(mode);
    LoadZones(mode);
    std::ifstream in(root_ / "modes" / mode / "world.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    const json::Value& env = v["environment"];
    if (!env.isObject()) return;
    if (env["time"].isNumber()) SetTime(float(env["time"].asNumber()), env["paused"].asBool(false));
    if (env["weather"].isNumber()) SetWeather(int(env["weather"].asNumber()));
    else if (env["weather"].asString() == "auto") SetWeather(-1);
    Log("Mode environment: time %.2f%s, weather %s", timeHours_, timePaused_ ? " (stopped)" : "", env["weather"].isNumber() ? std::to_string(int(env["weather"].asNumber())).c_str() : "auto");
}
void Server::LoadWorldEdits() {
    worldEdits_.clear();
    wardrobe_ = {};
    const auto file = root_ / "modes" / script_->mode() / "world.json";
    std::ifstream in(file, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    if (ss.str().empty()) return;
    json::Value v;
    if (!json::Value::Parse(ss.str(), v) || !v.isObject()) { Log("modes/%s/world.json is not valid JSON; ignored", script_->mode().c_str()); return; }
    worldEdits_ = ss.str();
    if (const json::Value& w = v["logic"]["wardrobe"]; w.isObject() || (w.isArray() && w.size() > 0)) {
        const json::Value& at = w.isArray() ? w[size_t(0)] : w;
        wardrobe_ = {true, float(at["x"].asNumber()), float(at["y"].asNumber()), float(at["z"].asNumber()), float(at["yaw"].asNumber())};
        Log("Skin choosing place: %.0f %.0f %.0f", wardrobe_.x, wardrobe_.y, wardrobe_.z);
    }
    Log("World edits: %zu removed, %zu moved, %zu added", v["remove"].size(), v["move"].size(), v["add"].size());
}
void Server::SendWorldEdits(ENetPeer* peer) {
    constexpr size_t kChunk = 60000;
    const size_t parts = std::max<size_t>(1, (worldEdits_.size() + kChunk - 1) / kChunk);
    for (size_t i = 0; i < parts; ++i) {
        net::Writer w; w.Op(net::Msg::WorldEdits); w.U16(uint16_t(i)); w.U16(uint16_t(parts));
        w.Str(worldEdits_.empty() ? std::string() : worldEdits_.substr(i * kChunk, kChunk));
        SendTo(peer, w, true);
    }
}
void Server::SendWorldItem(ENetPeer* peer, const WorldItem& item) {
    net::Writer w; w.Op(net::Msg::WorldItem); w.U16(item.id); w.U8(1); w.Str(item.key); w.I32(item.count); w.F32(item.x); w.F32(item.y); w.F32(item.z); w.F32(item.yaw);
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

// ---------------------------------------------------------------- PvP and zones
bool Zone::Contains(float px, float py, float pz) const {
    if (std::fabs(pz - z) > height) return false;
    const float dx = px - x, dy = py - y;
    if (!box) return dx * dx + dy * dy <= radius * radius;
    const float a = yaw * 0.017453293f, c = std::cos(a), s = std::sin(a);
    return std::fabs(dx * c + dy * s) <= halfX && std::fabs(-dx * s + dy * c) <= halfY;
}
json::Value Server::ZoneJson(const Zone& z) {
    json::Value v = json::Value::Object();
    v.set("name", z.name); v.set("type", std::string(z.pvp ? "pvp" : "pve")); v.set("shape", std::string(z.box ? "box" : "circle"));
    v.set("x", double(z.x)); v.set("y", double(z.y)); v.set("z", double(z.z)); v.set("yaw", double(z.yaw));
    if (z.box) { v.set("length", double(z.halfX * 2 / 100)); v.set("width", double(z.halfY * 2 / 100)); }
    else v.set("radius", double(z.radius / 100));
    v.set("height", double(z.height / 100)); v.set("world", double(z.world));
    v.set("enabled", z.enabled); v.set("notify", z.notify); v.set("text", z.text);
    return v;
}
bool Server::ZoneFromJson(const json::Value& v, Zone& z, std::string& error) {
    if (!v.isObject()) { error = "a table expected"; return false; }
    if (v["name"].isString()) z.name = v["name"].asString().substr(0, 64);
    if (const auto& t = v["type"].asString(); !t.empty()) {
        if (t != "pve" && t != "pvp") { error = "type must be \"pve\" or \"pvp\""; return false; }
        z.pvp = t == "pvp";
    }
    if (const auto& s = v["shape"].asString(); !s.empty()) {
        if (s != "circle" && s != "box") { error = "shape must be \"circle\" or \"box\""; return false; }
        z.box = s == "box";
    } else if (v["width"].isNumber() || v["length"].isNumber()) z.box = true;
    auto num = [&](const char* key, float& out, float scale, float lo) {
        if (v[key].isNumber() && std::isfinite(v[key].asNumber())) out = std::max(lo, float(v[key].asNumber()) * scale);
    };
    num("x", z.x, 1, -1e9f); num("y", z.y, 1, -1e9f); num("z", z.z, 1, -1e9f); num("yaw", z.yaw, 1, -1e9f);
    num("radius", z.radius, 100, 50); num("length", z.halfX, 50, 50); num("width", z.halfY, 50, 50); num("height", z.height, 100, 100);
    if (v["world"].isNumber()) z.world = int32_t(v["world"].asNumber());
    if (v["enabled"].type() == json::Value::Type::Bool) z.enabled = v["enabled"].asBool();
    if (v["notify"].type() == json::Value::Type::Bool) z.notify = v["notify"].asBool();
    if (v["text"].isString()) z.text = v["text"].asString().substr(0, 256);
    if (z.name.empty()) { error = "a zone needs a name"; return false; }
    return true;
}
void Server::LoadZones(const std::string& mode) {
    zones_.clear(); pvp_ = true;
    std::ifstream in(root_ / "modes" / mode / "world.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    if (const json::Value& rules = v["rules"]; rules.isObject()) pvp_ = rules["pvp"].asBool(true);
    for (auto& e : v["zones"].elements()) {
        Zone z; std::string error;
        if (!ZoneFromJson(e, z, error)) { Log("world.json: zone skipped (%s)", error.c_str()); continue; }
        SetZone(std::move(z));
    }
    int pve = 0; for (auto& z : zones_) pve += !z.pvp;
    if (!zones_.empty() || !pvp_) Log("PvP %s; zones: %d PvE, %d PvP", pvp_ ? "on" : "off", pve, int(zones_.size()) - pve);
}
void Server::SetPvp(bool on) {
    if (pvp_ == on) return;
    pvp_ = on;
    for (auto p : Players()) UpdateZoneHud(p);
}
void Server::SetZone(Zone z) {
    for (auto& old : zones_) if (old.name == z.name) { old = std::move(z); return; }
    zones_.push_back(std::move(z));
}
bool Server::RemoveZone(const std::string& name) {
    const auto before = zones_.size();
    std::erase_if(zones_, [&](const Zone& z) { return z.name == name; });
    return zones_.size() != before;
}
Zone* Server::FindZone(const std::string& name) { for (auto& z : zones_) if (z.name == name) return &z; return nullptr; }
const Zone* Server::RuleZone(const Player& p) const {
    const Zone* pvpZone = nullptr;
    for (auto& z : zones_) {
        if (!z.enabled || (z.world != -1 && z.world != p.world) || !z.Contains(p.state.x, p.state.y, p.state.z)) continue;
        if (!z.pvp) return &z;  // safe beats an arena where they overlap
        if (!pvpZone) pvpZone = &z;
    }
    return pvpZone;
}
// Nobody hurts or is hurt in a PvE zone (shooting out of one neither). With PvP on, players hurt each
// other anywhere else; with it off, only when both stand in PvP zones. player:setPvp(false) takes a
// player out of it all; setPvp(true) on both of them lets them fight anywhere (a duel).
bool Server::CanHurt(const Player& victim, const Player& attacker) const {
    if (victim.pvp == 0 || attacker.pvp == 0) return false;
    if (victim.pvp == 1 && attacker.pvp == 1) return true;
    const Zone* v = RuleZone(victim); const Zone* a = RuleZone(attacker);
    if ((v && !v->pvp) || (a && !a->pvp)) return false;
    return pvp_ || (v && a);
}
// The line at the top of the screen while the zone changes the rule (or says something of its own).
void Server::SetMoney(Player* p, int64_t amount) {
    if (!p) return;
    p->money = std::clamp<int64_t>(amount, -999999999, 999999999);
    ShowMoney(p);
}
void Server::ShowMoney(Player* p) {
    if (!p || p->bot || !p->peer) return;
    if (!p->money) { SetHud(p, "dgmp.money", ""); return; }
    json::Value h = json::Value::Object();
    const std::string sum = std::to_string(p->money < 0 ? -p->money : p->money);
    h.set("text", (p->money < 0 ? "{E07070}-$ " : "{7CD67C}$ ") + sum);
    h.set("x", 0.985); h.set("y", 0.06); h.set("align", std::string("right")); h.set("size", 1.5);
    SetHud(p, "dgmp.money", h.dump());
}
void Server::UpdateZoneHud(Player* p) {
    if (!p || p->bot || !p->peer) return;
    std::string text;
    if (const Zone* z = RuleZone(*p); z && z->notify && (!z->text.empty() || z->pvp != pvp_)) {
        const bool ru = p->lang == "ru";
        text = !z->text.empty() ? z->text
             : z->pvp ? (ru ? "{E07070}PvP-зона{FFFFFF}: здесь игроки могут ранить друг друга" : "{E07070}PvP zone{FFFFFF}: players can hurt each other here")
                      : (ru ? "{7CD67C}Безопасная зона{FFFFFF}: игроки не могут ранить друг друга" : "{7CD67C}Safe zone{FFFFFF}: players cannot hurt each other");
    }
    if (text == p->zoneHud) return;
    p->zoneHud = text;
    if (text.empty()) { SetHud(p, "dgmp.zone", ""); return; }
    json::Value h = json::Value::Object();
    h.set("text", text); h.set("x", 0.5); h.set("y", 0.035); h.set("align", std::string("center")); h.set("size", 1.1);
    h.set("bg", std::string("#00000088"));
    SetHud(p, "dgmp.zone", h.dump());
}
void Server::TickZones() {
    struct Event { uint16_t player; double joinedAt; std::string zone; bool inside; };
    std::vector<Event> events;
    for (auto p : Players()) {
        std::vector<std::string> now;
        for (auto& z : zones_)
            if (z.enabled && (z.world == -1 || z.world == p->world) && z.Contains(p->state.x, p->state.y, p->state.z)) now.push_back(z.name);
        for (auto& n : p->zones) if (std::find(now.begin(), now.end(), n) == now.end()) events.push_back({p->id, p->joinedAt, n, false});
        for (auto& n : now) if (std::find(p->zones.begin(), p->zones.end(), n) == p->zones.end()) events.push_back({p->id, p->joinedAt, n, true});
        p->zones = std::move(now);
        UpdateZoneHud(p);
    }
    for (const auto& e : events) {  // handlers may kick players and change zones
        Player* p = Find(e.player);
        if (p && p->joinedAt == e.joinedAt) script_->ZoneEvent(*p, e.zone, e.inside);
    }
}

void Server::TickBots(float dt) {
    for (auto& [id, p] : players_) {
        if (!p->bot) continue;
        if (p->botDeadUntil) {  // shot: lies still, then comes back with full health
            if (Now() < p->botDeadUntil) { p->state.vx = p->state.vy = p->state.vz = 0; p->state.anim = net::AnimIdle; p->state.flags = net::StateDead; p->state.time = Now(); continue; }
            p->botDeadUntil = 0; p->state.health = 100;
        }
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
void Server::Spectate(Player* p, bool on, const std::vector<uint16_t>& only) {
    if (!p || p->bot) return;
    p->spectating = on;
    net::Writer w; w.Op(net::Msg::Spectate); w.U8(on); w.U8(uint8_t(std::min<size_t>(only.size(), 255)));
    for (size_t i = 0; i < only.size() && i < 255; ++i) w.U16(only[i]);
    Send(p, w, true);
}
void Server::LoadSkins() {
    std::ifstream in(root_ / "data" / "skins.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    for (auto& [nick, skin] : v.items()) if (skin.isString()) skins_[nick] = skin.asString();
    Log("Skins of %zu players", skins_.size());
}
void Server::SaveSkins() {
    json::Value v = json::Value::Object();
    for (auto& [nick, skin] : skins_) v.set(nick, skin);
    std::error_code ec; std::filesystem::create_directories(root_ / "data", ec);
    const auto file = root_ / "data" / "skins.json", tmp = root_ / "data" / "skins.json.tmp";
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << v.dump(); }
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(file, ec); std::filesystem::rename(tmp, file, ec); }
}
void Server::WearEffective(Player* p) {
    if (!p) return;
    const std::string& look = p->uniform.empty() ? p->skin : p->uniform;
    if (!look.empty() && look != p->look) SetLook(p, look);
}
void Server::SetSkin(Player* p, const std::string& skin) {
    if (!p) return;
    p->skin = skin;
    std::string key = p->nick; for (auto& ch : key) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (skin.empty()) skins_.erase(key); else skins_[key] = skin;
    if (!p->bot) SaveSkins();
    WearEffective(p);
}
void Server::SetUniform(Player* p, const std::string& look) {
    if (!p) return;
    p->uniform = look;
    WearEffective(p);
}
void Server::Customize(Player* p, bool cancel, const std::string& title, const std::string& families,
                       const std::vector<const SkinChoice*>& choices, bool uniform, const Spot& place, bool back) {
    if (!p || p->bot) return;
    // Chosen at a place of the mode (a room under the ground): there and, when done, back.
    if (place.set) {
        if (!p->inWardrobe) { p->inWardrobe = true; p->backX = p->state.x; p->backY = p->state.y; p->backZ = p->state.z; p->backYaw = p->state.yaw; }
        p->wardrobeReturn = back;
        Teleport(p, place.x, place.y, place.z + 100.f, place.yaw);
    }
    p->offered.clear(); p->offeredUniform = uniform;
    const std::string current = uniform && !p->uniform.empty() ? p->uniform : (p->skin.empty() ? p->look : p->skin);
    net::Writer w; w.Op(net::Msg::Customize); w.U8(cancel); w.Str(title); w.Str(families); w.Str(current);
    const size_t n = std::min<size_t>(choices.size(), 255);
    w.U8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) {
        const SkinChoice& c = *choices[i];
        const std::string& name = p->lang == "ru" && !c.ru.empty() ? c.ru : (c.en.empty() ? c.id : c.en);
        w.Str(name.substr(0, 128)); w.Str(c.look);
        p->offered.push_back(c.look);
    }
    w.U8(place.set);
    if (place.set) { w.F32(place.x); w.F32(place.y); w.F32(place.z + 100.f); w.F32(place.yaw); }
    Send(p, w, true);
}
uint16_t Server::ShowDialog(Player* p, net::DialogStyle style, const std::string& title, const std::string& text, const std::string& button1,
                            const std::string& button2, const std::string& defaultText, const std::string& headers, const std::vector<std::string>& items) {
    if (!p || p->bot) return 0;
    if (!++nextDialog_) ++nextDialog_;
    p->dialogId = nextDialog_; p->dialogStyle = style;
    p->dialogItems.assign(items.begin(), items.begin() + long(std::min(items.size(), net::kDialogItemsMax)));
    net::Writer w; w.Op(net::Msg::Dialog); w.U16(p->dialogId); w.U8(uint8_t(style));
    w.Str(title.substr(0, 256)); w.Str(text.substr(0, net::kDialogTextMax)); w.Str(button1.substr(0, 64)); w.Str(button2.substr(0, 64));
    w.Str(defaultText.substr(0, 256)); w.Str(headers.substr(0, 512));
    w.U8(uint8_t(p->dialogItems.size()));
    for (auto& it : p->dialogItems) w.Str(it.substr(0, 512));
    Send(p, w, true);
    return p->dialogId;
}
void Server::CloseDialog(Player* p) {
    if (!p || p->bot || !p->dialogId) return;
    net::Writer w; w.Op(net::Msg::Dialog); w.U16(p->dialogId); w.U8(uint8_t(net::DialogStyle::Close));
    p->dialogId = 0; p->dialogItems.clear();
    Send(p, w, true);
}
void Server::SetHud(Player* p, const std::string& id, const std::string& json) {
    if (!p || p->bot) return;
    net::Writer w; w.Op(net::Msg::HudElement); w.Str(id.substr(0, 64)); w.Str(json.size() <= 4096 ? json : std::string());
    Send(p, w, true);
}
void Server::SendInteraction(ENetPeer* peer, const Interaction& in) {
    net::Writer w; w.Op(net::Msg::Interaction); w.U16(in.id); w.U8(1);
    w.F32(in.x); w.F32(in.y); w.F32(in.z); w.F32(in.radius); w.U8(in.key); w.Str(in.text.substr(0, 256)); w.I32(in.world);
    SendTo(peer, w, true);
}
uint16_t Server::AddInteraction(Interaction in) {
    while (interactions_.count(nextInteraction_) || !nextInteraction_) ++nextInteraction_;
    in.id = nextInteraction_++;
    interactions_[in.id] = in;
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendInteraction(p->peer, in);
    return in.id;
}
bool Server::RemoveInteraction(uint16_t id) {
    if (!interactions_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Interaction); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
std::string Server::NickKey(const std::string& nick) {
    std::string key = nick; for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    return key;
}
void Server::SendVehicle(Player* to, const Vehicle& v) {
    for (auto& [id, p] : players_) {
        if ((to && p.get() != to) || !p->ready || !p->peer) continue;
        net::Writer w; w.Op(net::Msg::Vehicle); w.U16(v.id); w.U8(1);
        const std::string key = NickKey(p->nick);
        const bool yours = !v.owner.empty() && v.owner == key;
        const bool marked = v.blip == 2 || (v.blip == 1 && yours) || v.blipFor.count(key);
        w.Vehicle(v.state); w.I32(v.world); w.U16(v.driver); w.U8(v.locked); w.U8(yours); w.U8(marked); w.U32(v.blipColor); w.Str(v.blipIcon); w.Str(v.model); w.U8(v.npc); w.Str(v.npcLook); w.Str(v.npcName.substr(0, 63)); w.U16(v.passenger); w.U8(uint8_t(std::count_if(v.riders.begin(), v.riders.end(), [](uint16_t r) { return r != 0xFFFF; })));
        SendTo(p->peer, w, true);
    }
}
uint16_t Server::AddVehicle(Vehicle v) {
    while (vehicles_.count(nextVehicle_) || !nextVehicle_) ++nextVehicle_;
    v.id = nextVehicle_++; v.driver = 0xFFFF;
    vehicles_[v.id] = v;
    SendVehicle(nullptr, v);
    return v.id;
}
bool Server::UpdateVehicle(const Vehicle& v) {
    auto it = vehicles_.find(v.id);
    if (it == vehicles_.end()) return false;
    const uint16_t driver = it->second.driver;
    it->second = v; it->second.driver = driver;
    SendVehicle(nullptr, it->second);
    return true;
}
bool Server::RemoveVehicle(uint16_t id) {
    auto it = vehicles_.find(id);
    if (it == vehicles_.end()) return false;
    if (Player* d = Find(it->second.driver)) { d->vehicle = 0; net::Writer e; e.Op(net::Msg::VehicleEject); e.U16(id); Send(d, e, true); }
    if (Player* q = Find(it->second.passenger)) { q->passengerOf = 0; net::Writer e; e.Op(net::Msg::VehicleEject); e.U16(id); Send(q, e, true); }
    for (uint16_t r : it->second.riders) if (Player* q = Find(r)) { q->passengerOf = 0; net::Writer e; e.Op(net::Msg::VehicleEject); e.U16(id); Send(q, e, true); }
    vehicles_.erase(it);
    net::Writer w; w.Op(net::Msg::Vehicle); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
// Where a vehicle driving `route` is after `dist` units: true when past its end (not looping).
// (Vertical legs, a helicopter's climb and descent, take three times longer: their length counts x3.)
static bool RouteAt(const std::vector<float>& pts, bool loop, double dist, net::VehicleState& at) {
    const size_t n = pts.size() / 3;
    if (n == 0) return true;
    auto len = [&](size_t a, size_t b) {
        const float dx = pts[b * 3] - pts[a * 3], dy = pts[b * 3 + 1] - pts[a * 3 + 1], dz = pts[b * 3 + 2] - pts[a * 3 + 2];
        const float flat = std::sqrt(dx * dx + dy * dy), l = std::sqrt(flat * flat + dz * dz);
        return flat < 0.3f * l ? l * 3 : l;
    };
    const size_t segs = loop ? n : n - 1;
    double total = 0;
    for (size_t i = 0; i < segs; ++i) total += len(i, (i + 1) % n);
    if (segs == 0 || total < 1) { at.x = pts[0]; at.y = pts[1]; at.z = pts[2]; return !loop; }
    bool ended = false;
    if (loop) dist = std::fmod(std::max(0.0, dist), total);
    else if (dist >= total) { dist = total; ended = true; }
    for (size_t i = 0; i < segs; ++i) {
        const size_t j = (i + 1) % n;
        const float l = len(i, j);
        if (dist > l && i + 1 < segs) { dist -= l; continue; }
        const float k = l > 0 ? float(std::min(1.0, dist / l)) : 1.f;
        at.x = pts[i * 3] + (pts[j * 3] - pts[i * 3]) * k; at.y = pts[i * 3 + 1] + (pts[j * 3 + 1] - pts[i * 3 + 1]) * k; at.z = pts[i * 3 + 2] + (pts[j * 3 + 2] - pts[i * 3 + 2]) * k;
        const float hx = pts[j * 3] - pts[i * 3], hy = pts[j * 3 + 1] - pts[i * 3 + 1];
        if (hx * hx + hy * hy > 1.f) at.yaw = std::atan2(hy, hx) * 57.2957795f;
        break;
    }
    return ended;
}
void Server::SendRoute(Player* to, const Vehicle& v) {
    net::Writer w; w.Op(net::Msg::VehicleRoute); w.U16(v.id); w.F32(v.routeSpeed); w.F32(float(Now() - v.routeStart)); w.U8(v.routeLoop);
    const size_t n = std::min<size_t>(v.route.size() / 3, 512);
    w.U16(uint16_t(n));
    for (size_t i = 0; i < n * 3; ++i) w.F32(v.route[i]);
    if (to) Send(to, w, true); else Send(nullptr, w, true);
}
bool Server::DriveVehicle(uint16_t id, const std::vector<float>& points, float speed, bool loop, float altitude) {
    Vehicle* v = FindVehicle(id);
    if (!v || (!v->npc && !v->Helicopter()) || points.size() < 3 || speed <= 0) return false;
    v->route = {v->state.x, v->state.y, v->state.z};
    if (v->Helicopter() && altitude > 0) {  // up, over the points, down onto the last
        v->route.insert(v->route.end(), {v->state.x, v->state.y, v->state.z + altitude});
        for (size_t i = 0; i + 2 < points.size(); i += 3) v->route.insert(v->route.end(), {points[i], points[i + 1], points[i + 2] + altitude});
        if (!loop) v->route.insert(v->route.end(), points.end() - 3, points.end());
    } else v->route.insert(v->route.end(), points.begin(), points.end());
    v->routeSpeed = speed; v->routeLoop = loop; v->routeStart = Now();
    SendRoute(nullptr, *v);
    return true;
}
bool Server::StopVehicle(uint16_t id) {
    Vehicle* v = FindVehicle(id);
    if (!v || v->route.empty()) return false;
    RouteAt(v->route, v->routeLoop, (Now() - v->routeStart) * v->routeSpeed, v->state);
    v->route.clear();
    SendRoute(nullptr, *v);
    SendVehicle(nullptr, *v);
    return true;
}
void Server::TickVehicles() {
    std::vector<uint16_t> arrived;
    const double now = Now();
    for (auto& [id, v] : vehicles_) {
        if (v.route.empty()) continue;
        if (RouteAt(v.route, v.routeLoop, (now - v.routeStart) * v.routeSpeed, v.state)) arrived.push_back(id);
    }
    for (uint16_t id : arrived) {
        Vehicle* v = FindVehicle(id);
        if (!v) continue;
        v->route.clear();
        SendRoute(nullptr, *v);
        SendVehicle(nullptr, *v);
        script_->VehicleArrive(id);
    }
}
bool Server::PutInVehicle(Player* p, uint16_t id, bool passenger) {
    Vehicle* v = FindVehicle(id);
    if (!p || p->bot || !v) return false;
    if (v->Helicopter()) {  // a seat in it (nobody drives it: its NPC, or nobody, flies it)
        v->riders.resize(kHelicopterSeats, 0xFFFF);
        const auto free = std::find(v->riders.begin(), v->riders.end(), uint16_t(0xFFFF));
        if (free == v->riders.end()) return false;
        if (script_->VehicleEnter(*p, id, true, true) == 1) return false;
        if (p->vehicle || p->passengerOf) EjectFromVehicle(p);
        *free = p->id; p->passengerOf = id;
        SendVehicle(nullptr, *v);
        net::Writer w; w.Op(net::Msg::VehicleAllow); w.U16(id); w.U8(uint8_t(1 + (free - v->riders.begin()))); Send(p, w, true);
        return true;
    }
    if (passenger ? v->passenger != 0xFFFF || (v->driver == 0xFFFF && !v->npc) : v->driver != 0xFFFF || v->npc) return false;
    if (p->vehicle || p->passengerOf) EjectFromVehicle(p);
    if (passenger) { v->passenger = p->id; p->passengerOf = id; }
    else { v->driver = p->id; p->vehicle = id; }
    SendVehicle(nullptr, *v);  // the others hide their copy: the rider brings it
    net::Writer w; w.Op(net::Msg::VehicleAllow); w.U16(id); w.U8(passenger); Send(p, w, true);
    return true;
}
void Server::LeaveVehicle(Player* p, const net::VehicleState* where) {
    if (p && p->passengerOf) {  // off from behind
        const uint16_t id = p->passengerOf;
        p->passengerOf = 0;
        Vehicle* v = FindVehicle(id);
        if (!v) return;
        if (auto seat = std::find(v->riders.begin(), v->riders.end(), p->id); seat != v->riders.end()) *seat = 0xFFFF;
        else if (v->passenger == p->id) v->passenger = 0xFFFF;
        else return;
        SendVehicle(nullptr, *v);
        script_->VehicleExit(*p, id);
        return;
    }
    Vehicle* v = p && p->vehicle ? FindVehicle(p->vehicle) : nullptr;
    const uint16_t id = p ? p->vehicle : 0;
    if (p) p->vehicle = 0;
    if (!v || v->driver != p->id) return;
    v->driver = 0xFFFF;
    if (!where) { v->state.x = p->state.bx; v->state.y = p->state.by; v->state.z = p->state.bz; }  // gone while riding: where its bike was
    if (Player* q = Find(v->passenger)) EjectFromVehicle(q);  // the passenger gets off with the driver
    SendVehicle(nullptr, *v);
    script_->VehicleExit(*p, id);
}
void Server::MarkVehicle(Player* p, uint16_t id, bool on) {
    Vehicle* v = FindVehicle(id);
    if (!p || !v) return;
    if (on) v->blipFor.insert(NickKey(p->nick)); else v->blipFor.erase(NickKey(p->nick));
    SendVehicle(p, *v);
}
void Server::EjectFromVehicle(Player* p) {
    if (!p || (!p->vehicle && !p->passengerOf)) return;
    net::Writer w; w.Op(net::Msg::VehicleEject); w.U16(p->vehicle ? p->vehicle : p->passengerOf); Send(p, w, true);
    LeaveVehicle(p, nullptr);
}
// The mode's images: modes/<mode>/images/*.png|jpg|jpeg|gif, up to 2 MB each and 32 MB in all.
void Server::LoadImages(const std::string& mode) {
    images_.clear();
    std::error_code ec;
    size_t total = 0;
    for (auto it = std::filesystem::directory_iterator(root_ / "modes" / mode / "images", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string name = it->path().filename().string();
        std::string ext = it->path().extension().string(); for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif") continue;
        bool plain = name.size() <= 100;
        for (char c : name) plain &= std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        if (!plain) { Log("modes/%s/images/%s: names are latin letters, digits, _ - . (skipped)", mode.c_str(), name.c_str()); continue; }
        const auto size = it->file_size(ec);
        if (ec || size > 2u * 1024 * 1024 || total + size > 32u * 1024 * 1024) { Log("modes/%s/images/%s: too large (skipped)", mode.c_str(), name.c_str()); continue; }
        std::ifstream in(it->path(), std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        total += data.size();
        images_.push_back({name, Sha256Hex(data.data(), data.size()), std::move(data)});
    }
    if (!images_.empty()) Log("Mode images: %zu (%zu KB)", images_.size(), total / 1024);
    SendImageList(nullptr);
}
void Server::SendImageList(Player* to) {
    net::Writer w; w.Op(net::Msg::ImageList); w.U16(uint16_t(std::min<size_t>(images_.size(), 4000)));
    for (size_t i = 0; i < images_.size() && i < 4000; ++i) { w.Str(images_[i].name); w.Str(images_[i].hash); }
    Send(to, w, true);
}
void Server::SendIcon(ENetPeer* peer, const Icon& i) {
    net::Writer w; w.Op(net::Msg::Icon); w.U16(i.id); w.U8(1);
    w.F32(i.x); w.F32(i.y); w.F32(i.z); w.F32(i.size); w.F32(i.distance); w.I32(i.world); w.U32(i.color);
    w.Str(i.image.substr(0, 127)); w.Str(i.text.substr(0, 255)); w.U8(i.throughWalls);
    SendTo(peer, w, true);
}
uint16_t Server::AddIcon(Icon i) {
    while (icons_.count(nextIcon_) || !nextIcon_) ++nextIcon_;
    i.id = nextIcon_++;
    icons_[i.id] = i;
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendIcon(p->peer, i);
    return i.id;
}
bool Server::UpdateIcon(const Icon& i) {
    auto it = icons_.find(i.id);
    if (it == icons_.end()) return false;
    it->second = i;
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendIcon(p->peer, i);
    return true;
}
bool Server::RemoveIcon(uint16_t id) {
    if (!icons_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Icon); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
void Server::SetWorld(Player* p, int32_t world) {
    if (!p || p->world == world) return;
    p->world = world;
    if (p->bot) return;
    net::Writer w; w.Op(net::Msg::SetWorld); w.I32(world); Send(p, w, true);
}
void Server::SendActor(ENetPeer* peer, const Actor& a) {
    net::Writer w; w.Op(net::Msg::Actor); w.U16(a.id); w.U8(1);
    w.F32(a.x); w.F32(a.y); w.F32(a.z); w.F32(a.yaw); w.I32(a.world);
    w.Str(a.look.substr(0, net::kLookPathMax - 1)); w.Str(a.anim.substr(0, 511)); w.Str(a.name.substr(0, 127)); w.U8(a.solid);
    SendTo(peer, w, true);
}
uint16_t Server::AddActor(Actor a) {
    while (actors_.count(nextActor_) || !nextActor_) ++nextActor_;
    a.id = nextActor_++;
    actors_[a.id] = a;
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendActor(p->peer, a);
    return a.id;
}
bool Server::UpdateActor(const Actor& a) {
    auto it = actors_.find(a.id);
    if (it == actors_.end()) return false;
    it->second = a;
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendActor(p->peer, a);
    return true;
}
bool Server::RemoveActor(uint16_t id) {
    if (!actors_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Actor); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
// What the clients can show (src/carry.cpp kModels).
static const char* const kCarryModels[] = {"crate", "cooler", "medcase", "centrifuge"};
bool Carry::KnownModel(const std::string& m) { for (auto k : kCarryModels) if (m == k) return true; return false; }
const char* Carry::ModelList() {
    static const std::string list = [] { std::string s; for (auto k : kCarryModels) s += (s.empty() ? "" : ", ") + std::string(k); return s; }();
    return list.c_str();
}
// The boxes the editor placed for the mode (a new mode or a reload: whatever lay before goes).
void Server::LoadCarries(const std::string& mode) {
    std::vector<uint16_t> old;
    for (auto& [id, c] : carries_) old.push_back(id);
    for (auto id : old) RemoveCarry(id);
    std::ifstream in(root_ / "modes" / mode / "world.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    size_t made = 0;
    for (auto& e : v["carries"].elements()) {
        Carry c;
        c.key = e["key"].asString().substr(0, 64); c.tag = e["tag"].asString().substr(0, 64);
        c.model = e["model"].isString() ? e["model"].asString() : "crate";
        c.x = float(e["x"].asNumber()); c.y = float(e["y"].asNumber()); c.z = float(e["z"].asNumber()); c.yaw = float(e["yaw"].asNumber());
        c.world = e["world"].isNumber() ? int32_t(e["world"].asNumber()) : -1;
        const std::string roll = e["roll"].asString();
        c.roll = roll == "drop" ? net::CarryRoll::Drop : roll == "break" ? net::CarryRoll::Break : net::CarryRoll::Keep;
        if (!Carry::KnownModel(c.model)) { Log("world.json: box %s skipped (unknown model \"%s\")", c.key.c_str(), c.model.c_str()); continue; }
        if (!c.key.empty() && FindCarry(c.key)) { Log("world.json: box %s skipped (the key is taken)", c.key.c_str()); continue; }
        AddCarry(c); ++made;
    }
    if (made) Log("Mode boxes: %zu", made);
}
void Server::SendCarry(Player* to, const Carry& c) {
    net::Writer w; w.Op(net::Msg::Carry); w.U16(c.id); w.U8(1);
    w.Str(c.model); w.F32(c.x); w.F32(c.y); w.F32(c.z); w.F32(c.yaw); w.I32(c.world); w.U16(c.carrier); w.U8(uint8_t(c.roll));
    Send(to, w, true);
}
uint16_t Server::AddCarry(Carry c) {
    while (carries_.count(nextCarry_) || !nextCarry_) ++nextCarry_;
    c.id = nextCarry_++; c.carrier = 0xFFFF;
    carries_[c.id] = c;
    SendCarry(nullptr, c);
    return c.id;
}
bool Server::UpdateCarry(const Carry& c) {
    auto it = carries_.find(c.id);
    if (it == carries_.end()) return false;
    if (it->second.carrier != c.carrier) {
        if (Player* was = Find(it->second.carrier); was && was->carrying == c.id) was->carrying = 0;
        if (Player* now = Find(c.carrier)) now->carrying = c.id;
    }
    it->second = c;
    SendCarry(nullptr, c);
    return true;
}
bool Server::RemoveCarry(uint16_t id) {
    auto it = carries_.find(id);
    if (it == carries_.end()) return false;
    if (Player* p = Find(it->second.carrier); p && p->carrying == id) p->carrying = 0;
    carries_.erase(it);
    net::Writer w; w.Op(net::Msg::Carry); w.U16(id); w.U8(2); Send(nullptr, w, true);
    return true;
}
bool Server::GiveCarry(Player* p, uint16_t id) {
    Carry* c = FindCarry(id);
    if (!p || p->bot || !c) return false;
    if (p->carrying) return p->carrying == id;
    Carry n = *c; n.carrier = p->id;  // from someone else's hands too
    return UpdateCarry(n);
}
uint16_t Server::DropCarry(Player* p, const float* at) {
    Carry* c = p ? FindCarry(p->carrying) : nullptr;
    if (!c) { if (p) p->carrying = 0; return 0; }
    Carry n = *c;
    if (at) { n.x = at[0]; n.y = at[1]; n.z = at[2]; n.yaw = at[3]; }
    else { n.x = p->state.x; n.y = p->state.y; n.z = p->state.z - 90.f; n.yaw = p->state.yaw; }  // state: the capsule centre
    if (n.world >= 0) n.world = p->world;
    n.carrier = 0xFFFF;
    UpdateCarry(n);
    return n.id;
}
void Server::LoseCarry(Player& p, float x, float y, float z, float yaw, const char* reason) {
    const uint16_t id = p.carrying;
    const float at[4] = {x, y, z, yaw};
    if (!DropCarry(&p, at)) return;
    script_->CarryDrop(p, id, x, y, z, reason);  // told after: it lies there now (the mode may still move or remove it)
}
void Server::OnCarryAction(Player& p, net::Reader& r) {
    const uint16_t id = r.U16();
    const auto op = net::CarryOp(r.U8());
    float at[4]; for (float& f : at) f = r.F32();
    Carry* c = FindCarry(id);
    if (!r.ok() || !c) return;
    // The spot the client names must be a real one next to its player; else where the player stands.
    const float sx = at[0] - p.state.x, sy = at[1] - p.state.y, sz = at[2] - p.state.z;
    if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2]) || !std::isfinite(at[3]) ||
        sx * sx + sy * sy > 600.f * 600.f || std::fabs(sz) > 600.f) {
        at[0] = p.state.x; at[1] = p.state.y; at[2] = p.state.z - 90.f; at[3] = p.state.yaw;
    }
    switch (op) {
    case net::CarryOp::Pick: {
        if (c->carrier != 0xFFFF || p.carrying || p.vehicle || p.passengerOf || (p.state.flags & net::StateDead)) break;
        if (c->world >= 0 && c->world != p.world) break;
        const float dx = p.state.x - c->x, dy = p.state.y - c->y, dz = p.state.z - c->z;
        if (dx * dx + dy * dy > 350.f * 350.f || std::fabs(dz) > 350.f) break;  // not at it
        if (!script_->CarryPick(p, id)) break;
        if (!GiveCarry(&p, id)) break;  // (the handler may have changed it)
        break;
    }
    case net::CarryOp::Put: {
        if (c->carrier != p.id) break;
        if (!script_->CarryDrop(p, id, at[0], at[1], at[2], "put")) { SendCarry(&p, *c); break; }  // refused: it stays in the hands
        c = FindCarry(id);
        if (c && c->carrier == p.id) DropCarry(&p, at);  // (the handler may have taken it: a delivery)
        break;
    }
    case net::CarryOp::Lost:
    case net::CarryOp::Roll:
        if (c->carrier == p.id) LoseCarry(p, at[0], at[1], at[2], at[3], op == net::CarryOp::Roll ? "roll" : "lost");
        break;
    case net::CarryOp::Break: {
        // It broke in a dodge roll: it goes, unless the mode put it somewhere else in carryDrop (back on the shelf).
        if (c->carrier != p.id || c->roll != net::CarryRoll::Break) break;
        DropCarry(&p, at);
        script_->CarryDrop(p, id, at[0], at[1], at[2], "break");
        c = FindCarry(id);
        if (c && c->carrier == 0xFFFF && c->x == at[0] && c->y == at[1] && c->z == at[2]) RemoveCarry(id);
        break;
    }
    }
}
void Server::LoadAnims() {
    std::ifstream in(root_ / "data" / "anims.txt");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto t1 = line.find('\t'); if (t1 == std::string::npos) continue;
        const auto t2 = line.find('\t', t1 + 1);
        std::string name = line.substr(0, t1); for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        anims_.emplace(name, line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1));
    }
    Log("Animations catalog: %zu entries", anims_.size());
}
std::string Server::ResolveAnim(const std::string& nameOrPath) const {
    if (!nameOrPath.empty() && nameOrPath[0] == '/') return nameOrPath;
    std::string key = nameOrPath; for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto it = anims_.find(key);
    return it == anims_.end() ? std::string() : it->second;
}
void Server::SendWatchedKeys(Player* to) {
    net::Writer w; w.Op(net::Msg::WatchKeys); w.U8(uint8_t(std::min<size_t>(watchedKeys_.size(), 255)));
    for (uint8_t k : watchedKeys_) w.U8(k);
    Send(to, w, true);
}
void Server::WatchKey(uint8_t key, bool on) {
    if (on ? !watchedKeys_.insert(key).second : !watchedKeys_.erase(key)) return;
    SendWatchedKeys(nullptr);
}
std::string Server::ResolveSkinOrLook(const std::string& want) const {
    if (want.rfind("skin:", 0) == 0) return want.size() < net::kLookPathMax ? want : std::string();
    for (const char* kind : {"skins", "uniforms"}) if (auto c = FindSkin(kind, want)) return c->look;
    return ResolveLook(want);
}
// The NPCs the editor placed for the mode (a new mode or a reload: whatever stood before goes).
void Server::LoadWorldActors(const std::string& mode) {
    std::vector<uint16_t> old;
    for (auto& [id, a] : actors_) old.push_back(id);
    for (auto id : old) RemoveActor(id);
    std::ifstream in(root_ / "modes" / mode / "world.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    json::Value v;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), v) || !v.isObject()) return;
    size_t made = 0;
    for (auto& e : v["actors"].elements()) {
        Actor a;
        a.key = e["id"].asString(); a.group = e["group"].asString();
        a.x = float(e["x"].asNumber()); a.y = float(e["y"].asNumber()); a.z = float(e["z"].asNumber()); a.yaw = float(e["yaw"].asNumber());
        a.look = ResolveSkinOrLook(e["look"].asString());
        if (a.look.empty()) a.look = ResolveLook("deacon_standard");
        a.anim = ResolveAnim(e["anim"].isString() ? e["anim"].asString() : "idle");
        if (a.anim.empty()) a.anim = ResolveAnim("idle");
        a.name = e["name"].asString(); a.solid = e["solid"].asBool(true);
        a.world = e["world"].isNumber() ? int32_t(e["world"].asNumber()) : -1;
        if (a.key.empty()) continue;
        AddActor(a); ++made;
    }
    if (made) Log("Mode NPCs: %zu", made);
}
void Server::LoadModeSkins(const std::string& mode) {
    modeSkins_.clear();
    std::ifstream in(root_ / "modes" / mode / "skins.json", std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    if (ss.str().empty()) return;
    json::Value v;
    if (!json::Value::Parse(ss.str(), v) || !v.isObject()) { Log("modes/%s/skins.json is not valid JSON; ignored", mode.c_str()); return; }
    for (const char* kind : {"skins", "uniforms"}) {
        const json::Value& list = v[kind];
        for (size_t i = 0; i < list.size(); ++i) {
            const json::Value& e = list[i];
            SkinChoice c;
            c.id = e["id"].asString(); c.look = e["look"].asString();
            const json::Value& name = e["name"];
            if (name.isObject()) { c.en = name["en"].asString(); c.ru = name["ru"].asString(); } else c.en = name.asString();
            const bool lookOk = c.look.rfind("skin:", 0) == 0 || (!c.look.empty() && c.look[0] == '/');
            if (c.id.empty() || !lookOk || c.look.size() >= net::kLookPathMax) { Log("modes/%s/skins.json: %s #%zu skipped (no id or look)", mode.c_str(), kind, i + 1); continue; }
            modeSkins_[kind].push_back(std::move(c));
        }
    }
    Log("Mode skins: %zu to start with, %zu uniforms", SkinList("skins").size(), SkinList("uniforms").size());
}
const std::vector<Server::SkinChoice>& Server::SkinList(const std::string& kind) const {
    static const std::vector<SkinChoice> none;
    auto it = modeSkins_.find(kind);
    return it == modeSkins_.end() ? none : it->second;
}
const Server::SkinChoice* Server::FindSkin(const std::string& kind, const std::string& id) const {
    for (auto& c : SkinList(kind)) if (c.id == id) return &c;
    return nullptr;
}
void Server::Freeze(Player* p, bool on) {
    if (!p || p->bot) return;
    net::Writer w; w.Op(net::Msg::Freeze); w.U8(on); Send(p, w, true);
}
void Server::HurtBot(Player* bot, float amount, Player* attacker) {
    if (bot->botDeadUntil > Now()) return;
    const int left = int(bot->state.health) - int(std::lround(amount * 100.f / 135.f));  // 135: the player's maximum health
    if (left > 0) { bot->state.health = uint8_t(left); return; }
    bot->state.health = 0;
    bot->botDeadUntil = Now() + 4;
    net::Writer w; w.Op(net::Msg::Corpse); w.U16(bot->id);
    for (auto& [id, other] : players_) if (other->ready && other->peer) SendTo(other->peer, w, true);
    bot->inventoryAtDeath.clear();
    script_->PlayerDeath(*bot, attacker);
    Log("%s was killed by %s", bot->nick.c_str(), attacker ? attacker->nick.c_str() : "?");
}
void Server::Damage(Player* p, float amount, Player* attacker) {
    if (!p || !p->ready || !std::isfinite(amount) || amount <= 0) return;
    if (attacker) { p->lastAttacker = attacker->id; p->lastHitAt = Now(); }
    if (p->bot) { HurtBot(p, amount, attacker); return; }
    if (!p->peer) return;
    net::Writer w; w.Op(net::Msg::Damage); w.U16(attacker ? attacker->id : 0xFFFF); w.F32(std::min(amount, 10000.f)); w.U8(uint8_t(net::HitZone::Body));
    SendTo(p->peer, w, true);
}
Player* Server::Find(uint16_t id) { auto it = players_.find(id); return it != players_.end() && it->second->ready ? it->second.get() : nullptr; }
Player* Server::FindByName(const std::string& nick) {
    for (auto& [id, p] : players_) if (p->ready && StrICmp(p->nick.c_str(), nick.c_str()) == 0) return p.get();
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
    for (auto& [name, path] : looks_) if (StrICmp(name.c_str(), nameOrPath.c_str()) == 0) return path;
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
    net::Writer w; w.Op(net::Msg::Settings); w.U8(static_cast<uint8_t>(mode)); w.U8(cfg_.collisions); Send(nullptr, w, true);
}
void Server::SetCollisions(uint8_t mask) {
    cfg_.collisions = mask & (net::CollidePlayers | net::CollideBikes);
    SetNametags(cfg_.nametags);
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
    for (auto& [id, p] : players_) if (p->ready && p->peer) { Objective(p.get(), 4, 0, 0, "", ""); ObjectiveMeter(p.get(), false, "", 0); CloseDialog(p.get()); SetHud(p.get(), "", ""); }
    for (auto& [id, p] : players_) { p->pvp = -1; p->zones.clear(); p->zoneHud.clear(); p->money = 0; }  // money too: the new code sets it again (playerJoin)  // zones and PvP come again from the file and the new code
    ids.clear();
    for (auto& [id, in] : interactions_) ids.push_back(id);
    for (auto id : ids) RemoveInteraction(id);
    ids.clear();
    for (auto& [id, a] : actors_) ids.push_back(id);
    for (auto id : ids) RemoveActor(id);
    for (auto& [id, p] : players_) SetWorld(p.get(), 0);
    ids.clear();
    for (auto& [id, v] : vehicles_) ids.push_back(id);
    for (auto id : ids) RemoveVehicle(id);
    ids.clear();
    for (auto& [id, ic] : icons_) ids.push_back(id);
    for (auto id : ids) RemoveIcon(id);
    ids.clear();
    for (auto& [id, c] : carries_) ids.push_back(id);
    for (auto id : ids) RemoveCarry(id);
    if (!watchedKeys_.empty()) { watchedKeys_.clear(); SendWatchedKeys(nullptr); }
    LoadEnvironment(mode);
    if (!script_->Load(mode)) return false;
    LoadWorldEdits();
    for (auto& [id, p] : players_) if (p->ready && p->peer) SendWorldEdits(p->peer);
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
    } else if (cmd == "botgear") {  // test: bots carry what the first player carries
        const Player* source = nullptr;
        for (auto& [id, p] : players_) if (p->ready && !p->bot && !p->gear.empty()) { source = p.get(); break; }
        if (!source) { Log("No player has reported gear yet"); return; }
        for (auto& [id, b] : players_) {
            if (!b->bot) continue;
            b->gear = source->gear;
            net::Writer w; w.Op(net::Msg::PlayerGear); w.U16(b->id); w.Raw(b->gear.data(), b->gear.size());
            Send(nullptr, w, true);
        }
        Log("Bots carry %s's gear", source->nick.c_str());
    } else if (cmd == "bots" && rest == "off") {
        RemoveBots();
    } else if (cmd == "reload") {
        Log(ReloadMode() ? "Mode reloaded" : "Mode reload failed");
    } else if (cmd == "mode") {
        script_->Unload();
        LoadEnvironment(rest);
        if (script_->Load(rest)) { cfg_.mode = rest; LoadWorldEdits(); for (auto& [id, p] : players_) if (p->ready && p->peer) SendWorldEdits(p->peer); Log("Mode switched to %s", rest.c_str()); }
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
