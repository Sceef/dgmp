#include "throws.h"
#include "worldedit.h"
#include "policy.h"
#include "log.h"
#include "shared/netclient.h"
#include <cmath>
#include <cstring>
#include <deque>
#include <set>

using namespace ue;

namespace throws {
namespace {
constexpr double kLie = 10;  // a copy lies this long (the game's rock projectile lives 10 s)
UObject* observedRock = nullptr;
UObject* localPawn = nullptr;  // the world copies are spawned in (Update)
double scanUntil = 0;
std::set<std::pair<int, int>> reported;  // projectiles already sent (index, serial)
// A copy flies on its own (the rock's mesh has no collision of its own: the game's projectile flies by a
// sphere, so physics would drop the mesh through the ground): gravity, a trace to the next spot, and on
// hitting something it lies there.
struct Copy { UObject* actor; double until; FVector at, vel; bool flying = true; double last = 0; };
std::deque<Copy> copies;

double Clock() { return GetTickCount64() / 1000.0; }
UObject* ObjectProp(UObject* o, const char* name) {
    for (auto c = IsValid(o) ? o->Class : nullptr; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return Field<UObject*>(o, Field<int>(p, off::UProperty_Offset));
    return nullptr;
}
template <class T> T* PropPtr(UObject* o, const char* name) {
    for (auto c = IsValid(o) ? o->Class : nullptr; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return &Field<T>(o, Field<int>(p, off::UProperty_Offset));
    return nullptr;
}
FVector Location(UObject* a) {
    FVector v{}; if (auto f = Fn(a, "K2_GetActorLocation")) { Params p(f); Call(a, "K2_GetActorLocation", &p); v = p.Get<FVector>("ReturnValue"); } return v;
}
// The rock this player threw: its mesh, scale, where and how fast (the movement's velocity, or its first
// speed along where it faces while that is not set yet).
void Report(UObject* proj, net::Client& client) {
    UObject* mesh = ObjectProp(proj, "ProjectileMesh");
    UObject* asset = ObjectProp(mesh, "StaticMesh");
    if (!IsValid(asset)) return;
    float scale = 1;
    if (FVector* s = PropPtr<FVector>(mesh, "RelativeScale3D")) scale = s->X;
    UObject* move = ObjectProp(proj, "ProjectileComp");
    FVector vel{};
    if (FVector* v = PropPtr<FVector>(move, "Velocity")) vel = *v;
    if (vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z < 1.f) {
        float speed = 1650; if (float* s = PropPtr<float>(move, "InitialSpeed")) speed = *s;
        FVector dir{1, 0, 0};
        if (auto f = Fn(proj, "GetActorForwardVector")) { Params p(f); Call(proj, "GetActorForwardVector", &p); dir = p.Get<FVector>("ReturnValue"); }
        vel = {dir.X * speed, dir.Y * speed, dir.Z * speed};
    }
    const FVector at = Location(proj);
    client.SendThrow(FullName(asset), scale, at.X, at.Y, at.Z, vel.X, vel.Y, vel.Z);
    dg::Log("Throw: %s from %.0f %.0f %.0f at %.0f %.0f %.0f", GetName(asset).c_str(), at.X, at.Y, at.Z, vel.X, vel.Y, vel.Z);
}
}  // namespace

void OnEvent(UObject* object, const std::string& name) {
    if (!observedRock || object != observedRock) return;
    if (name == "OnFired") scanUntil = Clock() + 0.6;  // (released: its projectile is about to exist)
}

namespace {
// Where a straight segment from a to b first hits the world (visibility), if it does.
bool Hit(UObject* context, const FVector& a, const FVector& b, FVector& at) {
    static UObject *kismet = nullptr, *statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = IsValid(kismet) ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, brk = IsValid(statics) ? Fn(statics, "BreakHitResult") : nullptr;
    if (!trace || !brk || !IsValid(context)) return false;
    Params t(trace); t.Set("WorldContextObject", context); t.Set("Start", a); t.Set("End", b);
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    if (!t.GetBool("ReturnValue") || !hitProp || !inProp) return false;
    Params h(brk);
    std::memcpy(static_cast<uint8_t*>(h.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &h);
    at = h.Get<FVector>("ImpactPoint");
    return true;
}
void Fly(Copy& c, double now) {
    const float dt = float(std::fmin(0.1, now - c.last)); c.last = now;
    if (!c.flying || dt <= 0) return;
    FVector next{c.at.X + c.vel.X * dt, c.at.Y + c.vel.Y * dt, c.at.Z + c.vel.Z * dt};
    c.vel.Z -= 980.f * dt;
    FVector hit;
    if (Hit(localPawn, c.at, next, hit)) { next = {hit.X, hit.Y, hit.Z + 4.f}; c.flying = false; c.until = now + kLie; }
    if (auto f = Fn(c.actor, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", next); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(c.actor, "K2_SetActorLocation", &p); }
    if (c.flying) if (auto f = Fn(c.actor, "K2_AddActorLocalRotation")) {  // tumbling through the air
        Params p(f); p.Set("DeltaRotation", FRotator{dt * 400.f, dt * 250.f, 0}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(c.actor, "K2_AddActorLocalRotation", &p);
    }
    c.at = next;
}
}  // namespace

void Update(double now, UObject* pawn, net::Client& client) {
    // Copies fly; those that have lain long enough go (one that never lands: after 8 s).
    for (auto& c : copies) if (IsValid(c.actor)) Fly(c, now);
    while (!copies.empty() && (now >= copies.front().until || !IsValid(copies.front().actor))) {
        if (IsValid(copies.front().actor)) Call(copies.front().actor, "K2_DestroyActor");
        copies.pop_front();
    }
    localPawn = pawn;
    if (!IsValid(pawn)) return;
    // The rock weapon of this pawn: its script events (OnFired) come to OnEvent.
    UObject* rock = ObjectProp(ObjectProp(pawn, "EquippableManagerComponent"), "RockWeapon");
    if (IsValid(rock) && rock != observedRock) { policy::Observe(rock); observedRock = rock; }
    if (now >= scanUntil) return;
    // Just thrown: its projectile, ours (instigated by this pawn), not reported yet.
    static UObject* cls = nullptr;
    if (!cls) cls = FindClass("Bend_BulletProj_Rock_C");
    if (!cls) return;
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || (o->ObjectFlags & 0x10) || !IsA(o, cls) || !IsValid(o) || ObjectProp(o, "Instigator") != pawn) continue;
        if (!reported.insert({o->InternalIndex, SerialAt(o->InternalIndex)}).second) continue;
        Report(o, client);
        scanUntil = 0;
    }
    if (reported.size() > 256) reported.clear();
}

void OnThrow(uint16_t player, const std::string& mesh, float scale, const FVector& at, const FVector& vel) {
    if (!IsValid(localPawn)) return;
    const float yaw = std::atan2(vel.Y, vel.X) * 57.2957795f;
    UObject* actor = worldedit::SpawnMesh(mesh, at, FRotator{0, yaw, 0}, scale > 0 ? scale : 1.f, localPawn);
    if (!actor) { dg::Log("Throw: cannot show %s of player %u", mesh.c_str(), player); return; }
    if (auto f = Fn(actor, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(actor, "SetActorEnableCollision", &p); }  // in no one's way
    Copy c; c.actor = actor; c.at = at; c.vel = vel; c.last = Clock(); c.until = c.last + 8.0;
    copies.push_back(c);
    dg::Log("Throw: player %u's %s at %.0f %.0f %.0f", player, mesh.substr(mesh.rfind('/') + 1).c_str(), at.X, at.Y, at.Z);
    while (copies.size() > 24) { if (IsValid(copies.front().actor)) Call(copies.front().actor, "K2_DestroyActor"); copies.pop_front(); }
}

void Reset() {
    for (auto& c : copies) if (IsValid(c.actor)) Call(c.actor, "K2_DestroyActor");
    copies.clear(); observedRock = localPawn = nullptr; scanUntil = 0; reported.clear();
}

}  // namespace throws
