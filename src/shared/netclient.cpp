#include "netclient.h"
#include <windows.h>
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace net {
namespace {
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string Ip(const ENetAddress& a) { char ip[64] = {}; enet_address_get_host_ip(&a, ip, sizeof(ip)); return ip; }

// Sends query datagrams to every address, collects replies until timeout.
std::vector<QueryResult> RunQuery(const std::vector<ENetAddress>& addresses, const std::vector<std::string>& names, bool broadcast, int timeoutMs) {
    std::vector<QueryResult> results;
    if (!InitNetwork()) return results;
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) return results;
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    if (broadcast) enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress any{}; any.host = ENET_HOST_ANY; any.port = 0; enet_socket_bind(s, &any);

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const uint32_t token = rng();
    const auto packet = BuildQuery(token);
    const double start = Seconds();
    if (!broadcast) results.resize(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
        ENetBuffer b{}; b.data = const_cast<uint8_t*>(packet.data()); b.dataLength = packet.size();
        enet_socket_send(s, &addresses[i], &b, 1);
        if (!broadcast) { results[i].host = names[i]; results[i].port = addresses[i].port; }
    }
    size_t answered = 0;
    while (Seconds() - start < timeoutMs / 1000.0 && (broadcast || answered < addresses.size())) {
        enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(s, &cond, 20) != 0 || !(cond & ENET_SOCKET_WAIT_RECEIVE)) continue;
        uint8_t buf[1500]; ENetAddress from{};
        ENetBuffer b{}; b.data = buf; b.dataLength = sizeof(buf);
        const int n = enet_socket_receive(s, &from, &b, 1);
        if (n <= 0) continue;
        uint32_t tok = 0; ServerInfo info;
        if (!ParseReply(buf, static_cast<size_t>(n), tok, info) || tok != token) continue;
        const int ping = static_cast<int>((Seconds() - start) * 1000);
        if (broadcast) {
            bool dup = false;
            for (auto& r : results) dup |= r.host == Ip(from) && r.port == from.port;
            if (!dup) results.push_back({Ip(from), from.port, ping, info});
        } else {
            for (size_t i = 0; i < addresses.size(); ++i)
                if (results[i].pingMs < 0 && addresses[i].host == from.host && addresses[i].port == from.port) { results[i].pingMs = ping; results[i].info = info; ++answered; break; }
        }
    }
    enet_socket_destroy(s);
    return results;
}
}  // namespace

std::vector<std::pair<std::string, uint16_t>> QueryMaster(const std::string& master, int timeoutMs, std::string& error) {
    std::vector<std::pair<std::string, uint16_t>> out;
    std::string host; uint16_t port = kMasterPort;
    if (!ParseAddress(master, host, port)) { error = "no master server set"; return out; }
    if (master.find(':') == std::string::npos) port = kMasterPort;
    if (!InitNetwork()) { error = "network"; return out; }
    ENetAddress a{}; a.port = port;
    if (enet_address_set_host(&a, host.c_str()) != 0) { error = "cannot resolve " + host; return out; }
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) { error = "socket"; return out; }
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress any{}; any.host = ENET_HOST_ANY; enet_socket_bind(s, &any);
    std::mt19937 rng(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const uint32_t token = rng();
    Writer w; w.Raw(kListMagic, 4); w.U32(token); w.U16(kProtocolVersion);
    std::vector<bool> got; int parts = -1, have = 0;
    for (int attempt = 0; attempt < 2 && parts != have; ++attempt) {  // a lost datagram: ask once more
        ENetBuffer b{}; b.data = w.buf.data(); b.dataLength = w.buf.size();
        enet_socket_send(s, &a, &b, 1);
        const double until = Seconds() + timeoutMs / 2000.0;
        while (Seconds() < until && (parts < 0 || have < parts)) {
            enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
            if (enet_socket_wait(s, &cond, 20) != 0 || !(cond & ENET_SOCKET_WAIT_RECEIVE)) continue;
            uint8_t buf[1500]; ENetAddress from{};
            ENetBuffer rb{}; rb.data = buf; rb.dataLength = sizeof(buf);
            const int n = enet_socket_receive(s, &from, &rb, 1);
            if (n < 14 || std::memcmp(buf, kListReplyMagic, 4) != 0) continue;
            Reader r(buf + 4, size_t(n) - 4);
            const uint32_t tok = r.U32(); const uint16_t part = r.U16(), total = r.U16(), count = r.U16();
            if (!r.ok() || tok != token || !total || part >= total) continue;
            if (parts < 0) { parts = total; got.assign(total, false); }
            if (got[part]) continue;
            got[part] = true; ++have;
            for (int i = 0; i < count && r.ok(); ++i) {
                ENetAddress e{}; e.host = r.U32(); e.port = r.U16();
                if (r.ok()) out.push_back({Ip(e), e.port});
            }
        }
    }

    enet_socket_destroy(s);
    if (parts < 0) error = "the master server did not answer";
    return out;
}

