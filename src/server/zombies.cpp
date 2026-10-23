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
#include <random>
#include <vector>

namespace srv {
namespace {
constexpr double kSwitchDelay = 2.0;      // another player nearer by kSwitchMargin this long takes the zombie
constexpr float kSwitchMargin = 0.7f;     // ... nearer than 70 % of the owner's distance
constexpr float kRelayRange = 25000.f;    // puppets are shown to players within 250 m
constexpr double kCorpseTime = 30.0;      // a dead zombie is forgotten after this
constexpr size_t kZombiesMax = 200;
constexpr float kAttackReach = 1200.f;   // a zombie hurts players within 12 m of where its owner last said it is
constexpr float kAttackMax = 250.f;      // one zombie hit on a player is never more
constexpr double kZoneSleep = 30.0;       // a zone nobody has been near this long loses its zombies
constexpr int kZoneSpawnsPerTick = 2;     // a waking zone fills up gradually (0.5 s ticks)
constexpr float kSpawnClear = 2000.f;     // new zone zombies appear at least 20 m from every player when they can
constexpr int kHordeMax = 50;

std::mt19937& Rng() { static std::mt19937 rng{std::random_device{}()}; return rng; }
float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(Rng()); }

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
    auto it = zombies_.find(id);
    if (it == zombies_.end()) return false;
    if (!it->second.dead) ZombieGone(it->second, false);
    zombies_.erase(it);
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(id); w.U8(uint8_t(net::ZombieOp::Remove)); Send(nullptr, w, true);
    return true;
}

