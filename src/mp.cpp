#include "mp.h"
#include "diagnostics.h"
#include "hud.h"
#include "inventory.h"
#include "gameevents.h"
#include "worldedit.h"
#include "skins.h"
#include "mpui.h"
#include "vehicles.h"
#include "zombies.h"
#include "carry.h"
#include "objects.h"
#include "playeranim.h"
#include "blips.h"
#include "sounds.h"
#include "voice.h"
#include "players.h"
#include "images.h"
#include "webui.h"
#include "clientlua.h"
#include "traps.h"
#include "throws.h"
#include "overlay.h"
#include "imgui.h"
#include <atomic>
#include <set>
#include <mutex>
#include "lang.h"
#include <filesystem>
#include <sstream>
#include <fstream>
#include "profile.h"
#include "shared/json.h"
#include "panel.h"
#include "policy.h"
#include "log.h"
#include "../api/dgmp.h"
#include "shared/netclient.h"
#include "steam.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

using namespace ue;

namespace mp {
namespace {
// ---------------------------------------------------------------- session state
bool enabled = false;
Phase phase = Phase::Off;
std::wstring failReason;
std::string host, nick, password;
uint16_t port = net::kDefaultPort;
net::Client client;
bool connectStarted = false, lostAfterJoin = false, everJoined = false;
std::string serverName, modeName;
double lastTeleportAt = -1e9;
double respawnCoverUntil = 0;  // death: covered until the server's respawn teleport (or a timeout)
struct PendingRemoval { bool pending = false; uint8_t category = 0, style = 0; } pendingRemoval;  // objective Remove deferred  // gameevents start hooking 15 s after a teleport
struct Teleport { bool pending = false; FVector pos{}; float yaw = 0; bool initial = false; } teleport;
bool initialTeleportSeen = false;
net::NametagMode nametags = net::NametagMode::Visible;  // server setting
std::string wantedLook, reportedLook;  // look the server ordered for us / last look reported to the server
UObject* worldPawn = nullptr;   // world context for spawning
UObject* worldController = nullptr;

double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr); return s;
}
std::string Env(const wchar_t* name) {
    wchar_t buf[512] = {}; const DWORD n = GetEnvironmentVariableW(name, buf, 512);
    return n > 0 && n < 512 ? U(buf) : std::string();
}

// ---------------------------------------------------------------- engine helpers
FVector C2WPos(UObject* c) { return Field<FVector>(c, off::Scene_C2W_Trans); }
FQuat C2WRot(UObject* c) { return Field<FQuat>(c, off::Scene_C2W_Rot); }
FVector V(float x, float y, float z) { return {x, y, z}; }
float Dist2(const FVector& a, const FVector& b) { const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z; return dx * dx + dy * dy + dz * dz; }

UObject* Statics() {
    static UObject* s = nullptr;
    if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics");
    return s;
}
void BoolArg(UObject* o, const char* fn, const char* param, bool value) { auto f = IsValid(o) ? Fn(o, fn) : nullptr; if (!f) return; Params p(f); p.SetBool(param, value); Call(o, fn, &p); }
bool CallBool(UObject* o, const char* fn) { auto f = IsValid(o) ? Fn(o, fn) : nullptr; if (!f) return false; Params p(f); Call(o, fn, &p); return p.GetBool("ReturnValue"); }
float CallFloat(UObject* o, const char* fn) { auto f = IsValid(o) ? Fn(o, fn) : nullptr; if (!f) return 0; Params p(f); Call(o, fn, &p); return p.Get<float>("ReturnValue"); }

UObject* SpawnActor(UObject* cls, const FVector& pos) {
    if (!cls || !IsValid(worldPawn) || !Statics()) return nullptr;
    FTransform t; t.Translation = pos;
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", worldPawn); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    Params finish(Fn(Statics(), "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(Statics(), "FinishSpawningActor", &finish);
    Params col(Fn(actor, "SetActorEnableCollision")); col.SetBool("bNewActorEnableCollision", false); Call(actor, "SetActorEnableCollision", &col);
    return actor;
}
void DestroyActor(UObject*& a) { if (IsValid(a)) Call(a, "K2_DestroyActor"); a = nullptr; }
void SetActorTransform(UObject* actor, const FVector& pos, const FQuat& rot) {
    auto f = Fn(actor, "K2_SetActorLocationAndRotation"); if (!f) return;
    Params p(f); p.Set("NewLocation", pos); p.Set("NewRotation", QuatToRotator(rot)); p.SetBool("bTeleport", true);
    Call(actor, "K2_SetActorLocationAndRotation", &p);
}
void SetHidden(UObject* actor, bool hidden) {
    auto f = IsValid(actor) ? Fn(actor, "SetActorHiddenInGame") : nullptr; if (!f) return;
    Params p(f); p.SetBool("bNewHidden", hidden); Call(actor, "SetActorHiddenInGame", &p);
}
UObject* MeshComp(UObject* actor) { return Field<UObject*>(actor, off::SkelMeshActor_Component); }
void SetMesh(UObject* comp, UObject* mesh) {
    SetBoolProp(comp, "bDisableClothSimulation", true);  // APEX cloth on copies crashed the game in DGCoop
    Params p(Fn(comp, "SetSkeletalMesh")); p.Set("NewMesh", mesh); Call(comp, "SetSkeletalMesh", &p);
}
void AttachTo(UObject* child, UObject* parent, FName socket) {
    Params p(Fn(child, "K2_AttachRootComponentToActor"));
    p.Set("InParentActor", parent); p.Set("InSocketName", socket); p.Set<uint8_t>("AttachLocationType", 2);
    Call(child, "K2_AttachRootComponentToActor", &p);
}
void AttachCompTo(UObject* comp, UObject* parent, FName socket) {
    Params a(Fn(comp, "K2_AttachTo")); a.Set("InParent", parent); a.Set("InSocketName", socket); a.Set<uint8_t>("AttachType", 2);
    Call(comp, "K2_AttachTo", &a);
}
void SetRelative(UObject* comp, const FVector& loc, const FRotator& rot) {
    auto f = Fn(comp, "K2_SetRelativeLocationAndRotation"); if (!f) return;
    Params p(f); p.Set("NewLocation", loc); p.Set("NewRotation", rot); p.SetBool("bTeleport", true);
    Call(comp, "K2_SetRelativeLocationAndRotation", &p);
}
void CopyMaterials(UObject* src, UObject* dst) {
    auto getN = Fn(src, "GetNumMaterials"), get = Fn(src, "GetMaterial"), set = Fn(dst, "SetMaterial");
    if (!getN || !get || !set) return;
    Params pn(getN); Call(src, "GetNumMaterials", &pn);
    const int32_t count = pn.Get<int32_t>("ReturnValue");
    for (int32_t i = 0; i < count && i < 64; ++i) {
        Params pg(get); pg.Set("ElementIndex", i); Call(src, "GetMaterial", &pg);
        UObject* mat = pg.Get<UObject*>("ReturnValue"); if (!mat) continue;
        Params ps(set); ps.Set("ElementIndex", i); ps.Set("Material", mat); Call(dst, "SetMaterial", &ps);
    }
}
void SetCompHidden(UObject* comp, bool hidden) {
    auto f = IsValid(comp) ? Fn(comp, "SetHiddenInGame") : nullptr; if (!f) return;
    Params p(f); p.SetBool("NewHidden", hidden); p.SetBool("bPropagateToChildren", false); Call(comp, "SetHiddenInGame", &p);
}
void SetMaster(UObject* comp, UObject* master) {
    Params p(Fn(comp, "SetMasterPoseComponent")); p.Set("NewMasterBoneComponent", master); Call(comp, "SetMasterPoseComponent", &p);
}
bool HasValidMaster(UObject* comp) { return Resolve(Field<FWeakObjectPtr>(comp, off::Skinned_MasterPose)) != nullptr; }
FVector QRotate(const FQuat& q, const FVector& v) {
    const FVector u{q.X, q.Y, q.Z};
    auto cross = [](const FVector& a, const FVector& b) { return FVector{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X}; };
    const FVector t = cross(u, v), t2{2 * t.X, 2 * t.Y, 2 * t.Z}, c = cross(u, t2);
    return {v.X + q.W * t2.X + c.X, v.Y + q.W * t2.Y + c.Y, v.Z + q.W * t2.Z + c.Z};
}
UObject* Anim(int a) {
    static const char* paths[net::AnimCount] = {
        "/Game/Animation/Deacon/ambient/dcn_idle_normal.dcn_idle_normal",
        "/Game/Animation/Deacon/walk/dcn_walk.dcn_walk",
        "/Game/Animation/Deacon/run/dcn_jog.dcn_jog",
        "/Game/Animation/Deacon/run/dcn_run.dcn_run",
        "/Game/Animation/Deacon/run/dcn_sprint.dcn_sprint",
        "/Game/Animation/Deacon/bike/bike_driver_bike_lean_center.bike_driver_bike_lean_center",
        "/Game/Animation/Deacon/crouch/dcn_idle_crouch.dcn_idle_crouch",
        "/Game/Animation/Deacon/walk/dcn_walk_crouch_b.dcn_walk_crouch_b",
        "/Game/Animation/Deacon/fall/dcn_fall_loop.dcn_fall_loop",
        "/Game/Animation/Deacon/bike/bike_driver_bike_lean_exaggerated_left_s.bike_driver_bike_lean_exaggerated_left_s",
        "/Game/Animation/Deacon/bike/bike_driver_bike_lean_exaggerated_right_s.bike_driver_bike_lean_exaggerated_right_s",
    };
    static UObject* cache[net::AnimCount] = {};
    if (a < 0 || a >= net::AnimCount) return nullptr;
    if (!IsValid(cache[a])) { cache[a] = FindObject(paths[a]); if (!cache[a]) cache[a] = LoadAsset(paths[a]); }
    return cache[a];
}

// ---------------------------------------------------------------- the local player
struct Local {
    UObject* pawn = nullptr;
    UObject* mesh = nullptr;
    std::vector<UObject*> parts;
    UObject* ride = nullptr;
    UObject* bike = nullptr;
    UObject* bikeMesh = nullptr;
    std::vector<UObject*> bikeParts;
    double nextScan = 0;
} L;

void CollectMeshes(UObject* owner, const char* mainName, UObject*& main, std::vector<UObject*>& parts) {
    main = nullptr; parts.clear();
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o->Outer != owner || !IsValid(o) || !IsAByName(o, "SkinnedMeshComponent") || !Field<UObject*>(o, off::Skinned_SkeletalMesh)) continue;
        if (GetName(o) == mainName) main = o; else parts.push_back(o);
    }
}
void SetBike(UObject* bike) {
    if (bike == L.bike) return;
    L.bike = bike; L.bikeMesh = nullptr; L.bikeParts.clear();
    if (bike) CollectMeshes(bike, "VehicleMesh", L.bikeMesh, L.bikeParts);
}
UObject* NearestBike(const FVector& from) {
    UObject* cls = FindClass("VehiclePawn"); UObject* best = nullptr; float bestD = 6000.f * 6000.f;
    for (int32_t i = 0, n = NumObjects(); i < n && cls; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || (o->ObjectFlags & 0x10) || !IsA(o, cls) || !IsValid(o) || vehicles::IsServerBike(o)) continue;
        UObject* root = Field<UObject*>(o, off::Actor_RootComponent); if (!root) continue;
        const float d = Dist2(C2WPos(root), from); if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}
void RefreshLocal(double now, UObject* pawn) {
    if (pawn != L.pawn) { L = Local{}; L.pawn = pawn; }
    if (!IsValid(pawn)) return;
    if (IsValid(L.mesh) && now < L.nextScan) {
        if (IsValid(L.ride)) if (UObject* v = Resolve(Field<FWeakObjectPtr>(L.ride, off::Ride_VehiclePawn))) SetBike(v);
        if (L.bike && !(IsValid(L.bike) && IsValid(L.bikeMesh))) SetBike(nullptr);
        return;
    }
    L.nextScan = now + 3.0;
    UObject* mesh = nullptr; std::vector<UObject*> parts;
    CollectMeshes(pawn, "CharacterMesh0", mesh, parts);
    std::erase_if(parts, [](UObject* c) { return !HasValidMaster(c); });  // props like the radio don't follow the pose
    L.mesh = mesh; L.parts = std::move(parts);
    if (!IsValid(L.ride)) {
        UObject* rideCls = FindClass("BendRideVehicleComponent");
        for (int32_t i = 0, n = NumObjects(); i < n && rideCls; ++i) {
            UObject* o = ObjectAt(i);
            if (o && o->Outer == pawn && IsA(o, rideCls) && IsValid(o)) { L.ride = o; break; }
        }
    }
    UObject* v = IsValid(L.ride) ? Resolve(Field<FWeakObjectPtr>(L.ride, off::Ride_VehiclePawn)) : nullptr;
    if (!v && !IsValid(L.bike) && IsValid(L.mesh)) v = NearestBike(C2WPos(L.mesh));
    if (v) { if (v != L.bike || !IsValid(L.bikeMesh)) { L.bike = nullptr; SetBike(v); } }
    else if (L.bike && !IsValid(L.bike)) SetBike(nullptr);
}

// ---------------------------------------------------------------- gear
// What the player carries where others can see it: the visible meshes of the weapon actors attached
// to its body (in a hand, on the back, holstered) and their visible parts (scope, suppressor...).
struct RawList { UObject** data; int num, max; };
template <class T> T& PropRef(UObject* o, const char* name, int fallback) {
    int offset = fallback;
    for (auto c = o->Class; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) { offset = Field<int>(p, off::UProperty_Offset); break; }
    return Field<T>(o, offset);
}
UObject* MeshAsset(UObject* comp, bool& skeletal) {
    for (auto c = comp->Class; c; c = SuperOf(c)) {
        if (auto p = FindProperty(c, "SkeletalMesh")) { skeletal = true; return Field<UObject*>(comp, Field<int>(p, off::UProperty_Offset)); }
        if (auto p = FindProperty(c, "StaticMesh")) { skeletal = false; return Field<UObject*>(comp, Field<int>(p, off::UProperty_Offset)); }
    }
    return nullptr;
}
FQuat QMul(const FQuat& a, const FQuat& b) {
    return {a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y, a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
            a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W, a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z};
}
FVector QRot(const FQuat& q, const FVector& v) {
    const FVector u{q.X, q.Y, q.Z};
    const FVector t{2 * (u.Y * v.Z - u.Z * v.Y), 2 * (u.Z * v.X - u.X * v.Z), 2 * (u.X * v.Y - u.Y * v.X)};
    return {v.X + q.W * t.X + (u.Y * t.Z - u.Z * t.Y), v.Y + q.W * t.Y + (u.Z * t.X - u.X * t.Z), v.Z + q.W * t.Z + (u.X * t.Y - u.Y * t.X)};
}
std::vector<uint8_t> BuildGear(UObject* pawn) {
    net::Writer w;
    auto f = IsValid(pawn) ? Fn(pawn, "GetAttachedActors") : nullptr;
    if (!f || !IsValid(L.mesh)) { w.U8(0); return w.buf; }
    Params p(f); Call(pawn, "GetAttachedActors", &p);
    const RawList actors = p.Get<RawList>("OutActors");
    static UObject* meshCls = FindClass("MeshComponent");
    static UObject* equippableCls = FindClass("BendEquippableItem");  // weapons, the rock, the binoculars
    std::vector<UObject*> items;  // components already written, by index
    net::Writer body;
    for (int i = 0; i < actors.num; ++i) {
        UObject* a = actors.data[i];
        if (!IsValid(a) || GetBoolProp(a, "bHidden")) continue;
        if (GetName(a->Class).rfind("Bend_Wpn", 0) != 0 && !(equippableCls && IsA(a, equippableCls))) continue;
        auto g = Fn(a, "GetComponentsByClass"); if (!g) continue;
        Params q(g); q.Set("ComponentClass", meshCls); Call(a, "GetComponentsByClass", &q);
        const RawList comps = q.Get<RawList>("ReturnValue");
        // Parents first: a part hangs on a weapon written before it.
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < comps.num && items.size() < 40; ++k) {
                UObject* c = comps.data[k];
                if (!IsValid(c) || std::find(items.begin(), items.end(), c) != items.end()) continue;
                if (!GetBoolProp(c, "bVisible") || GetBoolProp(c, "bHiddenInGame")) continue;
                bool skeletal = false; UObject* mesh = MeshAsset(c, skeletal);
                if (!IsValid(mesh)) continue;
                UObject* parent = Field<UObject*>(c, 0xD0);  // SceneComponent.AttachParent
                int parentIndex = -2;
                if (parent == L.mesh) parentIndex = -1;
                else if (auto it = std::find(items.begin(), items.end(), parent); it != items.end()) parentIndex = int(it - items.begin());
                if (parentIndex == -2) continue;
                items.push_back(c);
                FVector loc = Field<FVector>(c, 0x170), scale = Field<FVector>(c, 0x1B0);
                FRotator rot = Field<FRotator>(c, 0x17C);
                FName socket = Field<FName>(c, 0xE8);
                if (parentIndex == -1) {
                    // Sockets belong to the player's own mesh; the bones under them are on every look.
                    // So: the bone of the socket, and the placement relative to that bone.
                    if (auto sg = Fn(L.mesh, "GetSocketBoneName")) { Params sq(sg); sq.Set("InSocketName", socket); Call(L.mesh, "GetSocketBoneName", &sq); const FName bone = sq.Get<FName>("ReturnValue"); if (bone.Index > 0) socket = bone; }
                    if (auto sg = Fn(L.mesh, "GetSocketTransform")) {
                        Params sq(sg); sq.Set("InSocketName", socket); sq.Set<uint8_t>("TransformSpace", 0); Call(L.mesh, "GetSocketTransform", &sq);  // RTS_World
                        const FTransform b = sq.Get<FTransform>("ReturnValue");
                        const FQuat inv{-b.Rotation.X, -b.Rotation.Y, -b.Rotation.Z, b.Rotation.W};
                        const FVector cl = C2WPos(c), cs = Field<FVector>(c, off::Scene_C2W_Trans + 0x10);
                        FVector d = QRot(inv, {cl.X - b.Translation.X, cl.Y - b.Translation.Y, cl.Z - b.Translation.Z});
                        auto div = [](float a, float b) { return std::fabs(b) > 1e-4f ? a / b : a; };
                        loc = {div(d.X, b.Scale3D.X), div(d.Y, b.Scale3D.Y), div(d.Z, b.Scale3D.Z)};
                        rot = QuatToRotator(QMul(inv, C2WRot(c)));
                        scale = {div(cs.X, b.Scale3D.X), div(cs.Y, b.Scale3D.Y), div(cs.Z, b.Scale3D.Z)};
                    }
                }
                // Rounded: worked out from world transforms, the numbers shake a little every time, and each
                // change is sent and makes the others rebuild every mesh this player carries.
                auto snap = [](float v, float step) { return std::round(v / step) * step; };
                loc = {snap(loc.X, 0.2f), snap(loc.Y, 0.2f), snap(loc.Z, 0.2f)};
                rot = {snap(rot.Pitch, 0.5f), snap(rot.Yaw, 0.5f), snap(rot.Roll, 0.5f)};
                scale = {snap(scale.X, 0.005f), snap(scale.Y, 0.005f), snap(scale.Z, 0.005f)};
                body.Str(FullName(mesh)); body.U8(skeletal); body.U8(uint8_t(int8_t(parentIndex))); body.Str(NameOf(socket));
                body.F32(loc.X); body.F32(loc.Y); body.F32(loc.Z); body.F32(rot.Pitch); body.F32(rot.Yaw); body.F32(rot.Roll);
                body.F32(scale.X); body.F32(scale.Y); body.F32(scale.Z);
            }
    }
    w.U8(uint8_t(items.size()));
    w.Raw(body.buf.data(), body.buf.size());
    return w.buf;
}
std::vector<uint8_t> reportedGear;
uint8_t collisions = 0;  // net::CollisionMask from the server