bool InitNetwork() {
    static int state = 0;  // 0 untried, 1 ok, -1 failed
    if (state == 0) state = enet_initialize() == 0 ? 1 : -1;
    return state == 1;
}

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port) {
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '/')) t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    if (t.empty()) return false;
    port = kDefaultPort;
    const auto colon = t.rfind(':');
    if (colon != std::string::npos && t.find(':') == colon) {
        const int p = std::atoi(t.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p); t.resize(colon);
    }
    host = t;
    return !host.empty();
}

std::vector<QueryResult> QueryServers(const std::vector<std::pair<std::string, uint16_t>>& targets, int timeoutMs) {
    std::vector<ENetAddress> addresses; std::vector<std::string> names;
    std::vector<QueryResult> unresolved;
    if (!InitNetwork()) return unresolved;
    for (auto& [host, port] : targets) {
        ENetAddress a{}; a.port = port;
        if (enet_address_set_host(&a, host.c_str()) != 0) { unresolved.push_back({host, port, -1, {}}); continue; }
        addresses.push_back(a); names.push_back(host);
    }
    auto results = RunQuery(addresses, names, false, timeoutMs);
    results.insert(results.end(), unresolved.begin(), unresolved.end());
    return results;
}

std::vector<QueryResult> ScanLan(int timeoutMs) {
    std::vector<ENetAddress> addresses; std::vector<std::string> names;
    for (int i = 0; i < kLanPortSpan; ++i) {
        ENetAddress a{}; a.host = ENET_HOST_BROADCAST; a.port = static_cast<uint16_t>(kDefaultPort + i);
        addresses.push_back(a); names.push_back("");
        // Loopback too: a server on this PC does not always see its own broadcast.
        ENetAddress l{}; enet_address_set_host_ip(&l, "127.0.0.1"); l.port = a.port;
        addresses.push_back(l); names.push_back("");
    }
    auto results = RunQuery(addresses, names, true, timeoutMs);
    // A local server answers both the broadcast and loopback; prefer the LAN address.
    std::vector<QueryResult> out;
    for (auto& r : results) {
        bool dup = false;
        for (auto& o : out) dup |= o.port == r.port && o.info.name == r.info.name && (o.host == "127.0.0.1" || r.host == "127.0.0.1");
        if (!dup) out.push_back(r);
        else if (r.host != "127.0.0.1") for (auto& o : out) if (o.port == r.port && o.info.name == r.info.name) o.host = r.host;
    }
    return out;
}

// ---------------- Client ----------------
Client::~Client() { Disconnect(); }

bool Client::Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password) {
    Disconnect();
    if (!InitNetwork()) { Fail("Network initialisation failed"); return false; }
    host_ = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (!host_) { Fail("Cannot create network socket"); return false; }
    ENetAddress a{}; a.port = port;
    if (enet_address_set_host(&a, host.c_str()) != 0) { Fail("Cannot resolve " + host); return false; }
    peer_ = enet_host_connect(host_, &a, kChannels, 0);
    if (!peer_) { Fail("Cannot start connection"); return false; }
    nick_ = nick; password_ = password; rejectReason_.clear();
    status_ = Status::Connecting; connectStarted_ = Seconds(); localId_ = 0xFFFF;
    return true;
}

