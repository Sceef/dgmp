// Voice chat (docs/API.md "Голосовой чат"). Clients send Steam Voice frames while a push-to-talk key is
// held; the server does not decode them, it picks who hears each frame and how:
//   - the mode's channels: a radio channel gets what is said with the radio key, a call channel all of it;
//     its members hear it without a position (as a radio / a phone);
//   - everyone else near the speaker in its virtual world hears it at the speaker's body.
// Limits: VoiceRate bytes a second from one player, VoiceListeners speakers heard at once by one player,
// VoiceBandwidth bytes a second of voice from the whole server.
#include "server.h"
#include "script.h"
#include "../shared/json.h"
#include <enet/enet.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace srv {
namespace {
constexpr double kTalkGap = 0.4;      // no frame for this long: the talk ended
constexpr size_t kChannelsMax = 16;   // channels one player is in
const char* KeyName(int key) { return key ? "radio" : "talk"; }
}  // namespace

const char* Server::Talking(const Player& p) {
    if (p.voiceUntil[1] && !p.voiceBlocked[1]) return "radio";
    if (p.voiceUntil[0] && !p.voiceBlocked[0]) return "talk";
    return nullptr;
}

void Server::SetVoiceRange(Player* p, float units) {
    if (!std::isfinite(units)) return;
    if (p) p->voiceRange = units < 0 ? -1.f : units;
    else voiceRange_ = std::max(0.f, units);
}

void Server::SetVoiceMuted(Player& p, bool muted) {
    if (p.voiceMuted == muted) return;
    p.voiceMuted = muted;
    SendVoiceSetup(p);
}

bool Server::JoinVoiceChannel(Player& p, const std::string& name, VoiceJoin how) {
    if (name.empty() || name.size() > net::kVoiceNameMax) return false;
    if (!p.voiceChannels.count(name) && p.voiceChannels.size() >= kChannelsMax) return false;
    p.voiceChannels[name] = how;
    SendVoiceSetup(p);
    return true;
}

void Server::LeaveVoiceChannel(Player& p, const std::string& name) {
    if (name.empty()) p.voiceChannels.clear(); else p.voiceChannels.erase(name);
    SendVoiceSetup(p);
}

void Server::SetVoiceEnabled(bool on) {
    if (voiceOn_ == on) return;
    voiceOn_ = on;
    for (auto& [id, p] : players_) SendVoiceSetup(*p);
}

// The Tab panel: everyone on the server with its ping, whether the server knows its Steam account
// (key: "s:<SteamID>" or "n:<nick>", what the client keeps its own mutes under) and the mode's cells.
void Server::SendPlayerList(Player& to) {
    if (!to.peer) return;
    json::Value list = json::Value::Object();
    list.set("voice", voiceOn_).set("max", cfg_.maxPlayers);
    json::Value columns = json::Value::Array();
    for (auto& c : scoreColumns_) columns.push(c);
    list.set("columns", std::move(columns));
    json::Value rows = json::Value::Array();
    for (auto& [id, p] : players_) {
        if (!p->ready) continue;
        json::Value row = json::Value::Object();
        row.set("id", unsigned(id)).set("name", p->nick).set("ping", p->peer ? unsigned(p->peer->roundTripTime) : 0u)
           .set("key", p->steamId ? "s:" + std::to_string(p->steamId) : "n:" + p->nick).set("admin", p->admin).set("bot", p->bot);
        json::Value cells = json::Value::Array();
        for (auto& c : p->scoreCells) cells.push(c);
        row.set("cells", std::move(cells));
        rows.push(std::move(row));
    }
    list.set("rows", std::move(rows));
    net::Writer w; w.Op(net::Msg::PlayerList); w.Str(list.dump());
    SendTo(to.peer, w, true);
}

