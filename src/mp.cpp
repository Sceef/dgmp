#include "mp.h"
#include "hud.h"
#include "inventory.h"
#include "gameevents.h"
#include "worldedit.h"
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
        if (!o || (o->ObjectFlags & 0x10) || !IsA(o, cls) || !IsValid(o)) continue;
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

bool BuildState(net::PlayerState& s, UObject* controller) {
    if (!IsValid(L.pawn) || !IsValid(L.mesh)) return false;
    UObject* root = Field<UObject*>(L.pawn, off::Actor_RootComponent);
    if (!IsValid(root)) return false;
    const FVector pos = C2WPos(root), vel = Field<FVector>(root, off::Scene_ComponentVelocity);
    s.x = pos.X; s.y = pos.Y; s.z = pos.Z; s.vx = vel.X; s.vy = vel.Y; s.vz = vel.Z;
    if (auto f = IsValid(controller) ? Fn(controller, "GetControlRotation") : nullptr) { Params p(f); Call(controller, "GetControlRotation", &p); s.yaw = p.Get<FRotator>("ReturnValue").Yaw; }
    const FVector mp = C2WPos(L.mesh); const FQuat mq = C2WRot(L.mesh);
    s.mx = mp.X; s.my = mp.Y; s.mz = mp.Z; s.mqx = mq.X; s.mqy = mq.Y; s.mqz = mq.Z; s.mqw = mq.W;
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
    if (IsValid(L.bike) && IsValid(L.bikeMesh)) {
        const FVector bp = C2WPos(L.bikeMesh); const FQuat bq = C2WRot(L.bikeMesh);
        s.flags |= net::StateHasBike;
        s.bx = bp.X; s.by = bp.Y; s.bz = bp.Z; s.bqx = bq.X; s.bqy = bq.Y; s.bqz = bq.Z; s.bqw = bq.W;
    }
    if (inventory::OwnsBike()) s.flags |= net::StateOwnsBike;  // assigned bike, however far away
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

struct PoseTrack {
    UObject* comp = nullptr;
    BoneList bones;
    bool active = false, mismatchLogged = false;
    uint32_t hash = 0;
    std::deque<PoseFrame> frames;
    double lastRecv = 0;

    void Reset() { comp = nullptr; active = false; frames.clear(); }
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
        return !frames.empty() && frames.back().local.size() == bones.names.size() && now - lastRecv < 0.6;
    }
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
        if (actor) SetActorTransform(actor, pos, rot);
        auto& arr = Field<FTransformArray>(comp, kPoseable_BoneSpaceTransforms);
        for (size_t i = 0; i < local.size(); ++i) { const int32_t idx = bones.index[i]; if (idx >= 0 && idx < arr.Num) arr.Data[idx] = local[i]; }
        // Touch the root through the API so the engine marks the pose dirty.
        if (auto f = Fn(comp, "SetBoneTransformByName")) {
            Params p(f); p.Set("BoneName", bones.names[0]); p.Set("InTransform", arr.Data[bones.index[0]]); p.Set<uint8_t>("BoneSpace", 1);
            Call(comp, "SetBoneTransformByName", &p);
        }
    }
};

// ---------------------------------------------------------------- remote players
struct GhostPart { UObject* actor = nullptr; FName socket{0, 0}; bool master = false; FVector relLoc{}; FRotator relRot{}; };

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
    return out;
}

void SwitchGroupPose(UObject* mainActor, std::vector<GhostPart>& parts, PoseTrack& t, bool on) {
    UObject* main = MeshComp(mainActor);
    UObject* target = on ? t.comp : main;
    SetCompHidden(t.comp, !on); SetCompHidden(main, on);
    for (auto& gp : parts) {
        UObject* comp = MeshComp(gp.actor);
        if (gp.master) SetMaster(comp, target);
        else { AttachCompTo(comp, target, gp.socket); SetRelative(comp, gp.relLoc, gp.relRot); }
    }
    for (UObject* part : LookParts(main)) SetMaster(part, target);  // clothing created by SetLook
    t.active = on;
}

