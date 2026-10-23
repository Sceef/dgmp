#include "traps.h"
#include "worldedit.h"
#include "log.h"
#include "shared/protocol.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

using namespace ue;

namespace traps {
namespace {
constexpr float kSpawnRange = 30000.f, kDespawnRange = 32000.f;  // 300 m
constexpr double kFireDelay = 1.0;  // a trap spawned for its own explosion (explode()) goes off once it is set up
constexpr double kStakesStay = 7.0; // a fired wire's stakes stay this long

const char* ClassFor(const std::string& type, bool explosive) {
    if (type == "mine") return "/Game/Weapons/Trap_Weapons/Mines/ProximityMine/MineTrap.MineTrap_C";
    if (type == "ied" || (type == "wire" && explosive)) return "/Game/Weapons/Trap_Weapons/Mines/IED/IEDTrap.IEDTrap_C";
    if (type == "beartrap") return "/Game/Weapons/Trap_Weapons/BearTrap/BearTrap.BearTrap_C";
    return nullptr;  // a plain wire's end, a zone: nothing of the game
}

struct Trap {
    std::string type;
    FVector a{}, b{};
    float yaw = 0;
    int world = 0;
    bool explosive = true;
    UObject* actor = nullptr;   // the game's trap
    UObject* wire = nullptr;    // a wire's line
    UObject* stakes[2] = {};    // a wire's stakes in the ground under its ends
    bool spawned = false, failed = false;
    double fireAt = 0;          // fire then (0: not asked)
    uint16_t victim = 0xFFFF;
    bool done = false;          // fired
    double cleanupAt = 0;       // a fired wire: its stakes go then
};
std::map<uint16_t, Trap> list;
int myWorld = 0;

double Clock() { return GetTickCount64() / 1000.0; }
UObject* ObjectProp(UObject* o, const char* name) {
    if (!IsValid(o)) return nullptr;
    for (UObject* cls = o->Class; cls; cls = SuperOf(cls))
        if (UObject* p = FindProperty(cls, name)) { UObject* v = Field<UObject*>(o, Field<int32_t>(p, off::UProperty_Offset)); return IsValid(v) ? v : nullptr; }
    return nullptr;
}
void Destroy(UObject*& a) { if (IsValid(a)) Call(a, "K2_DestroyActor"); a = nullptr; }
FVector ActorPos(UObject* actor) {
    auto root = IsValid(actor) ? Field<UObject*>(actor, off::Actor_RootComponent) : nullptr;
    return IsValid(root) ? Field<FVector>(root, off::Scene_C2W_Trans) : FVector{};
}

// The game's trap without a mind of its own: its triggers (pawns, hordes, the tutorial, a bear trap's jaws and
// shot) do not react, and its backpack pickup cannot be taken (it would hand out mines). Only those: the
// explosion uses the rest of it (its overlap sphere, the weapon), so nothing else is touched.
void Neutralize(UObject* actor) {
    SetBoolProp(actor, "PlayerCanTrigger", false);
    for (const char* name : {"PawnTrigger_Code", "HordeTrigger_Code", "HordeCriticalTrigger_Code", "TutorialTrigger", "ActivationTrigger", "ShootTrigger"})
        if (UObject* c = ObjectProp(actor, name)) if (auto f = Fn(c, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(c, "SetCollisionEnabled", &p); }
    if (UObject* pickup = ObjectProp(actor, "PickupRef")) if (auto f = Fn(pickup, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(pickup, "SetActorEnableCollision", &p); }
}

// A line from a to b: the engine's cylinder (1 m long, 1 m wide), stretched and turned along it.
UObject* SpawnWire(const FVector& a, const FVector& b, UObject* context) {
    const float dx = b.X - a.X, dy = b.Y - a.Y, dz = b.Z - a.Z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1) return nullptr;
    const FVector mid{(a.X + b.X) / 2, (a.Y + b.Y) / 2, (a.Z + b.Z) / 2};
    // The cylinder's axis is its Z: pitch -90 lays it along X, the elevation lifts it, the yaw turns it.
    const float yaw = std::atan2(dy, dx) * 57.2957795f, elev = std::asin(dz / len) * 57.2957795f;
    UObject* w = worldedit::SpawnMesh("/Engine/BasicShapes/Cylinder.Cylinder", mid, FRotator{-90.f + elev, yaw, 0}, 1.f, context);
    if (!IsValid(w)) return nullptr;
    if (auto f = Fn(w, "SetActorScale3D")) { Params p(f); p.Set("NewScale3D", FVector{0.012f, 0.012f, len / 100.f}); Call(w, "SetActorScale3D", &p); }
    if (auto f = Fn(w, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(w, "SetActorEnableCollision", &p); }
    return w;
}

// The ground under a point (the point itself when nothing is hit within 30 m below it).
FVector Ground(UObject* context, const FVector& at) {
    static UObject* kismet = nullptr; static UObject* statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, brk = statics ? Fn(statics, "BreakHitResult") : nullptr;
    if (!trace || !brk) return at;
    Params t(trace); t.Set("WorldContextObject", context); t.Set("Start", FVector{at.X, at.Y, at.Z + 20}); t.Set("End", FVector{at.X, at.Y, at.Z - 3000});
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    if (!t.GetBool("ReturnValue") || !hitProp || !inProp) return at;
    Params h(brk);
    std::memcpy(static_cast<uint8_t*>(h.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &h);
    return h.Get<FVector>("ImpactPoint");
}

void Spawn(Trap& t, UObject* pawn) {
    t.spawned = true;
    if (const char* cls = ClassFor(t.type, t.explosive)) {
        // A wire's ends are where the wire is (a shin or a rider's neck up): its IED lies on the ground under the first.
        const FVector at = t.type == "wire" ? Ground(pawn, t.a) : t.a;
        t.actor = worldedit::SpawnMesh(cls, at, FRotator{0, t.yaw, 0}, 1.f, pawn);
        if (IsValid(t.actor)) Neutralize(t.actor);
        else { t.failed = true; dg::Log("traps: %s did not spawn", cls); }
    }
    if (t.type == "wire") {
        t.wire = SpawnWire(FVector{t.a.X, t.a.Y, t.a.Z}, t.b, pawn);
        const float yaw = std::atan2(t.b.Y - t.a.Y, t.b.X - t.a.X) * 57.2957795f;
        const FVector ends[2] = {t.a, t.b};
        for (int i = 0; i < 2; ++i) {
            t.stakes[i] = worldedit::SpawnMesh("/Game/Environments/ManMade/Props/Ropes/ropes_stake_v2.ropes_stake_v2", Ground(pawn, ends[i]), FRotator{0, yaw, 0}, 1.f, pawn);
            if (IsValid(t.stakes[i])) if (auto f = Fn(t.stakes[i], "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(t.stakes[i], "SetActorEnableCollision", &p); }
        }
    }
    dg::Log("traps: %s spawned at %.0f %.0f %.0f%s", t.type.c_str(), t.a.X, t.a.Y, t.a.Z, IsValid(t.actor) ? "" : " (no game actor)");
}
void Despawn(Trap& t) { Destroy(t.actor); Destroy(t.wire); Destroy(t.stakes[0]); Destroy(t.stakes[1]); t.spawned = false; }

void FireNow(Trap& t, UObject* pawn, uint16_t localId) {
    t.done = true; t.fireAt = 0;
    dg::Log("traps: %s fires (victim %u, actor %s)", t.type.c_str(), unsigned(t.victim), IsValid(t.actor) ? "here" : "missing");
    if (t.type == "beartrap") {
        if (!IsValid(t.actor)) return;
        // Its victim is caught the game's way (stuck, mash to get free, bleeding); the others see it snap shut.
        if (t.victim == localId && IsValid(pawn)) if (UObject* f = Fn(t.actor, "Trap Player")) {
            Params p(f);
            if (UObject* first = FirstParam(f)) std::memcpy(static_cast<uint8_t*>(p.Data()) + Field<int32_t>(first, off::UProperty_Offset), &pawn, sizeof pawn);
            ProcessEvent(t.actor, f, p.Data());
            return;
        }
        Call(t.actor, "CloseTheTrapFinish");
        return;
    }
    Destroy(t.wire);
    if (t.type == "wire") t.cleanupAt = Clock() + kStakesStay;
    if (IsValid(t.actor)) Call(t.actor, "TrapExplode");  // the game's explosion: it removes the actor itself
    t.actor = nullptr;
}
}  // namespace

void OnMessage(const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    const uint16_t id = r.U16(); const auto op = net::TrapOp(r.U8());
    if (!r.ok()) return;
    if (op == net::TrapOp::Add) {
        Trap t;
        t.type = r.Str(32); t.a.X = r.F32(); t.a.Y = r.F32(); t.a.Z = r.F32(); t.yaw = r.F32();
        t.b.X = r.F32(); t.b.Y = r.F32(); t.b.Z = r.F32(); t.world = r.I32(); t.explosive = r.U8() != 0;
        if (!r.ok()) return;
        if (auto old = list.find(id); old != list.end()) Despawn(old->second);
        list[id] = t;
        return;
    }
    auto it = list.find(id);
    if (it == list.end()) return;
    if (op == net::TrapOp::Remove) {
        Trap& t = it->second;
        if (!t.done) Despawn(t); else { Destroy(t.wire); Destroy(t.stakes[0]); Destroy(t.stakes[1]); }
        list.erase(it); return;
    }
    if (op == net::TrapOp::Fire) {
        it->second.victim = r.U16();
        it->second.fireAt = Clock() + (it->second.spawned ? 0 : kFireDelay);  // (spawned in Update first)
        it->second.done = false;
    }
}

void SetWorld(int world) { myWorld = world; }

void Reset() { for (auto& [id, t] : list) Despawn(t); list.clear(); myWorld = 0; }

void Update(UObject* pawn, uint16_t localId) {
    if (!IsValid(pawn)) return;
    const FVector me = ActorPos(pawn);
    const double now = Clock();
    for (auto& [id, t] : list) {
        const float dx = t.a.X - me.X, dy = t.a.Y - me.Y, d2 = dx * dx + dy * dy;
        const bool inRange = t.world == myWorld && d2 < (t.spawned ? kDespawnRange * kDespawnRange : kSpawnRange * kSpawnRange);
        if (!inRange) { if (t.spawned) Despawn(t); t.fireAt = 0; continue; }  // (an explosion far away is not seen)
        if (!t.spawned && !t.done && !t.failed) { Spawn(t, pawn); if (t.fireAt) t.fireAt = std::max(t.fireAt, now + kFireDelay); }
        if (t.fireAt && now >= t.fireAt) FireNow(t, pawn, localId);
        if (t.cleanupAt && now >= t.cleanupAt) { t.cleanupAt = 0; Destroy(t.stakes[0]); Destroy(t.stakes[1]); }
    }
}

}  // namespace traps