bool localDead = false;  // the local player lies dead / waits for its respawn
float wantedHealth = -1;  // the server set the health (percent; -1: nothing to do)
// The game's health bar shows only the health above BendPawn.UnderArmorHealth (35 of 135): it is empty
// there, and the game stops a big first hit on it. Empty bar = dead (or down); percents are of the bar.
constexpr int kPawnUnderArmor = 0x1FD8;
float BarFloor(ue::UObject* pawn, float maxHp) {
    const float ua = IsValid(pawn) ? Field<float>(pawn, kPawnUnderArmor) : 0.f;
    return ua > 0 && ua < maxHp * 0.9f ? ua : 0.f;
}
float BarPercent(float hp, float maxHp, float floor) { return std::clamp((hp - floor) / std::max(1.f, maxHp - floor), 0.f, 1.f) * 100.f; }
float BarHealth(float percent, float maxHp, float floor) { return floor + (maxHp - floor) * std::clamp(percent, 0.f, 100.f) / 100.f; }
// Death mode "downed" (docs/API.md "Ранение"): a lethal hit lays the player down instead; the server says how
// long it may lie (DownedState) and ends it: revived (health back), or dead (SetHealth 0 next).
struct Downed {
    bool mode = false;          // the server's death mode is "downed"
    bool pending = false;       // fell, the server's answer not here yet
    bool on = false;            // lies wounded
    double fellAt = 0, until = 0, giveUpAt = 0, holdSince = 0, helpAt = -100, hitAt = 0;
    std::string anim;
} downed;
std::set<uint16_t> downedPlayers;  // other players lying wounded
constexpr const char* kDownedAnim = "/Game/Animation/Boozer/ambient/bzr_injury_lay_down_pillow_loop.bzr_injury_lay_down_pillow_loop";
bool spectatorHidden = false;  // spectating: the own body is hidden from everyone
// A skin choice at a place of the mode: opens once the player has arrived there.
struct PendingCreator { bool on = false; skins::Options options; FVector at{}; double since = 0; } pendingCreator;
struct Spectator {
    bool want = false, on = false;
    std::vector<uint16_t> only;
    uint16_t target = 0xFFFF;
    UObject* cam = nullptr;
    UObject* previousView = nullptr;
    FRotator rot{-15, 0, 0}, lastControl{};
    FVector point{};
    bool keys[4]{};
} spec;
bool BuildState(net::PlayerState& s, UObject* controller) {
    if (!IsValid(L.pawn) || !IsValid(L.mesh)) return false;
    UObject* root = Field<UObject*>(L.pawn, off::Actor_RootComponent);
    if (!IsValid(root)) return false;
    const FVector pos = C2WPos(root), vel = Field<FVector>(root, off::Scene_ComponentVelocity);
    s.x = pos.X; s.y = pos.Y; s.z = pos.Z; s.vx = vel.X; s.vy = vel.Y; s.vz = vel.Z;
    if (auto f = IsValid(controller) ? Fn(controller, "GetControlRotation") : nullptr) { Params p(f); Call(controller, "GetControlRotation", &p); s.yaw = p.Get<FRotator>("ReturnValue").Yaw; }
    const FVector mp = C2WPos(L.mesh); const FQuat mq = C2WRot(L.mesh);
    s.mx = mp.X; s.my = mp.Y; s.mz = mp.Z; s.mqx = mq.X; s.mqy = mq.Y; s.mqz = mq.Z; s.mqw = mq.W;
    {   // BendPawn health / max health
        const float hp = Field<float>(L.pawn, 0x1FD4), maxHp = Field<float>(L.pawn, 0x1FE4);
        s.health = uint8_t(maxHp > 0 ? std::clamp(int(std::lround(BarPercent(hp, maxHp, BarFloor(L.pawn, maxHp)))), 0, 100) : 100);
    }
    s.flags = net::StateHasMesh;
    const bool onBike = CallBool(L.pawn, "IsInVehicle") || (IsValid(L.ride) && CallBool(L.ride, "IsDriver"));
    if (onBike) {
        s.flags |= net::StateOnBike;
        const float lean = IsValid(L.ride) ? CallFloat(L.ride, "GetLeanAngleNeg1To1") : 0.f;
        s.anim = lean < -0.35f ? net::AnimRideLeft : lean > 0.35f ? net::AnimRideRight : net::AnimRide;
    } else {
        const float speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);
        UObject* move = Field<UObject*>(L.pawn, 0x3B8);
        const uint8_t mode = IsValid(move) ? Field<uint8_t>(move, 0x198) : 1;
        if (mode == 3) s.anim = net::AnimFall;
        else if (GetBoolProp(L.pawn, "bIsCrouched")) s.anim = speed < 15.f ? net::AnimCrouchIdle : net::AnimCrouchWalk;
        else s.anim = speed < 15.f ? net::AnimIdle : speed < 220.f ? net::AnimWalk : speed < 400.f ? net::AnimJog : speed < 580.f ? net::AnimRun : net::AnimSprint;
    }
    const bool parkedServerBike = vehicles::IsServerBike(L.bike) && (vehicles::RidingBehind(L.bike) || !(IsValid(L.ride) && Resolve(Field<FWeakObjectPtr>(L.ride, off::Ride_VehiclePawn)) == L.bike));  // or behind someone: their bike is drawn as theirs
    if (IsValid(L.bike) && IsValid(L.bikeMesh) && !parkedServerBike) {
        const FVector bp = C2WPos(L.bikeMesh); const FQuat bq = C2WRot(L.bikeMesh);
        s.flags |= net::StateHasBike;
        s.bx = bp.X; s.by = bp.Y; s.bz = bp.Z; s.bqx = bq.X; s.bqy = bq.Y; s.bqz = bq.Z; s.bqw = bq.W;
    }
    if (inventory::OwnsBike()) s.flags |= net::StateOwnsBike;  // assigned bike, however far away
    if (localDead) s.flags |= net::StateDead;
    if (spectatorHidden || skins::CreatorOpen() || vehicles::HelicopterSeat()) s.flags |= net::StateHidden;  // choosing a skin: alone; in a helicopter: inside it
    s.time = Clock();
    return true;
}

// ---------------------------------------------------------------- bone poses
// The sender reads the animation output (parent-relative bones) straight from the mesh; receivers
// write it into a PoseableMeshComponent. Mirrors every animation the game plays.
struct FTransformArray { FTransform* Data; int32_t Num; int32_t Max; };
constexpr int kSkelMesh_LocalAtoms = 0x910;
constexpr int kPoseable_BoneSpaceTransforms = 0x8C0;
constexpr size_t kPoseHeader = 1 + 2 + 4 + 2 + 8 + 7 * 4;

struct BoneList {
    UObject* builtFor = nullptr;
    UObject* builtMesh = nullptr;  // the component's SkeletalMesh when the list was built
    std::vector<FName> names;
    std::vector<int32_t> index;
    uint32_t hash = 0;
};

void BuildBoneList(UObject* comp, BoneList& out) {
    out = BoneList{}; out.builtFor = comp; out.builtMesh = Field<UObject*>(comp, off::Skinned_SkeletalMesh);
    UObject* getName = Fn(comp, "GetBoneName"); UObject* getParent = Fn(comp, "GetParentBone");
    if (!getName || !getParent) return;
    std::vector<FName> all, parents;
    for (int32_t i = 0; i < 4096; ++i) {
        Params p(getName); p.Set("BoneIndex", i); Call(comp, "GetBoneName", &p);
        const FName n = p.Get<FName>("ReturnValue"); if (n.Index <= 0) break;
        Params pp(getParent); pp.Set("BoneName", n); Call(comp, "GetParentBone", &pp);
        all.push_back(n); parents.push_back(pp.Get<FName>("ReturnValue"));
    }
    // The face rig hangs under "head": keep the head, drop its descendants.
    std::vector<bool> skip(all.size(), false);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (all[j].Index == parents[i].Index && all[j].Number == parents[i].Number) { skip[i] = skip[j] || _stricmp(NameOf(all[j]).c_str(), "head") == 0; break; }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < all.size(); ++i) {
        if (skip[i] || int(out.names.size()) >= net::kMaxPoseBones) continue;
        bool parentKept = out.names.empty();
        for (auto& n : out.names) if (n.Index == parents[i].Index && n.Number == parents[i].Number) { parentKept = true; break; }
        if (!parentKept) continue;
        out.names.push_back(all[i]); out.index.push_back(int32_t(i));
        for (char c : NameOf(all[i])) { h ^= uint8_t(c); h *= 16777619u; }
        h ^= 0xFF; h *= 16777619u;
    }
    out.hash = h;
    dg::Log("MP bone list for %s: %zu of %zu bones, hash %08X", GetName(comp).c_str(), out.names.size(), all.size(), h);
}
net::QBone Quantize(const FTransform& t) {
    auto q16 = [](float v, float scale) { return int16_t(std::clamp(v * scale, -32767.f, 32767.f)); };
    FQuat r = t.Rotation; if (r.W < 0) r = {-r.X, -r.Y, -r.Z, -r.W};
    return {q16(t.Translation.X, net::kPosePosScale), q16(t.Translation.Y, net::kPosePosScale), q16(t.Translation.Z, net::kPosePosScale),
            q16(r.X, 32767.f), q16(r.Y, 32767.f), q16(r.Z, 32767.f), q16(r.W, 32767.f),
            q16(t.Scale3D.X, 8192.f), q16(t.Scale3D.Y, 8192.f), q16(t.Scale3D.Z, 8192.f)};
}
FTransform Dequantize(const net::QBone& b) {
    FTransform t;
    t.Translation = {b.px / net::kPosePosScale, b.py / net::kPosePosScale, b.pz / net::kPosePosScale};
    FQuat r{b.qx / 32767.f, b.qy / 32767.f, b.qz / 32767.f, b.qw / 32767.f};
    const float len = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W);
    if (len > 0) { r.X /= len; r.Y /= len; r.Z /= len; r.W /= len; }
    t.Rotation = r; t.Scale3D = {b.sx / 8192.f, b.sy / 8192.f, b.sz / 8192.f};
    return t;
}
BoneList localBones[2];
uint16_t poseSeq[2] = {};

std::vector<uint8_t> BuildPosePayload(int channel) {
    UObject* mesh = channel == net::kPoseBike ? L.bikeMesh : L.mesh;
    if (!IsValid(mesh)) return {};
    BoneList& list = localBones[channel];
    if (list.builtFor != mesh || list.builtMesh != Field<UObject*>(mesh, off::Skinned_SkeletalMesh)) BuildBoneList(mesh, list);
    if (list.names.empty()) return {};
    const auto& arr = Field<FTransformArray>(mesh, kSkelMesh_LocalAtoms);
    if (!arr.Data || arr.Num <= 0) return {};
    net::Writer w;
    w.U8(uint8_t(channel)); w.U16(poseSeq[channel]++); w.U32(list.hash); w.U16(uint16_t(list.names.size())); w.Put(Clock());
    const FVector p = C2WPos(mesh); const FQuat q = C2WRot(mesh);
    w.F32(p.X); w.F32(p.Y); w.F32(p.Z); w.F32(q.X); w.F32(q.Y); w.F32(q.Z); w.F32(q.W);
    for (size_t i = 0; i < list.names.size(); ++i) {
        const int32_t idx = list.index[i];
        const auto b = Quantize(idx < arr.Num ? arr.Data[idx] : FTransform{});
        w.Raw(&b, sizeof(b));
    }
    return w.buf;
}

// Arrival times jitter: frames are placed on the timeline by sender time + smallest observed transit
// offset, and played back with a delay covering the measured jitter.
struct ClockSync {
    std::deque<std::pair<double, double>> samples;
    double jitter = 0;
    double Map(double sendTime, double recvTime) {
        samples.push_back({recvTime, recvTime - sendTime});
        while (!samples.empty() && recvTime - samples.front().first > 5.0) samples.pop_front();
        double lo = 1e18, hi = -1e18;
        for (auto& [t, o] : samples) { lo = std::min(lo, o); hi = std::max(hi, o); }
        jitter = std::min(hi - lo, 0.5);
        return sendTime + lo;
    }
    double Delay(double interval) const { return interval + jitter + 0.03; }
};

struct PoseFrame { double t; FVector pos; FQuat rot; std::vector<FTransform> local; };

// A clothing part's own posed copy. The game's parts never tick (bCanEverTick false): their skeletal
// master pushes each pose into them, a poseable mesh does not (they stood in the base pose), and set
// again as their master every frame they took a broken buffer (a crumpled body). So each part gets a
// poseable mesh of its own, with its materials, written with the same bones by name.
UObject* CreatePoseable(UObject* actor, UObject* src);
UObject* DamageTypeFor(const std::string& weapon);
void Footstep(UObject* mesh, const FVector& foot, bool close);
struct PoseTwin { UObject* src = nullptr; UObject* comp = nullptr; std::vector<int32_t> index; };

struct PoseTrack {
    UObject* comp = nullptr;
    BoneList bones;
    std::vector<PoseTwin> twins;
    bool active = false, mismatchLogged = false;
    uint32_t hash = 0;
    std::deque<PoseFrame> frames;
    double lastRecv = 0;

    void Reset() { comp = nullptr; active = false; frames.clear(); twins.clear(); }
    void DropTwins() {
        for (auto& tw : twins) {
            if (IsValid(tw.comp)) if (auto f = Fn(tw.comp, "K2_DestroyComponent")) { Params p(f); p.Set("Object", tw.comp); Call(tw.comp, "K2_DestroyComponent", &p); }
            if (IsValid(tw.src)) SetCompHidden(tw.src, false);
        }
        twins.clear();
    }
    // The parts shown with this pose: twins made for new ones, dropped for gone ones.
    void SyncTwins(UObject* actor, const std::vector<UObject*>& parts) {
        bool same = parts.size() == twins.size();
        for (size_t i = 0; same && i < parts.size(); ++i) same = twins[i].src == parts[i] && IsValid(twins[i].comp);
        if (same) return;
        DropTwins();
        auto boneIndex = Fn(comp, "GetBoneIndex");
        for (UObject* part : parts) {
            PoseTwin tw; tw.src = part;
            tw.comp = CreatePoseable(actor, part);
            if (!tw.comp) continue;
            for (const FName& n : bones.names) {
                int32_t idx = -1;
                if (boneIndex) { Params p(boneIndex); p.Set("BoneName", n); Call(tw.comp, "GetBoneIndex", &p); idx = p.Get<int32_t>("ReturnValue"); }
                tw.index.push_back(idx);
            }
            SetCompHidden(part, true);
            twins.push_back(std::move(tw));
        }
    }
    void Push(ClockSync& clock, uint32_t h, double sendTime, const FVector& pos, const FQuat& rot, const net::QBone* b, int count, double now) {
        hash = h;
        const double t = clock.Map(sendTime, now);
        if (!frames.empty() && t <= frames.back().t) return;
        PoseFrame f{t, pos, rot, std::vector<FTransform>(size_t(count))};
        for (int i = 0; i < count; ++i) { net::QBone q; std::memcpy(&q, b + i, sizeof(q)); f.local[i] = Dequantize(q); }
        frames.push_back(std::move(f));
        while (frames.size() > 16) frames.pop_front();
        lastRecv = now;
    }
    bool Usable(double now) {
        if (!comp || bones.names.empty()) return false;
        if (hash && hash != bones.hash) {
            if (!mismatchLogged) dg::Log("MP pose: bone list mismatch (theirs %08X, ours %08X)", hash, bones.hash);
            mismatchLogged = true; return false;
        }
        // A gap in the stream (lost packets) keeps the last pose for a while: the fallback animations are
        // Deacon's, and on another skeleton (a woman's) they do not play, the body stands in its base pose.
        return !frames.empty() && frames.back().local.size() == bones.names.size() && now - lastRecv < 3.0;
    }
    bool Stale(double now) const { return now - lastRecv > 0.4; }  // no fresh pose: the body goes where the state says
    bool Sample(const ClockSync& clock, double now, FVector& pos, FQuat& rot, std::vector<FTransform>& local) {
        if (frames.empty()) return false;
        const double t = now - std::max(0.12, clock.Delay(1.0 / 15));
        while (frames.size() > 2 && frames[1].t <= t) frames.pop_front();
        const PoseFrame& a = frames.front();
        if (frames.size() == 1 || t <= a.t) { pos = a.pos; rot = a.rot; local = a.local; return true; }
        const PoseFrame& b = frames[1];
        const float k = float(std::clamp((t - a.t) / std::max(b.t - a.t, 1e-3), 0.0, 1.0));
        auto lerp = [k](const FVector& x, const FVector& y) { return FVector{x.X + (y.X - x.X) * k, x.Y + (y.Y - x.Y) * k, x.Z + (y.Z - x.Z) * k}; };
        pos = lerp(a.pos, b.pos); rot = Slerp(a.rot, b.rot, k);
        local.resize(b.local.size());
        for (size_t i = 0; i < local.size() && i < a.local.size(); ++i) {
            local[i].Translation = lerp(a.local[i].Translation, b.local[i].Translation);
            local[i].Scale3D = lerp(a.local[i].Scale3D, b.local[i].Scale3D);
            local[i].Rotation = Slerp(a.local[i].Rotation, b.local[i].Rotation, k);
        }
        return true;
    }
    void Apply(const ClockSync& clock, double now, UObject* actor) {
        FVector pos; FQuat rot; std::vector<FTransform> local;
        if (!Sample(clock, now, pos, rot, local) || local.size() != bones.names.size()) return;
        if (actor && !Stale(now)) SetActorTransform(actor, pos, rot);
        auto write = [&](UObject* target, const std::vector<int32_t>& index) {
            auto& arr = Field<FTransformArray>(target, kPoseable_BoneSpaceTransforms);
            int32_t first = -1;
            for (size_t i = 0; i < local.size() && i < index.size(); ++i) {
                const int32_t idx = index[i];
                if (idx >= 0 && idx < arr.Num) { arr.Data[idx] = local[i]; if (first < 0) first = int32_t(i); }
            }
            // Touch a bone through the API so the engine marks the pose dirty.
            if (first >= 0) if (auto f = Fn(target, "SetBoneTransformByName")) {
                Params p(f); p.Set("BoneName", bones.names[first]); p.Set("InTransform", arr.Data[index[first]]); p.Set<uint8_t>("BoneSpace", 1);
                Call(target, "SetBoneTransformByName", &p);
            }
        };
        write(comp, bones.index);
        for (auto& tw : twins) if (IsValid(tw.comp)) write(tw.comp, tw.index);
    }
};

// ---------------------------------------------------------------- remote players
// FText made by the engine (below): safe to put in the engine's own objects.
struct TextValue { uint8_t bytes[0x18]; };
bool EngineText(const std::wstring& value, TextValue& out);
struct GhostPart { UObject* actor = nullptr; FName socket{0, 0}; bool master = false; FVector relLoc{}; FRotator relRot{}; };

// Meshes for other players' bikes when this player has none: a server vehicle's bike standing here.
void BikeTemplate(UObject*& main, const std::vector<UObject*>*& parts) {
    static UObject* from = nullptr; static UObject* mesh = nullptr; static std::vector<UObject*> list;
    UObject* any = vehicles::AnyBike();
    if (any != from || !IsValid(mesh)) { from = any; mesh = nullptr; list.clear(); if (any) CollectMeshes(any, "VehicleMesh", mesh, list); }
    main = mesh; parts = &list;
}
UObject* SpawnMeshGroup(UObject* mainSrc, const std::vector<UObject*>& partsSrc, const FVector& pos, std::vector<GhostPart>& out) {
    UObject* cls = FindClass("SkeletalMeshActor");
    UObject* main = SpawnActor(cls, pos);
    if (!main) return nullptr;
    UObject* mainComp = MeshComp(main);
    SetMesh(mainComp, Field<UObject*>(mainSrc, off::Skinned_SkeletalMesh));
    CopyMaterials(mainSrc, mainComp);
    for (UObject* src : partsSrc) {
        if (!IsValid(src)) continue;
        UObject* part = SpawnActor(cls, pos); if (!part) continue;
        UObject* comp = MeshComp(part);
        SetMesh(comp, Field<UObject*>(src, off::Skinned_SkeletalMesh));
        CopyMaterials(src, comp);
        GhostPart gp;
        gp.actor = part; gp.socket = Field<FName>(src, 0xE8); gp.master = HasValidMaster(src);
        gp.relLoc = Field<FVector>(src, 0x170); gp.relRot = Field<FRotator>(src, 0x17C);
        AttachTo(part, main, gp.socket);
        if (gp.master) SetMaster(comp, mainComp); else SetRelative(comp, gp.relLoc, gp.relRot);
        out.push_back(gp);
    }
    return main;
}
UObject* CreatePoseable(UObject* actor, UObject* src) {
    UObject* cls = FindClass("PoseableMeshComponent"); if (!cls) return nullptr;
    UObject* comp = NewObject(cls, actor); if (!comp) return nullptr;
    SetMesh(comp, Field<UObject*>(src, off::Skinned_SkeletalMesh));
    CopyMaterials(src, comp);
    RegisterComponent(comp);
    AttachCompTo(comp, MeshComp(actor), FName{0, 0});
    return comp;
}
// ---------------------------------------------------------------- character looks
// A look (BendSkeletalMeshLook) is the game's outfit: a master mesh plus clothing parts. Any
// SkeletalMeshComponent can wear one (SetLook creates the parts, slaved to its pose).
UObject* LoadLook(const std::string& path) {
    static std::map<std::string, UObject*> cache;
    auto& slot = cache[path];
    if (!IsValid(slot)) { slot = FindObject(path.c_str()); if (!slot) slot = LoadAsset(path.c_str()); }
    return slot;
}
bool WearLook(UObject* comp, const std::string& path) {
    UObject* look = IsValid(comp) ? LoadLook(path) : nullptr;
    auto f = look ? Fn(comp, "SetLook") : nullptr;
    if (!f) { dg::Log("MP look %s could not be applied", path.c_str()); return false; }
    SetBoolProp(comp, "bDisableClothSimulation", true);
    Params p(f); p.Set("NewLook", look); p.SetBool("bNeedsToWorkWithRagdoll", false); Call(comp, "SetLook", &p);
    return true;
}
std::string CurrentLook(UObject* comp) {
    if (!IsValid(comp)) return {};
    for (auto cls = comp->Class; cls; cls = SuperOf(cls)) if (auto prop = FindProperty(cls, "Look")) {
        UObject* look = Field<UObject*>(comp, Field<int>(prop, off::UProperty_Offset));
        return IsValid(look) ? FullName(look) : std::string();
    }
    return {};
}
std::vector<UObject*> LookParts(UObject* comp) {
    struct Arr { UObject** data; int32_t num, max; };
    std::vector<UObject*> out;
    for (auto cls = IsValid(comp) ? comp->Class : nullptr; cls; cls = SuperOf(cls)) if (auto prop = FindProperty(cls, "CreatedLookParts")) {
        const auto a = Field<Arr>(comp, Field<int>(prop, off::UProperty_Offset));
        for (int i = 0; a.data && i < a.num && i < 64; ++i) if (IsValid(a.data[i])) out.push_back(a.data[i]);
        break;
    }
    for (UObject* extra : skins::ExtraParts(comp)) out.push_back(extra);  // slots a skin added
    return out;
}

void SwitchGroupPose(UObject* mainActor, std::vector<GhostPart>& parts, PoseTrack& t, bool on) {
    UObject* main = MeshComp(mainActor);
    UObject* target = on ? t.comp : main;
    SetCompHidden(t.comp, !on); SetCompHidden(main, on);
    for (auto& gp : parts) {
        UObject* comp = MeshComp(gp.actor);
        if (gp.master) SetMaster(comp, main);  // posed through its twin (PoseTwin)
        else { AttachCompTo(comp, target, gp.socket); SetRelative(comp, gp.relLoc, gp.relRot); }
    }
    for (UObject* part : LookParts(main)) SetMaster(part, main);  // clothing created by SetLook: twins too
    if (!on) t.DropTwins();
    t.active = on;
}