void Client::Disconnect() {
    if (peer_ && (status_ == Status::Connected || status_ == Status::Handshake)) {
        enet_peer_disconnect(peer_, 1);
        // Give the goodbye a moment to leave; the server times us out otherwise.
        ENetEvent e; const double until = Seconds() + 0.3;
        while (Seconds() < until && enet_host_service(host_, &e, 10) >= 0) {
            if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
            if (e.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
    }
    if (host_) enet_host_destroy(host_);
    host_ = nullptr; peer_ = nullptr; players_.clear();
    if (status_ != Status::Failed) status_ = Status::Idle;
}

void Client::Fail(const std::string& reason) {
    status_ = Status::Failed;
    if (on.disconnected) on.disconnected(reason);
}

int Client::pingMs() const { return peer_ ? static_cast<int>(peer_->roundTripTime) : -1; }

void Client::Service(double now) {
    if (!host_) return;
    ENetEvent e;
    while (host_ && enet_host_service(host_, &e, 0) > 0) {
        switch (e.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            status_ = Status::Handshake;
            Writer w; w.Op(Msg::Hello); w.U16(kProtocolVersion); w.Str(nick_); w.Str(password_);
            char lang[16] = "en"; DWORD n = GetEnvironmentVariableA("DGMP_LANG", lang, sizeof(lang)); if (!n || n >= sizeof(lang)) strcpy_s(lang, "en");
            w.Str(lang);
            w.U32(uint32_t(steamId_)); w.U32(uint32_t(steamId_ >> 32)); w.Str(ticket_.size() <= kTicketMax ? ticket_ : std::string());
            enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: OnPacket(e.packet->data, e.packet->dataLength, now); enet_packet_destroy(e.packet); break;
        case ENET_EVENT_TYPE_DISCONNECT: {
            const auto reason = !rejectReason_.empty() ? rejectReason_ : status_ == Status::Connecting ? "Server did not answer" : "Connection lost";
            enet_host_destroy(host_); host_ = nullptr; peer_ = nullptr; players_.clear();
            Fail(reason);
            return;
        }
        default: break;
        }
    }
    if (status_ == Status::Connecting && Seconds() - connectStarted_ > 10) { Disconnect(); Fail("Server did not answer"); }
}

void Client::OnPacket(const uint8_t* data, size_t size, double now) {
    if (size < 1) return;
    Reader r(data + 1, size - 1);
    switch (static_cast<Msg>(data[0])) {
    case Msg::Welcome: {
        localId_ = r.U16(); const auto name = r.Str(), mode = r.Str(); r.U8();
        if (!r.ok()) return;
        status_ = Status::Connected;
        if (on.welcome) on.welcome(name, mode);
        break;
    }
    case Msg::Reject: case Msg::Kick: rejectReason_ = r.Str(); break;
    case Msg::PlayerJoin: {
        RemotePlayer p; p.id = r.U16(); p.nick = r.Str(96);
        if (!r.ok() || p.id == localId_) return;
        players_[p.id] = p;
        if (on.playerJoin) on.playerJoin(p);
        break;
    }
    case Msg::PlayerQuit: {
        const auto id = r.U16(); const auto reason = r.U8();
        if (!r.ok()) return;
        players_.erase(id);
        if (on.playerQuit) on.playerQuit(id, reason);
        break;
    }
    case Msg::States: {
        const auto count = r.U8();
        for (int i = 0; i < count && r.ok(); ++i) {
            const auto id = r.U16(); const auto s = r.State();
            if (!r.ok()) break;
            if (auto it = players_.find(id); it != players_.end()) { it->second.state = s; it->second.stateTime = now; if (on.state) on.state(id, s); }
        }
        break;
    }
    case Msg::PlayerPose: {
        const auto id = r.U16(); size_t n = 0; const uint8_t* rest = r.Rest(n);
        if (r.ok() && on.pose && players_.count(id)) on.pose(id, rest, n);
        break;
    }
    case Msg::Item: {
        const auto action = static_cast<ItemAction>(r.U8()); const auto key = r.Str(kItemKeyMax); const auto amount = r.I32();
        if (r.ok() && on.item) on.item(action, key, amount);
        break;
    }
    case Msg::Bike: {
        const auto action = static_cast<BikeAction>(r.U8()); const float x = r.F32(), y = r.F32(), z = r.F32(), yaw = r.F32();
        if (r.ok() && on.bike) on.bike(action, x, y, z, yaw);
        break;
    }
    case Msg::PlayerLook: { const auto id = r.U16(); const auto path = r.Str(kLookPathMax); if (r.ok() && on.look) on.look(id, path); break; }
    case Msg::Objective: {
        const auto state = r.U8(), category = r.U8(), style = r.U8(); const auto title = r.Str(512), text = r.Str(2048); const int xp = r.left() >= 4 ? r.I32() : 0;
        if (r.ok() && on.objective) on.objective(state, category, style, title, text, xp);
        break;
    }
    case Msg::ObjectiveMeter: {
        const auto show = r.U8(); const auto title = r.Str(512); const float percent = r.F32();
        if (r.ok() && on.objectiveMeter) on.objectiveMeter(show != 0, title, percent);
        break;
    }
    case Msg::WorldEdits: {
        const auto part = r.U16(), parts = r.U16(); auto chunk = r.Str();
        if (!r.ok()) break;
        if (part == 0) worldEditsBuffer_.clear();
        worldEditsBuffer_ += chunk;
        if (part + 1 >= parts && on.worldEdits) on.worldEdits(worldEditsBuffer_);
        break;
    }
    case Msg::Corpse: { const auto id = r.U16(); if (r.ok() && on.corpse) on.corpse(id); break; }
    case Msg::Spectate: {
        const bool spectating = r.U8() != 0; const int n = r.U8();
        std::vector<uint16_t> only;
        for (int i = 0; i < n && r.ok(); ++i) only.push_back(r.U16());
        if (r.ok() && on.spectate) on.spectate(spectating, only);
        break;
    }
    case Msg::Customize: {
        const bool cancel = r.U8() != 0; const auto title = r.Str(256), families = r.Str(256), current = r.Str(kLookPathMax);
        std::vector<std::pair<std::string, std::string>> choices;
        const int n = r.U8();
        for (int i = 0; i < n && r.ok(); ++i) { auto name = r.Str(256); auto look = r.Str(kLookPathMax); choices.emplace_back(std::move(name), std::move(look)); }
        float place[4] = {};
        const bool hasPlace = r.U8() != 0;
        if (hasPlace) for (float& f : place) f = r.F32();
        if (r.ok() && on.customize) on.customize(cancel, title, families, current, choices, hasPlace ? place : nullptr);
        break;
    }
    case Msg::Dialog: {
        Events::DialogInfo d;
        d.id = r.U16(); d.style = static_cast<DialogStyle>(r.U8());
        if (d.style != DialogStyle::Close) {
            d.title = r.Str(256); d.text = r.Str(kDialogTextMax); d.button1 = r.Str(64); d.button2 = r.Str(64); d.defaultText = r.Str(256); d.headers = r.Str(512);
            const int n = r.U8();
            for (int i = 0; i < n && r.ok(); ++i) d.items.push_back(r.Str(512));
        }
        if (r.ok() && on.dialog) on.dialog(d);
        break;
    }
    case Msg::HudElement: {
        const auto id = r.Str(64), json = r.Str(4096);
        if (r.ok() && on.hud) on.hud(id, json);
        break;
    }
    case Msg::Interaction: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::InteractionInfo info;
            info.x = r.F32(); info.y = r.F32(); info.z = r.F32(); info.radius = r.F32(); info.key = r.U8(); info.text = r.Str(256); info.world = r.I32();
            if (r.ok() && on.interaction) on.interaction(id, &info);
        } else if (r.ok() && on.interaction) on.interaction(id, nullptr);
        break;
    }
    case Msg::VehicleRoute: {
        const uint16_t id = r.U16(); const float speed = r.F32(), elapsed = r.F32(); const bool loop = r.U8() != 0;
        const uint16_t n = r.U16();
        std::vector<float> points;
        for (uint16_t i = 0; i < n && i < 512 && r.ok(); ++i) { points.push_back(r.F32()); points.push_back(r.F32()); points.push_back(r.F32()); }
        if (r.ok() && on.vehicleRoute) on.vehicleRoute(id, points, speed, elapsed, loop);
        break;
    }
    case Msg::Vehicle: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::VehicleInfo v;
            v.state = r.Vehicle(); v.world = r.I32(); v.driver = r.U16(); v.locked = r.U8() != 0; v.yours = r.U8() != 0; v.marked = r.U8() != 0; v.color = r.U32(); v.icon = r.Str(64); v.model = r.Str(32); v.npc = r.U8() != 0; v.npcLook = r.Str(net::kLookPathMax); v.npcName = r.Str(64); v.passenger = r.U16(); v.riders = r.U8();
            if (r.ok() && on.vehicle) on.vehicle(id, &v);
        } else if (r.ok() && on.vehicle) on.vehicle(id, nullptr);
        break;
    }
    case Msg::VehicleAllow: { const uint16_t id = r.U16(); const int seat = r.U8(); if (r.ok() && on.vehicleAllow) on.vehicleAllow(id, seat); break; }
    case Msg::VehicleEject: { const uint16_t id = r.U16(); if (r.ok() && on.vehicleEject) on.vehicleEject(id); break; }
    case Msg::ImageList: {
        std::vector<std::pair<std::string, std::string>> list;
        const int n = r.U16();
        for (int i = 0; i < n && r.ok(); ++i) { auto name = r.Str(128); auto hash = r.Str(64); list.emplace_back(std::move(name), std::move(hash)); }
        if (r.ok() && on.imageList) on.imageList(list);
        break;
    }
    case Msg::ImageData: {
        const auto name = r.Str(128); const int part = r.U16(), parts = r.U16(); const auto bytes = r.Str();
        if (r.ok() && on.imageData) on.imageData(name, part, parts, bytes);
        break;
    }
    case Msg::Icon: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::IconInfo i;
            i.x = r.F32(); i.y = r.F32(); i.z = r.F32(); i.size = r.F32(); i.distance = r.F32(); i.world = r.I32(); i.color = r.U32();
            i.image = r.Str(128); i.text = r.Str(256); i.throughWalls = r.U8() != 0;
            if (r.ok() && on.icon) on.icon(id, &i);
        } else if (r.ok() && on.icon) on.icon(id, nullptr);
        break;
    }
    case Msg::SetWorld: { const int world = r.I32(); if (r.ok() && on.setWorld) on.setWorld(world); break; }
    case Msg::Actor: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::ActorInfo a;
            a.x = r.F32(); a.y = r.F32(); a.z = r.F32(); a.yaw = r.F32(); a.world = r.I32();
            a.look = r.Str(kLookPathMax); a.anim = r.Str(512); a.name = r.Str(128); a.solid = r.U8() != 0;
            if (r.ok() && on.actor) on.actor(id, &a);
        } else if (r.ok() && on.actor) on.actor(id, nullptr);
        break;
    }
    case Msg::Carry: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::CarryInfo c;
            c.model = r.Str(64); c.x = r.F32(); c.y = r.F32(); c.z = r.F32(); c.yaw = r.F32(); c.world = r.I32(); c.carrier = r.U16(); c.roll = r.U8();
            if (r.ok() && on.carry) on.carry(id, &c);
        } else if (r.ok() && on.carry) on.carry(id, nullptr);
        break;
    }
    case Msg::Blip: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::BlipInfo b;
            b.x = r.F32(); b.y = r.F32(); b.z = r.F32(); b.icon = r.Str(256); b.color = r.U32(); b.scale = r.F32();
            const uint8_t flags = r.U8(); b.world = r.I32();
            b.map = flags & BlipMap; b.radar = flags & BlipRadar; b.edge = flags & BlipEdge;
            if (r.ok() && on.blip) on.blip(id, &b);
        } else if (r.ok() && on.blip) on.blip(id, nullptr);
        break;
    }
    case Msg::Object: {
        const uint16_t id = r.U16(); const uint8_t op = r.U8();
        if (op == 1) {
            Events::ObjectInfo o;
            o.asset = r.Str(512); o.x = r.F32(); o.y = r.F32(); o.z = r.F32(); o.pitch = r.F32(); o.yaw = r.F32(); o.roll = r.F32(); o.scale = r.F32();
            o.world = r.I32(); o.collision = r.U8() != 0;
            if (r.ok() && on.object) on.object(id, &o);
        } else if (op == 3) {
            float to[6]; for (auto& v : to) v = r.F32();
            const float seconds = r.F32(), elapsed = r.F32();
            if (r.ok() && on.objectMove) on.objectMove(id, to, seconds, elapsed);
        } else if (r.ok() && on.object) on.object(id, nullptr);
        break;
    }
    case Msg::WatchKeys: {
        std::vector<uint8_t> keys;
        const int n = r.U8();
        for (int i = 0; i < n && r.ok(); ++i) keys.push_back(r.U8());
        if (r.ok() && on.watchKeys) on.watchKeys(keys);
        break;
    }
    case Msg::Freeze: { const bool frozen = r.U8() != 0; if (r.ok() && on.freeze) on.freeze(frozen); break; }
    case Msg::PlayAnim: { const auto path = r.Str(512); const uint8_t flags = r.U8(); const float rate = r.F32(); if (r.ok() && on.playAnim) on.playAnim(path, flags, rate); break; }
    case Msg::SetHealth: { const float percent = r.F32(); if (r.ok() && std::isfinite(percent) && on.setHealth) on.setHealth(percent); break; }
    case Msg::Damage: { const auto from = r.U16(); const float amount = r.F32(); const auto zone = r.U8(); if (r.ok() && on.damage) on.damage(from, amount, HitZone(zone)); break; }
    case Msg::WorldItem: {
        const auto id = r.U16(); const auto op = r.U8();
        if (!r.ok() || !on.worldItem) break;
        if (op == 2) { on.worldItem(id, nullptr, 0, 0, 0, 0, 0); break; }
        const auto key = r.Str(kItemKeyMax); const int count = r.I32(); const float x = r.F32(), y = r.F32(), z = r.F32();
        const float yaw = r.left() >= 4 ? r.F32() : 0.f;
        if (r.ok()) on.worldItem(id, &key, count, x, y, z, yaw);
        break;
    }
    case Msg::Marker: {
        const auto id = r.U16(); const auto op = r.U8();
        if (!r.ok() || !on.marker) break;
        if (op == 2) { on.marker(id, nullptr); break; }
        Events::MarkerInfo m{}; m.x = r.F32(); m.y = r.F32(); m.z = r.F32(); m.category = r.U8(); m.style = r.U8(); m.route = r.U8() != 0; m.icon = r.Str(256); m.title = r.Str(512); m.description = r.Str(2048);
        if (r.ok()) on.marker(id, &m);
        break;
    }
    case Msg::Settings: { const auto tags = static_cast<NametagMode>(r.U8()); const uint8_t coll = r.left() ? r.U8() : 0; if (r.ok() && on.settings) on.settings(tags, coll); break; }
    case Msg::PlayerGear: { const auto id = r.U16(); size_t n = 0; const uint8_t* rest = r.Rest(n); if (r.ok() && on.gear) on.gear(id, rest, n); break; }
    case Msg::Message: { const auto c = r.U32(); const auto t = r.Str(); if (r.ok() && on.message) on.message(c, t); break; }
    case Msg::SetTime: { const auto h = r.F32(); const auto p = r.U8(); if (r.ok() && on.setTime) on.setTime(h, p != 0); break; }
    case Msg::SetWeather: { const auto w = r.I32(); if (r.ok() && on.setWeather) on.setWeather(w); break; }
    case Msg::Teleport: {
        const float x = r.F32(), y = r.F32(), z = r.F32(), yaw = r.F32();
        if (r.ok() && on.teleport) on.teleport(x, y, z, yaw);
        break;
    }
    case Msg::Hint: {
        const auto title = r.Str(), text = r.Str(), image = r.Str(); const auto pause = r.U8(); const float seconds = r.left() >= 4 ? r.F32() : 0.f;
        if (r.ok() && on.hint) on.hint(title, text, image, pause != 0, seconds);
        break;
    }
    default: break;
    }
}

