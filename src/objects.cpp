#include "objects.h"
#include "worldedit.h"
#include "log.h"
#include <cmath>
#include <map>

using namespace ue;

namespace objects {
namespace {
constexpr float kSpawnRange = 40000.f;   // 400 m, as the mode's props of world.json
constexpr float kDespawnRange = 42000.f;

struct Obj {
    net::Client::Events::ObjectInfo info;
    float from[6]{}, to[6]{};              // x, y, z, pitch, yaw, roll
    double moveStart = 0, moveTime = 0;    // local clock; moveTime 0: standing at `from`
    UObject* actor = nullptr;
    bool failed = false;                   // the asset does not load: not tried again
    bool collisionOff = false;
    bool hidden = false;                   // a ghost's blink
};
std::map<uint16_t, Obj> objs;
bool outlineShown = false;                 // the game's highlight post process is on for our ghosts

// A ghost is drawn with the game's survival-vision outline (custom depth stencil 2, shown by the controller's
// FocusPostHighlightOnly post process) and blinks, so that it reads as "here it would go", not as the thing.
void Outline(UObject* actor, bool on) {
    static UObject* primitive = nullptr;
    if (!primitive) primitive = FindClass("PrimitiveComponent");
    auto f = IsValid(actor) && primitive ? Fn(actor, "GetComponentsByClass") : nullptr;
    if (!f) return;
    Params p(f); p.Set("ComponentClass", primitive); Call(actor, "GetComponentsByClass", &p);
    struct Raw { UObject** data; int num, max; };
    const Raw list = p.Get<Raw>("ReturnValue");
    for (int i = 0; i < list.num && i < 64; ++i) {
        UObject* c = list.data[i];
        if (!IsValid(c)) continue;
        if (auto g = Fn(c, "SetRenderCustomDepth")) { Params q(g); q.SetBool("bValue", on); Call(c, "SetRenderCustomDepth", &q); }
        if (auto g = Fn(c, "SetCustomDepthStencilValue")) { Params q(g); q.Set<int>("Value", on ? 2 : 0); Call(c, "SetCustomDepthStencilValue", &q); }
    }
}
void ShowOutlines(UObject* pawn, bool on) {
    if (on == outlineShown || !IsValid(pawn)) return;
    UObject* controller = Field<UObject*>(pawn, off::Pawn_Controller);
    UObject* post = nullptr;
    if (IsValid(controller)) if (auto prop = FindProperty(controller->Class, "FocusPostHighlightOnly")) post = Field<UObject*>(controller, Field<int>(prop, off::UProperty_Offset));
    if (IsValid(post)) { SetBoolProp(post, "bEnabled", on); outlineShown = on; }
}
int myWorld = 0;

double Clock() { return GetTickCount64() / 1000.0; }

void Current(const Obj& o, double now, float out[6]) {
    const float k = o.moveTime > 0 ? float(std::fmin(1.0, std::fmax(0.0, (now - o.moveStart) / o.moveTime))) : 1.f;
    for (int i = 0; i < 6; ++i) {
        float d = o.to[i] - o.from[i];
        if (i >= 3) d = std::fmod(std::fmod(d + 180.f, 360.f) + 360.f, 360.f) - 180.f;
        out[i] = o.from[i] + d * k;
    }
}
void Despawn(Obj& o) {
    if (IsValid(o.actor)) Call(o.actor, "K2_DestroyActor");
    o.actor = nullptr;
}
}  // namespace

void OnObject(uint16_t id, const net::Client::Events::ObjectInfo* info) {
    if (!info) {
        auto it = objs.find(id);
        if (it != objs.end()) { Despawn(it->second); objs.erase(it); }
        return;
    }
    Obj& o = objs[id];
    const bool remake = o.info.asset != info->asset || o.info.collision != info->collision || o.info.ghost != info->ghost || std::fabs(o.info.scale - info->scale) > 0.0001f;
    o.info = *info;
    const float at[6] = {info->x, info->y, info->z, info->pitch, info->yaw, info->roll};
    for (int i = 0; i < 6; ++i) o.from[i] = o.to[i] = at[i];
    o.moveTime = 0;
    if (remake) { Despawn(o); o.failed = false; }
    else if (IsValid(o.actor)) worldedit::PlaceActor(o.actor, FVector{at[0], at[1], at[2]}, FRotator{at[3], at[4], at[5]}, o.info.scale);  // there at once
}

void OnMove(uint16_t id, const float to[6], float seconds, float elapsed) {
    auto it = objs.find(id);
    if (it == objs.end()) return;
    Obj& o = it->second;
    const double now = Clock();
    float at[6]; Current(o, now, at);
    for (int i = 0; i < 6; ++i) { o.from[i] = at[i]; o.to[i] = to[i]; }
    // Joined during the move: it set off `elapsed` seconds ago from where the add message put it.
    o.moveStart = now - elapsed; o.moveTime = seconds;
    if (elapsed > 0) {
        const float base[6] = {o.info.x, o.info.y, o.info.z, o.info.pitch, o.info.yaw, o.info.roll};
        for (int i = 0; i < 6; ++i) o.from[i] = base[i];
    }
    if (seconds <= 0) { for (int i = 0; i < 6; ++i) o.from[i] = to[i]; o.moveTime = 0; }
}

void SetWorld(int world) { myWorld = world; }

void Reset() {
    for (auto& [id, o] : objs) Despawn(o);
    objs.clear();
    outlineShown = false;  // (the controller goes with the session)
}

void Update(double, UObject* pawn) {
    if (!IsValid(pawn)) return;
    UObject* root = Field<UObject*>(pawn, off::Actor_RootComponent);
    if (!IsValid(root)) return;
    const FVector me = Field<FVector>(root, off::Scene_C2W_Trans);
    const double now = Clock();
    bool ghosts = false;
    for (auto& [id, o] : objs) {
        float at[6]; Current(o, now, at);
        const float dx = at[0] - me.X, dy = at[1] - me.Y, dz = at[2] - me.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        const bool here = o.info.world < 0 || o.info.world == myWorld;
        if (o.actor && !IsValid(o.actor)) o.actor = nullptr;  // streamed out with its level
        if (!here || d2 > kDespawnRange * kDespawnRange) { Despawn(o); continue; }
        if (!o.actor) {
            if (o.failed || d2 > kSpawnRange * kSpawnRange) continue;
            o.actor = worldedit::SpawnMesh(o.info.asset, FVector{at[0], at[1], at[2]}, FRotator{at[3], at[4], at[5]}, o.info.scale, pawn);
            if (!o.actor) { o.failed = true; dg::Log("Objects: cannot spawn %u (%s)", id, o.info.asset.c_str()); continue; }
            if (!o.info.collision) if (auto f = Fn(o.actor, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(o.actor, "SetActorEnableCollision", &p); }
            if (o.info.ghost) { Outline(o.actor, true); o.hidden = false; }
            continue;
        }
        if (o.info.ghost) {  // on 0.5 s, off 0.15 s
            ghosts = true;
            const bool hide = std::fmod(now, 0.65) > 0.5;
            if (hide != o.hidden) if (auto f = Fn(o.actor, "SetActorHiddenInGame")) { Params p(f); p.SetBool("bNewHidden", hide); Call(o.actor, "SetActorHiddenInGame", &p); o.hidden = hide; }
        }
        if (o.moveTime > 0) {
            worldedit::PlaceActor(o.actor, FVector{at[0], at[1], at[2]}, FRotator{at[3], at[4], at[5]}, o.info.scale);
            if (now >= o.moveStart + o.moveTime) { for (int i = 0; i < 6; ++i) o.from[i] = o.to[i]; o.moveTime = 0; }
        }
    }
    ShowOutlines(pawn, ghosts);
}

}  // namespace objects