// Health bars under the names: screen points collected each frame on the game thread, drawn by the
// overlay (render thread).
struct Plate { float x, y, distance; int health; bool talking; };
std::mutex plateLock;
std::vector<Plate> plates, platesNext;
void NewPlate(float x, float y, float distance, int health, bool talking) { platesNext.push_back({x, y, distance, health, talking}); }
void PublishPlates() { std::lock_guard g(plateLock); plates.swap(platesNext); platesNext.clear(); }
std::string spectatorCaption;  // under plateLock
// Multiplayer frees F for the modes' interactions: the flashlight goes to L (the game's own remapping,
// as its controls menu does it: BendInputMapper.UpdateInputMapping). True when done or already so.
bool MoveFlashlightToL() {
    UObject* mapper = FindRuntimeInstance("BendInputRemapper_C");
    static UObject* entry = FindObject("/Script/BendGame.BendInputMappingEntry");
    auto get = IsValid(mapper) ? Fn(mapper, "GetInputMappingByName") : nullptr, update = IsValid(mapper) ? Fn(mapper, "UpdateInputMapping") : nullptr;
    UObject* primaryProp = entry ? FindProperty(entry, "PrimaryMapping") : nullptr;
    UObject* outProp = get ? FindProperty(get, "OutMapping") : nullptr;
    if (!get || !update || !primaryProp || !outProp) return false;
    Params g(get); g.Set("ActionName", FindName("LightToggle")); Call(mapper, "GetInputMappingByName", &g);
    UObject* primary = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(g.Data()) + Field<int>(outProp, off::UProperty_Offset) + Field<int>(primaryProp, off::UProperty_Offset));
    if (!IsValid(primary)) return false;
    struct KeyRaw { FName name; void* details = nullptr; void* refs = nullptr; };
    const FName l = FindName("L");
    if (auto f = Fn(primary, "GetKey")) { Params k(f); Call(primary, "GetKey", &k); if (k.Get<KeyRaw>("ReturnValue").name.Index == l.Index) return true; }
    Params u(update); u.Set("BendInputMapping", primary); u.Set("NewKey", KeyRaw{l}); Call(mapper, "UpdateInputMapping", &u);
    dg::Log("MP: flashlight moved to L (%s)", u.GetBool("ReturnValue") ? "ok" : "refused");
    return true;
}
void DrawPlates();
// A hit on another player (a bullet or a swing this player made): a small cross at the centre of the
// screen for a moment, as games show it, since the other body barely reacts here.
std::atomic<double> hitMarkUntil{0};
void MarkHit() { hitMarkUntil = Clock() + 0.25; }
void DrawHitMark() {
    const double left = hitMarkUntil.load() - Clock();
    if (left <= 0) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 s = ImGui::GetIO().DisplaySize;
    const float cx = s.x * 0.5f, cy = s.y * 0.5f, k = s.y / 1080.f, in = 6.f * k, out = 15.f * k;
    const ImU32 c = IM_COL32(255, 255, 255, int(255 * std::min(1.0, left / 0.12)));
    for (int sx : {-1, 1}) for (int sy : {-1, 1})
        dl->AddLine(ImVec2(cx + sx * in, cy + sy * in), ImVec2(cx + sx * out, cy + sy * out), c, 2.5f * k);
}
void DrawHudLayer() { DrawPlates(); DrawHitMark(); traps::Draw(); clientlua::Draw(); mpui::DrawHud(); voice::DrawHud(); }
void DrawPlates() {
    std::vector<Plate> list;
    std::string caption;
    { std::lock_guard g(plateLock); list = plates; caption = spectatorCaption; }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!caption.empty()) {
        ImFont* font = static_cast<ImFont*>(overlay::LargeFont()); if (!font) font = ImGui::GetFont();
        const ImVec2 size = font->CalcTextSizeA(26, FLT_MAX, 0, caption.c_str());
        const float x = (ImGui::GetIO().DisplaySize.x - size.x) / 2, y = ImGui::GetIO().DisplaySize.y - 110;
        dl->AddRectFilled(ImVec2(x - 14, y - 8), ImVec2(x + size.x + 14, y + size.y + 8), IM_COL32(0, 0, 0, 140), 6);
        dl->AddText(font, 26, ImVec2(x, y), IM_COL32(240, 240, 240, 255), caption.c_str());
    }
    for (const Plate& p : list) {
        const float k = std::clamp(1500.f / std::max(p.distance, 1.f), 0.45f, 1.f);  // smaller far away
        const float w = 74.f * k, h = std::max(4.f, 7.f * k);
        const ImVec2 a(p.x - w / 2, p.y + 2), b(p.x + w / 2, p.y + 2 + h);
        const float f = std::clamp(p.health / 100.f, 0.f, 1.f);
        const ImU32 fill = f > 0.6f ? IM_COL32(108, 214, 108, 235) : f > 0.3f ? IM_COL32(242, 209, 107, 235) : IM_COL32(224, 90, 74, 235);
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 170), 2.f);
        if (p.talking) {  // voice chat: a speaking mark left of the bar
            const ImVec2 c(a.x - 6 - 5 * k, (a.y + b.y) / 2);
            dl->AddCircleFilled(c, 4.5f * k + 1, IM_COL32(0, 0, 0, 170));
            dl->AddCircleFilled(c, 4.5f * k, IM_COL32(110, 210, 110, 255));
        }
        if (f > 0) dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * f, b.y), fill, 2.f);
    }
}

struct Ghost {
    uint16_t id = 0;
    std::string nick;
    std::string lookPath;           // the look this player wears ("" until known)
    ClockSync clock;
    struct Snap { double t; net::PlayerState s; };
    std::deque<Snap> snaps;
    double lastRecv = 0, nextSpawnTry = 0, nextBikeTry = 0;
    bool footUp[2] = {false, false};  // each foot (left, right) lifted since its last step (Footsteps; not at first)
    double footAt[2] = {0, 0};
    UObject* body = nullptr;
    std::vector<GhostPart> bodyParts;
    UObject* bike = nullptr;
    std::vector<GhostPart> bikeParts;
    UObject* tag = nullptr;
    int health = 100;               // percent, from the player's state
    double hiddenUntil = 0;         // its corpse was just left: hidden until its dead state arrives
    FVector lastMeshPos{};          // where its body is (spectator camera)
    std::vector<uint8_t> gear;      // GearList payload: what it carries
    bool gearDirty = false;
    std::vector<UObject*> gearComps;
    UObject* blocker = nullptr;     // capsule that blocks other players (server setting)
    UObject* bikeBlocker = nullptr; // box that blocks other players and bikes on its bike
    uint8_t blockersOn = 0xFF;
    bool blockerRiding = false;  // on a vehicle: the body blocks nothing (see UpdateBlockers)
    bool deadShown = false;
    std::map<UObject*, net::HitZone> hitZones;  // hit boxes on the body and the zone each one counts as
    struct HitBox { UObject* comp; FName bone; FVector loc; FRotator rot; };
    std::vector<HitBox> hitBoxes;
    // The visible body mesh: the posed copy while poses stream in, else the animated body (bots, and
    // until the first pose). Hit boxes and carried weapons hang on it.
    UObject* BodyMesh() { return tracks[net::kPoseBody].active && IsValid(tracks[net::kPoseBody].comp) ? tracks[net::kPoseBody].comp : MeshComp(body); }
    void ReattachBody() {
        UObject* mesh = BodyMesh();
        for (auto& h : hitBoxes) if (IsValid(h.comp)) { AttachCompTo(h.comp, mesh, h.bone); SetRelative(h.comp, h.loc, h.rot); }
        gearDirty = !gear.empty();
    }
    UObject* blip = nullptr;        // map / minimap marker
    float tagSize = 0;
    bool tagHidden = false, bikeHidden = false;
    int anim = -1;
    PoseTrack tracks[2];
    FName head{-1, 0};