// The players it muted for itself: their voices are no longer sent to it (no traffic, no VoiceListeners
// place taken). The mode hears each change (voiceIgnore), e.g. to count complaints about a spammer.
void Server::OnVoiceIgnore(Player& p, net::Reader& r) {
    const int n = r.U8();
    std::set<uint16_t> now;
    for (int i = 0; i < n && r.ok(); ++i) { const uint16_t id = r.U16(); if (id != p.id) now.insert(id); }
    if (!r.ok()) return;
    const uint16_t self = p.id;
    std::vector<std::pair<uint16_t, bool>> changes;
    for (auto id : now) if (!p.voiceIgnored.count(id)) changes.push_back({id, true});
    for (auto id : p.voiceIgnored) if (!now.count(id)) changes.push_back({id, false});
    p.voiceIgnored = std::move(now);
    for (auto id : p.voiceIgnored) p.voiceHeard.erase(id);
    for (auto& [id, on] : changes) {
        Player* me = Find(self); Player* target = Find(id);
        if (!me) return;  // a handler kicked it
        if (target) script_->VoiceIgnore(*me, *target, on);
    }
}

void Server::SendVoiceSetup(Player& p) {
    if (!p.peer || !p.ready) return;
    uint8_t flags = voiceOn_ ? net::VoiceOn : 0;
    if (p.voiceMuted) flags |= net::VoiceMuted;
    std::string radio, call;
    for (auto& [name, how] : p.voiceChannels) {
        if (how == VoiceJoin::Radio && radio.empty()) { radio = name; flags |= net::VoiceRadio; }
        if (how == VoiceJoin::Call && call.empty()) { call = name; flags |= net::VoiceCall; }
    }
    net::Writer w; w.Op(net::Msg::VoiceSetup); w.U8(flags); w.Str(radio); w.Str(call);
    SendTo(p.peer, w, true);
}

void Server::OnVoice(Player& from, const uint8_t* payload, size_t size) {
    if (!voiceOn_ || size < 4 || size - 3 > net::kVoiceFrameMax) return;
    const int key = payload[0];
    if (key > 1 || payload[1] != uint8_t(net::VoiceCodec::Steam)) return;
    const double now = Now();
    // VoiceRate: a bucket of one second's bytes (a burst after silence fits).
    const double rate = std::max(1, cfg_.voiceRate);
    from.voiceTokens = std::min(rate, from.voiceTokens + (now - from.voiceTokensAt) * rate);
    from.voiceTokensAt = now;
    if (from.voiceTokens < double(size)) { ++voiceDropped_; return; }
    from.voiceTokens -= double(size);
    from.voiceBytesIn += size;
    if (from.voiceMuted) return;
    if (!from.voiceUntil[key]) {
        // A new talk: the mode may refuse it (voice event returning false) until it ends.
        const uint16_t id = from.id;
        from.voiceUntil[key] = now + kTalkGap;
        const bool allowed = script_->Voice(from, true, KeyName(key));
        if (Find(id) != &from) return;  // the handler kicked it
        from.voiceBlocked[key] = !allowed;
    }
    from.voiceUntil[key] = now + kTalkGap;
    if (from.voiceBlocked[key]) return;

    // Who hears it and how: the channels first (a member near the speaker hears the radio, not both).
    std::vector<std::pair<Player*, net::VoiceRoute>> to;
    auto heard = [&](const Player* l) { for (auto& t : to) if (t.first == l) return true; return false; };
    for (auto& [name, how] : from.voiceChannels) {
        if (how == VoiceJoin::Listen || (how == VoiceJoin::Radio && key != int(net::VoiceKey::Radio))) continue;
        const auto route = how == VoiceJoin::Call ? net::VoiceRoute::Call : net::VoiceRoute::Radio;
        for (auto& [id, l] : players_)
            if (l.get() != &from && l->ready && l->peer && l->voiceChannels.count(name) && !heard(l.get()) && !l->voiceIgnored.count(from.id)) to.push_back({l.get(), route});
    }
    const float range = VoiceRange(from);
    if (range > 0) {
        const float range2 = range * range;
        for (auto& [id, l] : players_) {
            if (l.get() == &from || !l->ready || !l->peer || Apart(*l, from) || heard(l.get()) || l->voiceIgnored.count(from.id)) continue;
            const float dx = l->state.x - from.state.x, dy = l->state.y - from.state.y, dz = l->state.z - from.state.z;
            if (dx * dx + dy * dy + dz * dz <= range2) to.push_back({l.get(), net::VoiceRoute::Near});
        }
    }
    if (to.empty()) return;

    const size_t frame = size - 3;
    const uint8_t gain = std::min(payload[2], net::kVoiceGainMax);
    const size_t packetSize = 1 + 2 + 1 + 1 + 1 + 4 + frame;
    if (cfg_.voiceBandwidth > 0) {
        const double cap = cfg_.voiceBandwidth;
        voiceBudget_ = std::min(cap, voiceBudget_ + (now - voiceBudgetAt_) * cap);
        voiceBudgetAt_ = now;
    }
    for (auto& [l, route] : to) {
        // VoiceListeners: a speaker it does not hear yet waits until one of those it hears falls silent.
        auto it = l->voiceHeard.find(from.id);
        if (it == l->voiceHeard.end() && int(l->voiceHeard.size()) >= cfg_.voiceListeners) { ++voiceDropped_; continue; }
        if (cfg_.voiceBandwidth > 0) {
            if (voiceBudget_ < double(packetSize)) { ++voiceDropped_; continue; }
            voiceBudget_ -= double(packetSize);
        }
        l->voiceHeard[from.id] = now;
        net::Writer w; w.Op(net::Msg::PlayerVoice); w.U16(from.id); w.U8(uint8_t(route)); w.U8(payload[1]); w.U8(gain); w.F32(range); w.Raw(payload + 3, frame);
        enet_peer_send(l->peer, net::kChanVoice, enet_packet_create(w.buf.data(), w.buf.size(), 0));
        voiceBytesOut_ += w.buf.size();
    }
}

