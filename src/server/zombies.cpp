// Freakers (docs/zombies.md, experimental: server.ini Zombies=1). The mode creates zombies; the server keeps
// where each one is and picks its owner, the nearest player within ZombieOwnRange in its virtual world. That
// player's client spawns a real freaker and runs the game's AI on it, so the zombie hunts that player;
// it reports the zombie's place ~10 times a second (ZombieStates) and its death (ZombieDied). Every other
// client nearby shows a puppet (AI stopped) that walks after those reports (ZombieMoves). A player who
// hurts a puppet sends ZombieHit: the server passes the damage to the owner, whose game applies it.
// Ownership changes only when another player stays clearly nearer for a while (kSwitchDelay).
#include "server.h"
#include "script.h"
#include <enet/enet.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace srv {
namespace {
constexpr double kSwitchDelay = 2.0;      // another player nearer by kSwitchMargin this long takes the zombie
constexpr float kSwitchMargin = 0.7f;     // ... nearer than 70 % of the owner's distance
constexpr float kRelayRange = 25000.f;    // puppets are shown to players within 250 m
constexpr double kCorpseTime = 30.0;      // a dead zombie is forgotten after this
constexpr size_t kZombiesMax = 200;

float Dist2(const Player& p, const Zombie& z) {
    const float dx = p.state.x - z.x, dy = p.state.y - z.y, dz = p.state.z - z.z; return dx * dx + dy * dy + dz * dz;
}
bool Eligible(const Player& p, const Zombie& z) {
    return p.ready && p.peer && !p.bot && p.world == z.world && !(p.state.flags & net::StateDead) && !p.spectating;
}
void PutState(net::Writer& w, const Zombie& z) {
    w.U16(z.id); w.F32(z.x); w.F32(z.y); w.F32(z.z); w.F32(z.yaw); w.F32(z.speed); w.F32(z.health);
    w.U8(z.montageSeq); w.Str(z.montage);
}
}  // namespace

const Zombie* Server::CreateZombie(const std::string& type, float x, float y, float z, float yaw, int32_t world) {
    if (!cfg_.zombies || zombies_.size() >= kZombiesMax || type.empty() || type.size() > net::kZombieTypeMax) return nullptr;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return nullptr;
    uint16_t id = nextZombie_;
    while (zombies_.count(id) || id == 0) ++id;
    nextZombie_ = uint16_t(id + 1);
    Zombie& zb = zombies_[id];
    zb.id = id; zb.type = type; zb.x = x; zb.y = y; zb.z = z; zb.yaw = yaw; zb.world = world;
    SendZombie(nullptr, zb);
    return &zb;
}

