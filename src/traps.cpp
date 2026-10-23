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
    bool lethal = true;         // bear trap: the player it holds bleeds out in `bleed` seconds (the game's own bleeding
    float bleed = 20;           // stops at about a quarter of health)
    bool ours = false;          // a bear trap that went off on this player (it is hostile to it now)
    bool held = false;          // ...and holds it: the player stands in its jaws
    double heldSince = 0; float heldHealth = 0;  // since when, with how much health (the bleeding's schedule)
    UObject* actor = nullptr;   // the game's trap
    UObject* wire = nullptr;    // a wire's line
    UObject* stakes[2] = {};    // a wire's stakes in the ground under its ends
    bool spawned = false, failed = false;
    double fireAt = 0;          // fire then (0: not asked)
    uint16_t victim = 0xFFFF;
    bool done = false;          // fired
    double cleanupAt = 0;       // a fired wire: its stakes go then
    double pullAt = 0;          // a bear trap catching this player: it steps out of the jaws' reach then, and in
    int pullStep = 0;           // 1: out (next: in)
    int catching = 0;           // a bear trap catching this player: 1 moved aside, 2 brought under its feet (next frames)
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

void SetCollision(UObject* component, uint8_t type) {  // ECollisionEnabled: 0 none, 1 query only
    if (auto f = IsValid(component) ? Fn(component, "SetCollisionEnabled") : nullptr) { Params p(f); p.Set<uint8_t>("NewType", type); Call(component, "SetCollisionEnabled", &p); }
}
// The game's trap without a mind of its own: its triggers (pawns, hordes, the tutorial, a bear trap's jaws and
// shot) do not react, and it cannot be picked up into the backpack (it would hand out mines and traps). Only
// those: the explosion uses the rest of it (its overlap sphere, the weapon), so nothing else is touched.
void Neutralize(UObject* actor) {
    SetBoolProp(actor, "PlayerCanTrigger", false);
    for (const char* name : {"PawnTrigger_Code", "HordeTrigger_Code", "HordeCriticalTrigger_Code", "TutorialTrigger", "ActivationTrigger", "ShootTrigger",
                             "InventoryPickup", "BearTrapPickup"})
        SetCollision(ObjectProp(actor, name), 0);
    // The pickup actor stays (the trap's scripts refer to it) but a kilometre under the ground: no "[E] pick up".
    if (UObject* pickup = ObjectProp(actor, "PickupRef")) {
        if (auto f = Fn(pickup, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(pickup, "SetActorEnableCollision", &p); }
        FVector at = ActorPos(pickup); at.Z -= 100000.f;
        worldedit::PlaceActor(pickup, at, FRotator{0, 0, 0}, 1.f);
    }
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
        // Its victim is caught the game's way (stuck, mash F to get free, bleeding). The game catches a pawn that
        // moves into the jaws' trigger of a hostile, armed trap (a spawned one is friendly to the player; the trap
        // itself does not move): so it turns hostile and armed with its trigger on, and the victim is put at its
        // centre (it may have walked half a metre on by the time the server's word came): that move is the overlap.
        // (Switching the trigger on under a standing pawn raises none.) The others see it snap shut.
        if (t.victim == localId && IsValid(pawn)) {
            SetBoolProp(t.actor, "PlayerCanTrigger", true);  // (Neutralize took it: the jaws' handler checks it)
            SetBoolProp(t.actor, "IsPlayerPlaced", false);   // a spawned one counts as the player's own, which never catches the player
            Call(t.actor, "SetUnFriendly");
            Call(t.actor, "TurnOnBearTrap");
            SetCollision(ObjectProp(t.actor, "ActivationTrigger"), 1);
            t.pullAt = Clock() + 0.15; t.pullStep = 1; t.ours = true;  // (Update: arming takes the game a moment; pulled at once, only the trap turned)
            return;
        }
        Call(t.actor, "CloseTheTrapFinish");
        return;
    }
    Destroy(t.wire);
    if (t.type == "wire") t.cleanupAt = Clock() + kStakesStay;
    // The way an NPC sets it off: the trap fires its weapon (the blast, its smoke and debris, the damage) and
    // goes. TrapExplode alone only shakes the camera and removes it.
    if (IsValid(t.actor)) Call(t.actor, "TriggerWeaponAIPawn");
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
        t.b.X = r.F32(); t.b.Y = r.F32(); t.b.Z = r.F32(); t.world = r.I32(); t.explosive = r.U8() != 0; t.lethal = r.U8() != 0; t.bleed = r.F32();
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

// The victim comes into the armed jaws from outside them (a pawn already standing inside raises no overlap):
// first a step out, 1.5 m away from the trap, then, a frame later, into its centre.
void Pull(Trap& t, UObject* pawn) {
    t.pullAt = 0;
    if (!IsValid(t.actor) || !IsValid(pawn)) return;
    const FVector trap = ActorPos(t.actor), body = ActorPos(pawn);
    FVector to{trap.X, trap.Y, body.Z};
    if (t.pullStep == 1) {
        float dx = body.X - trap.X, dy = body.Y - trap.Y; const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 1) { dx /= d; dy /= d; } else { dx = 1; dy = 0; }
        to = {trap.X + dx * 150.f, trap.Y + dy * 150.f, body.Z};
        t.pullStep = 2; t.pullAt = Clock() + 0.05;
    } else t.pullStep = 0;
    if (auto f = Fn(pawn, "K2_SetActorLocation")) {
        Params p(f); p.Set("NewLocation", to); p.SetBool("bSweep", false); p.SetBool("bTeleport", true);
        Call(pawn, "K2_SetActorLocation", &p);
    }
}
// The bear trap holding this player, or null: one that went off on it while the player stands in its jaws (the game
// keeps the player there until the mash-the-key escape succeeds; then it walks off and the trap lets go of it).
const Trap* Holder() {
    for (auto& [id, t] : list) if (t.held) return &t;
    return nullptr;
}