    bool Spawned() const { return body != nullptr; }
    bool AllValid() const {
        auto ok = [](UObject* a) { return a == nullptr || IsValid(a); };
        if (!ok(body) || !ok(bike) || !ok(tag) || !ok(tracks[0].comp) || !ok(tracks[1].comp)) return false;
        for (auto& p : bodyParts) if (!ok(p.actor)) return false;
        for (auto& p : bikeParts) if (!ok(p.actor)) return false;
        return true;
    }
    void Forget() { body = bike = tag = blip = blocker = bikeBlocker = nullptr; gearComps.clear(); gearDirty = !gear.empty(); blockersOn = 0xFF; hitZones.clear(); hitBoxes.clear(); bodyParts.clear(); bikeParts.clear(); for (auto& t : tracks) t.Reset(); anim = -1; }
    void Despawn() {
        for (auto& p : bodyParts) DestroyActor(p.actor);
        for (auto& p : bikeParts) DestroyActor(p.actor);
        if (IsValid(blip)) { policy::AllowMarker(blip, false); Call(blip, "K2_DestroyComponent"); }
        blip = nullptr;
        DestroyActor(body); DestroyActor(bike); DestroyActor(tag); hitZones.clear(); hitBoxes.clear(); gearComps.clear(); blocker = bikeBlocker = nullptr; gearDirty = !gear.empty(); blockersOn = 0xFF;
        Forget();
    }
    void SetupTrack(int ch, UObject* actor) {
        PoseTrack& t = tracks[ch]; t.Reset();
        t.comp = CreatePoseable(actor, MeshComp(actor));
        if (!t.comp) return;
        SetCompHidden(t.comp, true);
        BuildBoneList(t.comp, t.bones);
        t.mismatchLogged = false;
    }
    void SpawnTag(const FVector& at) {
        tag = SpawnActor(FindClass("TextRenderActor"), at);
        UObject* tc = tag ? Field<UObject*>(tag, off::TextRenderActor_Component) : nullptr;
        if (!IsValid(tc)) return;
        const std::wstring wname = W(nick);
        Params t(Fn(tc, "SetText")); t.Set("Value", FString{wname.c_str(), int32_t(wname.size() + 1), int32_t(wname.size() + 1)}); Call(tc, "SetText", &t);
        Params al(Fn(tc, "SetHorizontalAlignment")); al.Set<uint8_t>("Value", 1); Call(tc, "SetHorizontalAlignment", &al);  // centre
        if (auto f = Fn(tc, "SetVerticalAlignment")) { Params va(f); va.Set<uint8_t>("Value", 2); Call(tc, "SetVerticalAlignment", &va); }
        Params col(Fn(tc, "SetTextRenderColor")); col.Set<uint32_t>("Value", 0xFFF2EADF); Call(tc, "SetTextRenderColor", &col);  // ARGB
        tagSize = 0;
    }
    // The carried weapons: the same meshes on the same sockets of the posed body.
    void BuildGear() {
        gearDirty = false;
        for (UObject* c : gearComps) if (IsValid(c)) if (auto f = Fn(c, "K2_DestroyComponent")) { Params p(f); p.Set("Object", c); Call(c, "K2_DestroyComponent", &p); }
        gearComps.clear();
        UObject* pose = tracks[net::kPoseBody].comp;
        if (!IsValid(body) || !IsValid(pose) || gear.empty()) return;
        net::Reader r(gear.data(), gear.size());
        const int count = r.U8();
        static UObject *skelCls = FindClass("SkeletalMeshComponent"), *staticCls = FindClass("StaticMeshComponent");
        for (int i = 0; i < count && r.ok(); ++i) {
            const std::string path = r.Str(net::kLookPathMax); const bool skeletal = r.U8() != 0; const int parent = int8_t(r.U8());
            const std::string socket = r.Str(128);
            FVector loc, scale; FRotator rot;
            loc.X = r.F32(); loc.Y = r.F32(); loc.Z = r.F32(); rot.Pitch = r.F32(); rot.Yaw = r.F32(); rot.Roll = r.F32();
            scale.X = r.F32(); scale.Y = r.F32(); scale.Z = r.F32();
            if (!r.ok()) break;
            UObject* parentComp = parent < 0 ? BodyMesh() : parent < int(gearComps.size()) ? gearComps[parent] : nullptr;
            UObject* mesh = FindObject(path.c_str());
            if (!mesh) mesh = LoadAsset(path.c_str());
            UObject* c = (IsValid(parentComp) && mesh) ? NewObject(skeletal ? skelCls : staticCls, body) : nullptr;
            gearComps.push_back(c);  // keeps indices in step even when one is missing
            if (!c) continue;
            PropRef<uint8_t>(c, "Mobility", 0x1C8) = 2;  // movable before it registers
            if (skeletal) SetMesh(c, mesh);
            else if (auto f = Fn(c, "SetStaticMesh")) { Params p(f); p.Set("NewMesh", mesh); Call(c, "SetStaticMesh", &p); }
            RegisterComponent(c);
            if (auto f = Fn(c, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(c, "SetCollisionEnabled", &p); }
            AttachCompTo(c, parentComp, socket.empty() || socket == "None" ? FName{0, 0} : FindName(socket.c_str()));
            SetRelative(c, loc, rot);
            if (auto f = Fn(c, "SetRelativeScale3D")) { Params p(f); p.Set("NewScale3D", scale); Call(c, "SetRelativeScale3D", &p); }
        }
        dg::Log("MP ghost %s carries %zu meshes", nick.c_str(), gearComps.size());
    }
    // Server setting: players' bodies and bikes block each other. A capsule on the body and a box
    // on the bike; they ignore bullets and the camera.
    UObject* MakeBlocker(UObject* owner, UObject* parent, bool box) {
        static UObject *capsule = FindClass("CapsuleComponent"), *boxCls = FindClass("BoxComponent");
        UObject* c = NewObject(box ? boxCls : capsule, owner);
        if (!c) return nullptr;
        RegisterComponent(c);
        AttachCompTo(c, parent, FName{0, 0});
        if (box) {
            SetRelative(c, {0, 0, 55}, {0, 0, 0});
            if (auto f = Fn(c, "SetBoxExtent")) { Params p(f); p.Set("InBoxExtent", FVector{110, 35, 55}); Call(c, "SetBoxExtent", &p); }
        } else {
            SetRelative(c, {0, 0, 90}, {0, 0, 0});  // the body's origin is at its feet
            if (auto f = Fn(c, "SetCapsuleSize")) { Params p(f); p.Set<float>("InRadius", 34); p.Set<float>("InHalfHeight", 88); Call(c, "SetCapsuleSize", &p); }
        }
        if (auto f = Fn(c, "SetCollisionObjectType")) { Params p(f); p.Set<uint8_t>("Channel", 1); Call(c, "SetCollisionObjectType", &p); }  // WorldDynamic
        if (auto f = Fn(c, "SetCollisionResponseToAllChannels")) { Params p(f); p.Set<uint8_t>("NewResponse", 0); Call(c, "SetCollisionResponseToAllChannels", &p); }
        if (auto f = Fn(c, "SetCollisionResponseToChannel"))
            for (uint8_t channel : {uint8_t(1), uint8_t(2), uint8_t(5), uint8_t(6)}) {  // WorldDynamic, Pawn, PhysicsBody, Vehicle
                Params p(f); p.Set<uint8_t>("Channel", channel); p.Set<uint8_t>("NewResponse", 2); Call(c, "SetCollisionResponseToChannel", &p);
            }
        return c;
    }
    // The bike drawn under this player: hidden once they are off it, and then out of everything's way
    // (its blocker too): the server's bike they left stands right there and would be thrown into the air.
    void ShowBike(bool show) {
        SetHidden(bike, !show); for (auto& p : bikeParts) SetHidden(p.actor, !show);
        if (auto f = IsValid(bike) ? Fn(bike, "SetActorEnableCollision") : nullptr) { Params p(f); p.SetBool("bNewActorEnableCollision", show); Call(bike, "SetActorEnableCollision", &p); }
        for (auto& part : bikeParts) if (auto f = IsValid(part.actor) ? Fn(part.actor, "SetActorEnableCollision") : nullptr) { Params p(f); p.SetBool("bNewActorEnableCollision", show); Call(part.actor, "SetActorEnableCollision", &p); }
        bikeHidden = !show;
    }
    void UpdateBlockers() {
        if (blockersOn == collisions && (!(collisions & net::CollideBikes) || !bike || bikeBlocker)) return;
        auto enable = [](UObject* c, bool on) { if (auto f = IsValid(c) ? Fn(c, "SetCollisionEnabled") : nullptr) { Params p(f); p.Set<uint8_t>("NewType", on ? 3 : 0); Call(c, "SetCollisionEnabled", &p); } };
        if ((collisions & net::CollidePlayers) && !blocker && IsValid(body)) blocker = MakeBlocker(body, MeshComp(body), false);
        if ((collisions & net::CollideBikes) && !bikeBlocker && IsValid(bike)) {
            bikeBlocker = MakeBlocker(bike, MeshComp(bike), true);
            if (auto f = Fn(bike, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", true); Call(bike, "SetActorEnableCollision", &p); }
            if (auto f = Fn(MeshComp(bike), "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(MeshComp(bike), "SetCollisionEnabled", &p); }
        }
        // A rider's body blocks nothing: behind someone it sits on their bike, and a capsule in its seat
        // throws that bike (and its driver) into the air on the driver's screen.
        enable(blocker, (collisions & net::CollidePlayers) != 0 && !blockerRiding);
        enable(bikeBlocker, (collisions & net::CollideBikes) != 0);
        blockersOn = collisions;
    }
    // Hit boxes: the shapes of the body's physics asset (the game's own hit volumes) on the bones of
    // the posed body. They answer only bullets (collision profile PawnBulletCollision), so players
    // still walk through each other.
    void SpawnHitBoxes() {
        UObject* pose = tracks[net::kPoseBody].comp;
        UObject* main = MeshComp(body);
        if (!IsValid(pose) || !IsValid(main)) return;
        auto objProp = [](UObject* o, const char* name) -> uint8_t* {
            for (auto c = o->Class; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return reinterpret_cast<uint8_t*>(o) + Field<int>(p, off::UProperty_Offset);
            return nullptr;
        };
        UObject* mesh = Field<UObject*>(main, off::Skinned_SkeletalMesh);
        uint8_t* assetField = IsValid(mesh) ? objProp(mesh, "PhysicsAsset") : nullptr;
        UObject* asset = assetField ? *reinterpret_cast<UObject**>(assetField) : nullptr;
        uint8_t* setups = IsValid(asset) ? objProp(asset, "BodySetup") : nullptr;
        if (!setups) { dg::Log("MP ghost %s: no physics asset for hit boxes", nick.c_str()); return; }
        struct Raw { uint8_t* data; int num, max; };
        const Raw list = *reinterpret_cast<Raw*>(setups);
        static UObject *capsule = FindClass("CapsuleComponent"), *sphere = FindClass("SphereComponent"), *box = FindClass("BoxComponent");
        static const FName profile = FindName("PawnBulletCollision");
        // The body itself takes no hits (its bones stay in the reference pose) and blocks nothing.
        for (UObject* c : {main, pose}) if (auto f = Fn(c, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(c, "SetCollisionEnabled", &p); }
        if (auto f = Fn(body, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", true); Call(body, "SetActorEnableCollision", &p); }
        auto make = [&](UObject* cls, FName bone, const FVector& center, const FQuat& rot, net::HitZone zone) -> UObject* {
            if (!cls) return nullptr;
            UObject* c = NewObject(cls, body); if (!c) return nullptr;
            RegisterComponent(c);
            AttachCompTo(c, BodyMesh(), bone);
            SetRelative(c, center, QuatToRotator(rot));
            hitBoxes.push_back({c, bone, center, QuatToRotator(rot)});
            if (auto f = Fn(c, "SetCollisionProfileName")) { Params p(f); p.Set("InCollisionProfileName", profile); Call(c, "SetCollisionProfileName", &p); }
            hitZones[c] = zone;
            return c;
        };
        int made = 0;
        for (int i = 0; i < list.num; ++i) {
            UObject* setup = reinterpret_cast<UObject**>(list.data)[i];
            if (!IsValid(setup)) continue;
            const FName bone = Field<FName>(setup, 0x70);            // BodySetup.BoneName
            const uint8_t* geom = reinterpret_cast<uint8_t*>(setup) + 0x28;  // BodySetup.AggGeom
            const std::string b = NameOf(bone);
            auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
            const net::HitZone zone = b == "Head" || b == "head" ? net::HitZone::Head
                : has("Arm") || has("Hand") ? net::HitZone::Arm
                : has("Leg") || has("Foot") ? net::HitZone::Leg : net::HitZone::Body;
            const Raw spheres = *reinterpret_cast<const Raw*>(geom), boxes = *reinterpret_cast<const Raw*>(geom + 0x10), sphyls = *reinterpret_cast<const Raw*>(geom + 0x20);
            for (int k = 0; k < spheres.num; ++k) {  // KSphereElem: Center @0x60, Radius @0x6C
                const uint8_t* e = spheres.data + k * 0x70;
                if (UObject* c = make(sphere, bone, *reinterpret_cast<const FVector*>(e + 0x60), FQuat{0, 0, 0, 1}, zone)) {
                    if (auto f = Fn(c, "SetSphereRadius")) { Params p(f); p.Set<float>("InSphereRadius", *reinterpret_cast<const float*>(e + 0x6C)); Call(c, "SetSphereRadius", &p); }
                    ++made;
                }
            }
            for (int k = 0; k < boxes.num; ++k) {  // KBoxElem: Center @0x60, Orientation @0x70, X/Y/Z (full size) @0x80
                const uint8_t* e = boxes.data + k * 0x90;
                if (UObject* c = make(box, bone, *reinterpret_cast<const FVector*>(e + 0x60), *reinterpret_cast<const FQuat*>(e + 0x70), zone)) {
                    const float* s = reinterpret_cast<const float*>(e + 0x80);
                    if (auto f = Fn(c, "SetBoxExtent")) { Params p(f); p.Set("InBoxExtent", FVector{s[0] / 2, s[1] / 2, s[2] / 2}); Call(c, "SetBoxExtent", &p); }
                    ++made;
                }
            }
            for (int k = 0; k < sphyls.num; ++k) {  // KSphylElem: Center @0x60, Orientation @0x70, Radius @0x80, Length @0x84 (along Z)
                const uint8_t* e = sphyls.data + k * 0x90;
                if (UObject* c = make(capsule, bone, *reinterpret_cast<const FVector*>(e + 0x60), *reinterpret_cast<const FQuat*>(e + 0x70), zone)) {
                    const float radius = *reinterpret_cast<const float*>(e + 0x80), length = *reinterpret_cast<const float*>(e + 0x84);
                    if (auto f = Fn(c, "SetCapsuleSize")) { Params p(f); p.Set<float>("InRadius", radius); p.Set<float>("InHalfHeight", length / 2 + radius); Call(c, "SetCapsuleSize", &p); }
                    ++made;
                }
            }
        }
        policy::WatchDamage(body);
        dg::Log("MP ghost %s: %d hit boxes from %s", nick.c_str(), made, GetName(asset).c_str());
    }
    // A map marker that follows the ghost body, like the game's bike marker.
    void SpawnBlip() {
        UObject* cls = FindClass("MiniMapLocationComponent");
        if (!cls || !body) return;
        blip = NewObject(cls, body);
        if (!blip) return;
        auto prop = [&](const char* name) -> uint8_t* {
            for (auto c = blip->Class; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return reinterpret_cast<uint8_t*>(blip) + Field<int>(p, off::UProperty_Offset);
            return nullptr;
        };
        static UObject* icon = nullptr;
        if (!IsValid(icon)) { icon = FindObject("/Game/Libraries/UI/Textures/MAP_player_marker.MAP_player_marker"); if (!icon) icon = LoadAsset("/Game/Libraries/UI/Textures/MAP_player_marker.MAP_player_marker"); }
        if (auto p = prop("MapIcon")) *reinterpret_cast<UObject**>(p) = icon;
        if (auto p = prop("LocationType")) *p = 0;   // Player category
        if (auto p = prop("DisplayRule")) *p = 0;    // AlwaysVisible
        if (auto p = prop("EdgeDisplay")) *p = 1;    // stay on the minimap edge when far away
        if (auto p = prop("EdgeDistance")) *p = 0;   // Infinite
        if (auto p = prop("IconScale")) *reinterpret_cast<float*>(p) = 0.55f;
        // On hover: the player's name on the plain card, not the game's story mission card (Objective).
        if (auto p = prop("RollOverType")) *p = 7;   // EMapRolloverType::Basic
        SetBoolProp(blip, "bUseSimpleMapInfoDisplay", true);
        {
            TextValue text{};
            if (EngineText(W(nick), text)) for (auto c = blip->Class; c; c = SuperOf(c)) if (auto p = FindProperty(c, "DisplayName")) { Field<TextValue>(blip, Field<int>(p, off::UProperty_Offset)) = text; break; }
        }
        const float warm[4] = {1.f, 0.78f, 0.35f, 1.f};
        if (auto p = prop("MinimapColor")) std::memcpy(p, warm, sizeof(warm));
        if (auto p = prop("MarkerColor")) std::memcpy(p, warm, sizeof(warm));
        // No name on the marker: an FString written here would point at our heap, and the engine frees
        // it with its own allocator when the component is collected (fatal error after a despawn).
        SetBoolProp(blip, "bGetPositionFromOwner", true);
        RegisterComponent(blip);
        policy::AllowMarker(blip, true);
    }
    void Spawn(const FVector& at) {
        if (!IsValid(L.mesh)) return;
        // Built from the player's look (the game creates the clothing parts itself). Until a client
        // reports its look, and for server bots, the standard Deacon is shown.
        static const std::string kDefaultLook = "/Game/Characters/Deacon/looks/deacon_standard.deacon_standard";
        body = SpawnActor(FindClass("SkeletalMeshActor"), at);
        if (body && !skins::Wear(MeshComp(body), lookPath.empty() ? kDefaultLook : lookPath)) DestroyActor(body);
        if (!body) { dg::Log("MP ghost %s: body spawn failed", nick.c_str()); return; }
        Params mode(Fn(MeshComp(body), "SetAnimationMode")); mode.Set<uint8_t>("InAnimationMode", 1); Call(MeshComp(body), "SetAnimationMode", &mode);
        SetupTrack(net::kPoseBody, body);
        SpawnHitBoxes();
        head = FindName("head");
        SpawnTag(at);
        SpawnBlip();
        dg::Log("MP ghost %s spawned: look=%s parts=%zu tag=%p blip=%p", nick.c_str(), lookPath.empty() ? "(default)" : lookPath.c_str(), bodyParts.size(), tag, blip);
    }
    void PlayAnim(int a) {
        if (tracks[net::kPoseBody].active || a == anim) return;
        UObject* asset = Anim(a); if (!asset) return;
        Params p(Fn(MeshComp(body), "PlayAnimation")); p.Set("NewAnimToPlay", asset); p.SetBool("bLooping", true);
        Call(MeshComp(body), "PlayAnimation", &p);
        anim = a;
    }
    // A remote body's footsteps: the game plays them from its walk animations' notifies, which a body
    // posed from the network has none of. A foot coming down to the ground while the player moves is a
    // step: the game's own step sound and dust of the surface under it (BendSurfaceTypeManager).
    void Footsteps(double now, const FVector& feet, const FVector& cam, const net::PlayerState& s) {
        const PoseTrack& t = tracks[net::kPoseBody];
        if (!t.active || !IsValid(t.comp) || (s.flags & net::StateOnBike)) return;
        const float camD2 = (cam.X - feet.X) * (cam.X - feet.X) + (cam.Y - feet.Y) * (cam.Y - feet.Y) + (cam.Z - feet.Z) * (cam.Z - feet.Z);
        if (camD2 > 4000.f * 4000.f) return;  // 40 m: not heard
        const float speed = std::sqrt(s.vx * s.vx + s.vy * s.vy);
        static const FName bones[2] = {FindName("LeftFoot"), FindName("RightFoot")};
        auto f = Fn(t.comp, "GetBoneLocationByName"); if (!f) return;
        for (int i = 0; i < 2; ++i) {
            Params p(f); p.Set("BoneName", bones[i]); p.Set<uint8_t>("BoneSpace", 0); Call(t.comp, "GetBoneLocationByName", &p);
            const FVector foot = p.Get<FVector>("ReturnValue");
            if (foot.X == 0 && foot.Y == 0) continue;  // no such bone
            const float height = foot.Z - feet.Z;
            if (height > 18.f) { footUp[i] = true; continue; }
            if (height > 12.f || !footUp[i] || speed < 60.f || now - footAt[i] < 0.22) continue;
            footUp[i] = false; footAt[i] = now;
            Footstep(MeshComp(body), foot, camD2 < 1200.f * 1200.f);
        }
    }
    void UpdateTrack(int ch, UObject* actor, std::vector<GhostPart>& parts, double now) {
        PoseTrack& t = tracks[ch];
        if (!actor || !t.comp) return;
        const bool want = t.Usable(now);
        if (want != t.active) {
            SwitchGroupPose(actor, parts, t, want);
            if (ch == net::kPoseBody) {
                anim = -1; ReattachBody();
                // The hidden main mesh would go on playing the fallback animation: its footstep notifies put
                // dirt where its feet are not (on another skeleton, at the hands or the hips).
                if (want) if (UObject* main = MeshComp(actor); IsValid(main)) if (auto f = Fn(main, "Stop")) Call(main, "Stop");
            }
        }
        if (t.active) {
            std::vector<UObject*> slaved;
            for (auto& gp : parts) if (gp.master && IsValid(MeshComp(gp.actor))) slaved.push_back(MeshComp(gp.actor));
            if (ch == net::kPoseBody) for (UObject* part : LookParts(MeshComp(actor))) slaved.push_back(part);
            t.SyncTwins(actor, slaved);
            t.Apply(clock, now, actor);
        }
    }
    bool Sample(double now, net::PlayerState& out) {
        if (snaps.empty()) return false;
        const double t = now - clock.Delay(0.05);
        while (snaps.size() > 2 && snaps[1].t <= t) snaps.pop_front();
        const Snap& a = snaps.front();
        if (snaps.size() == 1 || t <= a.t) { out = a.s; return true; }
        const Snap& b = snaps[1];
        const float k = float(std::clamp((t - a.t) / std::max(b.t - a.t, 1e-3), 0.0, 1.0));
        auto mix = [k](float x, float y) { return x + (y - x) * k; };
        out = b.s;
        out.x = mix(a.s.x, b.s.x); out.y = mix(a.s.y, b.s.y); out.z = mix(a.s.z, b.s.z);
        out.mx = mix(a.s.mx, b.s.mx); out.my = mix(a.s.my, b.s.my); out.mz = mix(a.s.mz, b.s.mz);
        const FQuat mq = Slerp({a.s.mqx, a.s.mqy, a.s.mqz, a.s.mqw}, {b.s.mqx, b.s.mqy, b.s.mqz, b.s.mqw}, k);
        out.mqx = mq.X; out.mqy = mq.Y; out.mqz = mq.Z; out.mqw = mq.W;
        if ((a.s.flags & net::StateHasBike) && (b.s.flags & net::StateHasBike)) {
            out.bx = mix(a.s.bx, b.s.bx); out.by = mix(a.s.by, b.s.by); out.bz = mix(a.s.bz, b.s.bz);
            const FQuat bq = Slerp({a.s.bqx, a.s.bqy, a.s.bqz, a.s.bqw}, {b.s.bqx, b.s.bqy, b.s.bqz, b.s.bqw}, k);
            out.bqx = bq.X; out.bqy = bq.Y; out.bqz = bq.Z; out.bqw = bq.W;
        }
        return true;
    }
    void PlaceTag(const FVector& meshPos, const FVector& cam) {
        if (!IsValid(tag)) return;
        // Above the head bone of whichever mesh is visible; fall back to a fixed height.
        FVector p{meshPos.X, meshPos.Y, meshPos.Z + 205.f};
        UObject* visible = tracks[net::kPoseBody].active ? tracks[net::kPoseBody].comp : MeshComp(body);
        if (head.Index >= 0 && IsValid(visible)) if (auto f = Fn(visible, "GetSocketLocation")) {
            Params sp(f); sp.Set("InSocketName", head); Call(visible, "GetSocketLocation", &sp);
            const FVector h = sp.Get<FVector>("ReturnValue");
            if (Dist2(h, meshPos) < 400.f * 400.f) p = {h.X, h.Y, h.Z + 38.f};
        }
        const float d = std::sqrt(Dist2(p, cam));
        const bool walls = nametags == net::NametagMode::ThroughWalls;
        const bool hide = nametags == net::NametagMode::Hidden || d > (walls ? 15000.f : 9000.f);  // 150 / 90 m
        if (hide != tagHidden) { SetHidden(tag, hide); tagHidden = hide; }
        if (hide) return;
        // The health bar is drawn on screen under the name (DrawPlates); the game says where.
        if (auto f = IsValid(worldController) ? Fn(worldController, "ProjectWorldLocationToScreen") : nullptr) {
            Params pr(f); pr.Set("WorldLocation", FVector{p.X, p.Y, p.Z - 4.f}); Call(worldController, "ProjectWorldLocationToScreen", &pr);
            if (pr.GetBool("ReturnValue")) { const auto at = pr.Get<FVector2D>("ScreenLocation"); NewPlate(at.X, at.Y, d, health, voice::Talking(id)); }
        }
        // Keep it readable from afar without becoming huge up close.
        float size = std::clamp(12.f + d * 0.009f, 14.f, 60.f);
        if (walls && d > 150.f) {
            // Still a world object, but pulled along the view ray to 1.5 m from the camera and scaled
            // down by the same factor: it covers the same spot on screen and nothing stands in front.
            const float k = 150.f / d;
            p = {cam.X + (p.X - cam.X) * k, cam.Y + (p.Y - cam.Y) * k, cam.Z + (p.Z - cam.Z) * k};
            size *= k;
        }
        if (std::fabs(size - tagSize) > (walls ? 0.02f : 1.f)) {
            UObject* tc = Field<UObject*>(tag, off::TextRenderActor_Component);
            Params sz(Fn(tc, "SetWorldSize")); sz.Set("Value", size); Call(tc, "SetWorldSize", &sz);
            tagSize = size;
        }
        const float yaw = std::atan2(cam.Y - p.Y, cam.X - p.X) * 57.2957795f;
        SetActorTransform(tag, p, RotatorToQuat({0, yaw, 0}));
    }
    void Update(double now, const FVector& cam) {
        if (Spawned() && !AllValid()) { dg::Log("MP ghost %s lost its actors (level change?)", nick.c_str()); Forget(); }
        if (now - lastRecv > 10.0) { if (Spawned()) Despawn(); return; }
        net::PlayerState s;
        if (!Sample(now, s)) return;
        FVector meshPos{s.mx, s.my, s.mz}; FQuat meshRot{s.mqx, s.mqy, s.mqz, s.mqw};
        if (!(s.flags & net::StateHasMesh)) {  // bots and scripted players: derive from the actor
            meshPos = {s.x, s.y, s.z - 90.f}; meshRot = RotatorToQuat({0, s.yaw - 90.f, 0});
        }
        if (!Spawned()) {
            if (now < nextSpawnTry) return;
            nextSpawnTry = now + 3.0;
            Spawn(meshPos);
            if (!Spawned()) return;
        }
        // Dead: only the corpse it left is seen; the body (and its hit boxes) comes back with the respawn.
        lastMeshPos = meshPos;
        const bool dead = (s.flags & (net::StateDead | net::StateHidden)) || now < hiddenUntil || skins::CreatorOpen();
        if (dead != deadShown) {
            deadShown = dead;
            SetHidden(body, dead); for (auto& part : bodyParts) SetHidden(part.actor, dead);
            if (auto f = Fn(body, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", !dead); Call(body, "SetActorEnableCollision", &p); }
            blockersOn = 0xFF;  // the body's collision came back: re-apply the blocker setting
            if (dead && IsValid(tag)) { SetHidden(tag, true); tagHidden = true; }
        }
        if ((s.flags & net::StateOtherWorld) && bike && !bikeHidden) ShowBike(false);
        if (dead) return;
        if (gearDirty) BuildGear();
        // Off the bike again, its capsule waits until it is clear of this player (the driver it got off
        // behind): switched on against the driver's bike, it would throw them both.
        bool riding = (s.flags & net::StateOnBike) != 0;
        if (!riding && blockerRiding && IsValid(L.mesh)) {
            const FVector me = C2WPos(L.mesh);
            riding = (me.X - meshPos.X) * (me.X - meshPos.X) + (me.Y - meshPos.Y) * (me.Y - meshPos.Y) < 180.f * 180.f;
        }
        if (riding != blockerRiding) { blockerRiding = riding; blockersOn = 0xFF; }
        UpdateBlockers();
        if (!tracks[net::kPoseBody].active || tracks[net::kPoseBody].Stale(now)) SetActorTransform(body, meshPos, meshRot);
        UpdateTrack(net::kPoseBody, body, bodyParts, now);
        if (!tracks[net::kPoseBody].active) PlayAnim(s.anim);

        // On a model (a truck, the golf cart): the vehicle itself follows the player, no bike of ours.
        UObject* model = nullptr;
        const bool onModel = vehicles::DrivesModel(id, model);
        if (onModel && model && (s.flags & net::StateHasBike)) SetActorTransform(model, {s.bx, s.by, s.bz}, {s.bqx, s.bqy, s.bqz, s.bqw});
        if (onModel) s.flags &= ~net::StateHasBike;
        UObject* srcMesh = L.bikeMesh; const std::vector<UObject*>* srcParts = &L.bikeParts;
        if (!IsValid(srcMesh) || vehicles::IsModel(L.bike)) BikeTemplate(srcMesh, srcParts);  // a bike's meshes, not a truck's
        if (!bike && (s.flags & net::StateHasBike) && IsValid(srcMesh) && now >= nextBikeTry) {
            nextBikeTry = now + 3.0;
            bike = SpawnMeshGroup(srcMesh, *srcParts, {s.bx, s.by, s.bz}, bikeParts);
            if (bike) SetupTrack(net::kPoseBike, bike);
            bikeHidden = false;
        }
        if (bike) {
            const bool hide = !(s.flags & net::StateHasBike);
            if (hide != bikeHidden) ShowBike(!hide);
            if (!hide) {
                if (!tracks[net::kPoseBike].active) SetActorTransform(bike, {s.bx, s.by, s.bz}, {s.bqx, s.bqy, s.bqz, s.bqw});
                UpdateTrack(net::kPoseBike, bike, bikeParts, now);
            }
        }
        health = s.health;
        Footsteps(now, meshPos, cam, s);
        PlaceTag(meshPos, cam);
    }
};
std::map<uint16_t, std::unique_ptr<Ghost>> ghosts;

// ---------------------------------------------------------------- the mode's actors (NPCs)
// Characters placed by the mode (createActor): a body wearing a look and looping an animation, a name
// above it, a capsule players bump into. Spawned near the player, in its virtual world only.
int myWorld = 0;  // the virtual world the server put this player in
std::map<uint16_t, net::Client::Events::IconInfo> worldIcons;  // the mode's icons (createIcon)
struct IconSight { double next = 0; bool clear = true; };
std::map<uint16_t, IconSight> iconSight;  // whether nothing stands between the camera and an icon (checked 5 times a second)
// True when a straight line from `from` reaches close to `to` (the last half metre is not checked:
// the icon hangs over something, like a shopkeeper's head).
bool LineClear(UObject* context, const FVector& from, const FVector& to) {
    static UObject* lib = nullptr;
    if (!IsValid(lib)) lib = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    auto f = lib && IsValid(context) ? Fn(lib, "LineTraceSingle_NEW") : nullptr; if (!f) return true;
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 60) return true;
    const float k = (len - 50) / len;
    Params p(f); p.Set("WorldContextObject", context); p.Set("Start", from); p.Set("End", FVector{from.X + dx * k, from.Y + dy * k, from.Z + dz * k});
    p.Set<uint8_t>("TraceChannel", 0); p.SetBool("bTraceComplex", false); p.SetBool("bIgnoreSelf", true);
    Call(lib, "LineTraceSingle_NEW", &p);
    return !p.GetBool("ReturnValue");
}
struct Npc {
    net::Client::Events::ActorInfo info{};
    bool changed = true;
    UObject* body = nullptr;
    UObject* tag = nullptr;
    UObject* blocker = nullptr;
    std::string worn, playing, named;
    float tagSize = 0;
    double nextTry = 0, nextTag = 0;
    bool tagSight = true;
    void DropTag() { DestroyActor(tag); named.clear(); tagSize = 0; }
    void Despawn() { DestroyActor(body); DropTag(); blocker = nullptr; worn.clear(); playing.clear(); changed = true; }
};
std::map<uint16_t, Npc> npcs;
std::vector<mpui::WorldIcon> npcLabels;
UObject* NpcBlocker(UObject* owner, UObject* parent) {
    static UObject* capsule = FindClass("CapsuleComponent");
    UObject* c = capsule ? NewObject(capsule, owner) : nullptr;
    if (!c) return nullptr;
    RegisterComponent(c);
    AttachCompTo(c, parent, FName{0, 0});
    SetRelative(c, {0, 0, 90}, {0, 0, 0});  // the body's origin is at its feet
    if (auto f = Fn(c, "SetCapsuleSize")) { Params p(f); p.Set<float>("InRadius", 34); p.Set<float>("InHalfHeight", 88); Call(c, "SetCapsuleSize", &p); }
    if (auto f = Fn(c, "SetCollisionObjectType")) { Params p(f); p.Set<uint8_t>("Channel", 1); Call(c, "SetCollisionObjectType", &p); }  // WorldDynamic
    if (auto f = Fn(c, "SetCollisionResponseToAllChannels")) { Params p(f); p.Set<uint8_t>("NewResponse", 0); Call(c, "SetCollisionResponseToAllChannels", &p); }
    if (auto f = Fn(c, "SetCollisionResponseToChannel"))
        for (uint8_t channel : {uint8_t(1), uint8_t(2), uint8_t(5), uint8_t(6)}) { Params p(f); p.Set<uint8_t>("Channel", channel); p.Set<uint8_t>("NewResponse", 2); Call(c, "SetCollisionResponseToChannel", &p); }
    if (auto f = Fn(c, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 3); Call(c, "SetCollisionEnabled", &p); }
    return c;
}
void UpdateNpcs(double now, const FVector& player, const FVector& cam) {
    npcLabels.clear();
    for (auto& [id, n] : npcs) {
        if (n.body && !IsValid(n.body)) n.Despawn();  // the level went away
        if (n.tag && !IsValid(n.tag)) n.DropTag();
        const FVector at{n.info.x, n.info.y, n.info.z};
        const float d2 = Dist2(at, player);
        const bool here = n.info.world < 0 || n.info.world == myWorld;
        const bool want = here && d2 < (n.body ? 30000.f * 30000.f : 25000.f * 25000.f);  // 250 m, kept up to 300 m
        if (!want) { if (n.body || n.tag) n.Despawn(); continue; }
        if (!n.body) {
            n.DropTag();  // a name never outlives its body: it is made again with the new one
            if (now < n.nextTry) continue;
            n.nextTry = now + 3;
            n.body = SpawnActor(FindClass("SkeletalMeshActor"), at);
            if (!n.body) continue;
            n.changed = true;
        }
        UObject* mesh = MeshComp(n.body);
        if (n.changed) {
            n.changed = false;
            SetActorTransform(n.body, at, RotatorToQuat({0, n.info.yaw - 90.f, 0}));  // meshes face their +Y
            if (n.worn != n.info.look && skins::Wear(mesh, n.info.look)) { n.worn = n.info.look; n.playing.clear(); }
            if (n.info.solid && !n.blocker) n.blocker = NpcBlocker(n.body, mesh);
            if (IsValid(n.blocker)) if (auto f = Fn(n.blocker, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", n.info.solid ? 3 : 0); Call(n.blocker, "SetCollisionEnabled", &p); }
            if (auto f = Fn(n.body, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", n.info.solid); Call(n.body, "SetActorEnableCollision", &p); }
            if (IsValid(mesh)) if (auto f = Fn(mesh, "SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType", 0); Call(mesh, "SetCollisionEnabled", &p); }
        }
        // The animation loops (a new look stops it).
        if (IsValid(mesh) && !n.info.anim.empty()) {
            bool playing = n.playing == n.info.anim;
            if (playing) if (auto f = Fn(mesh, "IsPlaying")) { Params p(f); Call(mesh, "IsPlaying", &p); playing = p.GetBool("ReturnValue"); }
            if (!playing) {
                UObject* anim = FindObject(n.info.anim.c_str());
                if (!anim) anim = LoadAsset(n.info.anim.c_str());
                if (anim) {
                    if (auto f = Fn(mesh, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 1); Call(mesh, "SetAnimationMode", &p); }
                    if (auto f = Fn(mesh, "PlayAnimation")) { Params p(f); p.Set("NewAnimToPlay", anim); p.SetBool("bLooping", true); Call(mesh, "PlayAnimation", &p); }
                }
                n.playing = n.info.anim;
            }
        }
        // Use the UTF-8 HUD font rather than the game's Latin-only TextRender font.
        // Publish projected labels to the render thread through the existing icon snapshot.
        n.DropTag();
        const float dc = std::sqrt(Dist2(at, cam));
        if(n.info.name.empty() || dc>=2500.f || nametags==net::NametagMode::Hidden) continue;
        const FVector label{at.X,at.Y,at.Z+205.f};
        if(nametags!=net::NametagMode::ThroughWalls) {
            if(now>=n.nextTag) { n.nextTag=now+.2; n.tagSight=LineClear(worldPawn,cam,label); }
            if(!n.tagSight) continue;
        }
        if(auto project=IsValid(worldController)?Fn(worldController,"ProjectWorldLocationToScreen"):nullptr) {
            Params pr(project); pr.Set("WorldLocation",label); Call(worldController,"ProjectWorldLocationToScreen",&pr);
            if(pr.GetBool("ReturnValue")) {
                const auto pos=pr.Get<FVector2D>("ScreenLocation");
                npcLabels.push_back({pos.X,pos.Y,0,std::clamp(1500.f/std::max(dc,1.f),.7f,1.f),0xFF6BD1F2,"","{F2D16B}"+n.info.name});
            }
        }
    }
}

void DespawnAll() { for (auto& [id, g] : ghosts) g->Despawn(); for (auto& [id, n] : npcs) n.Despawn(); }

// ---------------------------------------------------------------- quest markers
// Each marker is the game's own MapWaypointMarkerBP actor (as used for mission objectives): map and
// minimap icon, in-world waypoint icon and an optional road route. Arrival is decided by the server.
// All three coordinates belong to the mode/editor. Do not ground-snap: a vertical trace can select
// a building's roof instead of its interior floor, moving the visible icon away from the actual task.
struct QuestMarker {
    net::Client::Events::MarkerInfo info{};
    UObject* actor = nullptr;
};
std::map<uint16_t, QuestMarker> questMarkers;

void DestroyMarker(QuestMarker& m) {
    if (IsValid(m.actor)) {
        if (auto mini = Field<UObject*>(m.actor, 0x348); IsValid(mini)) policy::AllowMarker(mini, false);  // MiniMapLocationComponent
        DestroyActor(m.actor);
    }
    m.actor = nullptr;
}
void DressWaypoint(UObject* actor, const net::Client::Events::MarkerInfo& info);
void SpawnMarker(QuestMarker& m) {
    UObject* cls = FindClass("MapWaypointMarkerBP_C");
    if (!cls) return;
    m.actor = SpawnActor(cls, {m.info.x, m.info.y, m.info.z});
    if (!m.actor) return;
    if (auto f = Fn(m.actor, "SetWaypointMarkerType")) { Params p(f); p.Set<uint8_t>("Type", 3); Call(m.actor, "SetWaypointMarkerType", &p); }  // ScriptWaypoint
    DressWaypoint(m.actor, m.info);
    dg::Log("MP marker spawned at %.0f %.0f %.0f (category %u style %u route %d)", m.info.x, m.info.y, m.info.z, m.info.category, m.info.style, m.info.route);
}
// FText made by the engine (Conv_StringToText): the parameter frame is never destructed, so its
// reference moves into the property we copy it to. Never build texts on our own heap.
bool EngineText(const std::wstring& value, TextValue& out) {
    static UObject* lib = nullptr;
    if (!IsValid(lib)) lib = FindObject("/Script/Engine.Default__KismetTextLibrary");
    auto f = lib ? Fn(lib, "Conv_StringToText") : nullptr; if (!f) return false;
    Params p(f); p.Set("InString", FString{value.c_str(), int32_t(value.size() + 1), int32_t(value.size() + 1)}); Call(lib, "Conv_StringToText", &p);
    out = p.Get<TextValue>("ReturnValue"); return true;
}
void SetMarkerTooltip(UObject* mini, const net::Client::Events::MarkerInfo& info) {
    if (!IsValid(mini) || (info.title.empty() && info.description.empty())) return;
    for (const auto& [prop, value] : {std::pair<const char*, const std::string*>{"DisplayName", &info.title}, {"DisplayDescription", &info.description}}) {
        TextValue text{};
        if (value->empty() || !EngineText(W(*value), text)) continue;
        for (auto cls = mini->Class; cls; cls = SuperOf(cls)) if (auto p = FindProperty(cls, prop)) { Field<TextValue>(mini, Field<int>(p, off::UProperty_Offset)) = text; break; }
    }
}
UObject* MarkerIcon(const net::Client::Events::MarkerInfo& info) {
    static const char* icons[] = {"MAP_objective_story", "MAP_objective_encampment", "MAP_objective_infestation", "MAP_objective_ambush",
                                  "MAP_objective_NERO_checkpoint", "MAP_enemy_death", "MAP_nest", "MAP_alert", "MAP_objective_encampment_minor", "MAP_objective_story_minor"};
    std::string path = info.icon;
    if (path.empty()) { const char* n = info.category < 10 ? icons[info.category] : "MAP_objective_story_minor"; path = std::string("/Game/Libraries/UI/Textures/") + n + "." + n; }
    UObject* icon = FindObject(path.c_str());
    return icon ? icon : LoadAsset(path.c_str());
}
void DressWaypoint(UObject* actor, const net::Client::Events::MarkerInfo& info) {
    if (auto f = Fn(actor, "SetWaypointType")) { Params p(f); p.Set<uint8_t>("Category", info.category); p.Set<uint8_t>("VisualCategory", info.style); Call(actor, "SetWaypointType", &p); }
    UObject* icon = MarkerIcon(info);
    if (icon) if (auto f = Fn(actor, "SetMarkerTexture")) { Params p(f); p.Set("Texture", icon); Call(actor, "SetMarkerTexture", &p); }
    if (auto mini = Field<UObject*>(actor, 0x348); IsValid(mini)) {  // MiniMapLocationComponent
        if (icon) for (auto cls = mini->Class; cls; cls = SuperOf(cls)) if (auto prop = FindProperty(cls, "MapIcon")) { Field<UObject*>(mini, Field<int>(prop, off::UProperty_Offset)) = icon; break; }
        SetBoolProp(mini, "bRenderWaypointMarker", true);  // the in-world icon, before the map policy records it
        SetMarkerTooltip(mini, info);
        policy::AllowMarker(mini, true);
    }
}

// The map draws a road route only for the waypoint its MissionWaypointMarker field points at (it asks
// that actor for the path points). The game's own mission waypoint hides itself without a tracked
// story mission, so the newest marker with route=true lends the map our script waypoint instead.
uint16_t routeMarker = 0;
UObject* routeActor = nullptr;   // our waypoint currently lent to the map
UObject* gameMission = nullptr;  // the game's mission waypoint, restored afterwards
UObject* ObjectField(UObject* o, const char* name) {
    for (auto cls = IsValid(o) ? o->Class : nullptr; cls; cls = SuperOf(cls)) if (auto p = FindProperty(cls, name)) return Field<UObject*>(o, Field<int>(p, off::UProperty_Offset));
    return nullptr;
}
void SetObjectField(UObject* o, const char* name, UObject* value) {
    for (auto cls = IsValid(o) ? o->Class : nullptr; cls; cls = SuperOf(cls)) if (auto p = FindProperty(cls, name)) { Field<UObject*>(o, Field<int>(p, off::UProperty_Offset)) = value; return; }
}
void RefreshMapRoute(UObject* map) {
    if (auto f = IsValid(map) ? Fn(map, "UpdateWaypointPathsBP") : nullptr) { Params p(f); p.Set<uint8_t>("WaypointMarkerType", 0); Call(map, "UpdateWaypointPathsBP", &p); }
}
// The minimap draws only the route of the game's own mission waypoint (the one the HUD hands out),
// and only from where it was built. It stays hidden (its icon would double ours) and is rebuilt
// as the player moves.
FVector routeTarget{}, routeFrom{};
uint8_t MarkerTypeOf(UObject* o) {
    for (auto cls = o->Class; cls; cls = SuperOf(cls)) if (auto p = FindProperty(cls, "WaypointMarkerType")) return Field<uint8_t>(o, Field<int>(p, off::UProperty_Offset));
    return 255;
}
UObject* GameMissionWaypoint(UObject*) {
    if (IsValid(gameMission)) return gameMission;
    UObject* cls = FindClass("MapWaypointMarkerBP_C");
    for (int i = 0, n = NumObjects(); i < n && cls; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o->Class != cls || !IsValid(o) || o == routeActor || MarkerTypeOf(o) != 0) continue;
        bool ours = false; for (auto& [id, m] : questMarkers) ours |= m.actor == o;
        if (!ours) return gameMission = o;
    }
    return nullptr;
}
void DrawPath(UObject* actor, const FVector& from, const FVector& to) {
    if (!IsValid(actor)) return;
    if (auto f = Fn(actor, "InitPath")) { Params p(f); p.Set("StartPosition", from); p.Set("EndPosition", to); Call(actor, "InitPath", &p); }
    if (auto f = Fn(actor, "SetPathVisible")) { Params p(f); p.SetBool("Visible", true); Call(actor, "SetPathVisible", &p); }
}
void ClearPath(UObject* actor) {
    if (!IsValid(actor)) return;
    Call(actor, "ClearPath");
    if (auto f = Fn(actor, "SetPathVisible")) { Params p(f); p.SetBool("Visible", false); Call(actor, "SetPathVisible", &p); }
}
void UpdateRoute(double now, const FVector& player) {
    uint16_t want = 0;
    for (auto& [id, m] : questMarkers) if (m.info.route && IsValid(m.actor)) want = id;  // highest id = newest
    UObject* map = FindRuntimeInstance("BP_Map3D_C");
    static double next = 0;
    if (!want) {
        if (routeMarker) {
            ClearPath(GameMissionWaypoint(map));
            if (IsValid(map)) { SetObjectField(map, "MissionWaypointMarker", IsValid(gameMission) ? gameMission : nullptr); RefreshMapRoute(map); }
            policy::SetRouteOwned(false);  // the story suppression hides the route again
            routeMarker = 0; routeActor = nullptr;
        }
        return;
    }
    if (!IsValid(map)) return;
    UObject* actor = questMarkers[want].actor;
    UObject* current = ObjectField(map, "MissionWaypointMarker");
    if (current != actor) {
        if (IsValid(current) && current != routeActor) gameMission = current;
        SetObjectField(map, "MissionWaypointMarker", actor);
        routeMarker = want; routeActor = actor; next = 0; routeTarget = {};
        policy::SetRouteOwned(true);
        dg::Log("MP route to marker %u", want);
    }
    if (now < next) return;
    next = now + 1.0;
    const auto& info = questMarkers[want].info;
    const FVector target{info.x, info.y, info.z};
    // Rebuilt when the target changed or the player has gone 30 m from where the route starts.
    const float dx = player.X - routeFrom.X, dy = player.Y - routeFrom.Y;
    const bool moved = dx * dx + dy * dy > 3000.f * 3000.f;
    const bool retarget = target.X != routeTarget.X || target.Y != routeTarget.Y || target.Z != routeTarget.Z;
    if (!moved && !retarget) return;
    routeFrom = player; routeTarget = target;
    UObject* game = GameMissionWaypoint(map);
    if (IsValid(game)) { SetHidden(game, true); DrawPath(game, player, target); }  // minimap
    DrawPath(actor, player, target);                                           // big map
    if (UObject* path = ObjectField(map, "PathObjectiveWaypoint_BP"); IsValid(path)) {
        SetHidden(path, false);
        if (auto f = Fn(path, "SetActorTickEnabled")) { Params p(f); p.SetBool("bEnabled", true); Call(path, "SetActorTickEnabled", &p); }
    }
    RefreshMapRoute(map);
}

// The game's death flow reloads the last save to give the player a new body; in multiplayer that is
// skipped (policy) and a new body is made here where the old one fell, the same way the game makes it
// (GameMode.SpawnDefaultPawnFor). The server then moves it to the respawn point.
void RespawnInPlace(UObject* dead, UObject* controller) {
    UObject* gm = FindRuntimeInstance("BendDefaultGameMode_C");
    auto spawn = gm ? Fn(gm, "SpawnDefaultPawnFor") : nullptr;
    if (!spawn || !IsValid(controller) || !IsValid(dead)) { dg::Log("MP respawn in place: not possible"); return; }
    Params sp(spawn); sp.Set("NewPlayer", controller); sp.Set("StartSpot", dead);
    Call(gm, "SpawnDefaultPawnFor", &sp);
    UObject* body = sp.Get<UObject*>("ReturnValue");
    if (!IsValid(body)) { dg::Log("MP respawn in place: no pawn spawned"); return; }
    Call(controller, "UnPossess");
    if (auto f = Fn(controller, "Possess")) { Params p(f); p.Set("InPawn", body); Call(controller, "Possess", &p); }
    Call(dead, "K2_DestroyActor");
    // BendPlayerController.MyBendPawn is set by the restart the game would have done; the pause menu
    // (OpenPauseMenu) refuses to open while it still names the dead body.
    for (auto c = controller->Class; c; c = SuperOf(c)) if (auto prop = FindProperty(c, "MyBendPawn")) { Field<UObject*>(controller, Field<int>(prop, off::UProperty_Offset)) = body; break; }
    for (auto [obj, fn] : {std::pair<UObject*, const char*>{controller, "OnPlayerRespawned"}, {gm, "PlayerRespawned"}})
        if (auto f = Fn(obj, fn)) { Params p(f); p.Set("PlayerController", controller); Call(obj, fn, &p); }
    if (UObject* hud = FindRuntimeInstance("UI_HudWidget_C")) if (auto f = Fn(hud, "SetHudVisible")) { Params p(f); p.SetBool("Visible", true); Call(hud, "SetHudVisible", &p); }
    // The death flow turned the pause menu off; the reload it no longer runs would turn it back on.
    if (UObject* ui = FindRuntimeInstance("UIManagerBP_C")) if (auto f = Fn(ui, "SetPauseMenuDisabled")) { Params p(f); p.SetBool("IsDisabled", false); Call(ui, "SetPauseMenuDisabled", &p); }
    dg::Log("MP respawned in place: %s", GetName(body).c_str());
}

// ---------------------------------------------------------------- death: bodies left behind
// A copy of the dead player's look, at its last pose and place, playing one of Deacon's death
// animations and staying on its last frame. The newest few are kept for a few minutes.
struct Corpse { UObject* body = nullptr; double at = 0; };
std::deque<Corpse> corpses;
void LeaveCorpse(UObject* mesh, const std::string& look) {
    if (!IsValid(mesh)) return;
    FVector at{}; FRotator rot{};
    if (auto f = Fn(mesh, "K2_GetComponentLocation")) { Params p(f); Call(mesh, "K2_GetComponentLocation", &p); at = p.Get<FVector>("ReturnValue"); }
    if (auto f = Fn(mesh, "K2_GetComponentRotation")) { Params p(f); Call(mesh, "K2_GetComponentRotation", &p); rot = p.Get<FRotator>("ReturnValue"); }
    UObject* body = SpawnActor(FindClass("SkeletalMeshActor"), at);
    if (!body) return;
    static const std::string kDefaultLook = "/Game/Characters/Deacon/looks/deacon_standard.deacon_standard";
    if (!skins::Wear(MeshComp(body), look.empty() ? kDefaultLook : look)) { DestroyActor(body); return; }
    if (auto f = Fn(body, "K2_SetActorRotation")) { Params p(f); p.Set("NewRotation", rot); Call(body, "K2_SetActorRotation", &p); }
    static const char* kAnims[] = {"/Game/Animation/Deacon/death/dcn_death_torso.dcn_death_torso", "/Game/Animation/Deacon/death/dcn_death_torso_02.dcn_death_torso_02",
                                   "/Game/Animation/Deacon/death/dcn_death_ranged.dcn_death_ranged"};
    UObject* anim = nullptr;
    for (int i = 0; i < 3 && !anim; ++i) { const char* path = kAnims[(GetTickCount64() / 7 + i) % 3]; anim = FindObject(path); if (!anim) anim = LoadAsset(path); }
    UObject* comp = MeshComp(body);
    if (auto f = Fn(comp, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 1); Call(comp, "SetAnimationMode", &p); }
    if (anim) if (auto f = Fn(comp, "PlayAnimation")) { Params p(f); p.Set("NewAnimToPlay", anim); p.SetBool("bLooping", false); Call(comp, "PlayAnimation", &p); }
    corpses.push_back({body, Clock()});
    while (corpses.size() > 8) { DestroyActor(corpses.front().body); corpses.pop_front(); }
    dg::Log("MP corpse left at %.0f %.0f %.0f", at.X, at.Y, at.Z);
}
void UpdateCorpses(double now) {
    while (!corpses.empty() && (now - corpses.front().at > 300 || !IsValid(corpses.front().body))) { DestroyActor(corpses.front().body); corpses.pop_front(); }
}
void SetHudVisible(bool visible) {
    if (UObject* hud = FindRuntimeInstance("UI_HudWidget_C")) if (auto f = Fn(hud, "SetHudVisible")) { Params p(f); p.SetBool("Visible", visible); Call(hud, "SetHudVisible", &p); }
}
// The local player's body (and what is attached to it: weapons, backpack) hidden while it "lies dead".
void HideLocalBody(UObject* pawn, bool hidden) {
    if (!IsValid(pawn)) return;
    SetHidden(pawn, hidden);
    if (auto f = Fn(pawn, "GetAttachedActors")) {
        Params p(f); Call(pawn, "GetAttachedActors", &p);
        struct Arr { UObject** data; int32_t num, max; };
        const auto arr = p.Get<Arr>("OutActors");
        for (int i = 0; i < arr.num && arr.data; ++i) if (IsValid(arr.data[i])) SetHidden(arr.data[i], hidden);
    }
}
// Hides again only what shows (a weapon it drew): the player's pawn counts its hides, every hide needs
// its own unhide, so hiding the hidden again would leave it invisible after HideLocalBody(false).
void KeepLocalBodyHidden(UObject* pawn) {
    if (!IsValid(pawn)) return;
    if (!GetBoolProp(pawn, "bHidden")) SetHidden(pawn, true);
    if (auto f = Fn(pawn, "GetAttachedActors")) {
        Params p(f); Call(pawn, "GetAttachedActors", &p);
        struct Arr { UObject** data; int32_t num, max; };
        const auto arr = p.Get<Arr>("OutActors");
        for (int i = 0; i < arr.num && arr.data; ++i) if (IsValid(arr.data[i]) && !GetBoolProp(arr.data[i], "bHidden")) SetHidden(arr.data[i], true);
    }
}

void UpdateMarkers(double now, const FVector& player) {
    static double nextShow = 0;
    const bool check = now >= nextShow;
    if (check) nextShow = now + 1.0;
    for (auto& [id, m] : questMarkers) {
        if (m.actor && !IsValid(m.actor)) m.actor = nullptr;  // level change
        if (!m.actor) SpawnMarker(m);
        // Opening the map hides script waypoints it does not own: show ours again.
        if (check && IsValid(m.actor) && GetBoolProp(m.actor, "bHidden")) SetHidden(m.actor, false);
    }
    UpdateRoute(now, player);
}

void OnPose(uint16_t id, const uint8_t* data, size_t size) {
    auto it = ghosts.find(id);
    if (it == ghosts.end() || size < kPoseHeader) return;
    net::Reader r(data, size);
    const uint8_t ch = r.U8(); r.U16(); const uint32_t hash = r.U32(); const uint16_t count = r.U16(); const double t = r.Get<double>();
    const FVector pos{r.F32(), r.F32(), r.F32()}; const FQuat rot{r.F32(), r.F32(), r.F32(), r.F32()};
    if (!r.ok() || ch > 1 || count > net::kMaxPoseBones || size != kPoseHeader + count * sizeof(net::QBone)) return;
    it->second->tracks[ch].Push(it->second->clock, hash, t, pos, rot, reinterpret_cast<const net::QBone*>(data + kPoseHeader), count, Clock());
}

void HookClient() {
    client.on.welcome = [](const std::string& name, const std::string& mode) {
        serverName = name; modeName = mode; phase = Phase::Joined; lostAfterJoin = false; everJoined = true; reportedLook.clear(); reportedGear.clear();
        clientlua::SetServer(host + "_" + std::to_string(port) + "_" + mode);  // the client scripts' storage file
        steam::CancelTicket();  // the server has checked it: nobody else can reuse it
        hud::SetBanner(L"", L"");
        hud::AddMessage(0xA9C4E4FF, std::wstring(lang::Pick(L"Подключено к ",L"Connected to ")) + W(name) + L" (" + W(mode) + L")");
        dg::Log("MP joined %s (mode %s) as id %u", name.c_str(), mode.c_str(), client.localId());
    };
    client.on.disconnected = [](const std::string& reason) {
        dg::Log("MP disconnected: %s", reason.c_str());
        steam::CancelTicket();
        mpui::Reset();
        vehicles::Reset();
        zombies::Reset();
        carry::Reset();
        objects::Reset();
        playeranim::Reset();
        blips::Reset();
        sounds::Reset();
        voice::Reset(); players::Reset(); downed = Downed{}; downedPlayers.clear(); policy::SetDownedInputBlock(false); policy::SetModeControls(0);
        webui::Reset();
        clientlua::Reset();
        traps::Reset();
        throws::Reset();
        images::Reset();
        worldIcons.clear(); iconSight.clear();
        myWorld = 0;
        for (auto& [id, n] : npcs) n.Despawn();
        npcs.clear();
        DespawnAll(); ghosts.clear();
        for (auto& [mid, m] : questMarkers) DestroyMarker(m);
        questMarkers.clear();
        if (!everJoined) { phase = Phase::Failed; failReason = W(reason); return; }
        lostAfterJoin = true; phase = Phase::Connecting;
        hud::SetBanner(lang::Pick(L"Соединение с сервером потеряно",L"Connection to the server lost"), W(reason) + lang::Pick(L"  ·  F9 — переподключиться",L"  ·  F9 — reconnect"));
        hud::AddMessage(0xE07070FF, std::wstring(lang::Pick(L"Отключено: ",L"Disconnected: ")) + W(reason));
    };
    client.on.playerJoin = [](const net::RemotePlayer& p) {
        auto g = std::make_unique<Ghost>(); g->id = p.id; g->nick = p.nick;
        ghosts[p.id] = std::move(g);
    };
    client.on.playerQuit = [](uint16_t id, uint8_t) {
        downedPlayers.erase(id);
        if (auto it = ghosts.find(id); it != ghosts.end()) { it->second->Despawn(); ghosts.erase(it); }
    };
    client.on.state = [](uint16_t id, const net::PlayerState& s) {
        auto it = ghosts.find(id); if (it == ghosts.end()) return;
        Ghost& g = *it->second;
        const double now = Clock();
        const double t = g.clock.Map(s.time > 0 ? s.time : now, now);
        if (!g.snaps.empty() && t <= g.snaps.back().t) return;
        g.snaps.push_back({t, s}); while (g.snaps.size() > 16) g.snaps.pop_front();
        g.lastRecv = now;
    };
    client.on.pose = OnPose;
    client.on.message = [](uint32_t rgba, const std::string& text) { hud::AddMessage(rgba, W(text)); };
    client.on.setTime = [](float hours, bool paused) { DGMP_SetTimeOfDay(hours); DGMP_SetTimePaused(paused ? 1 : 0); };
    client.on.setWeather = [](int id) { if (id < 0) DGMP_SetWeatherAutomatic(1); else DGMP_SetWeather(uint32_t(id)); };
    client.on.teleport = [](float x, float y, float z, float yaw) {
        teleport = {true, {x, y, z}, yaw, !initialTeleportSeen};
        lastTeleportAt = Clock();
        initialTeleportSeen = true;
    };
    client.on.hint = [](const std::string& title, const std::string& text, const std::string& image, bool pause, float seconds) {
        // The game's own tutorial popup; the overlay banner only if that is not possible right now.
        if (!policy::ShowNativeHint(worldController, W(title), W(text), image, pause, seconds)) hud::ShowHint(W(title), W(text), seconds > 0 ? seconds : 7.0);
    };
    client.on.item = [](net::ItemAction action, const std::string& key, int amount) { inventory::Apply(action, key, amount); };
    client.on.bike = [](net::BikeAction action, float x, float y, float z, float yaw) { inventory::RequestBike(action, {x, y, z}, yaw); };
    client.on.freeze = [](bool on) { inventory::SetFrozen(on); };
    client.on.setHealth = [](float percent) { wantedHealth = std::clamp(percent, 0.f, 100.f); dg::Log("MP: server sets health to %.0f%%", wantedHealth); };
    client.on.customize = [](bool cancel, const std::string& title, const std::string& families, const std::string& current,
                             const std::vector<std::pair<std::string, std::string>>& choices, const float* place) {
        skins::Options o; o.cancel = cancel; o.title = title; o.current = current.empty() ? reportedLook : current;
        for (auto& [name, look] : choices) o.choices.push_back({name, look});
        for (size_t at = 0; at < families.size();) { size_t k = families.find(',', at); if (k == std::string::npos) k = families.size(); if (k > at) o.families.push_back(families.substr(at, k - at)); at = k + 1; }
        if (place) {  // the server moves the player there first
            o.hasYaw = true; o.yaw = place[3];
            pendingCreator = {true, o, {place[0], place[1], place[2]}, Clock()};
            return;
        }
        skins::OpenCreator(o);
    };
    client.on.spectate = [](bool on, const std::vector<uint16_t>& only) { spec.want = on; spec.only = only; };
    client.on.dialog = [](const net::Client::Events::DialogInfo& d) { mpui::OnDialog(d); };
    client.on.itemWindow = [](uint16_t id, const std::string& json) { mpui::OnItemWindow(id, json); };
    client.on.playerList = [](const std::string& json) { players::OnList(json); };
    client.on.playerMenu = [](bool on, uint8_t key, const std::string& label, float range) { mpui::OnPlayerMenu(on, key, label, range); };
    client.on.ask = [](uint16_t id, const std::string& text, float seconds) { mpui::OnAsk(id, text, seconds); };
    client.on.controls = [](uint8_t f) { policy::SetModeControls(f); dg::Log("MP controls taken: %u", f); };
    client.on.deathMode = [](bool d) { downed.mode = d; dg::Log("MP death mode: %s", d ? "downed" : "instant"); };
    client.on.downedState = [](bool on, float bleed, float giveUp, const std::string& anim) {
        const double now = GetTickCount64() / 1000.0;
        if (on) {
            downed.pending = false; downed.on = true; downed.until = now + bleed; downed.giveUpAt = now + giveUp;
            if (!anim.empty() && anim != downed.anim) { downed.anim = anim; playeranim::OnPlay(anim, net::AnimLoop | net::AnimFreeze, 1); }
            dg::Log("MP downed: %.0f s to bleed out, give up in %.0f s", bleed, giveUp);
        } else if (downed.on || downed.pending) {
            downed = Downed{true};
            playeranim::OnPlay("", 0, 1);
            policy::SetDownedInputBlock(false);
            dg::Log("MP downed: over");
        }
    };
    client.on.playerDowned = [](uint16_t id, bool on) { if (on) downedPlayers.insert(id); else downedPlayers.erase(id); };
    client.on.trap = [](const uint8_t* data, size_t size) { traps::OnMessage(data, size); };
    client.on.hud = [](const std::string& id, const std::string& json) { mpui::OnHud(id, json); };
    client.on.interaction = [](uint16_t id, const net::Client::Events::InteractionInfo* info) { mpui::OnInteraction(id, info); };
    client.on.watchKeys = [](const std::vector<uint8_t>& keys) { mpui::OnWatchKeys(keys); };
    client.on.carry = [](uint16_t id, const net::Client::Events::CarryInfo* info) { carry::OnCarry(id, info); };
    client.on.object = [](uint16_t id, const net::Client::Events::ObjectInfo* info) { objects::OnObject(id, info); };
    client.on.sound = [](const std::string& path, const float* at, float volume, float pitch) {
        FVector v{}; if (at) v = FVector{at[0], at[1], at[2]};
        sounds::Play(path, at ? &v : nullptr, volume, pitch);
    };
    client.on.playerShot = [](uint16_t id, const std::string& cls, float x, float y, float z) { sounds::OnPlayerShot(id, cls, FVector{x, y, z}); };
    client.on.playerThrow = [](uint16_t id, const std::string& mesh, float scale, float x, float y, float z, float vx, float vy, float vz) { throws::OnThrow(id, mesh, scale, FVector{x, y, z}, FVector{vx, vy, vz}); };
    client.on.voice = [](uint16_t id, net::VoiceRoute route, net::VoiceCodec codec, int gain, float range, const uint8_t* frame, size_t size) { voice::OnFrame(id, route, codec, gain, range, frame, size); };
    client.on.voiceSetup = [](uint8_t flags, const std::string& radio, const std::string& call) { voice::OnSetup(flags, radio, call); };
    client.on.blip = [](uint16_t id, const net::Client::Events::BlipInfo* info) { blips::OnBlip(id, info); };
    client.on.playAnim = [](const std::string& path, uint8_t flags, float rate) { playeranim::OnPlay(path, flags, rate); };
    client.on.objectMove = [](uint16_t id, const float to[6], float seconds, float elapsed) { objects::OnMove(id, to, seconds, elapsed); };
    client.on.setWorld = [](int world) { myWorld = world; mpui::SetWorld(world); vehicles::SetWorld(world); carry::SetWorld(world); objects::SetWorld(world); blips::SetWorld(world); traps::SetWorld(world); dg::Log("MP: virtual world %d", world); };
    client.on.vehicle = [](uint16_t id, const net::Client::Events::VehicleInfo* info) { vehicles::OnVehicle(id, info); };
    client.on.vehicleAllow = [](uint16_t id, int seat) { vehicles::OnAllow(id, seat); };
    client.on.vehicleEject = [](uint16_t id) { vehicles::OnEject(id); };
    client.on.vehicleRoute = [](uint16_t id, const std::vector<float>& points, float speed, float elapsed, bool loop) { vehicles::OnRoute(id, points, speed, elapsed, loop); };
    client.on.zombie = [](net::Msg op, const uint8_t* data, size_t size) { zombies::OnMessage(op, data, size); };
    client.on.imageList = [](const std::vector<std::pair<std::string, std::string>>& list) { images::OnList(list, client); };
    client.on.imageData = [](const std::string& name, int part, int parts, const std::string& bytes) { images::OnData(name, part, parts, bytes); };
    client.on.uiList = [](const std::vector<std::pair<std::string, std::string>>& files) { webui::OnList(files, client); };
    client.on.uiData = [](const std::string& path, int part, int parts, const std::string& bytes) { webui::OnData(path, part, parts, bytes); };
    client.on.ui = [](net::UiOp op, const std::string& name, const std::string& text, bool focus) { webui::OnUi(op, name, text, focus); };
    client.on.scriptList = [](const std::vector<std::pair<std::string, std::string>>& files) { clientlua::OnList(files, client); };
    client.on.scriptData = [](const std::string& path, int part, int parts, const std::string& bytes) { clientlua::OnData(path, part, parts, bytes); };
    client.on.scriptEvent = [](const std::string& name, const std::string& json) { clientlua::OnServer(name, json); };
    client.on.icon = [](uint16_t id, const net::Client::Events::IconInfo* info) { if (info) worldIcons[id] = *info; else worldIcons.erase(id); };
    client.on.actor = [](uint16_t id, const net::Client::Events::ActorInfo* info) {
        if (!info) { if (auto it = npcs.find(id); it != npcs.end()) { it->second.Despawn(); npcs.erase(it); } return; }
        Npc& n = npcs[id]; n.info = *info; n.changed = true;
    };
    client.on.settings = [](net::NametagMode mode, uint8_t coll) { nametags = mode; collisions = coll; dg::Log("MP nametags mode %d, collisions %d", int(mode), int(coll)); };
    client.on.gear = [](uint16_t id, const uint8_t* data, size_t size) {
        auto it = ghosts.find(id);
        if (it == ghosts.end()) return;
        it->second->gear.assign(data, data + size);
        it->second->gearDirty = true;
    };
    // The game's own objective HUD (top-left panel, "new / completed / failed" callouts, meter).
    client.on.objective = [](uint8_t state, uint8_t category, uint8_t style, const std::string& title, const std::string& text, int xp) {
        // The reward panel lives inside the objective panel: removing the panel under it would hide it
        // and leave the game paused. Removal waits until it is closed (UpdateObjectiveRemoval).
        if (state == 4 && policy::RewardPanelOpen()) { pendingRemoval = {true, category, style}; return; }
        if (state != 4) pendingRemoval.pending = false;
        UObject* hud = FindRuntimeInstance("UI_HudWidget_C");
        auto f = hud ? Fn(hud, "DisplayObjective") : nullptr;
        if (!f) return;
        policy::SetQuestHud(state != 4);  // Remove: the story suppression takes the panel back
        if (state == 5 || state == 6) policy::WatchRewardScreens();  // the reward screens pause the game
        TextValue t{}, sub{};
        EngineText(W(title), t); EngineText(W(text), sub);
        Params p(f);
        p.Set<uint8_t>("HUD_State", state); p.Set<uint8_t>("Category", category); p.Set<uint8_t>("Type", style);
        p.Set("Title", t); p.Set("SubTitle", sub); p.SetBool("NewData", true); p.Set<int32_t>("Index", 0);
        // The big "new / completed" callout reads BendRewardsScreenData, not Title: fill it, and use the
        // Reward prompt type (Accept waits for a button press and never goes away on its own).
        if (auto rp = FindProperty(f, "RewardScreenData")) {
            uint8_t* data = static_cast<uint8_t*>(p.Data()) + Field<int>(rp, off::UProperty_Offset);
            TextValue desc{}; EngineText(W(text), desc);
            data[0x00] = category;                                     // Category
            std::memcpy(data + 0x08, &t, sizeof(t));                   // TitleText
            std::memcpy(data + 0x20, &sub, sizeof(sub));               // SubtitleText
            std::memcpy(data + 0x50, &xp, sizeof(xp));                 // XPGained
            std::memcpy(data + 0x58, &desc, sizeof(desc));             // DescriptionText
            data[0x70] = 1;                                            // PromptType = Reward
        }
        Call(hud, "DisplayObjective", &p);
        dg::Log("MP objective state %u: %s / %s", state, title.c_str(), text.c_str());
    };
    client.on.objectiveMeter = [](bool show, const std::string& title, float percent) {
        UObject* hud = FindRuntimeInstance("UI_HudWidget_C");
        if (!hud) return;
        static bool shown = false;
        if (!show) { if (shown) Call(hud, "RemoveObjectiveMeter"); shown = false; return; }
        if (!shown) {
            auto f = Fn(hud, "DisplayObjectiveMeter"); if (!f) return;
            TextValue t{}; EngineText(W(title), t);
            Params p(f); p.Set<uint8_t>("Type", 0); p.Set("Title", t); p.Set<float>("Percent", percent); p.SetBool("Invert", false); p.SetBool("IsHealthBar", false);
            Call(hud, "DisplayObjectiveMeter", &p);
            shown = true;
            dg::Log("MP objective meter: %s %.2f", title.c_str(), percent);
        }
        // The meter is the mission timer widget: left running it sweeps on its own. Stop the timer
        // and set the fill directly.
        UObject* timer = ObjectField(ObjectField(hud, "_Objectives"), "MissionTimerWidget");
        if (!IsValid(timer)) return;
        if (auto f = Fn(timer, "SetTimerPaused")) { Params p(f); p.SetBool("IsPaused", true); Call(timer, "SetTimerPaused", &p); }
        if (auto f = Fn(timer, "SetPercent")) { Params p(f); p.Set<float>("Percent", percent); Call(timer, "SetPercent", &p); }
    };
    client.on.worldEdits = [](const std::string& text) {
        json::Value v;
        if (text.empty() || !json::Value::Parse(text, v)) { worldedit::Clear(); return; }
        worldedit::Set(v, false);
    };
    client.on.damage = [](uint16_t from, float amount, net::HitZone zone, const std::string& weapon) {
        // Through the game's own damage path: hit reactions, armour, and the MP death (ModifyDamage). With
        // the game's damage type of that weapon and from where the attacker stands, the body flinches the
        // way it does when an NPC hits it (a plain ApplyDamage only reddened the screen).
        if (!IsValid(worldPawn) || !Statics()) return;
        UObject* type = DamageTypeFor(weapon);
        FVector from_{}, at{};
        if (auto it = ghosts.find(from); it != ghosts.end()) from_ = it->second->lastMeshPos;
        if (auto f = Fn(worldPawn, "K2_GetActorLocation")) { Params p(f); Call(worldPawn, "K2_GetActorLocation", &p); at = p.Get<FVector>("ReturnValue"); }
        auto point = Fn(Statics(), "ApplyPointDamage");
        if (point && (from_.X != 0 || from_.Y != 0)) {
            const FVector dir{at.X - from_.X, at.Y - from_.Y, 0.f};
            Params p(point); p.Set("DamagedActor", worldPawn); p.Set<float>("BaseDamage", amount); p.Set("HitFromDirection", dir);
            p.Set("DamageCauser", worldPawn); if (type) p.Set("DamageTypeClass", type);
            Call(Statics(), "ApplyPointDamage", &p);
        } else if (auto f = Fn(Statics(), "ApplyDamage")) {
            Params p(f); p.Set("DamagedActor", worldPawn); p.Set<float>("BaseDamage", amount); p.Set("DamageCauser", worldPawn);
            if (type) p.Set("DamageTypeClass", type);
            Call(Statics(), "ApplyDamage", &p);
        }
        dg::Log("MP: hit by player %u for %.1f (zone %d), health now %.1f", from, amount, int(zone), Field<float>(worldPawn, 0x1FD4));
    };
    client.on.corpse = [](uint16_t id) {
        auto it = ghosts.find(id);
        if (it != ghosts.end() && it->second->body) {
            LeaveCorpse(MeshComp(it->second->body), it->second->lookPath);
            it->second->hiddenUntil = GetTickCount64() / 1000.0 + 2;  // until its dead state arrives
        }
    };
    client.on.worldItem = [](uint16_t id, const std::string* item, int count, float x, float y, float z, float yaw) { gameevents::SetWorldItem(id, item, count, {x, y, z}, yaw); };
    client.on.marker = [](uint16_t id, const net::Client::Events::MarkerInfo* info) {
        auto it = questMarkers.find(id);
        if (!info) { if (it != questMarkers.end()) { DestroyMarker(it->second); questMarkers.erase(it); } return; }
        auto& m = questMarkers[id];
        DestroyMarker(m);  // updated: rebuilt with the new settings
        m.info = *info;
    };
    client.on.look = [](uint16_t id, const std::string& path) {
        if (id == client.localId()) { wantedLook = path; dg::Log("MP server look for us: %s", path.c_str()); return; }
        auto it = ghosts.find(id); if (it == ghosts.end() || it->second->lookPath == path) return;
        it->second->lookPath = path;
        if (it->second->Spawned()) it->second->Despawn();  // rebuilt in the new look next frame
    };
}

// Who we are on Steam, for the server to check (admins and bans go by SteamID).
void SetSteamIdentity() {
    uint64_t id = 0; std::string ticket;
    if (steam::Identity(id)) steam::NewTicket(ticket);
    client.SetIdentity(id, ticket);
}

void StartConnect() {
    connectStarted = true; phase = Phase::Connecting; initialTeleportSeen = false;
    dg::Log("MP connecting to %s:%u as %s", host.c_str(), port, nick.c_str());
    SetSteamIdentity();
    if (!client.Connect(host, port, nick, password)) { phase = Phase::Failed; if (failReason.empty()) failReason = lang::Pick(L"Не удалось начать подключение",L"Could not start connecting"); }
}
// ---------------------------------------------------------------- editor: quest preview
// The in-game quest editor (editorui) writes the quest being edited to quest_preview.json; its points are
// shown with the same quest markers a server would send (ids from 50000 up). Selected step: gold.
constexpr uint16_t kPreviewBase = 50000;
// Quest items of the previewed quest, as the real pickup models (moved/turned in place when edited).
struct PreviewItem { std::string key; FVector at{}; float yaw = 0; UObject* actor = nullptr; };
std::vector<PreviewItem> previewItems;
void UpdatePreviewItems(const std::vector<PreviewItem>& want) {
    for (size_t i = want.size(); i < previewItems.size(); ++i) DestroyActor(previewItems[i].actor);
    previewItems.resize(want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        auto& have = previewItems[i];
        const auto& w = want[i];
        if (have.key != w.key && have.actor) DestroyActor(have.actor);
        if (!IsValid(have.actor)) {
            have = w;
            have.actor = gameevents::SpawnPreviewItem(w.key, w.at, w.yaw, worldPawn);
            continue;
        }
        if (std::fabs(have.at.X - w.at.X) > 0.5f || std::fabs(have.at.Y - w.at.Y) > 0.5f || std::fabs(have.at.Z - w.at.Z) > 0.5f || std::fabs(have.yaw - w.yaw) > 0.5f) {
            const float half = w.yaw * 0.00872664626f;
            SetActorTransform(have.actor, w.at, FQuat{0, 0, std::sin(half), std::cos(half)});
            have.at = w.at; have.yaw = w.yaw;
        }
    }
}
void ClearPreview() {
    UpdatePreviewItems({});
    for (auto it = questMarkers.begin(); it != questMarkers.end();) {
        if (it->first >= kPreviewBase) { DestroyMarker(it->second); it = questMarkers.erase(it); } else ++it;
    }
}
// Markers are updated in place (moved, re-dressed, added, removed): rebuilding all of them on every edit
// destroyed waypoint widgets the renderer was still using.
void LoadPreview(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    json::Value quest;
    if (ss.str().empty() || !json::Value::Parse(ss.str(), quest) || !quest.isObject()) { ClearPreview(); return; }
    const int selected = int(quest["selected"].asNumber(-1));
    const json::Value& steps = quest["steps"];
    std::vector<net::Client::Events::MarkerInfo> want;
    std::vector<PreviewItem> items;
    auto add = [&](const json::Value& at, int step, uint8_t style, const std::string& title, const std::string& text) {
        if (!at["x"].isNumber() || !at["y"].isNumber()) return;
        net::Client::Events::MarkerInfo m{};
        m.x = float(at["x"].asNumber()); m.y = float(at["y"].asNumber()); m.z = float(at["z"].asNumber());
        m.category = step == selected ? 0 : 9;  // story (gold) for the selected step, task otherwise
        m.style = style; m.route = false;
        m.title = title; m.description = text;
        want.push_back(m);
    };
    for (size_t i = 0; i < steps.size(); ++i) {
        const json::Value& st = steps[i];
        const json::Value& tv = st["text"];
        const std::string type = st["type"].asString(), text = tv.isObject() ? (tv["en"].asString().empty() ? tv["ru"].asString() : tv["en"].asString()) : tv.asString();
        const std::string title = lang::Pick("Этап ", "Step ") + std::to_string(i + 1);
        if (type == "goto" || type == "talk") add(st, int(i), 0, title, text);  // (a talk at an NPC has no point here)
        else if (type == "hold") add(st, int(i), 5, title, text);                     // search
        else if (type == "deliver") {                                                  // boxes: pick-up (search look) and drop-off
            if (st["pickup"].isObject()) add(st["pickup"], int(i), 5, title, text);
            if (st["dropoff"].isObject()) add(st["dropoff"], int(i), 0, title, text);
        }
        else if (type == "collect") {
            if (st["area"].isObject()) add(st["area"], int(i), 2, title, text);        // area look for zones
            for (const auto& item : st["spawn"].elements()) {
                items.push_back({item["item"].asString(), {float(item["x"].asNumber()), float(item["y"].asNumber()), float(item["z"].asNumber())}, float(item["yaw"].asNumber())});
            }
        }
    }
    if (quest["kind"].asString() == "job" && quest["giver"].isObject()) {                 // the NPC who gives the job
        const json::Value& name = quest["giver"]["name"];
        add(quest["giver"], -2, 0, lang::Pick("Выдаёт работу", "Gives the job"), name.isObject() ? name["en"].asString() : name.asString());
    }
    UpdatePreviewItems(items);
    unsigned changed = 0;
    for (size_t i = 0; i < want.size(); ++i) {
        const uint16_t id = uint16_t(kPreviewBase + i);
        auto it = questMarkers.find(id);
        const auto& w = want[i];
        if (it == questMarkers.end()) { QuestMarker m; m.info = w; questMarkers[id] = m; ++changed; continue; }  // spawned by UpdateMarkers
        auto& m = it->second;
        const bool look = m.info.category != w.category || m.info.style != w.style || m.info.title != w.title || m.info.description != w.description;
        const bool moved = std::fabs(m.info.x - w.x) > 1 || std::fabs(m.info.y - w.y) > 1 || std::fabs(m.info.z - w.z) > 1;
        if (look && IsValid(m.actor)) DressWaypoint(m.actor, w);
        if (moved) {
            m.info = w;
            if (IsValid(m.actor)) SetActorTransform(m.actor, {w.x, w.y, w.z}, FQuat{0, 0, 0, 1});
        } else if (look) {
            m.info = w;
        }
        changed += look || moved;
    }
    for (auto it = questMarkers.lower_bound(uint16_t(kPreviewBase + want.size())); it != questMarkers.end();) {
        DestroyMarker(it->second); it = questMarkers.erase(it); ++changed;
    }
    if (changed) dg::Log("Editor quest preview: %zu markers (%u changed)", want.size(), changed);
}

}  // namespace

void UpdateEditorPreview(double now, UObject* pawn) {
    if (!IsValid(pawn)) return;
    worldPawn = pawn;
    static double nextCheck = 0;
    static std::filesystem::file_time_type seen{};
    if (now >= nextCheck) {
        nextCheck = now + 0.5;
        const auto file = std::filesystem::path(profile::Root()) / L"quest_preview.json";
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        if (ec) { if (seen != std::filesystem::file_time_type{}) { seen = {}; ClearPreview(); } }
        else if (stamp != seen) { seen = stamp; LoadPreview(file); }
    }
    FVector here{};
    if (auto f = Fn(pawn, "K2_GetActorLocation")) { Params p(f); Call(pawn, "K2_GetActorLocation", &p); here = p.Get<FVector>("ReturnValue"); }
    UpdateMarkers(now, here);
}

namespace {
}  // namespace

// ---------------------------------------------------------------- spectator
// The own body is hidden and parked under the watched player (the world streams in around it); a
// camera of its own circles the watched player, turned with the mouse. Left/right arrows (or the
// mouse buttons) switch players.
FRotator ControlRotation() {
    auto f = IsValid(worldController) ? Fn(worldController, "GetControlRotation") : nullptr;
    if (!f) return {};
    Params p(f); Call(worldController, "GetControlRotation", &p); return p.Get<FRotator>("ReturnValue");
}
void ViewThrough(UObject* target) {
    if (auto f = IsValid(worldController) && IsValid(target) ? Fn(worldController, "SetViewTargetWithBlend") : nullptr) { Params p(f); p.Set("NewViewTarget", target); p.Set<float>("BlendTime", 0.f); Call(worldController, "SetViewTargetWithBlend", &p); }
}
// A camera actor that fills the whole window (a letterboxed view target leaves the screen broken).
UObject* SpawnCamera(const FVector& at) {
    UObject* cam = SpawnActor(FindClass("CameraActor"), at);
    if (cam) for (auto k = cam->Class; k; k = SuperOf(k)) if (auto prop = FindProperty(k, "CameraComponent")) {
        UObject* comp = Field<UObject*>(cam, Field<int>(prop, off::UProperty_Offset));
        if (IsValid(comp)) if (auto f = Fn(comp, "SetConstraintAspectRatio")) { Params a(f); a.SetBool("bInConstrainAspectRatio", false); Call(comp, "SetConstraintAspectRatio", &a); }
        break;
    }
    return cam;
}
void SetMovementNone(UObject* pawn, bool none) {
    UObject* move = nullptr;
    for (auto c = pawn->Class; c && !move; c = SuperOf(c)) if (auto prop = FindProperty(c, "CharacterMovement")) move = Field<UObject*>(pawn, Field<int>(prop, off::UProperty_Offset));
    if (auto f = IsValid(move) ? Fn(move, "SetMovementMode") : nullptr) { Params p(f); p.Set<uint8_t>("NewMovementMode", none ? 0 : 1); Call(move, "SetMovementMode", &p); }
}
float WrapDeg(float a) { a = std::fmod(a + 180.f, 360.f); if (a < 0) a += 360.f; return a - 180.f; }
// A camera at `at` looking at `from`, but not through the ground or a wall: in front of what is between.
FVector ClearView(UObject* context, const FVector& from, FVector at) {
    static UObject* kismet = nullptr; static UObject* statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr; auto brk = statics ? Fn(statics, "BreakHitResult") : nullptr;
    if (!trace || !brk || !IsValid(context)) return at;
    Params t(trace); t.Set("WorldContextObject", context); t.Set("Start", from); t.Set("End", at);
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    if (!t.GetBool("ReturnValue") || !hitProp || !inProp) return at;
    Params b(brk);
    std::memcpy(static_cast<uint8_t*>(b.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &b);
    const FVector hitAt = b.Get<FVector>("Location");
    const float dx = from.X - hitAt.X, dy = from.Y - hitAt.Y, dz = from.Z - hitAt.Z, l = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (l > 1.f) { const float k = std::min(60.f, l) / l; at = {hitAt.X + dx * k, hitAt.Y + dy * k, hitAt.Z + dz * k}; }
    return at;
}
// In a helicopter: the body hidden inside it, the view from a camera round it (the mouse turns it).
void UpdateHeliView(UObject* pawn) {
    static UObject* cam = nullptr; static bool on = false;
    UObject* heli = vehicles::HelicopterSeat();
    UObject* root = heli ? Field<UObject*>(heli, off::Actor_RootComponent) : nullptr;
    if (IsValid(root) && !on) {
        HideLocalBody(pawn, true);
        cam = SpawnCamera(C2WPos(root));
        ViewThrough(cam);
        on = true;
    } else if (!IsValid(root) && on) {
        ViewThrough(pawn);
        DestroyActor(cam);
        HideLocalBody(pawn, false);
        on = false;
    }
    if (!on || !IsValid(cam)) return;
    FRotator r = ControlRotation();
    r.Pitch = std::clamp(WrapDeg(r.Pitch), -60.f, 15.f); r.Roll = 0;
    const float rad = 0.0174532925f, cp = std::cos(r.Pitch * rad);
    const FVector c = C2WPos(root);
    const FVector from{c.X, c.Y, c.Z + 250.f};
    const FVector at = ClearView(heli, from, {c.X - std::cos(r.Yaw * rad) * cp * 1800.f, c.Y - std::sin(r.Yaw * rad) * cp * 1800.f, c.Z + 250.f - std::sin(r.Pitch * rad) * 1800.f});
    SetActorTransform(cam, at, RotatorToQuat(r));
}
void UpdateSpectator(UObject* pawn) {
    if (!IsValid(pawn) || !IsValid(worldController)) return;
    if (spec.want && !spec.on) {
        HideLocalBody(pawn, true);
        BoolArg(worldController, "SetIgnoreMoveInput", "bNewMoveInput", true);
        BoolArg(pawn, "SetActorEnableCollision", "bNewActorEnableCollision", false);
        SetMovementNone(pawn, true);
        if (auto f = Fn(worldController, "GetViewTarget")) { Params p(f); Call(worldController, "GetViewTarget", &p); spec.previousView = p.Get<UObject*>("ReturnValue"); }
        spec.cam = SpawnCamera(C2WPos(Field<UObject*>(pawn, off::Actor_RootComponent)));
        ViewThrough(spec.cam);
        spec.lastControl = ControlRotation(); spec.target = 0xFFFF; spec.on = true; spectatorHidden = true;
        // Its hidden body does nothing: no attacks, weapons, interactions (their sounds were heard).
        policy::SetSpectatorInputBlock(true);
        if (auto f = Fn(pawn, "SetCanMeleeAttack")) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), false); Call(pawn, "SetCanMeleeAttack", &p); }
        SetBoolProp(pawn, "CanDoMelee", false);
        dg::Log("MP spectator on");
    } else if (!spec.want && spec.on) {
        ViewThrough(IsValid(spec.previousView) ? spec.previousView : pawn);
        DestroyActor(spec.cam);
        SetMovementNone(pawn, false);
        BoolArg(pawn, "SetActorEnableCollision", "bNewActorEnableCollision", true);
        BoolArg(worldController, "SetIgnoreMoveInput", "bNewMoveInput", false);
        HideLocalBody(pawn, false);
        policy::SetSpectatorInputBlock(false);
        if (auto f = Fn(pawn, "SetCanMeleeAttack")) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), true); Call(pawn, "SetCanMeleeAttack", &p); }
        SetBoolProp(pawn, "CanDoMelee", true);
        spec.on = false; spectatorHidden = false;
        { std::lock_guard g(plateLock); spectatorCaption.clear(); }
        dg::Log("MP spectator off");
    }
    if (!spec.on) return;
    if (!IsValid(spec.cam)) { spec.cam = SpawnCamera(spec.point); ViewThrough(spec.cam); }
    // Who can be watched, in id order.
    std::vector<uint16_t> list;
    for (auto& [id, g] : ghosts)
        if (g->Spawned() && !g->deadShown && (spec.only.empty() || std::find(spec.only.begin(), spec.only.end(), id) != spec.only.end())) list.push_back(id);
    int at = int(std::find(list.begin(), list.end(), spec.target) - list.begin());
    if (at >= int(list.size())) at = 0;
    const bool focus = [] { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }();
    const int keys[4] = {VK_RIGHT, VK_LBUTTON, VK_LEFT, VK_RBUTTON};
    int step = 0;
    for (int k = 0; k < 4; ++k) {
        const bool down = focus && (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
        if (down && !spec.keys[k]) step += k < 2 ? 1 : -1;
        spec.keys[k] = down;
    }
    if (!list.empty()) { at = (at + step + int(list.size()) * 4) % int(list.size()); spec.target = list[at]; }
    // Orbit: the game's mouse look turns the control rotation; the camera follows its changes.
    const FRotator c = ControlRotation();
    spec.rot.Yaw = WrapDeg(spec.rot.Yaw + WrapDeg(c.Yaw - spec.lastControl.Yaw));
    spec.rot.Pitch = std::clamp(spec.rot.Pitch + WrapDeg(c.Pitch - spec.lastControl.Pitch), -70.f, 40.f);
    spec.lastControl = c;
    std::string caption;
    if (auto g = ghosts.find(spec.target); g != ghosts.end() && !list.empty()) {
        const FVector m = g->second->lastMeshPos;
        spec.point = {m.X, m.Y, m.Z + 150.f};
        caption = std::string(lang::Pick("Наблюдение: ", "Spectating: ")) + g->second->nick + "    \xE2\x86\x90 \xE2\x86\x92";
    } else caption = lang::Pick("Наблюдение: ждём игроков", "Spectating: waiting for players");
    { std::lock_guard g(plateLock); spectatorCaption = caption; }
    const float rad = 0.0174532925f, cp = std::cos(spec.rot.Pitch * rad), sp = std::sin(spec.rot.Pitch * rad), cy = std::cos(spec.rot.Yaw * rad), sy = std::sin(spec.rot.Yaw * rad);
    const float dist = 550.f;
    const FVector eye = ClearView(pawn, spec.point, {spec.point.X - cp * cy * dist, spec.point.Y - cp * sy * dist, spec.point.Z - sp * dist});  // not under the ground
    SetActorTransform(spec.cam, eye, RotatorToQuat(spec.rot));
    // A weapon it drew, the game showing the body again: hidden again. It hangs under the ground: it must
    // not start walking (it fell and died there).
    static double nextHide = 0;
    if (const double t = GetTickCount64() / 1000.0; t >= nextHide) { nextHide = t + 0.5; KeepLocalBodyHidden(pawn); SetMovementNone(pawn, true); }
    // The hidden body waits under the watched player: the world around it streams in.
    if (auto f = Fn(pawn, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", FVector{spec.point.X, spec.point.Y, spec.point.Z - 400.f}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(pawn, "K2_SetActorLocation", &p); }
}

// A remote player's body took damage on this screen. Only hits this player made are reported; the
// victim's own client applies them.
void OnGhostDamage(UObject* actor, UObject* component, float damage, UObject* instigator, UObject* causer) {
    if (damage > 0) zombies::OnDamage(actor, damage, IsValid(worldController) && instigator == worldController, causer);  // (its own pawns)
    if (damage <= 0 || !IsValid(worldController) || instigator != worldController) return;
    for (auto& [id, g] : ghosts) {
        if (g->body != actor) continue;
        auto z = g->hitZones.find(component);
        const net::HitZone zone = z != g->hitZones.end() ? z->second : net::HitZone::Body;
        const std::string weapon = IsValid(causer) ? GetName(causer->Class) : std::string();
        client.SendHit(id, damage, zone, weapon);
        MarkHit();
        dg::Log("MP: hit %s for %.1f (zone %d, %s)", g->nick.c_str(), damage, int(zone), weapon.c_str());
        return;
    }
}

// ---------------------------------------------------------------- melee on players
// The game's melee only strikes its own characters: a remote player's body is not a target, so no punch
// or knife reached anyone. Each press of the melee button (the controller's Melee, a script function) is
// checked a moment later: when a swing is on, the nearest remote body in front within reach is hit.
// (The button does not come through ProcessEvent; the pawn's IsCurrentlyInAMeleeAttack does tell.)
double meleeSwingAt = 0, lastMeleeHit = 0;
bool meleeWasOn = false, meleePending = false;
struct MeleeKind { const char* key; float damage, reach; };
MeleeKind MeleeOf(const std::string& cls) {
    std::string c; for (char ch : cls) c += char(std::tolower(static_cast<unsigned char>(ch)));
    if (c.find("fist") != std::string::npos || c.empty()) return {"fists", 12.f, 170.f};
    if (c.find("knife") != std::string::npos) return {"knife", 30.f, 180.f};
    return {"weapon", 40.f, 230.f};  // bats, axes, machetes, stool legs
}
void MeleeSwing(UObject* pawn) {
    UObject* weapon = nullptr;
    if (auto f = Fn(pawn, "GetEquippedItemCurrentlyInHand")) { Params p(f); Call(pawn, "GetEquippedItemCurrentlyInHand", &p); weapon = p.Get<UObject*>("ReturnValue"); }
    const std::string cls = IsValid(weapon) ? GetName(weapon->Class) : "Bend_WpnMelee_Fists_C";
    if (IsValid(weapon) && cls.find("Melee") == std::string::npos && cls.find("Fist") == std::string::npos) return;  // a gun's butt: not here
    const MeleeKind kind = MeleeOf(cls);
    FVector at{}; FRotator rot{};
    if (auto f = Fn(pawn, "K2_GetActorLocation")) { Params p(f); Call(pawn, "K2_GetActorLocation", &p); at = p.Get<FVector>("ReturnValue"); }
    if (auto f = Fn(pawn, "K2_GetActorRotation")) { Params p(f); Call(pawn, "K2_GetActorRotation", &p); rot = p.Get<FRotator>("ReturnValue"); }
    const float yaw = rot.Yaw * 0.0174532925f, fx = std::cos(yaw), fy = std::sin(yaw);
    const float feet = at.Z - 90.f;
    uint16_t best = 0xFFFF; float bestD = kind.reach;
    for (auto& [id, g] : ghosts) {
        if (!g->Spawned() || g->deadShown) continue;
        const FVector m = g->lastMeshPos;  // the body's feet
        const float dx = m.X - at.X, dy = m.Y - at.Y, d = std::sqrt(dx * dx + dy * dy);
        if (d > bestD || std::fabs(m.Z - feet) > 150.f) continue;
        if (d > 40.f && (dx * fx + dy * fy) / d < 0.5f) continue;  // not in front (60 degrees each side)
        best = id; bestD = d;
    }
    if (best == 0xFFFF) { dg::Log("MP: melee swing, no one in reach (%s)", cls.c_str()); return; }
    lastMeleeHit = Clock();
    client.SendHit(best, kind.damage, net::HitZone::Body, cls);
    MarkHit();
    dg::Log("MP: melee hit %s for %.0f (%s, %.1f m)", ghosts[best]->nick.c_str(), kind.damage, cls.c_str(), bestD / 100);
}
void UpdateMelee(double now, UObject* pawn) {
    // A swing starts when the attack does (the flag stays on some 2.5 s after one swing: once per start).
    const bool on = CallBool(pawn, "IsCurrentlyInAMeleeAttack");
    if (on && !meleeWasOn) { meleeSwingAt = now; meleePending = true; }
    meleeWasOn = on;
    if (!meleePending || now - meleeSwingAt < 0.2) return;
    meleePending = false;
    if (now - lastMeleeHit >= 0.35) MeleeSwing(pawn);
}

namespace {
// One step of a remote body at `foot`: the game's surface sound and dust (step_foot_boot, the name its
// walk animations use) of what is under it. `near`: the player's own step sound (louder, close).
void Footstep(UObject* mesh, const FVector& foot, bool close) {
    static UObject *kismet = nullptr, *manager = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(manager)) {
        UObject* cdo = FindObject("/Script/BendGame.Default__BendSurfaceTypeManager");
        if (auto f = IsValid(cdo) ? Fn(cdo, "GetBendSurfaceTypeManager") : nullptr) { Params p(f); Call(cdo, "GetBendSurfaceTypeManager", &p); manager = p.Get<UObject*>("ReturnValue"); }
    }
    auto trace = IsValid(kismet) ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, surface = Statics() ? Fn(Statics(), "GetSurfaceType") : nullptr;
    auto play = IsValid(manager) ? Fn(manager, "PlaySurfaceImpactFX") : nullptr;
    if (!trace || !surface || !play || !IsValid(mesh)) return;
    Params t(trace); t.Set("WorldContextObject", mesh); t.Set("Start", FVector{foot.X, foot.Y, foot.Z + 40.f}); t.Set("End", FVector{foot.X, foot.Y, foot.Z - 60.f});
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", true); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    uint8_t type = 0; FVector at = foot;
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(surface, "Hit");
    if (t.GetBool("ReturnValue") && hitProp && inProp) {
        Params h(surface);
        std::memcpy(static_cast<uint8_t*>(h.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
        Call(Statics(), "GetSurfaceType", &h);
        type = h.Get<uint8_t>("ReturnValue");
    }
    static const FName step = FindName("step_foot_boot");
    Params p(play); p.Set("SkeletalMesh", mesh); p.Set<uint8_t>("SurfaceType", type); p.Set("FXName", step); p.SetBool("IsPlayer", close);
    p.Set("Location", at); p.Set<float>("Volume", 1.f);
    Call(manager, "PlaySurfaceImpactFX", &p);
    static int logged = 0;
    if (logged < 6) { ++logged; dg::Log("MP: remote footstep at %.0f %.0f %.0f, surface %u%s", at.X, at.Y, at.Z, type, close ? " (near)" : ""); }
}
// The game's damage type for another player's weapon class (its hit reactions): blades, blunt, fists, bullets.
UObject* DamageTypeFor(const std::string& weapon) {
    std::string c; for (char ch : weapon) c += char(std::tolower(static_cast<unsigned char>(ch)));
    const char* name = c.empty() ? "DmgTypeBP_Bullet" : c.find("fist") != std::string::npos ? "DmgTypeBP_Melee_Fist"
        : c.find("knife") != std::string::npos ? "DmgTypeBP_Melee_BootKnife" : c.find("melee") != std::string::npos ? "DmgTypeBP_Melee_Blunt"
        : c.find("rock") != std::string::npos ? "DmgTypeBP_BulletRock" : "DmgTypeBP_Bullet";
    static std::map<std::string, UObject*> cache;
    UObject*& cls = cache[name];
    if (!IsValid(cls)) {
        cls = FindClass((std::string(name) + "_C").c_str());
        if (!cls) {
            const std::string path = std::string("/Game/Libraries/Blueprints/DamageTypes/") + name + "." + name + "_C";
            cls = FindObject(path.c_str()); if (!cls) cls = LoadAsset(path.c_str());
        }
        if (!cls) dg::Log("MP: damage type %s not found", name);
    }
    return cls;
}
}  // namespace

bool Init() {
    const std::string target = Env(L"DGMP_CONNECT");
    nick = Env(L"DGMP_NICK"); password = Env(L"DGMP_PASSWORD");
    if (target.empty()) return false;
    policy::SetDamageWatcher(&OnGhostDamage);
    policy::DisableMenuPause();
    overlay::SetHud(&DrawHudLayer);
    overlay::SetDraw([] { mpui::DrawWindows(); voice::DrawWindow(); players::Draw(); });  // the mode's dialogs, the character creator, voice settings
    webui::Install();                      // the mode's HTML pages
    if (!net::ParseAddress(target, host, port) || !net::ValidNick(nick)) {
        dg::Log("MP: invalid launch parameters (address '%s', nick '%s')", target.c_str(), nick.c_str());
        enabled = true; phase = Phase::Failed; failReason = lang::Pick(L"Неверный адрес сервера или ник",L"Invalid server address or nickname");
        return true;
    }
    enabled = true;
    HookClient();
    hud::Start();
    dg::Log("MP client mode: %s:%u as %s", host.c_str(), port, nick.c_str());
    return true;
}
bool Enabled() { return enabled; }
Phase GetPhase() { return phase; }
const std::wstring& FailReason() { return failReason; }

bool Respawning() {
    if (respawnCoverUntil && Clock() > respawnCoverUntil) respawnCoverUntil = 0;
    return respawnCoverUntil != 0;
}
bool TakeTeleport(FVector& pos, float& yaw, bool& initial) {
    if (!teleport.pending) return false;
    pos = teleport.pos; yaw = teleport.yaw; initial = teleport.initial || respawnCoverUntil != 0; teleport.pending = false;  // a respawn always travels (covered)
    respawnCoverUntil = 0;  // the travel that follows keeps the loading cover up from here
    return true;
}

void Update(double, float dt, UObject* pawn, UObject* controller, bool inWorld, bool gameplay) {
    if (!enabled) return;
    const double now = Clock();
    worldPawn = pawn; worldController = controller;
    hud::SetVisible(inWorld);
    hud::SetChatAllowed(gameplay && phase == Phase::Joined);
    if (!connectStarted && inWorld && phase != Phase::Failed) StartConnect();
    diagnostics::Phase("mp-network"); client.Service(now);

    static bool f9 = false;
    const bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, f9Pressed = f9Now && !f9; f9 = f9Now;
    if (lostAfterJoin && gameplay && f9Pressed) {
        lostAfterJoin = false; phase = Phase::Connecting;
        hud::SetBanner(lang::Pick(L"Переподключение…",L"Reconnecting…"), W(host) + L":" + std::to_wstring(port));
        SetSteamIdentity();
        client.Connect(host, port, nick, password);
    }
    if (client.status() != net::Client::Status::Connected || !inWorld) {
        if (!inWorld) { inventory::Reset(); gameevents::Reset(); for (auto& [id, g] : ghosts) if (g->Spawned() && !g->AllValid()) g->Forget(); }
        return;
    }

    for (auto& line : hud::TakeSubmitted()) {
        if (line == L"/voice") { voice::OpenSettings(); continue; }  // the voice chat settings: the client's own command
        if (clientlua::OnChat(U(line))) continue;                     // a command of the mode's client scripts
        client.SendChat(U(line));
    }

    RefreshLocal(now, pawn);
    static double nextLook = 0;
    if (now >= nextLook && IsValid(L.mesh)) {
        nextLook = now + 1.0;
        // Not while the character creator dresses the body.
        static std::string worn;  // the look or skin last put on here
        std::string current = CurrentLook(L.mesh);
        if (!skins::CreatorOpen()) {
            // A skin is worn when its base look is on and nothing replaced it since.
            const bool wearing = !worn.empty() && worn == wantedLook && current == skins::BaseLook(worn);
            if (!wantedLook.empty() && !wearing && skins::Wear(L.mesh, wantedLook)) { worn = wantedLook; current = CurrentLook(L.mesh); L.nextScan = 0; }
            if (!worn.empty() && current == skins::BaseLook(worn)) current = worn;
            if (!current.empty() && current != reportedLook) { reportedLook = current; client.SendLook(current); dg::Log("MP wearing %s", current.c_str()); }
        }
    }
    static double nextGear = 0;
    if (now >= nextGear && IsValid(pawn) && phase == Phase::Joined) {
        nextGear = now + 0.25;
        auto gear = BuildGear(pawn);
        if (gear != reportedGear) { reportedGear = std::move(gear); client.SendGear(reportedGear); }
    }
    inventory::Update(pawn, controller);
    // Death without the game's death: no checkpoint reload, the backpack, pause menu and HUD stay.
    // A lethal hit is cancelled by policy (ModifyDamage) and becomes this death: the body is left
    // behind (LeaveCorpse), the player hidden for a moment, then covered and sent to the respawn point.
    constexpr int kPawnHealth = 0x1FD4, kPawnMaxHealth = 0x1FE4;
    static int deathPhase = 0;  // 1: lying dead (hidden), 2: respawning
    static double deathAt = 0;
    static float lastHp = 0;
    if (IsValid(pawn)) {
        policy::SetInterceptLethalHits(true);
        float& hp = Field<float>(pawn, kPawnHealth);
        const float maxHp = Field<float>(pawn, kPawnMaxHealth);
        // The server's setHealth / heal / kill: 0 goes the same way as a lethal hit.
        bool forced = false;  // the server's kill: no falling down first
        const float floor = BarFloor(pawn, maxHp);
        if (wantedHealth >= 0 && !deathPhase && maxHp > 0) { hp = wantedHealth > 0 ? BarHealth(std::max(1.f, wantedHealth), maxHp, floor) : 0.f; forced = wantedHealth == 0; wantedHealth = -1; }
        const float dead = std::max(1.5f, floor + 0.5f), lying = BarHealth(1, maxHp, floor);  // the bar empty / just above it
        bool lethal = policy::TakeLethalHit() || traps::TakeBledOut();  // (a bear trap does not let the game kill)
        // Death mode "downed": fall instead of dying; hits on the fallen go to the server (finished off or not).
        const bool fallen = downed.on || downed.pending;
        if (!deathPhase && !forced && ((fallen ? hp < lying - 0.5f : hp <= dead) || lethal) && (downed.mode || fallen)) {
            if (!fallen) {
                downed.pending = true; downed.fellAt = now; downed.holdSince = 0; downed.anim = kDownedAnim;
                if (CallBool(pawn, "IsInVehicle")) if (auto f = Fn(pawn, "TryExitVehicle")) { Params p(f); p.SetBool("bForce", true); Call(pawn, "TryExitVehicle", &p); }
                playeranim::OnPlay(kDownedAnim, net::AnimLoop | net::AnimFreeze, 1);
                policy::SetDownedInputBlock(true);
                client.SendDowned();
                dg::Log("MP downed (health %.1f of %.0f)", lastHp, maxHp);
            } else if (now - downed.hitAt > 0.5) { downed.hitAt = now; client.SendDownedAct(net::DownedAction::Hit); }
            hp = std::max(hp, lying); lethal = false;
        }
        if ((downed.on || downed.pending) && maxHp > 0) hp = std::max(hp, lying);
        // An old server never answers: after 5 s it is an ordinary death.
        if (downed.pending && now - downed.fellAt > 5) { downed = Downed{downed.mode}; playeranim::OnPlay("", 0, 1); policy::SetDownedInputBlock(false); forced = true; hp = 0; }
        if (forced && (downed.on || downed.pending)) { downed = Downed{downed.mode}; playeranim::OnPlay("", 0, 1); policy::SetDownedInputBlock(false); }
        if (!deathPhase && (hp <= dead || lethal)) {
            deathPhase = 1; deathAt = now;
            traps::OnLocalDeath(pawn);  // a bear trap holding the body lets go of it
            // Off any vehicle: the respawn is a fast travel, and the game's travel with a rider who died
            // on a thrown bike may never end (the loading cover stays up for good).
            if (CallBool(pawn, "IsInVehicle")) if (auto f = Fn(pawn, "TryExitVehicle")) { Params p(f); p.SetBool("bForce", true); Call(pawn, "TryExitVehicle", &p); }
            LeaveCorpse(L.mesh, reportedLook);
            HideLocalBody(pawn, true);
            SetHudVisible(false);  // the restored health would show while the player "lies dead"
            BoolArg(controller, "SetIgnoreMoveInput", "bNewMoveInput", true);
            client.SendGameEvent(net::GameEventType::Died, "", "", "", 0, 0, 0);
            dg::Log("MP death (health %.1f of %.0f)", lastHp, maxHp);
        }
        if (deathPhase) hp = maxHp;  // nothing more happens to the hidden body in between
        lastHp = hp;
    }
    if (deathPhase == 1 && now - deathAt > 2.5) {
        deathPhase = 2; respawnCoverUntil = now + 25;
        client.SendGameEvent(net::GameEventType::Respawned, "", "", "", 0, 0, 0);
    }
    if (deathPhase == 2 && now - deathAt > 4 && !Respawning() && !panel::IsLoading()) {
        deathPhase = 0;
        HideLocalBody(pawn, false);
        SetHudVisible(true);
        BoolArg(controller, "SetIgnoreMoveInput", "bNewMoveInput", false);
    }
    localDead = deathPhase != 0;
    // Lying wounded: H calls for help, E held for 3 s gives up (when the server allows it).
    {
        const bool lying = downed.on && !localDead;
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool focus = pid == GetCurrentProcessId() && !hud::ChatOpen() && !overlay::CursorMode();
        float hold = 0;
        if (lying) {
            static bool hWas = false;
            const bool h = focus && (GetAsyncKeyState('H') & 0x8000);
            if (h && !hWas && now - downed.helpAt > 10) { downed.helpAt = now; client.SendDownedAct(net::DownedAction::CallHelp); }
            hWas = h;
            const bool e = focus && (GetAsyncKeyState('E') & 0x8000) && now >= downed.giveUpAt;
            if (e && !downed.holdSince) downed.holdSince = now;
            if (!e) downed.holdSince = 0;
            if (downed.holdSince) hold = float(std::min(1.0, (now - downed.holdSince) / 3.0));
            if (hold >= 1) { downed.holdSince = 0; client.SendDownedAct(net::DownedAction::GiveUp); }
            policy::SetDownedInputBlock(true);
        }
        mpui::SetDownedHud(lying, float(downed.until - now), float(std::max(0.0, downed.giveUpAt - now)), hold, now - downed.helpAt < 10);
    }
    UpdateSpectator(pawn);
    static bool flashlightMoved = false;
    if (!flashlightMoved && gameplay) { static double nextTry = 0; if (now >= nextTry) { nextTry = now + 2; flashlightMoved = MoveFlashlightToL(); } }
    bool voiceAllowed = false;  // the push-to-talk keys work (set below)
    bool scriptKeys = false, scriptHud = false;  // the client scripts' keys work / their drawing shows (set below)
    {
        FVector at{};
        if (auto f = IsValid(pawn) ? Fn(pawn, "K2_GetActorLocation") : nullptr) { Params p(f); Call(pawn, "K2_GetActorLocation", &p); at = p.Get<FVector>("ReturnValue"); }
        // Menus do not pause a shared world: `gameplay` stays true under the pause menu.
        bool pauseMenu = false;
        if (auto f = IsValid(controller) ? Fn(controller, "IsPauseMenuActive") : nullptr) { Params p(f); Call(controller, "IsPauseMenuActive", &p); pauseMenu = p.GetBool("ReturnValue"); }
        const bool playing = gameplay && !pauseMenu && !policy::RewardPanelOpen();
        const bool canAct = playing && phase == Phase::Joined && !hud::ChatOpen() && !localDead && !spec.on && !Respawning() && !traps::Caught();  // (in a bear trap F is the game's escape)
        voiceAllowed = playing && phase == Phase::Joined && !hud::ChatOpen() && !mpui::DialogOpen();
        scriptKeys = canAct; scriptHud = playing && !spec.on;
        {
            // The aim for the interaction prompts: the camera's place and forward direction, sight tests from it.
            mpui::View view;
            if (IsValid(controller)) if (UObject* mgr = Field<UObject*>(controller, off::PC_CameraManager); IsValid(mgr)) {
                const FRotator r = Field<FRotator>(mgr, off::CamMgr_POV_Location + 12);
                const float rad = 0.0174532925f, pitch = r.Pitch * rad, yaw = r.Yaw * rad;
                view.cam = Field<FVector>(mgr, off::CamMgr_POV_Location);
                view.fwd = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
                view.valid = true;
                view.clear = [pawn](const FVector& from, const FVector& to) { return LineClear(pawn, from, to); };
            }
            mpui::SetView(std::move(view));
            std::vector<mpui::PlayerSpot> others;
            for (const auto& b : RemoteBodies()) if (b.alive) others.push_back({b.id, b.pos, downedPlayers.count(b.id) != 0});
            mpui::SetPlayerSpots(std::move(others));
        }
        mpui::Update(client, at, canAct, playing && !spec.on);
        webui::Update(client);
        diagnostics::Phase("mp-vehicles"); vehicles::Update(now, pawn, controller, client);
        zombies::Net(now, pawn, client, myWorld);
        carry::Update(now, pawn, client, canAct && !mpui::DialogOpen() && !webui::Focused());  // (keys typed into a page are not F)
        objects::Update(now, pawn);
        traps::Update(pawn, client.localId());
        blips::Update(pawn);
        sounds::Update(pawn, client);
        throws::Update(now, pawn, client);
        UpdateMelee(now, pawn);
        playeranim::Update(pawn, controller, client, canAct && !mpui::DialogOpen() && !webui::Focused(), localDead || Respawning());
        UpdateHeliView(pawn);  // after the helicopter moved this frame (else the view lags a frame and shakes)
        // The UI took or gave back the keyboard: nothing the game saw pressed stays held (a W let go
        // under a dialog kept the character running after it).
        static bool uiInput = false;
        if (overlay::CursorMode() != uiInput) {
            uiInput = overlay::CursorMode();
            static UObject* statics = FindObject("/Script/BendGame.Default__BendGameplayStatics");
            if (IsValid(statics)) if (auto f = Fn(statics, "FlushPlayerInput")) { Params p(f); Call(statics, "FlushPlayerInput", &p); }
        }
    }
    if (pendingCreator.on && IsValid(pawn)) {
        FVector here{};
        if (auto f = Fn(pawn, "K2_GetActorLocation")) { Params p(f); Call(pawn, "K2_GetActorLocation", &p); here = p.Get<FVector>("ReturnValue"); }
        const float dx = here.X - pendingCreator.at.X, dy = here.Y - pendingCreator.at.Y, dz = here.Z - pendingCreator.at.Z;
        const bool arrived = dx * dx + dy * dy + dz * dz < 500.f * 500.f && !panel::IsLoading() && !Respawning();
        if (arrived || now - pendingCreator.since > 40) { pendingCreator.on = false; skins::OpenCreator(pendingCreator.options); }
    }
    {
        bool ok = false; std::string skin;
        if (skins::UpdateCreator(pawn, controller, L.mesh, ok, skin)) {
            client.SendCustomizeDone(ok, skin);
            dg::Log("MP character creator closed: %s", ok ? skin.c_str() : "cancelled");
            if (!ok) skins::Wear(L.mesh, wantedLook.empty() ? reportedLook : wantedLook);  // back to what it wore
        }
    }
    // A shared world does not stop: the pause menu, the map and pausing popups open, time goes on.
    // (Not under the quest reward panel: unpausing beneath it loops its sound.)
    static double nextPauseCheck = 0;
    if (now >= nextPauseCheck && IsValid(controller) && Statics()) {
        nextPauseCheck = now + 0.1;
        if (auto f = Fn(Statics(), "IsGamePaused")) {
            Params p(f); p.Set("WorldContextObject", controller); Call(Statics(), "IsGamePaused", &p);
            if (p.GetBool("ReturnValue") && !policy::RewardPanelOpen()) if (auto g = Fn(Statics(), "SetGamePaused")) {
                Params q(g); q.Set("WorldContextObject", controller); q.SetBool("bPaused", false); Call(Statics(), "SetGamePaused", &q);
            }
        }
    }
        // Menus neither pause the world nor its sound here (policy::DisableMenuPause); should the
        // sound still end up paused with no menu open, it is given back (at most every 5 s).
        static bool menuWasOpen = false;
        bool menuOpen = false;
        if (auto f = Fn(controller, "IsPauseMenuActive")) { Params p(f); Call(controller, "IsPauseMenuActive", &p); menuOpen = p.GetBool("ReturnValue"); }
        UObject* audio = FindRuntimeInstance("BendAudioManager");
        if (IsValid(audio) && !menuOpen && !policy::RewardPanelOpen()) {
            static double nextFix = 0;
            bool silenced = false;  // menus no longer pause sounds (policy::DisableMenuPause): only a real pause
            if (now >= nextFix) if (auto f = Fn(audio, "IsAudioPaused")) { Params p(f); Call(audio, "IsAudioPaused", &p); silenced = p.GetBool("ReturnValue"); }
            if (silenced) {
                nextFix = now + 5;
                for (const char* fn : {"PauseMenuClosed", "UnpauseAllSounds", "UnpauseAllVO"}) if (auto f = Fn(audio, fn)) { Params p(f); Call(audio, fn, &p); }
                dg::Log("MP: game sound back after the pause menu");
            }
        }
        menuWasOpen = menuOpen;
    UpdateCorpses(now);
    worldedit::Update(pawn);
    // A real death anyway (some kills ignore the flag): the game's reload is skipped and a new body made.
    policy::SetBlockDeathReload(true);
    if (policy::TakeDeath()) { respawnCoverUntil = now + 25; client.SendGameEvent(net::GameEventType::Died, "", "", "", 0, 0, 0); }
    if (inventory::Snapshot snap; !Respawning() && inventory::TakeSnapshot(snap, now)) client.SendInventory(snap);
    // Hooking world objects starts once the spawn travel has settled: thousands of hooks during the
    // travel's streaming delayed it enough for the player to fall through unloaded ground.
    static std::vector<gameevents::Event> events;
    events.clear(); if (now - lastTeleportAt >= 15 && IsValid(pawn)) gameevents::Update(pawn, now, events);
    if (pendingRemoval.pending && !policy::RewardPanelOpen()) {
        pendingRemoval.pending = false;
        if (client.on.objective) client.on.objective(4, pendingRemoval.category, pendingRemoval.style, "", "", 0);
    }
    // Death: the game reloads a checkpoint (its own camp) with a new pawn; the server decides where to go.
    // The cover goes up at the moment of death so the checkpoint is never seen.
    static double respawnAt = 0;
    if (policy::TakeDeathReloadBlocked()) respawnAt = now + 0.5;
    if (respawnAt && now >= respawnAt) { respawnAt = 0; RespawnInPlace(pawn, controller); }
    static UObject* knownPawn = nullptr;
    if (IsValid(pawn) && pawn != knownPawn) {
        if (knownPawn) { client.SendGameEvent(net::GameEventType::Respawned, "", "", "", 0, 0, 0); dg::Log("MP respawned: new pawn %s", GetName(pawn).c_str()); }
        knownPawn = pawn;
    }
    for (const auto& e : events) client.SendGameEvent(e.type, e.cls, e.detail, e.id, e.at.X, e.at.Y, e.at.Z);
    static double nextState = 0, nextBody = 0, nextBike = 0;
    net::PlayerState s;
    const bool haveState = BuildState(s, controller);
    if (haveState && now >= nextState) { nextState = now + 0.05; client.SendState(s); }
    if (haveState && now >= nextBody) { nextBody = now + 1.0 / 15; client.SendPose(BuildPosePayload(net::kPoseBody)); }
    if (haveState && (s.flags & net::StateHasBike) && now >= nextBike) {
        nextBike = now + ((s.flags & net::StateOnBike) ? 1.0 / 15 : 0.5);
        client.SendPose(BuildPosePayload(net::kPoseBike));
    }

    FVector cam{s.x, s.y, s.z};
    if (IsValid(controller)) if (UObject* mgr = Field<UObject*>(controller, off::PC_CameraManager); IsValid(mgr)) cam = Field<FVector>(mgr, off::CamMgr_POV_Location);
    for (auto& [id, g] : ghosts) g->Update(now, cam);
    // Voice chat: push-to-talk (not while typing or in a dialog) and the voices heard, placed from the camera.
    {
        FRotator camRot{};
        if (IsValid(controller)) if (UObject* mgr = Field<UObject*>(controller, off::PC_CameraManager); IsValid(mgr)) camRot = Field<FRotator>(mgr, off::CamMgr_POV_Location + 12);
        voice::Update(client, voiceAllowed, cam, camRot);
        overlay::SetTabOwned(phase == Phase::Joined);
        players::Update(client, gameplay && phase == Phase::Joined && !hud::ChatOpen() && !mpui::DialogOpen() && !spec.on);
    }
    UpdateNpcs(now, {s.x, s.y, s.z}, cam);
    // The mode's icons in the world: on screen where they are in front of the camera and near enough.
    {
        std::vector<mpui::WorldIcon> shown=npcLabels;
        auto project = IsValid(controller) ? Fn(controller, "ProjectWorldLocationToScreen") : nullptr;
        for (auto& [id, ic] : worldIcons) {
            if (!project || (ic.world >= 0 && ic.world != myWorld)) continue;
            const FVector at{ic.x, ic.y, ic.z};
            const float d = std::sqrt(Dist2(at, cam));
            if (d > ic.distance) continue;
            if (!ic.throughWalls) {  // hidden behind walls, like the game's own things
                IconSight& sight = iconSight[id];
                if (now >= sight.next) { sight.next = now + 0.2; sight.clear = LineClear(pawn, cam, at); }
                if (!sight.clear) continue;
            }
            Params pr(project); pr.Set("WorldLocation", at); Call(controller, "ProjectWorldLocationToScreen", &pr);
            if (!pr.GetBool("ReturnValue")) continue;
            const auto p = pr.Get<FVector2D>("ScreenLocation");
            const unsigned rgba = ic.color;
            const unsigned abgr = (rgba >> 24) | ((rgba >> 8) & 0xFF00) | ((rgba << 8) & 0xFF0000) | ((rgba & 0xFF) << 24);  // ImGui packs ABGR
            shown.push_back({p.X, p.Y, ic.size, std::clamp(1500.f / std::max(d, 1.f), 0.5f, 1.f), abgr, ic.image, ic.text});
        }
        mpui::SetIcons(std::move(shown));
    }
    // The mode's client scripts (docs/client-lua.md).
    {
        clientlua::Frame f;
        f.dt = dt; f.pawn = pawn; f.controller = controller; f.hudShown = scriptHud; f.canAct = scriptKeys;
        f.id = client.localId(); f.name = nick; f.world = myWorld; f.dead = localDead;
        f.haveState = haveState; f.state = s; f.cam = cam;
        if (IsValid(controller)) if (UObject* mgr = Field<UObject*>(controller, off::PC_CameraManager); IsValid(mgr)) f.camRot = Field<FRotator>(mgr, off::CamMgr_POV_Location + 12);
        clientlua::Update(f, client);
    }
    PublishPlates();
    UpdateMarkers(now, {s.x, s.y, s.z});

    static double nextStatus = 0;
    if (now >= nextStatus) {
        nextStatus = now + 0.5;
        hud::SetStatus(W(serverName) + L"  ·  " + std::to_wstring(ghosts.size() + 1) + lang::Pick(L" игр.  ·  ",L" players  ·  ") + std::to_wstring(client.pingMs()) + lang::Pick(L" мс",L" ms"));
        // The map menu's "days gone" corner: players here, the server and its mode.
        std::wstring title = W(serverName);
        if (title.size() > 40) title = title.substr(0, 39) + L"…";
        policy::SetMenuBanner(std::to_wstring(ghosts.size() + 1), title, lang::Pick(L"игроков · ", L"players · ") + W(modeName));
    }
}

void Shutdown() { if (enabled) { DespawnAll(); client.Disconnect(); } }

std::vector<RemoteBody> RemoteBodies() {
    std::vector<RemoteBody> out;
    for (auto& [id, g] : ghosts) if (g->Spawned() && IsValid(g->body)) out.push_back({id, g->lastMeshPos, !g->deadShown});
    return out;
}
uint16_t PlayerOfActor(UObject* actor) {
    if (!actor) return 0;
    for (auto& [id, g] : ghosts) {
        if (g->body == actor || g->bike == actor) return id;
        for (auto& part : g->bodyParts) if (part.actor == actor) return id;
    }
    return 0;
}
std::string DescribeGhosts() {
    std::string out;
    auto hidden = [](UObject* c) { return IsValid(c) ? (GetBoolProp(c, "bHiddenInGame") ? "hidden" : "shown") : "none"; };
    for (auto& [id, g] : ghosts) {
        char line[512];
        const FVector at = IsValid(g->body) ? C2WPos(MeshComp(g->body)) : FVector{};
        const PoseTrack& t = g->tracks[net::kPoseBody];
        std::snprintf(line, sizeof line, "%u %s body=%p at (%.0f %.0f %.0f) actorHidden=%d dead=%d main=%s pose=%p %s active=%d frames=%zu twins=%zu",
            id, g->nick.c_str(), (void*)g->body, at.X, at.Y, at.Z, IsValid(g->body) ? int(GetBoolProp(g->body, "bHidden")) : -1, int(g->deadShown),
            IsValid(g->body) ? hidden(MeshComp(g->body)) : "none", (void*)t.comp, hidden(t.comp), int(t.active), t.frames.size(), t.twins.size());
        out += line; out += "\n";
        std::snprintf(line, sizeof line, "    pose: hash theirs %08X ours %08X, bones %zu, last frame %.2f s ago, newest t %.2f, now %.2f\n", t.hash, t.bones.hash, t.bones.names.size(),
            Clock() - t.lastRecv, t.frames.empty() ? 0.0 : t.frames.back().t, Clock());
        out += line;
        for (auto& tw : t.twins) {
            std::snprintf(line, sizeof line, "    twin %p (%s) of %s %s, %zu bones mapped\n", (void*)tw.comp, hidden(tw.comp), IsValid(tw.src) ? GetName(tw.src).c_str() : "-", hidden(tw.src),
                size_t(std::count_if(tw.index.begin(), tw.index.end(), [](int32_t i) { return i >= 0; })));
            out += line;
        }
    }
    return out.empty() ? "no remote players" : out;
}
std::vector<UObject*> GhostActors() {
    std::vector<UObject*> out;
    for (auto& [id, g] : ghosts) {
        if (IsValid(g->body)) out.push_back(g->body);
        for (auto& part : g->bodyParts) if (IsValid(part.actor)) out.push_back(part.actor);
    }
    return out;
}

}  // namespace mp
