#include "zombies.h"
#include "log.h"
#include "mp.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

using namespace ue;

namespace zombies {
namespace {
constexpr int kSpawnTypeSize = 1056;          // BendAmbientSpawnType
constexpr int kSpawnType_Faction = 0x0, kSpawnType_LoadedPawn = 0x130;
constexpr float kTeleportDistance = 800.f;    // a puppet farther than 8 m from where it should be jumps there
constexpr float kArriveDistance = 25.f;

struct FactionName { const char* name; uint8_t value; };
// EAIFactionType values of the ambient generator's pooled spawn types (dev console: sdump BendAmbientGenerator PooledSpawnTypes i).
constexpr FactionName kFactions[] = {
    {"Swarmer", 4}, {"Newt", 5}, {"LootNewt", 6}, {"RagerBear", 7}, {"FreakWolf", 9}, {"FreakCrow", 10},
    {"Survivor", 12}, {"Marauder", 14}, {"RIP", 23}, {"Wolf", 24}, {"Bear", 25}, {"Crow", 26}, {"Cougar", 27},
    {"Deer", 28}, {"Screamer", 33}, {"Breaker", 34}, {"Reacher", 35}, {"Bleacher", 36},
};

struct Sample { double t; FVector pos; float yaw; float speed; UObject* montage; };
struct Driven {
    FVector to{}; float yaw = 0;
    UObject* source = nullptr; float delay = 0; FVector offset{};  // Mirror
    std::deque<Sample> samples;
    UObject* lastMontage = nullptr;
    float speed = 0;                                                 // wanted ground speed (units/s), 0: walk speed as is
    FVector ordered{}; double orderedAt = -1; int moveSpeed = -1;   // the last MoveToLocation and SetMoveSpeed given
};
std::map<UObject*, Driven> driven;

struct PendingSpawn { double until; FVector at; UObject* pawnClass; std::set<UObject*> before; uint16_t tag = 0; };  // tag: server zombie id (0: dev console)
std::vector<PendingSpawn> pending;
uint16_t nextSpawnTag = 0;  // Spawn() files its pending spawn under this tag
UObject* lastSpawned = nullptr;
double nextPendingScan = 0;

double Clock() { return GetTickCount64() / 1000.0; }

int PropOffset(UObject* obj, const char* name) {
    for (auto cls = obj ? obj->Class : nullptr; cls; cls = SuperOf(cls))
        if (UObject* p = FindProperty(cls, name)) return Field<int32_t>(p, off::UProperty_Offset);
    return -1;
}
template <typename T> T Prop(UObject* obj, const char* name, T fallback = T{}) {
    const int at = PropOffset(obj, name); return at < 0 ? fallback : Field<T>(obj, at);
}
template <typename T> void SetProp(UObject* obj, const char* name, const T& v) {
    const int at = PropOffset(obj, name); if (at >= 0) Field<T>(obj, at) = v;
}
FVector ActorPos(UObject* actor) {
    auto root = Field<UObject*>(actor, off::Actor_RootComponent);
    return IsValid(root) ? Field<FVector>(root, off::Scene_C2W_Trans) : FVector{};
}
float ActorYaw(UObject* actor) {
    if (auto f = Fn(actor, "K2_GetActorRotation")) { Params p(f); Call(actor, "K2_GetActorRotation", &p); return p.Get<FRotator>("ReturnValue").Yaw; }
    return 0;
}
UObject* Controller(UObject* pawn) { auto c = Field<UObject*>(pawn, off::Pawn_Controller); return IsValid(c) ? c : nullptr; }
UObject* Movement(UObject* pawn) { auto m = Prop<UObject*>(pawn, "CharacterMovement"); return IsValid(m) ? m : nullptr; }
UObject* AnimInstance(UObject* pawn) {
    UObject* mesh = Prop<UObject*>(pawn, "Mesh"); if (!IsValid(mesh)) return nullptr;
    auto f = Fn(mesh, "GetAnimInstance"); if (!f) return nullptr;
    Params p(f); Call(mesh, "GetAnimInstance", &p); auto a = p.Get<UObject*>("ReturnValue"); return IsValid(a) ? a : nullptr;
}
UObject* CurrentMontage(UObject* pawn) {
    UObject* anim = AnimInstance(pawn); if (!anim) return nullptr;
    auto f = Fn(anim, "GetCurrentActiveMontage"); if (!f) return nullptr;
    Params p(f); Call(anim, "GetCurrentActiveMontage", &p); auto m = p.Get<UObject*>("ReturnValue"); return IsValid(m) ? m : nullptr;
}
void PlayMontage(UObject* pawn, UObject* montage) {
    UObject* anim = AnimInstance(pawn); if (!anim || !montage) return;
    if (auto f = Fn(anim, "Montage_Play")) { Params p(f); p.Set("MontageToPlay", montage); p.Set("InPlayRate", 1.f); Call(anim, "Montage_Play", &p); }
}
float GroundSpeed(UObject* pawn) {
    UObject* m = Movement(pawn); if (!m) return 0;
    const FVector v = Prop<FVector>(m, "Velocity"); return std::sqrt(v.X * v.X + v.Y * v.Y);
}
void Teleport(UObject* pawn, const FVector& at, float yaw) {
    if (auto f = Fn(pawn, "K2_SetActorLocationAndRotation")) {
        Params p(f); p.Set("NewLocation", at); p.Set("NewRotation", FRotator{0, yaw, 0}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true);
        Call(pawn, "K2_SetActorLocationAndRotation", &p);
    }
}
void Face(UObject* pawn, float yaw) {
    // Freakers turn with their controller's rotation (bUseControllerRotationYaw).
    if (UObject* c = Controller(pawn)) if (auto f = Fn(c, "SetControlRotation")) { Params p(f); p.Set("NewRotation", FRotator{0, yaw, 0}); Call(c, "SetControlRotation", &p); }
}

std::set<UObject*> VisibleAI() {
    std::set<UObject*> out; UObject* cls = FindClass("BendAIPawn"); if (!cls) return out;
    for (int i = 0, n = NumObjects(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (o && !(o->ObjectFlags & 0x10) && IsValid(o) && IsA(o, cls) && !GetBoolProp(o, "bHidden")) out.insert(o);
    }
    return out;
}

void Step(UObject* pawn, Driven& d, double now, float dt) {
    if (d.source) {
        if (!IsValid(d.source)) { d.source = nullptr; return; }
        d.samples.push_back({now, ActorPos(d.source), ActorYaw(d.source), GroundSpeed(d.source), CurrentMontage(d.source)});
        while (d.samples.size() > 2 && d.samples[1].t <= now - d.delay) d.samples.pop_front();
        const Sample& s = d.samples.front();
        d.to = {s.pos.X + d.offset.X, s.pos.Y + d.offset.Y, s.pos.Z + d.offset.Z}; d.yaw = s.yaw; d.speed = s.speed;
        if (s.montage != d.lastMontage) { d.lastMontage = s.montage; if (s.montage) PlayMontage(pawn, s.montage); }
    }
    const FVector at = ActorPos(pawn);
    const float dx = d.to.X - at.X, dy = d.to.Y - at.Y, dist = std::sqrt(dx * dx + dy * dy);
    if (dist > kTeleportDistance || std::fabs(d.to.Z - at.Z) > 400.f) { Teleport(pawn, d.to, d.yaw); return; }
    UObject* c = Controller(pawn);
    if (dist < kArriveDistance) { Face(pawn, d.yaw); return; }
    if (!c) return;
    // The game's own locomotion: a straight move order (no pathfinding) at one of the AI's speeds. Bend's
    // movement is animation driven and ignores plain movement input until the behaviour tree has run once.
    // Behind by more than 2 m: one speed faster to catch up.
    const float want = d.speed + (dist > 200.f ? 120.f : 0.f);
    const int speed = want < 235 ? 0 : want < 370 ? 1 : want < 430 ? 2 : want < 475 ? 3 : 4;  // EAIMoveSpeed Walk..Sprint
    if (speed != d.moveSpeed) {
        d.moveSpeed = speed;
        if (auto f = Fn(c, "SetMoveSpeed")) { Params p(f); p.Set<uint8_t>("moveType", uint8_t(speed)); Call(c, "SetMoveSpeed", &p); }
    }
    const float ox = d.to.X - d.ordered.X, oy = d.to.Y - d.ordered.Y;
    if (now - d.orderedAt > 0.5 || ox * ox + oy * oy > 50.f * 50.f) {
        d.ordered = d.to; d.orderedAt = now;
        if (auto f = Fn(c, "MoveToLocation")) {
            Params p(f); p.Set("Dest", d.to); p.Set("AcceptanceRadius", kArriveDistance); p.SetBool("bStopOnOverlap", false);
            p.SetBool("bUsePathfinding", false); p.SetBool("bProjectDestinationToNavigation", false); p.SetBool("bCanStrafe", false);
            p.SetBool("bAllowPartialPath", true);
            Call(c, "MoveToLocation", &p);
        }
    }
    (void)dt;
}
}  // namespace

bool Spawn(const std::string& faction, const FVector& at, float yaw, bool chase, std::string* error, int count, UObject* crowdGroup) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    int want = -1;
    for (const auto& f : kFactions) if (_stricmp(f.name, faction.c_str()) == 0) want = f.value;
    if (want < 0) { char* end = nullptr; const long v = std::strtol(faction.c_str(), &end, 10); if (end && !*end) want = int(v); }
    if (want < 0) return fail("unknown faction");
    UObject* gen = FindRuntimeInstance("BendAmbientGenerator"); if (!gen) return fail("no ambient generator");
    const int typesAt = PropOffset(gen, "PooledSpawnTypes"); if (typesAt < 0) return fail("no PooledSpawnTypes");
    struct A { uint8_t* d; int32_t n, m; }; const A types = Field<A>(gen, typesAt);
    const uint8_t* type = nullptr;
    for (int i = 0; i < types.n; ++i) {
        const uint8_t* t = types.d + size_t(i) * kSpawnTypeSize;
        if (t[kSpawnType_Faction] == want && *reinterpret_cast<UObject* const*>(t + kSpawnType_LoadedPawn)) { type = t; break; }
    }
    if (!type) return fail("this faction is not loaded here (the generator has no pawn class for it)");
    UObject* fn = Fn(gen, "SpawnForArcadeMode"); if (!fn) return fail("no SpawnForArcadeMode");
    alignas(16) uint8_t buf[0x600] = {};
    auto off = [&](const char* name) { UObject* p = FindProperty(fn, name); return p ? Field<int32_t>(p, off::UProperty_Offset) : -1; };
    std::memcpy(buf + off("SpawnType"), type, kSpawnTypeSize);  // shallow: the callee copies what it keeps
    *reinterpret_cast<FVector*>(buf + off("Location")) = at;
    *reinterpret_cast<FRotator*>(buf + off("Rotation")) = FRotator{0, yaw, 0};
    *reinterpret_cast<int32_t*>(buf + off("Count")) = count < 1 ? 1 : count;
    *reinterpret_cast<UObject**>(buf + off("Group")) = crowdGroup;
    *reinterpret_cast<float*>(buf + off("DistanceToDespawn")) = 1e7f;
    buf[off("bForceChasePlayer")] = chase ? 1 : 0;
    buf[off("bCanDespawnByInactivity")] = 0;
    PendingSpawn ps{Clock() + 6, at, *reinterpret_cast<UObject* const*>(type + kSpawnType_LoadedPawn), VisibleAI(), nextSpawnTag};
    ProcessEvent(gen, fn, buf);
    const bool ok = buf[off("ReturnValue")] != 0;
    dg::Log("zombies: SpawnForArcadeMode %s at (%.0f %.0f %.0f) -> %d", faction.c_str(), at.X, at.Y, at.Z, ok);
    if (!ok) return fail("the generator refused the spawn");
    pending.push_back(std::move(ps));
    return true;
}

void SetBrain(UObject* pawn, bool on) {
    UObject* c = IsValid(pawn) ? Controller(pawn) : nullptr; if (!c) return;
    UObject* brain = Prop<UObject*>(c, "BrainComponent"); if (!IsValid(brain)) return;
    if (on) Call(brain, "RestartLogic");
    else if (auto f = Fn(brain, "StopLogic")) {
        static std::wstring reason = L"dgmp"; FString s{reason.c_str(), int32_t(reason.size() + 1), int32_t(reason.size() + 1)};
        Params p(f); p.Set("Reason", s); Call(brain, "StopLogic", &p);
    }
}
void Drive(UObject* pawn, const FVector& to, float yaw) { if (!IsValid(pawn)) return; auto& d = driven[pawn]; d.source = nullptr; d.to = to; d.yaw = yaw; }
void Mirror(UObject* source, UObject* puppet, float delay, const FVector& offset) {
    if (!IsValid(source) || !IsValid(puppet)) return;
    auto& d = driven[puppet]; d.source = source; d.delay = delay; d.offset = offset; d.samples.clear(); d.lastMontage = nullptr;
}
void Release(UObject* pawn) { driven.erase(pawn); }
float Health(UObject* pawn) { return IsValid(pawn) && PropOffset(pawn, "Health") >= 0 ? Prop<float>(pawn, "Health") : -1.f; }
void Kill(UObject* pawn) {
    if (!IsValid(pawn)) return;
    UObject* lib = FindObject("/Script/Engine.Default__GameplayStatics"); UObject* f = lib ? Fn(lib, "ApplyDamage") : nullptr; if (!f) return;
    Params p(f); p.Set("DamagedActor", pawn); p.Set("BaseDamage", 100000.f); Call(lib, "ApplyDamage", &p);
}
UObject* LastSpawned() { return IsValid(lastSpawned) ? lastSpawned : nullptr; }
std::string Describe() {
    std::string out;
    for (auto& [pawn, d] : driven) {
        char b[200]; std::snprintf(b, sizeof(b), "%s -> (%.0f %.0f %.0f)%s\n", GetName(pawn).c_str(), d.to.X, d.to.Y, d.to.Z, d.source ? " mirror" : "");
        out += b;
    }
    return out.empty() ? "nothing driven" : out;
}

void Spawned(uint16_t tag, UObject* pawn);
void SpawnFailed(uint16_t tag);
bool Claimed(UObject* pawn);
bool IsTargetPawn(UObject* pawn);

void Update(double now, float dt, UObject* localPawn) {
    (void)localPawn;
    if (!pending.empty() && now >= nextPendingScan) {
        nextPendingScan = now + 0.25;
        const auto visible = VisibleAI();
        for (auto it = pending.begin(); it != pending.end();) {
            UObject* found = nullptr;
            for (UObject* o : visible) {
                if (it->before.count(o) || (it->pawnClass && !IsA(o, it->pawnClass)) || Claimed(o)) continue;
                const FVector p = ActorPos(o); const float dx = p.X - it->at.X, dy = p.Y - it->at.Y;
                if (dx * dx + dy * dy < 1500.f * 1500.f) { found = o; break; }
            }
            if (found) { lastSpawned = found; dg::Log("zombies: spawned %s", GetName(found).c_str()); const uint16_t tag = it->tag; it = pending.erase(it); if (tag) Spawned(tag, found); }
            else if (now > it->until) { dg::Log("zombies: spawn at (%.0f %.0f) produced no pawn", it->at.X, it->at.Y); const uint16_t tag = it->tag; it = pending.erase(it); if (tag) SpawnFailed(tag); }
            else ++it;
        }
    }
    for (auto it = driven.begin(); it != driven.end();) {
        if (!IsValid(it->first)) { it = driven.erase(it); continue; }
        Step(it->first, it->second, now, dt); ++it;
    }
}

// ---------------------------------------------------------------- multiplayer
namespace {
constexpr float kSpawnRange = 20000.f, kDespawnRange = 22000.f;  // 200 m: a zombie exists here only near this player
constexpr float kPuppetHealth = 100000.f;  // puppets never die from local hits: the hits go to the owner
constexpr double kReportInterval = 0.1;

struct Zed {
    std::string type;
    FVector pos{}; float yaw = 0, speed = 0, health = -1;
    int32_t world = 0;
    uint16_t owner = 0xFFFF;
    bool dead = false;
    uint8_t montageSeq = 0; std::string montage;  // the owner's (received) or ours (sent)
    UObject* lastMontage = nullptr;               // owner: the montage object montageSeq counts
    UObject* pawn = nullptr;
    bool spawning = false; double retryAt = 0;
    int brain = -1;                               // -1 not set up yet, 0 puppet, 1 our AI runs it
    bool diedSent = false;
    double puppetSince = 0;                       // health changes right after becoming a puppet are resets, not hits
    UObject* target = nullptr; double targetAt = 0;  // owner: the pawn its AI was last told to hunt (ours or a target pawn)
};
std::map<uint16_t, Zed> zeds;
uint16_t myId = 0xFFFF;
double nextReport = 0;

void Despawn(UObject* pawn) {
    if (!IsValid(pawn)) return;
    Release(pawn);
    UObject* gen = FindRuntimeInstance("BendAmbientGenerator");
    UObject* c = Controller(pawn);
    if (gen && c) if (auto f = Fn(gen, "DespawnUnit")) {
        Params p(f); p.Set("Controller", c); p.SetBool("notifyDespawn", true); p.SetBool("runFullDespawn", true); Call(gen, "DespawnUnit", &p); return;
    }
    Call(pawn, "K2_DestroyActor");
}
void ApplyDamage(UObject* pawn, float damage) {
    UObject* lib = FindObject("/Script/Engine.Default__GameplayStatics"); UObject* f = lib ? Fn(lib, "ApplyDamage") : nullptr;
    if (!f || !IsValid(pawn)) return;
    Params p(f); p.Set("DamagedActor", pawn); p.Set("BaseDamage", damage); Call(lib, "ApplyDamage", &p);
}
void SetPuppetHealth(UObject* pawn) { SetProp<float>(pawn, "Health", kPuppetHealth); }
void Montage(UObject* pawn, const std::string& path) {
    if (path.empty()) return;
    UObject* m = FindObject(path.c_str()); if (!m) m = LoadAsset(path.c_str());
    if (m) PlayMontage(pawn, m);
}
void BePuppet(Zed& z) {
    SetBrain(z.pawn, false); SetPuppetHealth(z.pawn); z.brain = 0; z.puppetSince = Clock(); z.target = nullptr;
    Drive(z.pawn, z.pos, z.yaw);
}
void BeOwned(Zed& z) {
    Release(z.pawn); SetBrain(z.pawn, true); z.brain = 1;
    // The last health the previous owner reported; none (or nonsense): a fresh one.
    const float full = Prop<float>(z.pawn, "MaxHealth", 200.f);
    SetProp<float>(z.pawn, "Health", z.health > 0 && z.health <= full ? z.health : full);
}
}  // namespace

void BoolCall(UObject* o, const char* fn, const char* param, bool v) {
    if (auto f = IsValid(o) ? Fn(o, fn) : nullptr) { Params p(f); p.SetBool(param, v); Call(o, fn, &p); }
}
// The AI sweep (SuppressAI) may have hidden this pool pawn and stopped its ticks in an earlier life.
void Revive(UObject* pawn) {
    BoolCall(pawn, "SetActorHiddenInGame", "bNewHidden", false);
    BoolCall(pawn, "SetActorEnableCollision", "bNewActorEnableCollision", true);
    BoolCall(pawn, "SetActorTickEnabled", "bEnabled", true);
    BoolCall(Controller(pawn), "SetActorTickEnabled", "bEnabled", true);
}

void Spawned(uint16_t tag, UObject* pawn) {
    auto it = zeds.find(tag);
    if (it == zeds.end() || it->second.dead) { Despawn(pawn); return; }
    Revive(pawn);
    Zed& z = it->second; z.spawning = false; z.pawn = pawn; z.brain = -1;
    dg::Log("zombies: zombie %u is %s here", unsigned(tag), GetName(pawn).c_str());
}
void SpawnFailed(uint16_t tag) { if (auto it = zeds.find(tag); it != zeds.end()) { it->second.spawning = false; it->second.retryAt = Clock() + 5; } }

bool Claimed(UObject* pawn) {
    if (driven.count(pawn) || IsTargetPawn(pawn)) return true;
    for (auto& [id, z] : zeds) if (z.pawn == pawn) return true;
    return false;
}
bool IsOurs(UObject* pawn) {
    if (Claimed(pawn)) return true;
    // Not claimed yet (Update finds new pawns every 0.25 s): anything showing up at a spawn we asked for.
    for (const auto& ps : pending) {
        if (ps.pawnClass && !IsA(pawn, ps.pawnClass)) continue;
        const FVector p = ActorPos(pawn); const float dx = p.X - ps.at.X, dy = p.Y - ps.at.Y;
        if (dx * dx + dy * dy < 1500.f * 1500.f) return true;
    }
    return false;
}
bool Active() { return !zeds.empty(); }

// ---------------------------------------------------------------- other players as targets
// The game's AI only perceives pawns; other players are drawn as skeletal mesh actors. So that the zombies
// this client runs also hunt the players near them (not just us), every such player gets an invisible
// target pawn glued to its body here: a human pawn of the player's faction with its AI frozen, a borrowed
// human skeleton to be hit on, and health we watch. Zombies nearer to that player than to us get it as
// their target: their own target picking runs every frame, so the blackboard is overwritten every frame
// (the approach DGCoop proved: freakers, wolves and marauders attack the target pawn). What the target
// pawn loses goes to the server as that player's damage (ZombieAttack).
namespace {
constexpr float kDecoyHealth = 100000.f;
constexpr float kTargetRange = 3000.f;    // a zombie goes for another player within 30 m (when nearer than us)
constexpr float kDecoyRange = 8000.f;     // target pawns exist for players within 80 m of a zombie we run
constexpr float kDecoyHitMax = 250.f;

struct Decoy { UObject* pawn = nullptr; FVector pos{}; bool alive = false; };
std::map<uint16_t, Decoy> decoys;  // by player id
UObject* borrowedHumanMesh = nullptr;

UObject* StaticsObj() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
UObject* SpawnPlain(UObject* cls, const FVector& at, UObject* context) {
    UObject* st = StaticsObj(); if (!cls || !st) return nullptr;
    FTransform t; t.Translation = at;
    Params begin(Fn(st, "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(st, "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue"); if (!actor) return nullptr;
    Params finish(Fn(st, "FinishSpawningActor")); finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(st, "FinishSpawningActor", &finish);
    return actor;
}
UObject* HumanMesh() {
    if (IsValid(borrowedHumanMesh)) return borrowedHumanMesh;
    UObject* cls = FindClass("BendHumanPawn"); if (!cls) return nullptr;
    for (int i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || (o->ObjectFlags & 0x10) || !IsValid(o) || !IsA(o, cls) || IsTargetPawn(o)) continue;
        UObject* mesh = Prop<UObject*>(o, "Mesh");
        if (IsValid(mesh)) if (UObject* sk = Field<UObject*>(mesh, off::Skinned_SkeletalMesh)) return borrowedHumanMesh = sk;
    }
    return nullptr;
}
void DestroyDecoy(Decoy& d) {
    if (IsValid(d.pawn)) { if (UObject* c = Controller(d.pawn)) Call(c, "K2_DestroyActor"); Call(d.pawn, "K2_DestroyActor"); }
    d.pawn = nullptr;
}
UObject* MakeDecoy(const FVector& at, UObject* localPawn) {
    UObject* cls = FindClass("HumanBasePawn_C"); if (!cls) cls = FindClass("SurvivorPawn_C");
    UObject* pawn = SpawnPlain(cls, at, localPawn);
    if (!pawn) { dg::Log("zombies: target pawn: spawn failed (%s)", cls ? GetName(cls).c_str() : "no human pawn class loaded"); return nullptr; }
    // The player's faction: enemies treat it as "a player".
    const uint8_t faction = Prop<uint8_t>(localPawn, "pawnDefaultFaction");
    SetProp<uint8_t>(pawn, "pawnDefaultFaction", faction);
    UObject* c = Controller(pawn);
    if (!c) if ((c = SpawnPlain(FindClass("BendHumanController"), at, localPawn))) { Params p(Fn(c, "Possess")); p.Set("InPawn", pawn); Call(c, "Possess", &p); c = Controller(pawn); }
    if (c) {
        if (auto f = Fn(c, "SetAIFactionType")) { Params p(f); p.Set<uint8_t>("eAIType", faction); Call(c, "SetAIFactionType", &p); }
        if (UObject* brain = Prop<UObject*>(c, "BrainComponent"); IsValid(brain)) {
            if (auto f = Fn(brain, "StopLogic")) { static std::wstring why = L"dgmp target"; FString s{why.c_str(), int32_t(why.size() + 1), int32_t(why.size() + 1)}; Params p(f); p.Set("Reason", s); Call(brain, "StopLogic", &p); }
            BoolCall(brain, "SetComponentTickEnabled", "bEnabled", false);
        }
        if (UObject* path = Prop<UObject*>(c, "PathFollowingComponent"); IsValid(path)) BoolCall(path, "SetComponentTickEnabled", "bEnabled", false);
        BoolCall(c, "SetActorTickEnabled", "bEnabled", false);
    }
    if (UObject* m = Movement(pawn)) BoolCall(m, "SetComponentTickEnabled", "bEnabled", false);
    if (auto f = Fn(pawn, "SetMaxHealth")) { Params p(f); p.Set("NewMaxHealth", kDecoyHealth); Call(pawn, "SetMaxHealth", &p); }
    SetProp<float>(pawn, "Health", kDecoyHealth);
    // Humans aim and hit through skeleton bodies: borrow a human NPC's skeleton, then hide the mesh.
    if (UObject* mesh = Prop<UObject*>(pawn, "Mesh"); IsValid(mesh)) {
        if (UObject* sk = HumanMesh()) if (auto f = Fn(mesh, "SetSkeletalMesh")) { Params p(f); p.Set("NewMesh", sk); Call(mesh, "SetSkeletalMesh", &p); }
        BoolCall(mesh, "SetHiddenInGame", "NewHidden", true);
        if (auto f = Fn(mesh, "SetVisibility")) { Params p(f); p.SetBool("bNewVisibility", false); p.SetBool("bPropagateToChildren", true); Call(mesh, "SetVisibility", &p); }
    }
    dg::Log("zombies: target pawn %s (faction %d, controller %s)", GetName(pawn).c_str(), int(faction), c ? GetName(c->Class).c_str() : "none");
    return pawn;
}
UObject* Blackboard(UObject* c) { UObject* bb = c ? Prop<UObject*>(c, "Blackboard") : nullptr; return IsValid(bb) ? bb : nullptr; }
void BBObject(UObject* bb, const char* key, UObject* v) {
    const FName k = FindName(key); auto f = Fn(bb, "SetValueAsObject"); if (k.Index < 0 || !f) return;
    Params p(f); p.Set("KeyName", k); p.Set("ObjectValue", v); Call(bb, "SetValueAsObject", &p);
}
void BBBool(UObject* bb, const char* key, bool v) {
    const FName k = FindName(key); auto f = Fn(bb, "SetValueAsBool"); if (k.Index < 0 || !f) return;
    Params p(f); p.Set("KeyName", k); p.SetBool("BoolValue", v); Call(bb, "SetValueAsBool", &p);
}
void BBEnum(UObject* bb, const char* key, uint8_t v) {
    const FName k = FindName(key); auto f = Fn(bb, "SetValueAsEnum"); if (k.Index < 0 || !f) return;
    Params p(f); p.Set("KeyName", k); p.Set("EnumValue", v); Call(bb, "SetValueAsEnum", &p);
}
float Dist2(const FVector& a, const FVector& b) { const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z; return dx * dx + dy * dy + dz * dz; }

// Verified in game: the blackboard alone left a swarmer wandering; BendAIController.SetAwarenessOfPawn +
// ForceAware made it run to the target pawn and hit it. Freaker blackboards have FireFocus, Focus and
// Awareness (EAIAwareState: 2 Aware); ForceAttack / TargetIsPlayer exist for humans only.
void Aware(UObject* c, UObject* target) {
    if (auto f = Fn(c, "SetAwarenessOfPawn")) { Params p(f); p.Set("awarePawn", target); p.Set("AwarenessLevel", 1.f); Call(c, "SetAwarenessOfPawn", &p); }
    if (auto f = Fn(c, "ForceAware")) { Params p(f); p.Set("forceFocus", target); Call(c, "ForceAware", &p); }
}
void Target(Zed& z, UObject* target, bool decoy, double now) {
    UObject* c = Controller(z.pawn); if (!c) return;
    if (target != z.target || now - z.targetAt > 2.0) { z.target = target; z.targetAt = now; Aware(c, target); }
    if (!decoy) return;  // our own player: the AI's own target picking does the rest
    UObject* bb = Blackboard(c); if (!bb) return;
    BBObject(bb, "FireFocus", target); BBObject(bb, "Focus", target);
    BBBool(bb, "ForceAttack", true); BBBool(bb, "TargetIsPlayer", true);
    BBEnum(bb, "Awareness", 2);  // Aware
    if (auto f = Fn(c, "SetFireFocus")) { Params p(f); p.Set("FireFocus", target); Call(c, "SetFireFocus", &p); }
}

// Every frame on the client that runs zombies: target pawns for the players near them, their damage, and
// the zombies' choice between us and them.
void UpdateTargets(UObject* localPawn, net::Client& client, double now) {
    std::vector<std::pair<uint16_t, UObject*>> mine;  // zombie id, pawn: the ones our AI runs
    for (auto& [id, z] : zeds) if (z.brain == 1 && !z.dead && IsValid(z.pawn) && Health(z.pawn) > 0) mine.push_back({id, z.pawn});
    std::map<uint16_t, bool> keep;
    if (!mine.empty()) for (const auto& b : mp::RemoteBodies()) {
        if (!b.alive) continue;
        bool nearAny = false;
        for (auto& [id, pawn] : mine) if (Dist2(ActorPos(pawn), b.pos) < kDecoyRange * kDecoyRange) { nearAny = true; break; }
        if (!nearAny) continue;
        keep[b.id] = true;
        Decoy& d = decoys[b.id]; d.pos = b.pos; d.alive = true;
        if (d.pawn && !IsValid(d.pawn)) d.pawn = nullptr;
        const FVector at{b.pos.X, b.pos.Y, b.pos.Z + 90.f};  // capsule centre over the feet
        if (!d.pawn && !(d.pawn = MakeDecoy(at, localPawn))) continue;
        if (auto f = Fn(d.pawn, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", at); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(d.pawn, "K2_SetActorLocation", &p); }
        const float hp = Health(d.pawn);
        if (hp <= 0) { dg::Log("zombies: target pawn of player %u died; making a new one", unsigned(b.id)); DestroyDecoy(d); continue; }
        if (hp < kDecoyHealth - 0.01f) {
            const float dmg = std::fmin(kDecoyHealth - hp, kDecoyHitMax);
            SetProp<float>(d.pawn, "Health", kDecoyHealth);
            // Credited to the zombie of ours nearest to that player.
            uint16_t by = 0; float best = 1e30f;
            for (auto& [id, pawn] : mine) { const float dd = Dist2(ActorPos(pawn), at); if (dd < best) { best = dd; by = id; } }
            if (by) { net::Writer w; w.Op(net::Msg::ZombieAttack); w.U16(by); w.U16(b.id); w.F32(dmg); client.SendZombie(w, true); dg::Log("zombies: zombie %u hit player %u for %.0f", unsigned(by), unsigned(b.id), dmg); }
        }
    }
    for (auto it = decoys.begin(); it != decoys.end();) {
        if (!keep.count(it->first)) { DestroyDecoy(it->second); it = decoys.erase(it); } else ++it;
    }
    const FVector me = IsValid(localPawn) ? ActorPos(localPawn) : FVector{1e9f, 1e9f, 1e9f};
    for (auto& [id, pawn] : mine) {
        Zed& z = zeds[id];
        const FVector at = ActorPos(pawn);
        const float dMe = Dist2(at, me);
        UObject* best = nullptr; float bestD = std::fmin(dMe, kTargetRange * kTargetRange);
        for (auto& [pid, d] : decoys) if (IsValid(d.pawn)) { const float dd = Dist2(at, d.pos); if (dd < bestD) { bestD = dd; best = d.pawn; } }
        if (best) Target(z, best, true, now);
        else if (z.target && z.target != localPawn) Target(z, localPawn, false, now);  // back to us
    }
}
}  // namespace

bool IsTargetPawn(UObject* pawn) { for (auto& [id, d] : decoys) if (d.pawn == pawn) return true; return false; }

void Reset() {
    for (auto& [id, d] : decoys) DestroyDecoy(d);
    decoys.clear();
    for (auto& [id, z] : zeds) if (IsValid(z.pawn)) Despawn(z.pawn);
    zeds.clear(); myId = 0xFFFF;
}

void OnMessage(net::Msg op, const uint8_t* data, size_t size) {
    net::Reader r(data, size);
    if (op == net::Msg::ZombieMoves) {
        const int n = r.U8();
        for (int i = 0; i < n && r.ok(); ++i) {
            const uint16_t id = r.U16(); const float x = r.F32(), y = r.F32(), zz = r.F32(), yaw = r.F32(), speed = r.F32(), health = r.F32();
            const uint8_t seq = r.U8(); const std::string montage = r.Str(net::kZombieMontageMax);
            if (!r.ok()) break;
            auto it = zeds.find(id); if (it == zeds.end()) continue;
            Zed& z = it->second; if (z.owner == myId) continue;  // stale: we run it now
            z.pos = {x, y, zz}; z.yaw = yaw; z.speed = speed; z.health = health;
            if (seq != z.montageSeq) { z.montageSeq = seq; z.montage = montage; if (IsValid(z.pawn) && z.brain == 0) Montage(z.pawn, montage); }
            if (IsValid(z.pawn) && z.brain == 0) { Drive(z.pawn, z.pos, z.yaw); driven[z.pawn].speed = speed; }
        }
        return;
    }
    const uint16_t id = r.U16(); const auto zop = net::ZombieOp(r.U8());
    if (!r.ok()) return;
    if (zop == net::ZombieOp::Add) {
        Zed& z = zeds[id];
        z.type = r.Str(net::kZombieTypeMax); z.pos.X = r.F32(); z.pos.Y = r.F32(); z.pos.Z = r.F32(); z.yaw = r.F32();
        z.world = r.I32(); z.owner = r.U16(); z.health = r.F32();
        return;
    }
    auto it = zeds.find(id); if (it == zeds.end()) return;
    Zed& z = it->second;
    if (zop == net::ZombieOp::Owner) { z.owner = r.U16(); return; }
    if (zop == net::ZombieOp::Damage) {
        const float damage = r.F32();
        if (r.ok() && z.brain == 1 && IsValid(z.pawn)) ApplyDamage(z.pawn, damage);
        return;
    }
    if (zop == net::ZombieOp::Died) {
        z.dead = true;
        // The puppet dies the game's way (ragdoll, body) where it stands.
        if (IsValid(z.pawn) && z.brain != 1) { Release(z.pawn); SetProp<float>(z.pawn, "Health", 1.f); Kill(z.pawn); }
        return;
    }
    if (zop == net::ZombieOp::Remove) { if (IsValid(z.pawn)) Despawn(z.pawn); zeds.erase(it); }
}

void Net(double now, UObject* localPawn, net::Client& client, int world) {
    myId = client.localId();
    if (zeds.empty() || !IsValid(localPawn)) { for (auto& [id, d] : decoys) DestroyDecoy(d); decoys.clear(); return; }
    const FVector me = ActorPos(localPawn);
    net::Writer report; int reported = 0;
    const bool reportNow = now >= nextReport;
    if (reportNow) nextReport = now + kReportInterval;
    for (auto& [id, z] : zeds) {
        if (z.pawn && !IsValid(z.pawn)) { z.pawn = nullptr; z.brain = -1; }
        const bool mine = z.owner == myId;
        const FVector at = z.pawn ? ActorPos(z.pawn) : z.pos;
        const float dx = at.X - me.X, dy = at.Y - me.Y, d2 = dx * dx + dy * dy;
        const bool inRange = z.world == world && d2 < (z.pawn ? kDespawnRange * kDespawnRange : kSpawnRange * kSpawnRange);
        if (!inRange) { if (z.pawn && !z.dead) { Despawn(z.pawn); z.pawn = nullptr; z.brain = -1; } continue; }
        if (z.dead) continue;
        if (!z.pawn) {
            if (!z.spawning && now >= z.retryAt) {
                nextSpawnTag = id; std::string error;
                z.spawning = Spawn(z.type, z.pos, z.yaw, false, &error);
                nextSpawnTag = 0;
                if (!z.spawning) { z.retryAt = now + 10; dg::Log("zombies: zombie %u (%s) cannot spawn here: %s", unsigned(id), z.type.c_str(), error.c_str()); }
            }
            continue;
        }
        if (mine && z.brain != 1) BeOwned(z);
        else if (!mine && z.brain != 0) BePuppet(z);
        if (mine) {
            const float health = Health(z.pawn);
            if (health <= 0) {
                if (!z.diedSent) { z.diedSent = true; net::Writer w; w.Op(net::Msg::ZombieDied); w.U16(id); client.SendZombie(w, true); }
                continue;
            }
            if (!reportNow || reported >= int(net::kZombiesPerPacket)) continue;
            UObject* montage = CurrentMontage(z.pawn);
            if (montage != z.lastMontage) { z.lastMontage = montage; if (montage) { ++z.montageSeq; z.montage = FullName(montage); } }
            if (!reported) { report.Op(net::Msg::ZombieStates); report.U8(0); }
            report.U16(id); report.F32(at.X); report.F32(at.Y); report.F32(at.Z); report.F32(ActorYaw(z.pawn)); report.F32(GroundSpeed(z.pawn)); report.F32(health);
            report.U8(z.montageSeq); report.Str(montage ? z.montage : std::string());
            z.pos = at; z.health = health;
            ++reported;
        } else {
            // A local hit on the puppet: its damage goes to the owner, the puppet is healed back.
            // The game resets a fresh pool pawn's health once after the spawn: a drop that large is no hit.
            const float health = Health(z.pawn);
            if (health >= 0 && health < kPuppetHealth - 0.5f) {
                const float hit = kPuppetHealth - health;
                // The game resets a fresh pool pawn's health (seen: to 160, 80000) in its first moments.
                const float plausible = std::fmax(1000.f, 5 * Prop<float>(z.pawn, "MaxHealth", 200.f));
                if (Clock() - z.puppetSince > 1.5 && hit <= plausible) { net::Writer w; w.Op(net::Msg::ZombieHit); w.U16(id); w.F32(hit); client.SendZombie(w, true); dg::Log("zombies: hit zombie %u (puppet) for %.0f", unsigned(id), hit); }
                SetPuppetHealth(z.pawn);
            }
        }
    }
    if (reported) { report.buf[1] = uint8_t(reported); client.SendZombie(report, false); }
    UpdateTargets(localPawn, client, now);
}

}  // namespace zombies
