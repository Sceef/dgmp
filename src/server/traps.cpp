// Traps (docs/API.md "Ловушки"): the game's own mine, IED and bear trap, a tripwire between two points, or an
// invisible zone. The server decides when one goes off: a player (or a zombie) inside a point trap's radius,
// or whose movement since the last check crossed a tripwire at a height its body has. Then the mode's
// onTrigger / "trap" handlers run (false: nothing happens), and every client fires its copy: an explosive
// explodes the game's way (each game hurts its own player and zombies), a bear trap catches its victim.
#include "server.h"
#include "script.h"
#include <enet/enet.h>
#include <algorithm>
#include <cmath>

namespace srv {
namespace {
constexpr size_t kTrapsMax = 500;

// Where the segments a-b and c-d cross on the ground plan: true and the fraction along a-b.
bool Cross2D(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy, float& t) {
    const float rx = bx - ax, ry = by - ay, sx = dx - cx, sy = dy - cy;
    const float den = rx * sy - ry * sx;
    if (std::fabs(den) < 1e-6f) return false;
    const float qx = cx - ax, qy = cy - ay;
    t = (qx * sy - qy * sx) / den;
    const float u = (qx * ry - qy * rx) / den;
    return t >= 0 && t <= 1 && u >= 0 && u <= 1;
}
}  // namespace

bool Trap::KnownType(const std::string& t) {
    return t == "mine" || t == "ied" || t == "beartrap" || t == "wire" || t == "zone";
}

// Does a body that moved from p to q (actor centres, ~90 above the feet) set it off?
bool Trap::Hit(float px, float py, float pz, float qx, float qy, float qz) const {
    if (type == "wire") {
        float t;
        if (!Cross2D(px, py, qx, qy, x, y, x2, y2, t)) return false;
        const float feet = pz + (qz - pz) * t - 90.f;
        // The wire's height where it was crossed (between its ends).
        const float wx = x2 - x, wy = y2 - y, len2 = wx * wx + wy * wy;
        const float cx = px + (qx - px) * t, cy = py + (qy - py) * t;
        const float u = len2 > 0 ? std::clamp(((cx - x) * wx + (cy - y) * wy) / len2, 0.f, 1.f) : 0.f;
        const float wz = z + (z2 - z) * u;
        return wz >= feet - 20.f && wz <= feet + 220.f;  // from the ankles up to over a rider's head
    }
    const float dx = qx - x, dy = qy - y, dz = (qz - 90.f) - z;
    return dx * dx + dy * dy <= radius * radius && dz > -150.f && dz < 250.f;
}

void Server::SendTrap(Player* to, const Trap& t) {
    net::Writer w; w.Op(net::Msg::Trap); w.U16(t.id); w.U8(uint8_t(net::TrapOp::Add));
    w.Str(t.type); w.F32(t.x); w.F32(t.y); w.F32(t.z); w.F32(t.yaw); w.F32(t.x2); w.F32(t.y2); w.F32(t.z2);
    w.I32(t.world); w.U8(t.explosive);
    Send(to, w, true);
}

uint16_t Server::AddTrap(Trap t) {
    if (traps_.size() >= kTrapsMax || !Trap::KnownType(t.type)) return 0;
    uint16_t id = nextTrap_;
    while (traps_.count(id) || id == 0) ++id;
    nextTrap_ = uint16_t(id + 1);
    t.id = id;
    traps_[id] = t;
    SendTrap(nullptr, traps_[id]);
    return id;
}

bool Server::RemoveTrap(uint16_t id) {
    if (!traps_.erase(id)) return false;
    net::Writer w; w.Op(net::Msg::Trap); w.U16(id); w.U8(uint8_t(net::TrapOp::Remove)); Send(nullptr, w, true);
    return true;
}

// Sets it off now: every client fires its copy (victim: the player it caught, 0xFFFF none). A trap that
// goes off once is forgotten 10 s later (its explosion plays out, a wire's stakes go first).
void Server::FireTrap(uint16_t id, uint16_t victim) {
    auto it = traps_.find(id);
    if (it == traps_.end() || it->second.fired) return;
    Trap& t = it->second;
    net::Writer w; w.Op(net::Msg::Trap); w.U16(id); w.U8(uint8_t(net::TrapOp::Fire)); w.U16(victim); Send(nullptr, w, true);
    const double now = Now();
    if (t.rearm > 0) { t.armedAt = now + t.rearm; return; }
    t.fired = true; t.firedAt = now;
}

void Server::TickTraps() {
    if (traps_.empty()) { trapPrev_.clear(); return; }
    const double now = Now();
    // Players: from where they were at the last check to where they are now.
    std::vector<std::pair<uint16_t, uint16_t>> hits;  // (trap, player); zombies: player 0xFFFF, zombie in zombieHits
    std::vector<std::pair<uint16_t, uint16_t>> zombieHits;
    for (auto& [pid, p] : players_) {
        if (!p->ready || p->bot || p->spectating || (p->state.flags & net::StateDead)) { trapPrev_.erase(pid); continue; }
        const auto prev = trapPrev_.find(pid);
        const float px = prev != trapPrev_.end() ? prev->second[0] : p->state.x, py = prev != trapPrev_.end() ? prev->second[1] : p->state.y,
                    pz = prev != trapPrev_.end() ? prev->second[2] : p->state.z;
        trapPrev_[pid] = {p->state.x, p->state.y, p->state.z};
        // A teleport is no walk through a wire.
        if ((p->state.x - px) * (p->state.x - px) + (p->state.y - py) * (p->state.y - py) > 4000.f * 4000.f) continue;
        for (auto& [tid, t] : traps_) {
            if (t.fired || !t.players || now < t.armedAt || t.world != p->world) continue;
            if (t.Hit(px, py, pz, p->state.x, p->state.y, p->state.z)) hits.emplace_back(tid, pid);
        }
    }
    for (auto& [zid, z] : zombies_) {
        if (z.dead) { zombiePrev_.erase(zid); continue; }
        const auto prev = zombiePrev_.find(zid);
        const float px = prev != zombiePrev_.end() ? prev->second[0] : z.x, py = prev != zombiePrev_.end() ? prev->second[1] : z.y,
                    pz = prev != zombiePrev_.end() ? prev->second[2] : z.z;
        zombiePrev_[zid] = {z.x, z.y, z.z};
        for (auto& [tid, t] : traps_)
            if (!t.fired && t.zombies && now >= t.armedAt && t.world == z.world && t.Hit(px, py, pz, z.x, z.y, z.z)) zombieHits.emplace_back(tid, zid);
    }
    for (auto [tid, pid] : hits) {
        auto it = traps_.find(tid); Player* p = Find(pid);
        if (it == traps_.end() || !p || it->second.fired || now < it->second.armedAt) continue;
        if (script_->TrapTriggered(tid, p, 0)) FireTrap(tid, pid);
        else if (auto again = traps_.find(tid); again != traps_.end()) again->second.armedAt = now + 1.0;  // refused: not every tick
    }
    for (auto [tid, zid] : zombieHits) {
        auto it = traps_.find(tid);
        if (it == traps_.end() || it->second.fired || now < it->second.armedAt) continue;
        if (script_->TrapTriggered(tid, nullptr, zid)) FireTrap(tid, 0xFFFF);
        else if (auto again = traps_.find(tid); again != traps_.end()) again->second.armedAt = now + 1.0;
    }
    std::vector<uint16_t> gone;
    for (auto& [tid, t] : traps_) if (t.fired && now - t.firedAt > 10.0) gone.push_back(tid);  // (clients take a wire's stakes away after 7 s)
    for (uint16_t tid : gone) RemoveTrap(tid);
    for (auto it = zombiePrev_.begin(); it != zombiePrev_.end();) it = zombies_.count(it->first) ? std::next(it) : zombiePrev_.erase(it);
}

}  // namespace srv