void Client::SendState(const PlayerState& s) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::State); w.State(s);
    enet_peer_send(peer_, kChanState, enet_packet_create(w.buf.data(), w.buf.size(), 0));
}

void Client::SendPose(const std::vector<uint8_t>& payload) {
    if (status_ != Status::Connected || payload.empty()) return;
    Writer w; w.Op(Msg::Pose); w.Raw(payload.data(), payload.size());
    enet_peer_send(peer_, kChanPose, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
}

void Client::SendInventory(const std::vector<std::pair<std::string, int>>& items) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::Inventory); w.U16(static_cast<uint16_t>(std::min<size_t>(items.size(), 2000)));
    for (size_t i = 0; i < items.size() && i < 2000; ++i) { w.Str(items[i].first); w.I32(items[i].second); }
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}

void Client::SendHit(uint16_t victim, float damage, HitZone zone, const std::string& weapon) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::Hit); w.U16(victim); w.F32(damage); w.U8(uint8_t(zone)); w.Str(weapon.substr(0, kItemKeyMax));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendGameEvent(GameEventType type, const std::string& cls, const std::string& detail, const std::string& id, float x, float y, float z) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::GameEvent); w.U8(uint8_t(type)); w.Str(cls.substr(0, 128)); w.Str(detail.substr(0, kItemKeyMax)); w.Str(id.substr(0, 128));
    w.F32(x); w.F32(y); w.F32(z);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}

