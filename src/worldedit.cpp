#include "worldedit.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <set>
#include <vector>

using namespace ue;

namespace worldedit {
namespace {
struct Transform { FVector at{}; FRotator rot{}; float scale = 1; };
// A light of the mode: colour (sRGB 0..1), intensity (the engine's units), reach in metres, the spot's
// cone in degrees. A small shape shows it in the editor; in a game only the light is there.
struct Light {
    bool spot = false, shadows = false;
    float color[3] = {1.f, 0.85f, 0.7f}, intensity = 5000.f, radius = 10.f, cone = 40.f;
    bool operator==(const Light&) const = default;
};
struct Added { std::string uid, asset; Transform t; bool light = false; Light cfg; UObject* actor = nullptr; UObject* lightComp = nullptr; bool failed = false; };

std::set<std::string> removed;
std::map<std::string, Transform> moved;
std::vector<Added> added;
bool previewMode = false;

// World objects already handled (pointer → key), so the object table is not searched again.
std::map<UObject*, std::string> known;
std::vector<UObject*> hidden;   // preview: hidden objects, shown again when the removal is undone
std::map<UObject*, Transform> original;  // preview: where moved objects were, for undoing a move
int cursor = 0;
UObject* actorClass = nullptr;

UObject* Statics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
FQuat ToQuat(const FRotator& r) {
    const float d = 0.00872664626f;  // degrees → half radians
    const float sp = sinf(r.Pitch * d), cp = cosf(r.Pitch * d), sy = sinf(r.Yaw * d), cy = cosf(r.Yaw * d), sr = sinf(r.Roll * d), cr = cosf(r.Roll * d);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}
void BoolCall(UObject* o, const char* fn, const char* param, bool v) {
    if (auto f = IsValid(o) ? Fn(o, fn) : nullptr) { Params p(f); p.SetBool(param, v); Call(o, fn, &p); }
}
// SceneComponent.Mobility = Movable. This game build exposes no SetMobility function; static
// components ignore SetStaticMesh and moves once registered.
void MakeMovable(UObject* comp) {
    static int offset = -1;
    if (offset < 0) if (auto cls = FindClass("SceneComponent")) if (auto prop = FindProperty(cls, "Mobility")) offset = Field<int>(prop, off::UProperty_Offset);
    if (offset > 0 && IsValid(comp)) *(reinterpret_cast<uint8_t*>(comp) + offset) = 2;
}
UObject* MeshComponent(UObject* actor) {
    for (auto c = actor->Class; c; c = SuperOf(c)) if (auto prop = FindProperty(c, "StaticMeshComponent")) return Field<UObject*>(actor, Field<int>(prop, off::UProperty_Offset));
    return nullptr;
}
UObject* Root(UObject* actor) {
    auto f = Fn(actor, "K2_GetRootComponent"); if (!f) return nullptr;
    Params p(f); Call(actor, "K2_GetRootComponent", &p); return p.Get<UObject*>("ReturnValue");
}
// Static objects cannot move until their root becomes movable.
void Place(UObject* actor, const Transform& t) {
    MakeMovable(Root(actor));
    if (auto f = Fn(actor, "K2_SetActorLocationAndRotation")) {
        Params p(f); p.Set("NewLocation", t.at); p.Set("NewRotation", t.rot); p.SetBool("bTeleport", true);
        Call(actor, "K2_SetActorLocationAndRotation", &p);
    }
    if (std::fabs(t.scale - 1) > 0.001f) if (auto f = Fn(actor, "SetActorScale3D")) { Params p(f); p.Set("NewScale3D", FVector{t.scale, t.scale, t.scale}); Call(actor, "SetActorScale3D", &p); }
}
void Hide(UObject* actor, bool hide) {
    BoolCall(actor, "SetActorHiddenInGame", "bNewHidden", hide);
    BoolCall(actor, "SetActorEnableCollision", "bNewActorEnableCollision", !hide);
    BoolCall(actor, "SetActorTickEnabled", "bEnabled", !hide);
}
void Remove(UObject* actor) {
    if (previewMode) { Hide(actor, true); hidden.push_back(actor); }
    else Call(actor, "K2_DestroyActor");
}
FRotator Rotation(UObject* actor) {
    auto f = Fn(actor, "K2_GetActorRotation"); if (!f) return {};
    Params p(f); Call(actor, "K2_GetActorRotation", &p); return p.Get<FRotator>("ReturnValue");
}
FVector Location(UObject* actor) {
    auto f = Fn(actor, "K2_GetActorLocation"); if (!f) return {};
    Params p(f); Call(actor, "K2_GetActorLocation", &p); return p.Get<FVector>("ReturnValue");
}
UObject* SpawnProp(const Added& a, UObject* context) {
    UObject* asset = FindObject(a.asset.c_str());
    if (!asset) asset = LoadAsset(a.asset.c_str());
    if (!asset) return nullptr;
    const bool blueprint = a.asset.size() > 2 && a.asset.compare(a.asset.size() - 2, 2, "_C") == 0;
    UObject* cls = blueprint ? asset : FindClass("StaticMeshActor");
    if (!cls || !Statics()) return nullptr;
    FTransform t; t.Translation = a.t.at; t.Rotation = ToQuat(a.t.rot);
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    if (!blueprint) {
        UObject* comp = MeshComponent(actor);
        MakeMovable(comp);
        if (IsValid(comp)) if (auto f = Fn(comp, "SetStaticMesh")) { Params p(f); p.Set("NewMesh", asset); Call(comp, "SetStaticMesh", &p); }
    }
    Params finish(Fn(Statics(), "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(Statics(), "FinishSpawningActor", &finish);
    Place(actor, a.t);
    return actor;
}
}  // namespace

UObject* SpawnMesh(const std::string& asset, const FVector& at, const FRotator& rot, float scale, UObject* context) {
    Added a; a.asset = asset; a.t.at = at; a.t.rot = rot; a.t.scale = scale;
    return SpawnProp(a, context);
}
void PlaceActor(UObject* actor, const FVector& at, const FRotator& rot, float scale) {
    if (!IsValid(actor)) return;
    Transform t; t.at = at; t.rot = rot; t.scale = scale;
    Place(actor, t);
}

namespace {
// Live settings of a light component (the rest needs a new one: type, shadows).
void ApplyLight(UObject* comp, UObject* shape, const Light& l) {
    if (!IsValid(comp)) return;
    if (auto f = Fn(comp, "SetIntensity")) { Params p(f); p.Set<float>("NewIntensity", l.intensity); Call(comp, "SetIntensity", &p); }
    struct { float r, g, b, a; } linear{std::pow(l.color[0], 2.2f), std::pow(l.color[1], 2.2f), std::pow(l.color[2], 2.2f), 1.f};
    if (auto f = Fn(comp, "SetLightColor")) { Params p(f); p.Set("NewLightColor", linear); p.SetBool("bSRGB", false); Call(comp, "SetLightColor", &p); }
    if (auto f = Fn(comp, "SetAttenuationRadius")) { Params p(f); p.Set<float>("NewRadius", l.radius * 100.f); Call(comp, "SetAttenuationRadius", &p); }
    if (l.spot) {
        if (auto f = Fn(comp, "SetOuterConeAngle")) { Params p(f); p.Set<float>("NewOuterConeAngle", l.cone); Call(comp, "SetOuterConeAngle", &p); }
        if (auto f = Fn(comp, "SetInnerConeAngle")) { Params p(f); p.Set<float>("NewInnerConeAngle", l.cone * 0.7f); Call(comp, "SetInnerConeAngle", &p); }
    }
    // The editor's shape takes the light's colour.
    if (IsValid(shape)) if (auto f = Fn(shape, "GetMaterial")) {
        Params g(f); g.Set<int>("ElementIndex", 0); Call(shape, "GetMaterial", &g);
        UObject* mat = g.Get<UObject*>("ReturnValue");
        if (IsValid(mat) && Fn(mat, "SetVectorParameterValue")) {
            static const FName colorName = FindName("Color");
            struct { float r, g, b, a; } c{l.color[0], l.color[1], l.color[2], 1.f};
            Params p(Fn(mat, "SetVectorParameterValue")); p.Set("ParameterName", colorName); p.Set("Value", c); Call(mat, "SetVectorParameterValue", &p);
        }
    }
}
UObject* EngineObject(const char* path) { UObject* o = FindObject(path); return o ? o : LoadAsset(path); }
// A light: a small sphere (point) or cone (spot, shining down out of its base) with the light on it.
UObject* SpawnLight(Added& a, UObject* context) {
    // Rooted: a cached pointer to an asset collected after a level change would crash.
    static UObject *sphereAsset = nullptr, *coneAsset = nullptr, *baseMatAsset = nullptr;
    auto kept = [](UObject*& cache, const char* path) { if (!IsValid(cache)) { cache = EngineObject(path); if (cache) AddToRoot(cache); } return cache; };
    UObject* sphere = kept(sphereAsset, "/Engine/BasicShapes/Sphere.Sphere");
    UObject* cone = kept(coneAsset, "/Engine/BasicShapes/Cone.Cone");
    UObject* baseMat = kept(baseMatAsset, "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
    UObject* cls = FindClass("StaticMeshActor");
    UObject* lightCls = FindClass(a.cfg.spot ? "SpotLightComponent" : "PointLightComponent");
    UObject* shapeMesh = a.cfg.spot ? cone : sphere;
    if (!cls || !lightCls || !shapeMesh || !Statics()) return nullptr;
    FTransform t; t.Translation = a.t.at; t.Rotation = ToQuat(a.t.rot);
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    UObject* shape = MeshComponent(actor);
    MakeMovable(shape);
    if (IsValid(shape)) if (auto f = Fn(shape, "SetStaticMesh")) { Params p(f); p.Set("NewMesh", shapeMesh); Call(shape, "SetStaticMesh", &p); }
    Params finish(Fn(Statics(), "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(Statics(), "FinishSpawningActor", &finish);
    Place(actor, a.t);
    if (auto f = Fn(actor, "SetActorScale3D")) { Params p(f); p.Set("NewScale3D", FVector{0.2f, 0.2f, 0.2f}); Call(actor, "SetActorScale3D", &p); }
    if (IsValid(shape)) {
        if (auto f = Fn(shape, "SetCastShadow")) { Params p(f); p.SetBool("NewCastShadow", false); Call(shape, "SetCastShadow", &p); }
        if (baseMat) if (auto f = Fn(shape, "CreateDynamicMaterialInstance")) { Params p(f); p.Set<int>("ElementIndex", 0); p.Set("SourceMaterial", baseMat); Call(shape, "CreateDynamicMaterialInstance", &p); }
        // A game shows only the light; the editor keeps the shape to click and move.
        if (!previewMode) {
            if (auto f = Fn(shape, "SetVisibility")) { Params p(f); p.SetBool("bNewVisibility", false); p.SetBool("bPropagateToChildren", false); Call(shape, "SetVisibility", &p); }
            BoolCall(actor, "SetActorEnableCollision", "bNewActorEnableCollision", false);
        }
    }
    UObject* light = NewObject(lightCls, actor);
    if (!light) { Call(actor, "K2_DestroyActor"); return nullptr; }
    MakeMovable(light);
    SetBoolProp(light, "CastShadows", a.cfg.shadows);
    SetBoolProp(light, "CastDynamicShadows", a.cfg.shadows);
    RegisterComponent(light);
    if (auto f = Fn(light, "K2_AttachTo")) { Params p(f); p.Set("InParent", shape); p.Set<uint8_t>("AttachType", 2); Call(light, "K2_AttachTo", &p); }
    if (a.cfg.spot) if (auto f = Fn(light, "K2_SetRelativeRotation")) { Params p(f); p.Set("NewRotation", FRotator{-90.f, 0, 0}); Call(light, "K2_SetRelativeRotation", &p); }
    a.lightComp = light;
    ApplyLight(light, shape, a.cfg);
    return actor;
}
Light ReadLight(const std::string& asset, const json::Value& v) {
    Light l;
    l.spot = asset == "light:spot";
    if (!v.isObject()) return l;
    const json::Value& c = v["color"];
    if (c.isArray() && c.size() >= 3) for (int i = 0; i < 3; ++i) l.color[i] = std::clamp(float(c[size_t(i)].asNumber()), 0.f, 1.f);
    l.intensity = std::max(0.f, float(v["intensity"].asNumber(l.intensity)));
    l.radius = std::clamp(float(v["radius"].asNumber(l.radius)), 0.1f, 500.f);
    l.cone = std::clamp(float(v["cone"].asNumber(l.cone)), 1.f, 89.f);
    l.shadows = v["shadows"].asBool(false);
    return l;
}
// ---------------------------------------------------------------- merged mesh collections
// The game merges small static objects of a level into one BendStaticMeshCollection whose drawing is
// baked: a single piece of it cannot be moved. Editing a piece replaces the whole collection by
// separate actors (one per piece, same mesh, materials and transform), and the edit applies to that.
// Native layout of BendStaticMeshCollectionComponent (verified in game, docs/world.md):
constexpr int kCollectionComponent = 0x348;   // BendStaticMeshCollection.Component
constexpr int kMeshes = 0x5E0;                // TArray<UStaticMesh*>
constexpr int kMaterials = 0x5F0;             // TArray<UMaterialInterface*>
constexpr int kMaterialList = 0x778;          // TArray<uint16> indices into kMaterials
constexpr int kMaterialSets = 0x788;          // TArray<{uint16 first, count}> ranges of kMaterialList
constexpr int kPieces = 0x768;                // TArray of 64-byte pieces: FTransform, uint16 mesh, set; +0x38 flags
constexpr uint16_t kCollisionOnly = 0x0100;   // invisible blocking volumes (Env_PawnCollide...)
struct Piece { UObject* mesh = nullptr; std::vector<UObject*> materials; FTransform t; bool hidden = false; };
// Pieces placed with the same transform are parts of one object (a car's body and glass): they share
// the key of the first of them and move together.
struct Collection { std::vector<UObject*> actors; std::vector<int> leader; std::vector<FTransform> origin; };  // by piece index (actor null: removed)
std::map<std::string, Collection> collections;         // collection key → its replacement
std::map<UObject*, std::string> pieceKeys;              // spawned piece → "<level>/<collection>#<index>"
std::map<UObject*, std::pair<const Collection*, int>> pieceOf;  // spawned piece → its collection and index
UObject* collectionClass = nullptr;

template <class T> struct RawArray { T* data; int num, max; };
template <class T> const RawArray<T>& ArrayAt(UObject* o, int offset) { return *reinterpret_cast<const RawArray<T>*>(reinterpret_cast<uint8_t*>(o) + offset); }
struct RawPiece { uint8_t bytes[64]; };
struct RawSet { uint16_t first, count; };
std::vector<Piece> ReadPieces(UObject* comp) {
    std::vector<Piece> out;
    const auto& meshes = ArrayAt<UObject*>(comp, kMeshes);
    const auto& materials = ArrayAt<UObject*>(comp, kMaterials);
    const auto& list = ArrayAt<uint16_t>(comp, kMaterialList);
    const auto& sets = ArrayAt<RawSet>(comp, kMaterialSets);
    const auto& pieces = ArrayAt<RawPiece>(comp, kPieces);
    if (!pieces.data || pieces.num <= 0 || pieces.num > 100000 || meshes.num < 0 || meshes.num > 100000) return out;
    out.resize(size_t(pieces.num));
    for (int i = 0; i < pieces.num; ++i) {
        const uint8_t* e = pieces.data[i].bytes;
        Piece& p = out[i];
        std::memcpy(&p.t, e, 48);
        const uint16_t mesh = *reinterpret_cast<const uint16_t*>(e + 0x30), set = *reinterpret_cast<const uint16_t*>(e + 0x32);
        p.hidden = (*reinterpret_cast<const uint16_t*>(e + 0x38) & kCollisionOnly) != 0;
        if (mesh < meshes.num) p.mesh = meshes.data[mesh];
        if (set < sets.num)
            for (int k = 0; k < sets.data[set].count; ++k) {
                const int at = sets.data[set].first + k;
                p.materials.push_back(at < list.num && list.data[at] < materials.num ? materials.data[list.data[at]] : nullptr);
            }
    }
    return out;
}
FVector Rotate(const FQuat& q, const FVector& v) {  // q * v * q^-1
    const FVector u{q.X, q.Y, q.Z};
    const FVector t{2 * (u.Y * v.Z - u.Z * v.Y), 2 * (u.Z * v.X - u.X * v.Z), 2 * (u.X * v.Y - u.Y * v.X)};
    return {v.X + q.W * t.X + (u.Y * t.Z - u.Z * t.Y), v.Y + q.W * t.Y + (u.Z * t.X - u.X * t.Z), v.Z + q.W * t.Z + (u.X * t.Y - u.Y * t.X)};
}
// `owner`: the collection actor. The piece is made in the collection's level, so it leaves with it
// (its materials may live in that level's package: a piece outliving it crashed the renderer).
UObject* SpawnPiece(const Piece& piece, UObject* context, UObject* owner) {
    static UObject* cls = nullptr; if (!cls) cls = FindClass("StaticMeshActor");
    if (!cls || !Statics() || !IsValid(piece.mesh)) return nullptr;
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", piece.t); begin.SetBool("bNoCollisionFail", true);
    begin.Set("Owner", owner);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    UObject* comp = MeshComponent(actor);
    MakeMovable(comp);
    if (IsValid(comp)) {
        if (auto f = Fn(comp, "SetStaticMesh")) { Params p(f); p.Set("NewMesh", piece.mesh); Call(comp, "SetStaticMesh", &p); }
        if (auto f = Fn(comp, "SetMaterial"))
            for (int k = 0; k < int(piece.materials.size()); ++k) {
                if (!piece.materials[k]) continue;
                Params p(f); p.Set<int>("ElementIndex", k); p.Set("Material", piece.materials[k]); Call(comp, "SetMaterial", &p);
            }
    }
    Params finish(Fn(Statics(), "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", piece.t);
    Call(Statics(), "FinishSpawningActor", &finish);
    if (auto f = Fn(actor, "SetActorScale3D")) { Params p(f); p.Set("NewScale3D", piece.t.Scale3D); Call(actor, "SetActorScale3D", &p); }
    if (piece.hidden) BoolCall(actor, "SetActorHiddenInGame", "bNewHidden", true);
    return actor;
}
std::string KeyOfLevelActor(UObject* actor);
FQuat Mul(const FQuat& a, const FQuat& b) {  // a * b: b first, then a
    return {a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y, a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
            a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W, a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z};
}
FRotator ToRotator(const FQuat& q) {  // FQuat::Rotator
    const float r2d = 57.2957795f, test = q.Z * q.X - q.W * q.Y;
    const float yawY = 2 * (q.W * q.Z + q.X * q.Y), yawX = 1 - 2 * (q.Y * q.Y + q.Z * q.Z);
    FRotator r{};
    r.Yaw = atan2f(yawY, yawX) * r2d;
    if (test < -0.4999995f) { r.Pitch = -90; r.Roll = -r.Yaw - 2 * atan2f(q.X, q.W) * r2d; }
    else if (test > 0.4999995f) { r.Pitch = 90; r.Roll = r.Yaw - 2 * atan2f(q.X, q.W) * r2d; }
    else { r.Pitch = asinf(2 * test) * r2d; r.Roll = atan2f(-2 * (q.W * q.X + q.Y * q.Z), 1 - 2 * (q.X * q.X + q.Y * q.Y)) * r2d; }
    return r;
}
// Mesh bounds of a piece in its own space: center and half size (scaled).
bool LocalBox(const Piece& p, FVector& center, FVector& half) {
    if (!IsValid(p.mesh)) return false;
    const float* b = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(p.mesh) + 0x128);  // UStaticMesh.ExtendedBounds
    const FVector& s = p.t.Scale3D;
    center = {b[0] * s.X, b[1] * s.Y, b[2] * s.Z};
    half = {std::fabs(b[3] * s.X), std::fabs(b[4] * s.Y), std::fabs(b[5] * s.Z)};
    return true;
}
bool Inside(const Piece& box, const FVector& world, float grow) {
    FVector c, h;
    if (!LocalBox(box, c, h)) return false;
    const FQuat inv{-box.t.Rotation.X, -box.t.Rotation.Y, -box.t.Rotation.Z, box.t.Rotation.W};
    const FVector l = Rotate(inv, {world.X - box.t.Translation.X, world.Y - box.t.Translation.Y, world.Z - box.t.Translation.Z});
    return std::fabs(l.X - c.X) <= h.X * grow + 10 && std::fabs(l.Y - c.Y) <= h.Y * grow + 10 && std::fabs(l.Z - c.Z) <= h.Z * grow + 10;
}
float Volume(const Piece& p) { FVector c, h; return LocalBox(p, c, h) ? 8 * h.X * h.Y * h.Z : 0; }
// Pieces of one object (a car: body, hood, trunk, wheels, cab). The cooker keeps an object's parts
// next to each other; a neighbour joins when its pivot lies in a part of the object or a part's pivot
// lies in it, and pieces sharing a pivot always join. Big pieces (roads, walls) stay alone.
std::vector<int> GroupPieces(const std::vector<Piece>& pieces) {
    const int n = int(pieces.size());
    std::vector<int> parent(n);
    std::vector<std::vector<int>> members(n);
    for (int i = 0; i < n; ++i) { parent[i] = i; members[i] = {i}; }
    auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    auto join = [&](int a, int b) {
        a = find(a); b = find(b);
        if (a == b) return;
        if (b < a) std::swap(a, b);
        parent[b] = a;
        members[a].insert(members[a].end(), members[b].begin(), members[b].end());
        members[b].clear();
    };
    const float kBig = 60e6f;  // 60 m³ in cm³
    auto small = [&](int i) { return IsValid(pieces[i].mesh) && !pieces[i].hidden && Volume(pieces[i]) < kBig; };
    auto related = [&](int a, int b) { return Inside(pieces[a], pieces[b].t.Translation, 1.1f) || Inside(pieces[b], pieces[a].t.Translation, 1.1f); };
    // Shared pivots first (a car's body, roof sign and wheels; its cab is stored far from the rest).
    std::map<std::tuple<long, long, long>, int> at;
    for (int i = 0; i < n; ++i) {
        if (!small(i)) continue;
        const FVector& t = pieces[i].t.Translation;
        const auto id = std::make_tuple(std::lround(t.X / 8), std::lround(t.Y / 8), std::lround(t.Z / 8));
        auto [it, fresh] = at.emplace(id, i);
        if (!fresh) join(it->second, i);
    }
    // Then neighbours in cooked order that touch any part of the object (hood, trunk).
    for (int i = 0; i < n; ++i) {
        if (!small(i)) continue;
        for (int j = i + 1; j < n && j <= i + 8; ++j) {
            if (!small(j)) break;
            if (find(i) == find(j)) continue;
            bool touch = false;
            for (int a : members[find(i)]) {
                for (int b : members[find(j)]) if (related(a, b)) { touch = true; break; }
                if (touch) break;
            }
            if (!touch) break;
            join(i, j);
        }
    }
    std::vector<int> leader(n);
    for (int i = 0; i < n; ++i) leader[i] = find(i);
    return leader;
}
// A moved object: its leading piece goes to `t`, the other parts keep their place relative to it.
void PlacePiece(UObject* actor, const Collection& c, int index, const Transform& t) {
    const int lead = c.leader[index];
    const FTransform& L = c.origin[lead];
    const FTransform& M = c.origin[index];
    const FQuat qLead{-L.Rotation.X, -L.Rotation.Y, -L.Rotation.Z, L.Rotation.W};
    const FQuat delta = Mul(ToQuat(t.rot), qLead);
    const FVector off = Rotate(delta, {M.Translation.X - L.Translation.X, M.Translation.Y - L.Translation.Y, M.Translation.Z - L.Translation.Z});
    Transform placed;
    placed.at = {t.at.X + off.X, t.at.Y + off.Y, t.at.Z + off.Z};
    placed.rot = ToRotator(Mul(delta, M.Rotation));
    Place(actor, placed);
}
// Replaces a collection by its pieces (once per collection; when its level streams in again, only the
// new collection is removed and the pieces stay).
// Spline pieces (roads, curbs) would come back straight: collections made of them are not replaced.
constexpr int kMaxSplines = 8;
int SplineCount(UObject* comp) { return *reinterpret_cast<const int16_t*>(reinterpret_cast<uint8_t*>(comp) + 0x622); }  // CachedMeshSplineMeshCount
// False when it cannot be done yet (its pieces are not loaded): try again later.
bool Replace(UObject* collectionActor, UObject* context) {
    const std::string key = KeyOfLevelActor(collectionActor);
    UObject* comp = Field<UObject*>(collectionActor, kCollectionComponent);
    if (!IsValid(comp)) return true;
    // Again when the level streamed out (its pieces left with it) and back in.
    bool spawn = !collections.count(key);
    if (!spawn) {
        bool any = false;
        for (UObject* a : collections[key].actors) if (IsValid(a)) { any = true; break; }
        if (!any) { for (auto it = pieceKeys.begin(); it != pieceKeys.end();) it = it->second.rfind(key + "#", 0) == 0 ? pieceKeys.erase(it) : std::next(it);
                    for (auto it = pieceOf.begin(); it != pieceOf.end();) it = !IsValid(it->first) ? pieceOf.erase(it) : std::next(it);
                    collections.erase(key); spawn = true; }
    }
    std::vector<Piece> pieces;
    if (spawn) {
        pieces = ReadPieces(comp);
        if (SplineCount(comp) > kMaxSplines) { dg::Log("World edit: collection %s has %d spline pieces, not replaced", key.c_str(), SplineCount(comp)); return true; }
        // Still streaming in: removing it now would leave the place empty (walls and roofs gone).
        if (pieces.empty()) { collections.erase(key); return false; }
    }
    if (auto f = Fn(comp, "K2_DestroyComponent")) { Params p(f); p.Set("Object", comp); Call(comp, "K2_DestroyComponent", &p); }
    if (!spawn) return true;
    const ULONGLONG started = GetTickCount64();
    Collection& c = collections[key];
    c.actors.assign(pieces.size(), nullptr);
    c.leader = GroupPieces(pieces);
    c.origin.resize(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i) c.origin[i] = pieces[i].t;
    int made = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const std::string pieceKey = key + "#" + std::to_string(c.leader[i]);
        if (!previewMode && (removed.count(pieceKey) || removed.count(pieceKey + ":" + std::to_string(i)))) continue;
        UObject* actor = SpawnPiece(pieces[i], context, collectionActor);
        if (!actor) continue;
        c.actors[i] = actor; pieceKeys[actor] = pieceKey; pieceOf[actor] = {&c, int(i)}; ++made;
    }
    dg::Log("World edit: collection %s replaced by %d of %zu pieces in %llu ms", key.c_str(), made, pieces.size(), GetTickCount64() - started);
    return true;
}
bool Edited(const std::string& collectionKey) {
    const std::string prefix = collectionKey + "#";
    auto r = removed.lower_bound(prefix);
    if (r != removed.end() && r->compare(0, prefix.size(), prefix) == 0) return true;
    auto m = moved.lower_bound(prefix);
    return m != moved.end() && m->first.compare(0, prefix.size(), prefix) == 0;
}

Transform ReadTransform(const json::Value& v) {
    Transform t;
    t.at = {float(v["x"].asNumber()), float(v["y"].asNumber()), float(v["z"].asNumber())};
    t.rot = {float(v["pitch"].asNumber()), float(v["yaw"].asNumber()), float(v["roll"].asNumber())};
    t.scale = float(v["scale"].asNumber(1));
    return t;
}
}  // namespace

namespace {
std::string KeyOfLevelActor(UObject* actor) {
    if (!IsValid(actor) || !actor->Outer || !actor->Outer->Outer) return {};
    return GetName(actor->Outer->Outer) + "/" + GetName(actor);  // world (level package) / actor
}
}  // namespace
std::string KeyOf(UObject* actor) {
    if (auto p = pieceKeys.find(actor); p != pieceKeys.end()) return p->second;
    return KeyOfLevelActor(actor);
}

void Clear() {
    for (auto& a : added) if (IsValid(a.actor)) Call(a.actor, "K2_DestroyActor");
    for (auto* o : hidden) if (IsValid(o)) Hide(o, false);
    for (auto& [o, t] : original) if (IsValid(o)) Place(o, t);
    removed.clear(); moved.clear(); added.clear(); hidden.clear(); original.clear(); known.clear(); cursor = 0;
}

void Set(const json::Value& edits, bool preview) {
    previewMode = preview;
    // Bring back what is no longer removed (preview), forget handled objects so moves are re-applied.
    std::set<std::string> nextRemoved;
    for (auto& k : edits["remove"].elements()) nextRemoved.insert(k.asString());
    for (auto it = hidden.begin(); it != hidden.end();) {
        auto k = known.find(*it);
        if (!IsValid(*it) || (k != known.end() && !nextRemoved.count(k->second))) { if (IsValid(*it)) Hide(*it, false); it = hidden.erase(it); } else ++it;
    }
    removed = std::move(nextRemoved);
    moved.clear();
    for (auto& [key, v] : edits["move"].items()) moved[key] = ReadTransform(v);
    for (auto it = original.begin(); it != original.end();) {  // moves undone: back where they were
        auto k = known.find(it->first);
        if (IsValid(it->first) && k != known.end() && !moved.count(k->second)) { Place(it->first, it->second); it = original.erase(it); }
        else ++it;
    }
    // Added props: keep the actors of unchanged entries, move edited ones, drop the rest.
    std::vector<Added> next;
    for (auto& v : edits["add"].elements()) {
        Added a; a.uid = v["uid"].asString(); a.asset = v["asset"].asString(); a.t = ReadTransform(v);
        a.light = a.asset.rfind("light:", 0) == 0;
        if (a.light) a.cfg = ReadLight(a.asset, v["light"]);
        for (auto& old : added) if (old.uid == a.uid && old.asset == a.asset && IsValid(old.actor) && (!a.light || old.cfg.shadows == a.cfg.shadows)) {
            a.actor = old.actor; old.actor = nullptr; a.lightComp = old.lightComp;
            Place(a.actor, a.t);
            if (a.light && !(old.cfg == a.cfg)) ApplyLight(a.lightComp, MeshComponent(a.actor), a.cfg);
        }
        next.push_back(std::move(a));
    }
    for (auto& old : added) if (IsValid(old.actor)) Call(old.actor, "K2_DestroyActor");
    added = std::move(next);
    known.clear(); cursor = 0;
    dg::Log("World edits: %zu removed, %zu moved, %zu added%s", removed.size(), moved.size(), added.size(), preview ? " (editor preview)" : "");
}

void Update(UObject* pawn) {
    if (!IsValid(pawn) || (removed.empty() && moved.empty() && added.empty())) return;
    if (!actorClass) actorClass = FindClass("Actor");
    if (!collectionClass) collectionClass = FindClass("BendStaticMeshCollection");
    // Streamed levels bring their objects back: a slice of the object table per frame.
    const int n = NumObjects();
    if (cursor >= n) {
        cursor = 0;
        for (auto it = known.begin(); it != known.end();) it = IsValid(it->first) ? std::next(it) : known.erase(it);
    }
    for (const int end = std::min(n, cursor + 16384); cursor < end; ++cursor) {
        UObject* o = ObjectAt(cursor);
        if (!o || (o->ObjectFlags & 0x10) || known.count(o) || !IsValid(o) || !IsA(o, actorClass)) continue;
        const std::string key = KeyOf(o);
        known[o] = key;
        if (IsA(o, collectionClass) && Edited(key)) { if (!Replace(o, pawn)) known.erase(o); continue; }  // not loaded yet: the next pass
        // A part of an object of a collection has its own edits, over those of the whole object.
        const auto piece = pieceOf.find(o);
        const std::string part = piece != pieceOf.end() ? key + ":" + std::to_string(piece->second.second) : std::string();
        if (!part.empty() && removed.count(part)) { Remove(o); continue; }
        if (!part.empty()) if (auto m = moved.find(part); m != moved.end()) {
            if (previewMode && !original.count(o)) original[o] = {Location(o), Rotation(o), 1};
            Place(o, m->second);
            continue;
        }
        if (removed.count(key)) Remove(o);
        else if (auto m = moved.find(key); m != moved.end()) {
            if (previewMode && !original.count(o)) original[o] = {Location(o), Rotation(o), 1};
            if (auto pi = pieceOf.find(o); pi != pieceOf.end()) PlacePiece(o, *pi->second.first, pi->second.second, m->second);
            else Place(o, m->second);
        }
    }
    // Added props appear within 400 m of the player.
    const FVector here = Location(pawn);
    for (auto& a : added) {
        if (IsValid(a.actor) || a.failed) continue;
        const float dx = a.t.at.X - here.X, dy = a.t.at.Y - here.Y;
        if (dx * dx + dy * dy > 40000.f * 40000.f) continue;
        a.actor = a.light ? SpawnLight(a, pawn) : SpawnProp(a, pawn);
        if (!a.actor) { a.failed = true; dg::Log("World edit: cannot spawn %s", a.asset.c_str()); }
    }
}

bool Editable(UObject* collectionActor) {
    if (!IsValid(collectionActor)) return false;
    if (collections.count(KeyOfLevelActor(collectionActor))) return true;
    UObject* comp = Field<UObject*>(collectionActor, kCollectionComponent);
    return IsValid(comp) && SplineCount(comp) <= kMaxSplines;
}
int PieceAt(UObject* collectionActor, const FVector& at) {
    if (!collectionClass) collectionClass = FindClass("BendStaticMeshCollection");
    if (!IsValid(collectionActor) || !IsA(collectionActor, collectionClass)) return -1;
    UObject* comp = Field<UObject*>(collectionActor, kCollectionComponent);
    if (!IsValid(comp)) return -1;
    const auto pieces = ReadPieces(comp);
    // The smallest piece whose mesh bounds hold the point (the hit lies on its surface).
    int best = -1; float bestVolume = 0;
    for (int i = 0; i < int(pieces.size()); ++i) {
        const Piece& p = pieces[i];
        if (!IsValid(p.mesh) || p.hidden) continue;
        const float* b = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(p.mesh) + 0x128);  // UStaticMesh.ExtendedBounds
        const FQuat inv{-p.t.Rotation.X, -p.t.Rotation.Y, -p.t.Rotation.Z, p.t.Rotation.W};
        FVector local = Rotate(inv, {at.X - p.t.Translation.X, at.Y - p.t.Translation.Y, at.Z - p.t.Translation.Z});
        const FVector& s = p.t.Scale3D;
        if (s.X == 0 || s.Y == 0 || s.Z == 0) continue;
        local = {local.X / s.X, local.Y / s.Y, local.Z / s.Z};
        const float margin = 5;
        if (std::fabs(local.X - b[0]) > b[3] + margin || std::fabs(local.Y - b[1]) > b[4] + margin || std::fabs(local.Z - b[2]) > b[5] + margin) continue;
        const float volume = b[3] * b[4] * b[5] * std::fabs(s.X * s.Y * s.Z);
        if (best < 0 || volume < bestVolume) { best = i; bestVolume = volume; }
    }
    return best;
}
UObject* Detach(UObject* collectionActor, int piece, UObject* context, bool part) {
    const std::string key = KeyOfLevelActor(collectionActor);
    Replace(collectionActor, context);
    auto c = collections.find(key);
    if (c == collections.end() || piece < 0 || piece >= int(c->second.actors.size())) return nullptr;
    if (part) return IsValid(c->second.actors[piece]) ? c->second.actors[piece] : nullptr;
    const int leader = c->second.leader[piece];
    UObject* actor = nullptr;
    for (size_t i = 0; i < c->second.actors.size(); ++i)
        if (c->second.leader[i] == leader && IsValid(c->second.actors[i])) {
            known[c->second.actors[i]] = key + "#" + std::to_string(leader);
            if (!actor || int(i) == leader) actor = c->second.actors[i];
        }
    return actor;
}

std::string PartKey(UObject* actor) {
    auto p = pieceOf.find(actor);
    return p == pieceOf.end() ? std::string() : KeyOf(actor) + ":" + std::to_string(p->second.second);
}
UObject* Primary(const std::string& key) {
    if (const size_t colon = key.rfind(':'); colon != std::string::npos && key.find('#') != std::string::npos && colon > key.find('#')) {
        const int index = std::atoi(key.c_str() + colon + 1);
        for (auto& [o, k] : pieceKeys) if (IsValid(o) && pieceOf[o].second == index && key.compare(0, colon, k) == 0) return o;
        return nullptr;
    }
    UObject* first = nullptr;
    for (auto& [o, k] : pieceKeys) {
        if (k != key || !IsValid(o)) continue;
        const auto& [c, index] = pieceOf[o];
        if (c->leader[index] == index) return o;
        if (!first) first = o;
    }
    return first ? first : FindWorld(key);
}
std::vector<UObject*> Members(const std::string& key) {
    std::vector<UObject*> out;
    if (key.find('#') != std::string::npos && key.rfind(':') > key.find('#') && key.rfind(':') != std::string::npos) {
        if (UObject* o = Primary(key)) out.push_back(o);
        return out;
    }
    for (auto& [o, k] : pieceKeys) if (k == key && IsValid(o)) out.push_back(o);
    if (out.empty()) if (UObject* o = FindWorld(key)) out.push_back(o);
    return out;
}

UObject* FindAdded(const std::string& uid) {
    for (auto& a : added) if (a.uid == uid) return IsValid(a.actor) ? a.actor : nullptr;
    return nullptr;
}
UObject* FindWorld(const std::string& key) {
    for (auto& [o, k] : known) if (k == key && IsValid(o)) return o;
    return nullptr;
}
}  // namespace worldedit