bool Server::SetZombieOwner(uint16_t id, Player* owner) {
    auto it = zombies_.find(id);
    if (it == zombies_.end()) return false;
    it->second.pinned = owner ? owner->id : 0xFFFF;
    return true;  // TickZombies hands it over
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
    TickZombieZones();
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
        if (Player* pin = z.pinned != 0xFFFF ? Find(z.pinned) : nullptr; pin && Eligible(*pin, z)) next = pin;  // the mode chose
        else if (!owner) next = best;
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

// The owner's game saw its zombie hurt another player: the target pawn standing in for that player there
// lost health. Believed only from the zombie's owner, for a victim near the zombie, a few times a second.
void Server::OnZombieAttack(Player& p, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const uint16_t id = r.U16(), victimId = r.U16(); const float damage = r.F32();
    if (!r.ok() || !std::isfinite(damage) || damage <= 0) return;
    auto it = zombies_.find(id);
    if (it == zombies_.end() || it->second.owner != p.id || it->second.dead) return;
    Zombie& z = it->second;
    Player* victim = Find(victimId);
    if (!victim || victim == &p || !Eligible(*victim, z) || Dist2(*victim, z) > kAttackReach * kAttackReach) return;
    const double now = Now();
    if (now - z.lastAttackAt < 0.25) return;
    z.lastAttackAt = now;
    victim->lastAttacker = 0xFFFF;  // a zombie, not the last player who shot it, gets the kill
    Damage(victim, std::min(damage, kAttackMax), nullptr);
}

void Server::OnZombieDied(Player& p, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const uint16_t id = r.U16();
    if (!r.ok()) return;
    const uint16_t killer = r.U16();  // (absent from older clients)
    auto it = zombies_.find(id);
    if (it == zombies_.end() || it->second.owner != p.id || it->second.dead) return;
    Zombie& z = it->second;
    // The owner's own shots are not reported (its game hurt the zombie directly): its game tells whether its
    // own hit came last (killer = the owner). Otherwise the last player whose hit reached the server, or
    // nobody (a fall, another freaker). Older clients do not tell: with no attacker known, the owner.
    if (r.ok() && killer == p.id) z.lastAttacker = p.id;
    if (!r.ok() && z.lastAttacker == 0xFFFF) z.lastAttacker = p.id;
    z.dead = true; z.diedAt = Now(); z.health = 0;
    net::Writer w; w.Op(net::Msg::Zombie); w.U16(id); w.U8(uint8_t(net::ZombieOp::Died)); w.U16(z.lastAttacker); Send(nullptr, w, true);
    Log("Zombie %u (%s) died; killer %u", unsigned(id), z.type.c_str(), unsigned(z.lastAttacker));
    script_->ZombieDeath(id, Find(z.lastAttacker));
    if (auto zit = zombies_.find(id); zit != zombies_.end()) ZombieGone(zit->second, true);  // (the handler may have removed it)
}

// ---------------------------------------------------------------- zones and hordes
void Server::ZombieGone(Zombie& z, bool died) {
    if (!z.zone.empty()) for (auto& zone : zombieZones_) if (zone.name == z.zone) {
        if (std::erase(zone.alive, z.id) && died) zone.due.push_back(Now() + zone.respawn);
        break;
    }
    if (z.horde) if (auto h = hordes_.find(z.horde); h != hordes_.end()) {
        std::erase(h->second.alive, z.id);
        if (died) ++h->second.died;
        const uint16_t hid = h->first; const bool fell = died && h->second.alive.empty();
        if (h->second.alive.empty()) hordes_.erase(h);
        if (fell) script_->HordeDeath(hid);
    }
    z.zone.clear(); z.horde = 0;
}

const ZombieZone* Server::FindZombieZone(const std::string& name) const {
    for (auto& z : zombieZones_) if (z.name == name) return &z;
    return nullptr;
}

json::Value Server::ZombieZoneJson(const ZombieZone& z) {
    json::Value v = json::Value::Object();
    v.set("name", z.name); v.set("x", double(z.x)); v.set("y", double(z.y)); v.set("z", double(z.z));
    v.set("radius", double(z.radius / 100));
    json::Value types = json::Value::Array(); for (auto& t : z.types) types.push(t);
    v.set("types", types);
    v.set("max", z.max); v.set("respawn", z.respawn); v.set("wake", double(z.wake / 100));
    v.set("world", double(z.world)); v.set("enabled", z.enabled);
    return v;
}

// Fields of a zone (metres, seconds) into a new one or the one of that name. "" fine, else why not.
std::string Server::SetZombieZone(const json::Value& v, bool create) {
    if (!v.isObject()) return "a table expected";
    const std::string name = v["name"].asString().substr(0, 64);
    if (name.empty()) return "a zone needs a name";
    ZombieZone* zone = nullptr;
    for (auto& z : zombieZones_) if (z.name == name) zone = &z;
    if (create && zone) return "a zone named \"" + name + "\" exists";
    if (!create && !zone) return "no zone \"" + name + "\"";
    ZombieZone z = zone ? *zone : ZombieZone{};
    z.name = name;
    auto num = [&](const char* key, float& out, float scale, float least) {
        if (v[key].isNumber() && std::isfinite(v[key].asNumber())) out = std::max(least, float(v[key].asNumber()) * scale);
    };
    num("x", z.x, 1, -1e9f); num("y", z.y, 1, -1e9f); num("z", z.z, 1, -1e9f);
    num("radius", z.radius, 100, 300); num("wake", z.wake, 100, 0);
    if (v["max"].isNumber()) z.max = std::clamp(v["max"].asInt(), 0, 50);
    if (v["respawn"].isNumber()) z.respawn = std::clamp(v["respawn"].asNumber(), 0.0, 86400.0);
    if (v["world"].isNumber()) z.world = int32_t(v["world"].asNumber());
    if (v["enabled"].type() == json::Value::Type::Bool) z.enabled = v["enabled"].asBool();
    if (v["types"].isArray() || v["type"].isString()) {
        z.types.clear();
        if (v["type"].isString()) z.types.push_back(v["type"].asString());
        for (auto& t : v["types"].elements()) if (t.isString() && !t.asString().empty()) z.types.push_back(t.asString());
        for (auto& t : z.types) if (t.size() > net::kZombieTypeMax) return "type \"" + t + "\" is too long";
        if (z.types.empty()) z.types.push_back("Swarmer");
    }
    if (zone) {
        const bool off = zone->enabled && !z.enabled;
        *zone = std::move(z);
        if (off) { const auto ids = zone->alive; for (uint16_t id : ids) RemoveZombie(id); zone->due.clear(); }
    } else zombieZones_.push_back(std::move(z));
    return "";
}

bool Server::RemoveZombieZone(const std::string& name) {
    for (auto it = zombieZones_.begin(); it != zombieZones_.end(); ++it) {
        if (it->name != name) continue;
        const auto ids = it->alive;
        zombieZones_.erase(it);
        for (uint16_t id : ids) RemoveZombie(id);
        return true;
    }
    return false;
}

void Server::LoadZombieZones(const json::Value& world) {
    zombieZones_.clear();
    for (auto& e : world["zombieZones"].elements()) {
        const std::string error = SetZombieZone(e, true);
        if (!error.empty()) Log("world.json: zombie zone skipped (%s)", error.c_str());
    }
    if (!zombieZones_.empty()) Log("Zombie zones: %zu%s", zombieZones_.size(), cfg_.zombies ? "" : " (off: server.ini Zombies=0)");
}

void Server::TickZombieZones() {
    if (!cfg_.zombies || zombieZones_.empty()) return;
    const double now = Now();
    if (now < nextZombieZoneTick_) return;
    nextZombieZoneTick_ = now + 0.5;
    for (auto& zone : zombieZones_) {
        std::erase_if(zone.alive, [&](uint16_t id) { auto it = zombies_.find(id); return it == zombies_.end() || it->second.zone != zone.name; });
        if (!zone.enabled) continue;
        // Awake while a player of its world is near (on the ground plan).
        const float reach = zone.radius + zone.wake;
        std::vector<const Player*> nearby;
        for (auto& [pid, p] : players_) {
            if (!p->ready || p->bot || p->world != zone.world || p->spectating) continue;
            const float dx = p->state.x - zone.x, dy = p->state.y - zone.y;
            if (dx * dx + dy * dy <= reach * reach) nearby.push_back(p.get());
        }
        if (!nearby.empty()) zone.lastAwake = now;
        else {
            if (now - zone.lastAwake > kZoneSleep && !zone.alive.empty()) {
                const auto ids = zone.alive;
                for (uint16_t id : ids) RemoveZombie(id);
                zone.due.clear();  // a sleeping zone is full again when someone comes back
            }
            continue;
        }
        std::erase_if(zone.due, [&](double at) { return at <= now; });  // (those may come back now)
        const int free = zone.max - int(zone.alive.size()) - int(zone.due.size());
        for (int i = 0; i < std::min(free, kZoneSpawnsPerTick) && !zone.types.empty(); ++i) {
            // A random point of the zone, away from the players when one can be found.
            float bx = zone.x, by = zone.y, bestClear = -1;
            for (int attempt = 0; attempt < 8; ++attempt) {
                const float r = zone.radius * 0.85f * std::sqrt(Rand(0, 1)), a = Rand(0, 6.2831853f);
                const float px = zone.x + r * std::cos(a), py = zone.y + r * std::sin(a);
                float clear = 1e30f;
                for (const Player* p : nearby) clear = std::min(clear, std::hypot(p->state.x - px, p->state.y - py));
                if (clear > bestClear) { bestClear = clear; bx = px; by = py; }
                if (clear >= kSpawnClear) break;
            }
            const std::string& type = zone.types[size_t(Rand(0, float(zone.types.size()) - 0.001f))];
            const Zombie* zb = CreateZombie(type, bx, by, zone.z, Rand(0, 360), zone.world);
            if (!zb) break;  // too many zombies on the server
            zombies_[zb->id].zone = zone.name;
            zone.alive.push_back(zb->id);
        }
    }
}

uint16_t Server::CreateHorde(float x, float y, float z, int count, float radius, const std::vector<std::string>& types, int32_t world) {
    if (!cfg_.zombies || types.empty()) return 0;
    count = std::clamp(count, 1, kHordeMax);
    uint16_t id = nextHorde_;
    while (hordes_.count(id) || id == 0) ++id;
    Horde h; h.id = id;
    for (int i = 0; i < count; ++i) {
        const float r = radius * std::sqrt(Rand(0, 1)), a = Rand(0, 6.2831853f);
        const std::string& type = types[size_t(Rand(0, float(types.size()) - 0.001f))];
        const Zombie* zb = CreateZombie(type, x + r * std::cos(a), y + r * std::sin(a), z, Rand(0, 360), world);
        if (!zb) break;
        zombies_[zb->id].horde = id;
        h.alive.push_back(zb->id);
    }
    if (h.alive.empty()) return 0;
    nextHorde_ = uint16_t(id + 1);
    hordes_[id] = std::move(h);
    return id;
}

bool Server::RemoveHorde(uint16_t id) {
    auto it = hordes_.find(id);
    if (it == hordes_.end()) return false;
    const auto ids = it->second.alive;
    hordes_.erase(it);
    for (uint16_t zid : ids) { if (auto z = zombies_.find(zid); z != zombies_.end()) z->second.horde = 0; RemoveZombie(zid); }
    return true;
}

}  // namespace srv