struct Ghost {
    uint16_t id = 0;
    std::string nick;
    std::string lookPath;           // the look this player wears ("" until known)
    ClockSync clock;
    struct Snap { double t; net::PlayerState s; };
    std::deque<Snap> snaps;
    double lastRecv = 0, nextSpawnTry = 0, nextBikeTry = 0;
    UObject* body = nullptr;
    std::vector<GhostPart> bodyParts;
    UObject* bike = nullptr;
    std::vector<GhostPart> bikeParts;
    UObject* tag = nullptr;
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
    void Forget() { body = bike = tag = blip = nullptr; bodyParts.clear(); bikeParts.clear(); for (auto& t : tracks) t.Reset(); anim = -1; }
    void Despawn() {
        for (auto& p : bodyParts) DestroyActor(p.actor);
        for (auto& p : bikeParts) DestroyActor(p.actor);
        if (IsValid(blip)) { policy::AllowMarker(blip, false); Call(blip, "K2_DestroyComponent"); }
        blip = nullptr;
        DestroyActor(body); DestroyActor(bike); DestroyActor(tag);
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
        if (body && !WearLook(MeshComp(body), lookPath.empty() ? kDefaultLook : lookPath)) DestroyActor(body);
        if (!body) { dg::Log("MP ghost %s: body spawn failed", nick.c_str()); return; }
        Params mode(Fn(MeshComp(body), "SetAnimationMode")); mode.Set<uint8_t>("InAnimationMode", 1); Call(MeshComp(body), "SetAnimationMode", &mode);
        SetupTrack(net::kPoseBody, body);
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
    void UpdateTrack(int ch, UObject* actor, std::vector<GhostPart>& parts, double now) {
        PoseTrack& t = tracks[ch];
        if (!actor || !t.comp) return;
        const bool want = t.Usable(now);
        if (want != t.active) { SwitchGroupPose(actor, parts, t, want); if (ch == net::kPoseBody) anim = -1; }
        if (t.active) t.Apply(clock, now, actor);
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
        if (!tracks[net::kPoseBody].active) SetActorTransform(body, meshPos, meshRot);
        UpdateTrack(net::kPoseBody, body, bodyParts, now);
        if (!tracks[net::kPoseBody].active) PlayAnim(s.anim);

        if (!bike && (s.flags & net::StateHasBike) && IsValid(L.bikeMesh) && now >= nextBikeTry) {
            nextBikeTry = now + 3.0;
            bike = SpawnMeshGroup(L.bikeMesh, L.bikeParts, {s.bx, s.by, s.bz}, bikeParts);
            if (bike) SetupTrack(net::kPoseBike, bike);
            bikeHidden = false;
        }
        if (bike) {
            const bool hide = !(s.flags & net::StateHasBike);
            if (hide != bikeHidden) { SetHidden(bike, hide); for (auto& p : bikeParts) SetHidden(p.actor, hide); bikeHidden = hide; }
            if (!hide) {
                if (!tracks[net::kPoseBike].active) SetActorTransform(bike, {s.bx, s.by, s.bz}, {s.bqx, s.bqy, s.bqz, s.bqw});
                UpdateTrack(net::kPoseBike, bike, bikeParts, now);
            }
        }
        PlaceTag(meshPos, cam);
    }
};
std::map<uint16_t, std::unique_ptr<Ghost>> ghosts;

void DespawnAll() { for (auto& [id, g] : ghosts) g->Despawn(); }

// ---------------------------------------------------------------- quest markers
// Each marker is the game's own MapWaypointMarkerBP actor (as used for mission objectives): map and
// minimap icon, in-world waypoint icon and an optional road route. Arrival is decided by the server.
struct QuestMarker {
    net::Client::Events::MarkerInfo info{};
    UObject* actor = nullptr;
    bool grounded = false;  // z snapped to the ground (needs the area streamed in)
};
// Server positions usually come from a player's height somewhere else: put markers on the ground.
// First down from just above the point (a floor under it); if nothing is there the point is
// underground, so from high above. Returns false when the area is not loaded yet.
bool GroundZ(UObject* context, float x, float y, float& z) {
    static UObject* lib = nullptr;
    if (!IsValid(lib)) lib = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    auto f = lib && IsValid(context) ? Fn(lib, "LineTraceSingle_NEW") : nullptr; if (!f) return false;
    for (const float top : {z + 200.f, z + 20000.f}) {
        const float bottom = z - 20000.f;
        Params p(f); p.Set("WorldContextObject", context); p.Set("Start", FVector{x, y, top}); p.Set("End", FVector{x, y, bottom});
        p.Set<uint8_t>("TraceChannel", 0); p.SetBool("bTraceComplex", false); p.SetBool("bIgnoreSelf", true);
        Call(lib, "LineTraceSingle_NEW", &p);
        if (!p.GetBool("ReturnValue")) continue;
        auto hit = FindProperty(f, "OutHit");
        const float time = *reinterpret_cast<const float*>(static_cast<uint8_t*>(p.Data()) + Field<int>(hit, off::UProperty_Offset) + 4);  // FHitResult::Time
        z = top + (bottom - top) * time;
        return true;
    }
    return false;
}
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
struct TextValue { uint8_t bytes[0x18]; };
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
    if (!WearLook(MeshComp(body), look.empty() ? kDefaultLook : look)) { DestroyActor(body); return; }
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

void UpdateMarkers(double now, const FVector& player) {
    static double nextShow = 0;
    const bool check = now >= nextShow;
    if (check) nextShow = now + 1.0;
    for (auto& [id, m] : questMarkers) {
        if (m.actor && !IsValid(m.actor)) m.actor = nullptr;  // level change
        if (!m.actor) SpawnMarker(m);
        if (check && IsValid(m.actor) && !m.grounded) {
            const float dx = m.info.x - player.X, dy = m.info.y - player.Y;
            float z = m.info.z;
            if (dx * dx + dy * dy < 400000.f * 400000.f && GroundZ(m.actor, m.info.x, m.info.y, z)) {
                m.grounded = true;
                if (std::fabs(z - m.info.z) > 50.f) {
                    m.info.z = z;
                    SetActorTransform(m.actor, {m.info.x, m.info.y, z}, FQuat{0, 0, 0, 1});
                    if (questMarkers.count(routeMarker) && &questMarkers[routeMarker] == &m) routeTarget = {};  // rebuild the route
                }
            }
        }
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
        serverName = name; modeName = mode; phase = Phase::Joined; lostAfterJoin = false; everJoined = true; reportedLook.clear();
        hud::SetBanner(L"", L"");
        hud::AddMessage(0xA9C4E4FF, std::wstring(lang::Pick(L"Подключено к ",L"Connected to ")) + W(name) + L" (" + W(mode) + L")");
        dg::Log("MP joined %s (mode %s) as id %u", name.c_str(), mode.c_str(), client.localId());
    };
    client.on.disconnected = [](const std::string& reason) {
        dg::Log("MP disconnected: %s", reason.c_str());
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
    client.on.settings = [](net::NametagMode mode) { nametags = mode; dg::Log("MP nametags mode %d", int(mode)); };
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
    client.on.corpse = [](uint16_t id) {
        auto it = ghosts.find(id);
        if (it != ghosts.end() && it->second->body) LeaveCorpse(MeshComp(it->second->body), it->second->lookPath);
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

void StartConnect() {
    connectStarted = true; phase = Phase::Connecting; initialTeleportSeen = false;
    dg::Log("MP connecting to %s:%u as %s", host.c_str(), port, nick.c_str());
    if (!client.Connect(host, port, nick, password)) { phase = Phase::Failed; if (failReason.empty()) failReason = lang::Pick(L"Не удалось начать подключение",L"Could not start connecting"); }
}
// ---------------------------------------------------------------- editor: quest preview
// The launcher's quest window writes the quest being edited to quest_preview.json; its points are
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
        if (type == "goto") add(st, int(i), 0, title, text);
        else if (type == "hold") add(st, int(i), 5, title, text);                     // search
        else if (type == "collect") {
            if (st["area"].isObject()) add(st["area"], int(i), 2, title, text);        // area look for zones
            for (const auto& item : st["spawn"].elements()) {
                items.push_back({item["item"].asString(), {float(item["x"].asNumber()), float(item["y"].asNumber()), float(item["z"].asNumber())}, float(item["yaw"].asNumber())});
            }
        }
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
        const bool moved = std::fabs(m.info.x - w.x) > 1 || std::fabs(m.info.y - w.y) > 1;
        if (look && IsValid(m.actor)) DressWaypoint(m.actor, w);
        if (moved) {
            m.info = w; m.grounded = false;
            if (IsValid(m.actor)) SetActorTransform(m.actor, {w.x, w.y, w.z}, FQuat{0, 0, 0, 1});
        } else if (look) {
            const float z = m.info.z; m.info = w; m.info.z = z;  // keep the ground-snapped height
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

bool Init() {
    const std::string target = Env(L"DGMP_CONNECT");
    nick = Env(L"DGMP_NICK"); password = Env(L"DGMP_PASSWORD");
    if (target.empty()) return false;
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

void Update(double, float, UObject* pawn, UObject* controller, bool inWorld, bool gameplay) {
    if (!enabled) return;
    const double now = Clock();
    worldPawn = pawn; worldController = controller;
    hud::SetVisible(inWorld);
    hud::SetChatAllowed(gameplay && phase == Phase::Joined);
    if (!connectStarted && inWorld && phase != Phase::Failed) StartConnect();
    client.Service(now);

    static bool f9 = false;
    const bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0, f9Pressed = f9Now && !f9; f9 = f9Now;
    if (lostAfterJoin && gameplay && f9Pressed) {
        lostAfterJoin = false; phase = Phase::Connecting;
        hud::SetBanner(lang::Pick(L"Переподключение…",L"Reconnecting…"), W(host) + L":" + std::to_wstring(port));
        client.Connect(host, port, nick, password);
    }
    if (client.status() != net::Client::Status::Connected || !inWorld) {
        if (!inWorld) { inventory::Reset(); gameevents::Reset(); for (auto& [id, g] : ghosts) if (g->Spawned() && !g->AllValid()) g->Forget(); }
        return;
    }

    for (auto& line : hud::TakeSubmitted()) client.SendChat(U(line));

    RefreshLocal(now, pawn);
    static double nextLook = 0;
    if (now >= nextLook && IsValid(L.mesh)) {
        nextLook = now + 1.0;
        std::string current = CurrentLook(L.mesh);
        if (!wantedLook.empty() && current != wantedLook && WearLook(L.mesh, wantedLook)) { current = CurrentLook(L.mesh); L.nextScan = 0; }
        if (!current.empty() && current != reportedLook) { reportedLook = current; client.SendLook(current); dg::Log("MP wearing %s", current.c_str()); }
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
        const bool lethal = policy::TakeLethalHit();
        if (!deathPhase && (hp <= 1.5f || lethal)) {
            deathPhase = 1; deathAt = now;
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
    UpdateMarkers(now, {s.x, s.y, s.z});

    static double nextStatus = 0;
    if (now >= nextStatus) {
        nextStatus = now + 0.5;
        hud::SetStatus(W(serverName) + L"  ·  " + std::to_wstring(ghosts.size() + 1) + lang::Pick(L" игр.  ·  ",L" players  ·  ") + std::to_wstring(client.pingMs()) + lang::Pick(L" мс",L" ms"));
    }
}

void Shutdown() { if (enabled) { DespawnAll(); client.Disconnect(); } }

}  // namespace mp