bool Server::RemoveZombie(uint16_t id) {
    if (!zombies_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(id); w.U8(uint8_t(net::ZombieOp::Remove)); Send(nullptr, w, true);
    return true;
}

void Server::SendZombie(Player* to, const Zombie& z) {
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(z.id); w.U8(uint8_t(net::ZombieOp::Add));
    w.Str(z.type); w.F32(z.x); w.F32(z.y); w.F32(z.z); w.F32(z.yaw); w.I32(z.world); w.U16(z.owner); w.F32(z.health);
    Send(to, w, true);
    if (z.dead) { net::Writer d; d.Op(net::Msg::Zombie); d.U16(z.id); d.U8(uint8_t(net::ZombieOp::Died)); d.U16(z.lastAttacker); Send(to, d, true); }
}

void Server::ZombiesPlayerGone(uint16_t id) {
    for (auto& [zid, z] : zombies_) if (z.owner == id) { z.owner = 0xFFFF; z.closer = 0xFFFF; }
    // TickZombies hands them to someone else and tells everyone.
}

void Server::TickZombies() {
    if (zombies_.empty()) return;
    const double now = Now();
    std::vector<uint16_t> forget;
    for (auto& [id, z] : zombies_) {
        if (z.dead) { if (now - z.diedAt > kCorpseTime) forget.push_back(id); continue; }
        Player* owner = z.owner != 0xFFFF ? Find(z.owner) : nullptr;
        if (owner && (!Eligible(*owner, z) || Dist2(*owner, z) > cfg_.zombieOwnRange * cfg_.zombieOwnRange)) owner = nullptr;
        Player* best = nullptr; float bestD = cfg_.zombieOwnRange * cfg_.zombieOwnRange;
        for (auto& [pid, p] : players_) if (Eligible(*p, z)) { const float d = Dist2(*p, z); if (d < bestD) { bestD = d; best = p.get(); } }
        Player* next = owner;
        if (!owner) next = best;
        else if (best && best != owner && bestD < Dist2(*owner, z) * kSwitchMargin * kSwitchMargin) {
            if (z.closer != best->id) { z.closer = best->id; z.closerSince = now; }
            else if (now - z.closerSince >= kSwitchDelay) next = best;
        } else z.closer = 0xFFFF;
        const uint16_t nextId = next ? next->id : 0xFFFF;
        if (nextId != z.owner) {
            z.owner = nextId; z.ownerSince = now; z.closer = 0xFFFF;
            net::Writer w; w.Op(net::Msg::Zombie); w.U16(z.id); w.U8(uint8_t(net::ZombieOp::Owner)); w.U16(z.owner); Send(nullptr, w, true);
        }
    }
    for (uint16_t id : forget) RemoveZombie(id);
}

void Server::OnZombieStates(Player& p, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const int n = r.U8();
    std::vector<const Zombie*> changed;
    for (int i = 0; i < n && r.ok(); ++i) {
        net::ZombieState s;
        s.id = r.U16(); s.x = r.F32(); s.y = r.F32(); s.z = r.F32(); s.yaw = r.F32(); s.speed = r.F32(); s.health = r.F32();
        s.montageSeq = r.U8(); s.montage = r.Str(net::kZombieMontageMax);
        if (!r.ok()) break;
        auto it = zombies_.find(s.id);
        if (it == zombies_.end() || it->second.owner != p.id || it->second.dead) continue;  // only its owner moves it
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.yaw) || !std::isfinite(s.health)) continue;
        Zombie& z = it->second;
        z.x = s.x; z.y = s.y; z.z = s.z; z.yaw = s.yaw; z.speed = std::clamp(s.speed, 0.f, 2000.f); z.health = s.health;
        z.montageSeq = s.montageSeq; z.montage = s.montage;
        changed.push_back(&z);
    }
    if (changed.empty()) return;
    // Relay to everyone else near enough to see the puppets.
    for (auto& [pid, other] : players_) {
        if (other.get() == &p || !other->ready || !other->peer) continue;
        net::Writer w; int count = 0;
        auto flush = [&] { if (!count) return; w.buf[1] = uint8_t(count); SendTo(other->peer, w, false); count = 0; };
        for (const Zombie* z : changed) {
            if (z->world != other->world || Dist2(*other, *z) > kRelayRange * kRelayRange) continue;
            if (!count) { w.buf.clear(); w.Op(net::Msg::ZombieMoves); w.U8(0); }
            PutState(w, *z);
            if (++count >= int(net::kZombiesPerPacket)) flush();
        }
        flush();
    }
}

void Server::OnZombieHit(Player& p, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const uint16_t id = r.U16(); const float damage = r.F32();
    if (!r.ok() || !std::isfinite(damage) || damage <= 0) return;
    auto it = zombies_.find(id);
    if (it == zombies_.end() || it->second.dead || it->second.world != p.world) return;
    Zombie& z = it->second;
    if (Dist2(p, z) > kRelayRange * kRelayRange) return;  // nobody hurts a zombie from 250 m
    z.lastAttacker = p.id;
    Player* owner = z.owner != 0xFFFF ? Find(z.owner) : nullptr;
    if (!owner || !owner->peer) return;
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(id); w.U8(uint8_t(net::ZombieOp::Damage)); w.F32(std::min(damage, cfg_.maxHitDamage > 0 ? cfg_.maxHitDamage : damage)); w.U16(p.id);
    SendTo(owner->peer, w, true);
}

void Server::OnZombieDied(Player& p, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const uint16_t id = r.U16();
    if (!r.ok()) return;
    auto it = zombies_.find(id);
    if (it == zombies_.end() || it->second.owner != p.id || it->second.dead) return;
    Zombie& z = it->second;
    // The owner's own shots are not reported (its game hurt the zombie directly): with no other attacker
    // the owner killed it.
    if (z.lastAttacker == 0xFFFF) z.lastAttacker = p.id;
    z.dead = true; z.diedAt = Now(); z.health = 0;
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(id); w.U8(uint8_t(net::ZombieOp::Died)); w.U16(z.lastAttacker); Send(nullptr, w, true);
    Log("Zombie %u (%s) died; killer %u", unsigned(id), z.type.c_str(), unsigned(z.lastAttacker));
    script_->ZombieDeath(id, Find(z.lastAttacker));
}

}  // namespace srv