void Update(UObject* pawn, uint16_t localId) {
    if (!IsValid(pawn)) return;
    const FVector me = ActorPos(pawn);
    const double now = Clock();
    // Which of our bear traps holds this player: in its jaws (60 cm) it is held, past 80 cm free again.
    for (auto& [id, t] : list) {
        if (!t.ours || !IsValid(t.actor)) { t.held = false; continue; }
        const FVector at = ActorPos(t.actor);
        const float d = std::sqrt((at.X - me.X) * (at.X - me.X) + (at.Y - me.Y) * (at.Y - me.Y));
        if (t.held && d > 80.f) { t.held = false; t.ours = false; dg::Log("traps: free of the bear trap"); }
        else if (!t.held && !t.pullAt && d < 60.f) { t.held = true; t.heldSince = now; t.heldHealth = Field<float>(pawn, 0x1FD4); }
    }
    // A lethal one: the player bleeds out in `bleed` seconds from the health it had when caught (the game's own
    // bleeding stops at about a quarter). Twice a second a wound of the game's bleeding damage type (the screen
    // reddens; health keeps to the schedule however the game weighs the wound), and at the end a lethal hit like
    // any other: the multiplayer death at once.
    static double nextBleed = 0;
    if (const Trap* holder = Holder(); holder && holder->lethal) {
        constexpr int kPawnHealth = 0x1FD4;  // (as src/mp.cpp)
        float& health = Field<float>(pawn, kPawnHealth);
        static UObject* statics = nullptr;
        static UObject* bleedType = nullptr;
        if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
        if (!IsValid(bleedType)) { bleedType = LoadAsset("/Game/Libraries/Blueprints/DamageTypes/DmgTypeBP_BleedingTick.DmgTypeBP_BleedingTick_C"); if (bleedType) AddToRoot(bleedType); }
        auto wound = [&](float damage) {
            auto f = IsValid(statics) ? Fn(statics, "ApplyDamage") : nullptr;
            if (!f) return;
            Params p(f); p.Set("DamagedActor", pawn); p.Set<float>("BaseDamage", damage); p.Set("DamageCauser", holder->actor);
            if (bleedType) p.Set("DamageTypeClass", bleedType);
            Call(statics, "ApplyDamage", &p);
        };
        if (now >= nextBleed && health > 0) {
            nextBleed = now + 0.5;
            const float left = 1.f - float(now - holder->heldSince) / std::max(1.f, holder->bleed);
            const float expected = holder->heldHealth * left;
            if (expected <= 1.f) { wound(100000.f); dg::Log("traps: bled out in the bear trap"); }  // (lethal: the death path)
            else {
                wound(std::max(0.5f, health - expected));
                if (health > expected) health = expected;
            }
        }
    } else nextBleed = now + 0.5;
    for (auto& [id, t] : list) {
        const float dx = t.a.X - me.X, dy = t.a.Y - me.Y, d2 = dx * dx + dy * dy;
        const bool inRange = t.world == myWorld && d2 < (t.spawned ? kDespawnRange * kDespawnRange : kSpawnRange * kSpawnRange);
        if (!inRange) { if (t.spawned) Despawn(t); t.fireAt = 0; continue; }  // (an explosion far away is not seen)
        if (!t.spawned && !t.done && !t.failed) { Spawn(t, pawn); if (t.fireAt) t.fireAt = std::max(t.fireAt, now + kFireDelay); }
        if (t.fireAt && now >= t.fireAt) FireNow(t, pawn, localId);
        if (t.catching == 1 && IsValid(t.actor)) {  // (see FireNow: the bear trap comes under its victim)
            FVector feet = ActorPos(pawn); feet.Z -= 90.f;
            worldedit::PlaceActor(t.actor, feet, FRotator{0, t.yaw, 0}, 1.f);
            t.catching = 2;
        }
        if (t.pullAt && now >= t.pullAt) Pull(t, pawn);
        if (t.cleanupAt && now >= t.cleanupAt) { t.cleanupAt = 0; Destroy(t.stakes[0]); Destroy(t.stakes[1]); }
    }
}

// Held while one of our bear traps has this player as its victim (it lets go once the escape succeeds or it dies).
bool Caught() { return Holder() != nullptr; }

void OnLocalDeath() {
    for (auto& [id, t] : list) {
        if (!t.ours) continue;
        // The game's own "the trapped player died": it lets go of the body (the multiplayer death keeps it).
        if (IsValid(t.actor)) { Call(t.actor, "PlayerDied"); Destroy(t.actor); }
        t.ours = t.held = false; t.done = true;
        dg::Log("traps: the bear trap let go of the dead player");
    }
}

}  // namespace traps