void Server::TickVoice() {
    const double now = Now();
    struct Ended { uint16_t id; int key; };
    std::vector<Ended> ended;
    for (auto& [id, p] : players_) {
        for (int k = 0; k < 2; ++k) {
            if (!p->voiceUntil[k] || now < p->voiceUntil[k]) continue;
            p->voiceUntil[k] = 0;
            if (!p->voiceBlocked[k]) ended.push_back({id, k});
            p->voiceBlocked[k] = false;
        }
        for (auto it = p->voiceHeard.begin(); it != p->voiceHeard.end();) it = now - it->second > kTalkGap ? p->voiceHeard.erase(it) : std::next(it);
    }
    // After the loop: a handler may kick.
    for (auto& e : ended) if (Player* p = Find(e.id)) script_->Voice(*p, false, KeyName(e.key));
}

void Server::VoiceStats() {
    Log("Voice %s, range %.0f m, VoiceRate %d B/s, VoiceListeners %d, VoiceBandwidth %d B/s; sent %llu KB, dropped %llu frames",
        voiceOn_ ? "on" : "off", voiceRange_ / 100.f, cfg_.voiceRate, cfg_.voiceListeners, cfg_.voiceBandwidth,
        static_cast<unsigned long long>(voiceBytesOut_ / 1024), static_cast<unsigned long long>(voiceDropped_));
    for (auto& [id, p] : players_) {
        if (!p->ready || p->bot) continue;
        std::string channels;
        for (auto& [name, how] : p->voiceChannels) channels += (channels.empty() ? "" : ", ") + name + (how == VoiceJoin::Radio ? " (radio)" : how == VoiceJoin::Call ? " (call)" : " (listen)");
        const char* talking = Talking(*p);
        Log("  %3u  %-24s %s%-6s range %.0f m, received %llu KB, hears %zu%s%s", p->id, p->nick.c_str(), p->voiceMuted ? "muted " : "",
            talking ? talking : "", VoiceRange(*p) / 100.f, static_cast<unsigned long long>(p->voiceBytesIn / 1024), p->voiceHeard.size(),
            channels.empty() ? "" : ", channels: ", channels.c_str());
    }
}

}  // namespace srv