void Client::SendDialogResponse(uint16_t id, bool ok, int item, const std::string& text) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::DialogResponse); w.U16(id); w.U8(ok); w.Put<int16_t>(int16_t(item)); w.Str(text.substr(0, 256));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendUse(uint16_t interaction) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::Use); w.U16(interaction);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendVehicle(Msg op, uint16_t id, const VehicleState* state) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(op); w.U16(id);
    if (state) w.Vehicle(*state);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendCarry(uint16_t id, CarryOp op, float x, float y, float z, float yaw) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::CarryAction); w.U16(id); w.U8(uint8_t(op)); w.F32(x); w.F32(y); w.F32(z); w.F32(yaw);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendImageRequest(const std::string& name) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::ImageRequest); w.Str(name.substr(0, 128));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendAnimEnd(uint8_t reason) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::AnimEnd); w.U8(reason);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendKey(uint8_t virtualKey) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::Key); w.U8(virtualKey);
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendCustomizeDone(bool ok, const std::string& skin) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::CustomizeDone); w.U8(ok); w.Str(skin.substr(0, kLookPathMax));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendGear(const std::vector<uint8_t>& gear) {
    if (status_ != Status::Connected || gear.size() > kGearMax) return;
    Writer w; w.Op(Msg::Gear); w.Raw(gear.data(), gear.size());
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}
void Client::SendLook(const std::string& lookPath) {
    if (status_ != Status::Connected) return;
    Writer w; w.Op(Msg::Look); w.Str(lookPath.substr(0, kLookPathMax));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}

void Client::SendChat(const std::string& text) {
    if (status_ != Status::Connected || text.empty()) return;
    Writer w; w.Op(Msg::Chat); w.Str(text.substr(0, kChatMax));
    enet_peer_send(peer_, kChanReliable, enet_packet_create(w.buf.data(), w.buf.size(), ENET_PACKET_FLAG_RELIABLE));
}

}  // namespace net
