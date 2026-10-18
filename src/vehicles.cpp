#include "vehicles.h"
#include "mpui.h"
#include "lang.h"
#include "log.h"
#include "policy.h"
#include "skins.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

using namespace ue;

namespace vehicles {
namespace {
struct Copy {
    net::Client::Events::VehicleInfo info{};
    UObject* bike = nullptr;   // the game's bike standing for it here
    double nextTry = 0;
    net::VehicleState placed{};  // the server's place it was put at (it may roll or fall from there itself)
    std::vector<UObject*> markers;  // the bike's own map/radar markers (the game's)
    int marked = -1;                // what was shown (Mark)
    uint32_t tint = 0;              // the colour of `blip`
    UObject* blip = nullptr;        // our radar blip for a marked bike that is not yours
    UObject* repair = nullptr;      // the bike's interaction actor (repair, fuel; its icon above the bike)
    double nextAdvert = 0;
    int advert = -1;                // what the icon above it shows (Advert)
    std::string icon;               // its texture name
    std::string model;              // what `bike` was spawned as
    bool scanned = false;           // its own markers looked for
    bool following = false;         // someone else drives it: no physics, moved to where they are
    std::vector<float> route;       // its NPC's way: x,y,z of each point (empty: standing)
    float speed = 0; bool loop = false;
    double routeT0 = 0;             // Clock() when it set off
    UObject* driver = nullptr;      // the NPC on its seat (a body playing the riding pose)
    std::string driverLook;
    float shownYaw = 0, shownPitch = 0, flyZ = 0; bool yawSet = false;  // a helicopter turns, tilts and rises smoothly
    double lastFly = 0;
    int sequence = -1;                         // a helicopter's animation: 0 flying, 1 landing, 2 on the ground, 3 taking off
    bool repairHidden = false;      // the game's repair/fuel actor of an NPC's vehicle hidden
};
std::map<uint16_t, Copy> list;
int world = 0;
uint16_t riding = 0;                 // the vehicle the server let this player on
uint16_t boarding = 0;               // allowed, not on it yet
bool behind = false;                 // `riding` / `boarding` as a passenger
bool meleeOff = false;               // no punching from a passenger seat
uint16_t heliSeat = 0;               // the helicopter this player sits in (its seat: heliSeatIndex)
uint16_t heliPending = 0;            // allowed into one, not seated yet
int heliSeatIndex = 0;
double boardUntil = 0, nextBoardTry = 0, nextReport = 0;
bool onIt = false;                   // on `riding` (not off it for long)
double offSince = 0, foreignSince = 0;  // not on the ridden one / on a server bike unasked: since when
int boardTries = 0;

// The vehicles a mode may place: the bike, and the game's other ridable ones (built like the bike: the
// same seats, fuel, health, getting on and off). reach: how near the "Get on" prompt shows; lift: how
// much higher than a bike it is put (bigger wheels); box: where characters bump into it (centre and
// half size from the vehicle's origin; the trucks have no such collider of their own, the golf cart has).
struct Model { const char* name; const char* path; float reach, lift; FVector boxCenter, boxHalf; };
constexpr Model kModels[] = {
    {"", "/Game/Vehicles/Bikes/BP_bike04Drifter.BP_bike04Drifter_C", 300.f, 0.f, {}, {}},
    {"golfcart", "/Game/Vehicles/GolfCart/BP_GolfCart.BP_GolfCart_C", 350.f, 20.f, {}, {}},
    {"dumptruck", "/Game/Vehicles/Truck/SemiDumptruck/BP_DrivableDumpTruck.BP_DrivableDumpTruck_C", 650.f, 120.f, {-265.f, 0.f, 185.f}, {470.f, 170.f, 170.f}},
    {"militarytruck", "/Game/Vehicles/MilitaryTransportTruck_v2/BP_DrivableMilitaryTransportTruck_v2.BP_DrivableMilitaryTransportTruck_v2_C", 650.f, 120.f, {-36.f, 0.f, 130.f}, {366.f, 120.f, 125.f}},
    // NERO's helicopter: not a vehicle of the game (it flies where it is put), never physical here.
    {"helicopter", "/Game/Libraries/Blueprints/AI/NERO/ScriptedEncounterChopper.ScriptedEncounterChopper_C", 900.f, 0.f, {}, {}},
};
const Model& ModelOf(const std::string& name) { for (auto& m : kModels) if (name == m.name) return m; return kModels[0]; }
double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
UObject* Statics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
// Kept loaded (in the root set): spawned again whenever a player comes near.
UObject* ClassOf(const Model& m) {
    static std::map<std::string, UObject*> classes;
    UObject*& c = classes[m.name];
    if (!IsValid(c)) { c = FindObject(m.path); if (!c) c = LoadAsset(m.path); if (c) AddToRoot(c); }
    return c;
}
FVector Location(UObject* a) { FVector v{}; if (auto f = Fn(a, "K2_GetActorLocation")) { Params p(f); Call(a, "K2_GetActorLocation", &p); v = p.Get<FVector>("ReturnValue"); } return v; }
float Yaw(UObject* a) { float y = 0; if (auto f = Fn(a, "K2_GetActorRotation")) { Params p(f); Call(a, "K2_GetActorRotation", &p); y = p.Get<FRotator>("ReturnValue").Yaw; } return y; }
UObject* Prop(UObject* o, const char* name) {
    for (auto c = o->Class; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return p;
    return nullptr;
}
UObject* FuelComp(UObject* bike) { UObject* p = Prop(bike, "FuelComponent"); return p ? Field<UObject*>(bike, Field<int>(p, off::UProperty_Offset)) : nullptr; }
float* FloatProp(UObject* o, const char* name) { UObject* p = Prop(o, name); return p ? reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(o) + Field<int>(p, off::UProperty_Offset)) : nullptr; }

net::VehicleState StateOf(UObject* bike) {
    net::VehicleState s;
    const FVector at = Location(bike);
    s.x = at.X; s.y = at.Y; s.z = at.Z; s.yaw = Yaw(bike);
    if (UObject* fuel = FuelComp(bike); IsValid(fuel)) if (auto f = Fn(fuel, "GetFuelRemaining0To1")) { Params p(f); Call(fuel, "GetFuelRemaining0To1", &p); s.fuel = p.Get<float>("ReturnValue"); }
    const float* health = FloatProp(bike, "Health"); const float* max = FloatProp(bike, "MaxHealth");
    if (health && max && *max > 0) s.health = std::fmax(0.f, std::fmin(1.f, *health / *max));
    return s;
}
// The fuel and health the server keeps, on the bike about to be ridden.
void ApplyState(UObject* bike, const net::VehicleState& s) {
    if (UObject* fuel = FuelComp(bike); IsValid(fuel)) if (auto f = Fn(fuel, "SetFuelRemaining0To1")) { Params p(f); p.Set<float>("FuelRemaining0To1", s.fuel); Call(fuel, "SetFuelRemaining0To1", &p); }
    float* health = FloatProp(bike, "Health"); const float* max = FloatProp(bike, "MaxHealth");
    if (health && max && *max > 0) { *health = *max * s.health; if (auto f = Fn(bike, "RefreshHealth")) { Params p(f); Call(bike, "RefreshHealth", &p); } }
}
void Place(UObject* bike, const net::VehicleState& s) {
    if (auto f = Fn(bike, "K2_SetActorLocationAndRotation")) {
        Params p(f); p.Set("NewLocation", FVector{s.x, s.y, s.z}); p.Set("NewRotation", FRotator{0, s.yaw, 0}); p.SetBool("bTeleport", true);
        Call(bike, "K2_SetActorLocationAndRotation", &p);
    }
}
void Park(UObject* bike) {
    if (auto f = Fn(bike, "LowerKickstand")) { Params p(f); p.SetBool("bLower", true); Call(bike, "LowerKickstand", &p); }
    if (auto f = Fn(bike, "Park")) { Params p(f); p.SetBool("bPark", true); Call(bike, "Park", &p); }
}
UObject* Component(UObject* actor, const char* name) { UObject* p = Prop(actor, name); UObject* c = p ? Field<UObject*>(actor, Field<int>(p, off::UProperty_Offset)) : nullptr; return IsValid(c) ? c : nullptr; }
// ---------------------------------------------------------------- NPC drivers
// Where the ground is under x,y (between zTop and zBottom); false when nothing is there.
bool GroundAt(UObject* self, float x, float y, float zTop, float zBottom, float& z) {
    static UObject* kismet = nullptr; static UObject* statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr; auto brk = statics ? Fn(statics, "BreakHitResult") : nullptr;
    if (!trace || !brk || !IsValid(self)) return false;
    Params t(trace); t.Set("WorldContextObject", self); t.Set("Start", FVector{x, y, zTop}); t.Set("End", FVector{x, y, zBottom});
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    if (!t.GetBool("ReturnValue")) return false;
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    if (!hitProp || !inProp) return false;
    Params b(brk);
    std::memcpy(static_cast<uint8_t*>(b.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &b);
    z = b.Get<FVector>("ImpactPoint").Z;
    return true;
}
// Where a vehicle on `route` is after `dist` units, and which way it faces.
// (As the server counts it: vertical legs, a helicopter's climb and descent, take three times longer.)
// climb: the current leg's rise per unit (-1 down .. 1 up); returns true when past the end.
bool RouteAt(const std::vector<float>& pts, bool loop, double dist, float& x, float& y, float& z, float& yaw, float* climb = nullptr) {
    const size_t n = pts.size() / 3;
    if (climb) *climb = 0;
    if (!n) return true;
    auto real = [&](size_t a, size_t b) { const float dx = pts[b * 3] - pts[a * 3], dy = pts[b * 3 + 1] - pts[a * 3 + 1], dz = pts[b * 3 + 2] - pts[a * 3 + 2]; return std::sqrt(dx * dx + dy * dy + dz * dz); };
    auto len = [&](size_t a, size_t b) {
        const float dx = pts[b * 3] - pts[a * 3], dy = pts[b * 3 + 1] - pts[a * 3 + 1], l = real(a, b);
        return std::sqrt(dx * dx + dy * dy) < 0.3f * l ? l * 3 : l;
    };
    const size_t segs = loop ? n : n - 1;
    double total = 0;
    for (size_t i = 0; i < segs; ++i) total += len(i, (i + 1) % n);
    x = pts[0]; y = pts[1]; z = pts[2];
    if (!segs || total < 1) return !loop;
    const bool ended = !loop && dist >= total;
    dist = loop ? std::fmod(std::max(0.0, dist), total) : std::min(std::max(0.0, dist), total);
    for (size_t i = 0; i < segs; ++i) {
        const size_t j = (i + 1) % n;
        const float l = len(i, j);
        if (dist > l && i + 1 < segs) { dist -= l; continue; }
        const float k = l > 0 ? float(std::min(1.0, dist / l)) : 1.f;
        x = pts[i * 3] + (pts[j * 3] - pts[i * 3]) * k; y = pts[i * 3 + 1] + (pts[j * 3 + 1] - pts[i * 3 + 1]) * k; z = pts[i * 3 + 2] + (pts[j * 3 + 2] - pts[i * 3 + 2]) * k;
        const float hx = pts[j * 3] - pts[i * 3], hy = pts[j * 3 + 1] - pts[i * 3 + 1];
        if (hx * hx + hy * hy > 1.f) yaw = std::atan2(hy, hx) * 57.2957795f;
        if (climb && real(i, j) > 0) *climb = (pts[j * 3 + 2] - pts[i * 3 + 2]) / real(i, j);
        return ended;
    }
    return ended;
}
// Where the driver's body is on each model (from the vehicle's origin; measured on the player riding them).
FVector DriverSeat(const std::string& model) {
    if (model == "golfcart") return {-22.4f, -26.6f, 20.8f};
    if (model == "dumptruck") return {175.1f, -41.4f, 466.5f};
    if (model == "militarytruck") return {105.3f, -48.2f, 137.4f};
    if (model == "helicopter") return {140.f, -45.f, 70.f};  // the pilot's seat, front left
    return {43.2f, 0.f, 25.3f};
}
// How high above the ground each model's origin is, and how far its wheels are from it (for the slope).
float RideHeight(const std::string& model) { return model.empty() ? 17.f : 0.f; }
float HalfBase(const std::string& model) { return model.empty() ? 70.f : model == "golfcart" ? 110.f : 250.f; }
// A helicopter: across its way, over the ground. Its height follows the way but keeps 25 m over the
// land under it and a few seconds ahead (it climbs before a hill), lands on the ground under it, and
// changes smoothly; it turns and tilts smoothly; its rotor animation goes by what it does.
void Fly(Copy& c, double now, float x, float y, float z, float yaw, float climb, bool moving) {
    const float dt = c.lastFly > 0 ? float(std::clamp(now - c.lastFly, 0.0, 0.1)) : 0.f;
    c.lastFly = now;
    z -= 60.f;  // the way is on the ground + 60 (as a bike stands): its origin is at its skids
    if (!c.yawSet) { c.shownYaw = yaw; c.flyZ = z; c.yawSet = true; }
    const float d = std::fmod(yaw - c.shownYaw + 540.f, 360.f) - 180.f;
    c.shownYaw += std::clamp(d, -70.f * dt, 70.f * dt);  // degrees a second
    const float rad = c.shownYaw * 0.0174532925f, ahead = std::max(3000.f, c.speed * 4.f);
    float ground = -1e9f, groundAhead = -1e9f;
    const bool hit = GroundAt(c.bike, x, y, std::max(z, c.flyZ) + 20000.f, z - 50000.f, ground);
    GroundAt(c.bike, x + std::cos(rad) * ahead, y + std::sin(rad) * ahead, std::max(z, c.flyZ) + 20000.f, z - 50000.f, groundAhead);
    const bool cruise = moving && std::fabs(climb) <= 0.7f;
    float target = z;
    if (cruise) target = std::max({z, ground + 2500.f, groundAhead + 2500.f});
    else if (hit) target = moving ? std::max(z, ground) : ground;  // up / down legs; standing: on the ground
    const float up = 1500.f * dt, down = 900.f * dt;
    c.flyZ += std::clamp(target - c.flyZ, -down, up);
    if (hit) c.flyZ = std::max(c.flyZ, ground);
    const bool landed = hit && c.flyZ - ground < 80.f;
    const int sequence = !moving ? (landed ? 2 : 1) : climb > 0.7f ? 3 : climb < -0.7f ? 1 : 0;
    if (sequence != c.sequence) {
        c.sequence = sequence;
        if (auto f = Fn(c.bike, "SetSequenceNumber")) { Params p(f); p.Set<int32_t>(GetName(FirstParam(f)).c_str(), sequence); Call(c.bike, "SetSequenceNumber", &p); }
        if (auto f = Fn(c.bike, "SetRotorMaterialState")) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), true); Call(c.bike, "SetRotorMaterialState", &p); }
    }
    const float pitch = sequence == 0 ? -6.f : 0.f;  // nose down in flight
    c.shownPitch += std::clamp(pitch - c.shownPitch, -8.f * dt, 8.f * dt);
    if (auto f = Fn(c.bike, "K2_SetActorLocationAndRotation")) {
        Params p(f); p.Set("NewLocation", FVector{x, y, c.flyZ}); p.Set("NewRotation", FRotator{c.shownPitch, c.shownYaw, 0}); p.SetBool("bTeleport", true);
        Call(c.bike, "K2_SetActorLocationAndRotation", &p);
    }
}
// Its NPC drives it (or it waits for one): no physics, put on the ground of its way every frame.
void Drive(Copy& c, double now) {
    float x = c.info.state.x, y = c.info.state.y, z = c.info.state.z, yaw = c.info.state.yaw, climb = 0;
    bool moving = false;
    if (!c.route.empty()) moving = !RouteAt(c.route, c.loop, (now - c.routeT0) * c.speed, x, y, z, yaw, &climb);
    if (c.model == "helicopter") { Fly(c, now, x, y, z, yaw, climb, moving); return; }
    const float rad = yaw * 0.0174532925f, fx = std::cos(rad), fy = std::sin(rad), half = HalfBase(c.model);
    float g = z, gf = z, gb = z;
    const bool hit = GroundAt(c.bike, x, y, z + 400.f, z - 2000.f, g);
    if (hit) {
        if (!GroundAt(c.bike, x + fx * half, y + fy * half, g + 300.f, g - 300.f, gf)) gf = g;
        if (!GroundAt(c.bike, x - fx * half, y - fy * half, g + 300.f, g - 300.f, gb)) gb = g;
    }
    const float pitch = hit ? std::atan2(gf - gb, 2 * half) * 57.2957795f : 0.f;
    if (auto f = Fn(c.bike, "K2_SetActorLocationAndRotation")) {
        Params p(f); p.Set("NewLocation", FVector{x, y, (hit ? g : z - 60.f) + RideHeight(c.model)}); p.Set("NewRotation", FRotator{pitch, yaw, 0}); p.SetBool("bTeleport", true);
        Call(c.bike, "K2_SetActorLocationAndRotation", &p);
    }
}
void DropDriver(Copy& c) { if (IsValid(c.driver)) Call(c.driver, "K2_DestroyActor"); c.driver = nullptr; c.driverLook.clear(); }
// The NPC on its seat: a body of its look, in the riding pose of the model.
void SeatDriver(Copy& c) {
    static const std::string kDefaultLook = "/Game/Characters/Deacon/looks/deacon_standard.deacon_standard";
    const std::string look = c.info.npcLook.empty() ? kDefaultLook : c.info.npcLook;
    if (IsValid(c.driver) && c.driverLook == look) return;
    DropDriver(c);
    UObject* cls = FindClass("SkeletalMeshActor");
    UObject* root = Field<UObject*>(c.bike, off::Actor_RootComponent);
    if (!cls || !IsValid(root) || !Statics()) return;
    FTransform t; t.Translation = Location(c.bike);
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", c.bike); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* body = begin.Get<UObject*>("ReturnValue");
    if (!body) return;
    { Params fin(Fn(Statics(), "FinishSpawningActor")); fin.Set("Actor", body); fin.Set("SpawnTransform", t); Call(Statics(), "FinishSpawningActor", &fin); }
    { Params col(Fn(body, "SetActorEnableCollision")); col.SetBool("bNewActorEnableCollision", false); Call(body, "SetActorEnableCollision", &col); }
    UObject* mesh = Field<UObject*>(body, off::SkelMeshActor_Component);
    if (!IsValid(mesh) || !skins::Wear(mesh, look)) { Call(body, "K2_DestroyActor"); return; }
    { Params a(Fn(body, "K2_AttachRootComponentTo")); a.Set("InParent", root); a.Set("InSocketName", FName{0, 0}); a.Set<uint8_t>("AttachLocationType", 2); Call(body, "K2_AttachRootComponentTo", &a); }
    // Where the game puts a rider's body (meshes face their +Y).
    if (auto f = Fn(mesh, "K2_SetRelativeLocationAndRotation")) { Params p(f); p.Set("NewLocation", DriverSeat(c.model)); p.Set("NewRotation", FRotator{0, -90.f, 0}); p.SetBool("bTeleport", true); Call(mesh, "K2_SetRelativeLocationAndRotation", &p); }
    c.driver = body; c.driverLook = look;
}
// Its riding pose, looping (started again whenever it stops: a look's parts load and reset the mesh).
void PoseDriver(Copy& c) {
    UObject* mesh = IsValid(c.driver) ? Field<UObject*>(c.driver, off::SkelMeshActor_Component) : nullptr;
    if (!IsValid(mesh)) return;
    if (auto f = Fn(mesh, "IsPlaying")) { Params p(f); Call(mesh, "IsPlaying", &p); if (p.GetBool("ReturnValue")) return; }
    const char* path = c.model.empty() ? "/Game/Animation/Deacon/bike/bike_driver_bike_lean_center.bike_driver_bike_lean_center"
                                       : "/Game/Animation/Deacon/bike/dcn_golfcart_driver_idle.dcn_golfcart_driver_idle";
    static std::map<std::string, UObject*> anims;
    UObject*& anim = anims[path];
    if (!IsValid(anim)) { anim = FindObject(path); if (!anim) anim = LoadAsset(path); if (anim) AddToRoot(anim); }
    if (!anim) return;
    if (auto f = Fn(mesh, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 1); Call(mesh, "SetAnimationMode", &p); }
    if (auto f = Fn(mesh, "PlayAnimation")) { Params p(f); p.Set("NewAnimToPlay", anim); p.SetBool("bLooping", true); Call(mesh, "PlayAnimation", &p); }
}

// ---------------------------------------------------------------- helicopter seats
// Where a passenger's body sits in the helicopter (its capsule's centre; 4 seats in the cabin).
FVector HeliSeat(int i) {
    static const FVector seats[] = {{-40.f, -50.f, 160.f}, {-40.f, 50.f, 160.f}, {-130.f, -50.f, 160.f}, {-130.f, 50.f, 160.f}};
    return seats[std::clamp(i, 0, 3)];
}
UObject* PawnMesh(UObject* pawn) { UObject* p = Prop(pawn, "Mesh"); UObject* m = p ? Field<UObject*>(pawn, Field<int>(p, off::UProperty_Offset)) : nullptr; return IsValid(m) ? m : nullptr; }
UObject* PawnMove(UObject* pawn) { UObject* p = Prop(pawn, "CharacterMovement"); UObject* m = p ? Field<UObject*>(pawn, Field<int>(p, off::UProperty_Offset)) : nullptr; return IsValid(m) ? m : nullptr; }
void BoolCall(UObject* o, const char* fn, bool v) { if (auto f = IsValid(o) ? Fn(o, fn) : nullptr) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), v); Call(o, fn, &p); } }
void MoveMode(UObject* pawn, uint8_t mode) { if (UObject* m = PawnMove(pawn)) if (auto f = Fn(m, "SetMovementMode")) { Params p(f); p.Set<uint8_t>("NewMovementMode", mode); Call(m, "SetMovementMode", &p); } }
// Into its seat: no walking, no collision, carried by the helicopter, sitting.
void SitIn(UObject* pawn, UObject* controller, UObject* heli, int seat) {
    UObject* root = Field<UObject*>(heli, off::Actor_RootComponent);
    if (!IsValid(root)) return;
    BoolCall(controller, "SetIgnoreMoveInput", true);
    BoolCall(pawn, "SetActorEnableCollision", false);
    MoveMode(pawn, 0);  // none
    { Params a(Fn(pawn, "K2_AttachRootComponentTo")); a.Set("InParent", root); a.Set("InSocketName", FName{0, 0}); a.Set<uint8_t>("AttachLocationType", 2); Call(pawn, "K2_AttachRootComponentTo", &a); }
    if (UObject* capsule = Field<UObject*>(pawn, off::Actor_RootComponent); IsValid(capsule))
        if (auto f = Fn(capsule, "K2_SetRelativeLocationAndRotation")) { Params p(f); p.Set("NewLocation", HeliSeat(seat)); p.Set("NewRotation", FRotator{}); p.SetBool("bTeleport", true); Call(capsule, "K2_SetRelativeLocationAndRotation", &p); }
    if (UObject* mesh = PawnMesh(pawn)) {
        const char* path = "/Game/Animation/Deacon/bike/dcn_golfcart_passenger_idle.dcn_golfcart_passenger_idle";
        UObject* anim = FindObject(path); if (!anim) anim = LoadAsset(path);
        if (anim) {
            if (auto f = Fn(mesh, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 1); Call(mesh, "SetAnimationMode", &p); }
            if (auto f = Fn(mesh, "PlayAnimation")) { Params p(f); p.Set("NewAnimToPlay", anim); p.SetBool("bLooping", true); Call(mesh, "PlayAnimation", &p); }
        }
    }
    dg::Log("Vehicles: in helicopter seat %d", seat + 1);
}
// Out of it: on the ground by its side, walking again.
void StandUp(UObject* pawn, UObject* controller, UObject* heli) {
    if (auto f = Fn(pawn, "DetachRootComponentFromParent")) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), true); Call(pawn, "DetachRootComponentFromParent", &p); }
    if (UObject* mesh = PawnMesh(pawn)) if (auto f = Fn(mesh, "SetAnimationMode")) { Params p(f); p.Set<uint8_t>("InAnimationMode", 0); Call(mesh, "SetAnimationMode", &p); }
    FVector at = Location(pawn);
    if (IsValid(heli)) {
        const FVector h = Location(heli); const float rad = Yaw(heli) * 0.0174532925f;
        at = {h.X - std::sin(rad) * 400.f, h.Y + std::cos(rad) * 400.f, h.Z + 150.f};  // by its right door
        float g = 0;
        if (GroundAt(pawn, at.X, at.Y, at.Z + 300.f, at.Z - 3000.f, g)) at.Z = g + 100.f;
    }
    if (auto f = Fn(pawn, "K2_SetActorLocationAndRotation")) { Params p(f); p.Set("NewLocation", at); p.Set("NewRotation", FRotator{0, Yaw(pawn), 0}); p.SetBool("bTeleport", true); Call(pawn, "K2_SetActorLocationAndRotation", &p); }
    BoolCall(pawn, "SetActorEnableCollision", true);
    MoveMode(pawn, 1);  // walking
    BoolCall(controller, "SetIgnoreMoveInput", false);
    dg::Log("Vehicles: out of the helicopter");
}

// An invisible box characters bump into (the game's collider of the golf cart: "VehicleRagdollCollider",
// blocking pawns only; the vehicle's own body collides with the world and other vehicles).
void AddPawnBox(UObject* vehicle, const Model& m) {
    static UObject* cube = nullptr;
    if (!IsValid(cube)) { cube = FindObject("/Engine/BasicShapes/Cube.Cube"); if (!cube) cube = LoadAsset("/Engine/BasicShapes/Cube.Cube"); if (cube) AddToRoot(cube); }
    static UObject* cls = FindClass("StaticMeshComponent");
    UObject* mesh = Field<UObject*>(vehicle, off::Actor_RootComponent);
    UObject* box = cube && cls && IsValid(mesh) ? NewObject(cls, vehicle) : nullptr;
    if (!box) return;
    { Params p(Fn(box, "SetStaticMesh")); p.Set("NewMesh", cube); Call(box, "SetStaticMesh", &p); }
    RegisterComponent(box);
    { Params p(Fn(box, "K2_AttachTo")); p.Set("InParent", mesh); p.Set<uint8_t>("AttachType", 2); Call(box, "K2_AttachTo", &p); }
    { Params p(Fn(box, "K2_SetRelativeLocationAndRotation")); p.Set("NewLocation", m.boxCenter); p.Set("NewRotation", FRotator{}); p.SetBool("bTeleport", true); Call(box, "K2_SetRelativeLocationAndRotation", &p); }
    { Params p(Fn(box, "SetRelativeScale3D")); p.Set("NewScale3D", FVector{m.boxHalf.X / 50.f, m.boxHalf.Y / 50.f, m.boxHalf.Z / 50.f}); Call(box, "SetRelativeScale3D", &p); }  // the cube is 100 across
    { Params p(Fn(box, "SetCollisionProfileName")); p.Set("InCollisionProfileName", FindName("VehicleRagdollCollider")); Call(box, "SetCollisionProfileName", &p); }
    { Params p(Fn(box, "SetHiddenInGame")); p.SetBool("NewHidden", true); Call(box, "SetHiddenInGame", &p); }
}
// The models came from missions where the game drove them: no reverse gear (the bike has its own way),
// no box for characters.
void Tune(UObject* vehicle, const Model& m) {
    if (UObject* move = Component(vehicle, "VehicleMovementComponent")) SetBoolProp(move, "bHasReverseGear", true);
    if (m.boxHalf.X > 0) AddPawnBox(vehicle, m);
    // The golf cart cannot be repaired (the game's repair is the bike's): it takes no damage instead.
    if (std::string(m.name) == "golfcart") if (UObject* damage = Component(vehicle, "VehicleDamage")) if (auto f = Fn(damage, "SetDamageEnabled")) {
        Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), false); Call(damage, "SetDamageEnabled", &p);
    }
}
// Someone else drives it: it only follows them (no physics of its own, still in the way of others).
// Off its stand too: a passenger gets on a parked bike beside it, not on it.
void Follow(UObject* vehicle) {
    if (auto f = Fn(vehicle, "LowerKickstand")) { Params p(f); p.SetBool("bLower", false); Call(vehicle, "LowerKickstand", &p); }
    if (auto f = Fn(vehicle, "Park")) { Params p(f); p.SetBool("bPark", false); Call(vehicle, "Park", &p); }
    if (UObject* move = Component(vehicle, "VehicleMovementComponent")) if (auto f = Fn(move, "Deactivate")) { Params p(f); Call(move, "Deactivate", &p); }
    if (UObject* mesh = Field<UObject*>(vehicle, off::Actor_RootComponent); IsValid(mesh)) if (auto f = Fn(mesh, "SetSimulatePhysics")) { Params p(f); p.SetBool("bSimulate", false); Call(mesh, "SetSimulatePhysics", &p); }
}
UObject* Spawn(UObject* context, const net::VehicleState& s, const Model& m) {
    UObject* cls = ClassOf(m);
    if (!cls || !Statics() || !IsValid(context)) return nullptr;
    FTransform t; t.Translation = {s.x, s.y, s.z + m.lift};
    const float h = s.yaw * 0.00872664626f; t.Rotation = {0, 0, std::sin(h), std::cos(h)};
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* bike = begin.Get<UObject*>("ReturnValue");
    if (!bike) return nullptr;
    Params finish(Fn(Statics(), "FinishSpawningActor")); finish.Set("Actor", bike); finish.Set("SpawnTransform", t);
    Call(Statics(), "FinishSpawningActor", &finish);
    if (*m.name) Tune(bike, m);
    Park(bike);
    return bike;
}
// The bike's own markers (map icon and radar): shown only when the server says so for this player.
// Markers. Yours: the bike's own map and radar marker of the game (and its icon above it). Others':
// none of that; marked for you by the mode (a job's, a gang's), a small bike of its colour on the
// radar only (the game's own marker cannot be recoloured).
UObject* RadarBlip(UObject* bike, uint32_t rgba, const std::string& iconName) {
    static UObject* cls = FindClass("MiniMapLocationComponent");
    UObject* blip = cls ? NewObject(cls, bike) : nullptr;
    if (!blip) return nullptr;
    auto prop = [&](const char* name) -> uint8_t* {
        for (auto k = blip->Class; k; k = SuperOf(k)) if (auto pr = FindProperty(k, name)) return reinterpret_cast<uint8_t*>(blip) + Field<int>(pr, off::UProperty_Offset);
        return nullptr;
    };
    const std::string name = iconName.empty() ? std::string("MAP_bike") : iconName;
    const std::string path = "/Game/Libraries/UI/Textures/" + name + "." + name;
    UObject* icon = FindObject(path.c_str());
    if (!icon) icon = LoadAsset(path.c_str());
    if (icon) AddToRoot(icon);
    if (auto pr = prop("MapIcon")) *reinterpret_cast<UObject**>(pr) = icon;
    if (auto pr = prop("LocationType")) *pr = 0;    // Player: the radar takes colour and size for it (not for Motorcycle)
    if (auto pr = prop("DisplayRule")) *pr = 0;     // AlwaysVisible
    if (auto pr = prop("EdgeDisplay")) *pr = 0;     // only when near
    if (auto pr = prop("IconScale")) *reinterpret_cast<float*>(pr) = 0.35f;
    const float color[4] = {float((rgba >> 24) & 0xFF) / 255.f, float((rgba >> 16) & 0xFF) / 255.f, float((rgba >> 8) & 0xFF) / 255.f, 1.f};
    if (auto pr = prop("MinimapColor")) std::memcpy(pr, color, sizeof(color));
    if (auto pr = prop("MarkerColor")) std::memcpy(pr, color, sizeof(color));
    SetBoolProp(blip, "bGetPositionFromOwner", true);
    RegisterComponent(blip);
    policy::AllowMarker(blip, true);
    policy::RadarOnly(blip, true);
    return blip;
}
// The bike's interaction actor (BP_BikeRepair: repair, fuel, its icon above the bike).
UObject* RepairActor(UObject* bike) {
    UObject* p = Prop(bike, "BikeInteractRef");
    UObject* actor = p ? Field<UObject*>(bike, Field<int>(p, off::UProperty_Offset)) : nullptr;
    return IsValid(actor) ? actor : nullptr;
}
// The icon above the bike: set when what it should show changes, and confirmed every few seconds (the
// game shows it again now and then).
void Advert(Copy& c, double now) {
    const int want = c.info.yours ? 1 : c.info.marked ? 2 : 0;
    if (want == c.advert && c.tint == (c.info.color ? c.info.color : 0x73BFFFFFu) && now < c.nextAdvert) return;
    c.nextAdvert = now + 3; c.advert = want;
    if (!IsValid(c.repair)) c.repair = RepairActor(c.bike);
    if (!IsValid(c.repair)) return;
    // Yours: as the game shows it; marked for you: in the vehicle's colour; the rest: transparent (the
    // game turns its visibility back on by itself, never its colour).
    const bool shown = c.info.yours || c.info.marked;
    UObject* q = Prop(c.repair, "BikeAdvertise");
    UObject* advert = q ? Field<UObject*>(c.repair, Field<int>(q, off::UProperty_Offset)) : nullptr;  // the UMG widget of the icon
    if (IsValid(advert)) if (auto f = Fn(advert, "SetColorAndOpacity")) {
        const uint32_t tint = c.info.yours ? 0xFFFFFFFFu : c.info.color ? c.info.color : 0x73BFFFFFu;
        struct { float r, g, b, a; } col{float((tint >> 24) & 0xFF) / 255.f, float((tint >> 16) & 0xFF) / 255.f, float((tint >> 8) & 0xFF) / 255.f, shown ? 1.f : 0.f};
        Params w(f); w.Set("InColorAndOpacity", col); Call(advert, "SetColorAndOpacity", &w);
    }
}
void DropBlip(Copy& c) {
    if (IsValid(c.blip)) { policy::AllowMarker(c.blip, false); policy::RadarOnly(c.blip, false); if (auto f = Fn(c.blip, "K2_DestroyComponent")) { Params p(f); p.Set("Object", c.blip); Call(c.blip, "K2_DestroyComponent", &p); } }
    c.blip = nullptr;
}
void Mark(Copy& c) {
    const uint32_t tint = c.info.color ? c.info.color : 0x73BFFFFFu;  // light blue unless the mode says
    const int state = c.info.yours ? 1 : c.info.marked ? 2 : 0;          // 1 the game's, 2 our radar blip, 0 none
    if (c.marked == state && (state != 2 || (IsValid(c.blip) && c.tint == tint && c.icon == c.info.icon)) && c.scanned) return;
    if (!c.scanned) {
        c.scanned = true;
        static UObject* cls = FindClass("MiniMapLocationComponent");
        if (auto f = cls ? Fn(c.bike, "GetComponentsByClass") : nullptr) {
            Params p(f); p.Set("ComponentClass", cls); Call(c.bike, "GetComponentsByClass", &p);
            struct Raw { UObject** data; int32_t num, max; };
            const Raw found = p.Get<Raw>("ReturnValue");
            for (int i = 0; i < found.num && i < 16; ++i) if (IsValid(found.data[i]) && found.data[i] != c.blip) c.markers.push_back(found.data[i]);
        }
    }
    for (UObject* m : c.markers) if (IsValid(m)) { policy::AllowMarker(m, state == 1); policy::DenyMarker(m, state != 1); }
    if (state == 2 && (!IsValid(c.blip) || c.tint != tint || c.icon != c.info.icon)) { DropBlip(c); c.blip = RadarBlip(c.bike, tint, c.info.icon); c.icon = c.info.icon; }
    if (state != 2) DropBlip(c);
    c.marked = state; c.tint = tint;
}
void Destroy(UObject*& bike) { if (IsValid(bike)) Call(bike, "K2_DestroyActor"); bike = nullptr; }
UObject* Mounted(UObject* controller) {
    auto f = IsValid(controller) ? Fn(controller, "GetMountedVehicle") : nullptr; if (!f) return nullptr;
    Params p(f); Call(controller, "GetMountedVehicle", &p);
    UObject* v = p.Get<UObject*>("ReturnValue");
    return IsValid(v) ? v : nullptr;
}
void GetOff(UObject* pawn) {
    if (auto f = IsValid(pawn) ? Fn(pawn, "TryExitVehicle") : nullptr) { Params p(f); p.SetBool("bForce", true); Call(pawn, "TryExitVehicle", &p); }
}
float Dist2(const FVector& a, const FVector& b) { const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z; return dx * dx + dy * dy + dz * dz; }
}  // namespace

void OnVehicle(uint16_t id, const net::Client::Events::VehicleInfo* info) {
    if (!info) {
        auto it = list.find(id);
        if (it != list.end()) { DropDriver(it->second); Destroy(it->second.bike); list.erase(it); }
        if (riding == id) riding = 0;
        if (boarding == id) boarding = 0;
        return;
    }
    Copy& c = list[id];
    c.info = *info;
    if (!c.info.npc) c.route.clear();
}
void OnRoute(uint16_t id, const std::vector<float>& points, float speed, float elapsed, bool loop) {
    auto it = list.find(id);
    if (it == list.end()) return;
    Copy& c = it->second;
    c.route = points; c.speed = speed; c.loop = loop; c.routeT0 = Clock() - elapsed;
}
void OnAllow(uint16_t id, int seat) {
    auto it = list.find(id);
    if (it != list.end() && it->second.info.model == "helicopter") { heliPending = id; heliSeatIndex = std::max(0, seat - 1); return; }
    boarding = id; behind = seat > 0; boardUntil = GetTickCount64() / 1000.0 + 15; nextBoardTry = 0; boardTries = 0;
}
bool heliStandUp = false;  // the server took this player out of its helicopter
void OnEject(uint16_t id) {
    if (heliSeat == id || heliPending == id) { heliPending = 0; heliStandUp = true; return; }
    if (boarding == id) boarding = 0; if (riding == id) riding = 0; onIt = false;
}
void SetWorld(int w) { world = w; }
void Reset() {
    for (auto& [id, c] : list) { DropDriver(c); Destroy(c.bike); }
    list.clear(); riding = boarding = 0; onIt = false; world = 0; heliSeat = heliPending = 0; heliStandUp = false;
    mpui::SetVehiclePrompt(0, "");
}
bool IsServerBike(UObject* bike) { if (!bike) return false; for (auto& [id, c] : list) if (c.bike == bike) return true; return false; }
bool IsModel(UObject* vehicle) { if (!vehicle) return false; for (auto& [id, c] : list) if (c.bike == vehicle) return !c.model.empty(); return false; }
UObject* HelicopterSeat() { auto it = heliSeat ? list.find(heliSeat) : list.end(); return it != list.end() && IsValid(it->second.bike) ? it->second.bike : nullptr; }
bool RidingBehind(UObject* vehicle) {
    const uint16_t id = riding ? riding : boarding;
    auto it = id && behind ? list.find(id) : list.end();
    return it != list.end() && vehicle && it->second.bike == vehicle;
}
bool DrivesModel(uint16_t playerId, UObject*& vehicle) {
    const uint16_t mineBehind = behind ? (riding ? riding : boarding) : 0;
    for (auto& [id, c] : list) if (c.info.driver == playerId && (!c.info.model.empty() || id == mineBehind)) { vehicle = c.following && IsValid(c.bike) ? c.bike : nullptr; return true; }
    vehicle = nullptr; return false;
}
UObject* AnyBike() { for (auto& [id, c] : list) if (IsValid(c.bike) && c.model.empty()) return c.bike; return nullptr; }

void Update(double now, UObject* pawn, UObject* controller, net::Client& client) {
    if (!IsValid(pawn)) return;
    const uint16_t me = client.localId();
    UObject* mounted = Mounted(controller);
    const FVector at = Location(pawn);
    // The bikes: parked ones here for everyone, the ridden one only for its rider.
    for (auto& [id, c] : list) {
        const bool mine = c.info.driver == me || (id == boarding && !behind);
        const bool model = !c.info.model.empty();
        const bool others = c.info.driver != 0xFFFF && !mine;  // someone else drives it
        const bool seated = behind && (id == riding || id == boarding);  // this player rides behind them
        const bool want = c.info.world == world && (!others || model || seated);
        if (c.bike && !IsValid(c.bike)) c.bike = nullptr;
        const bool npc = c.info.npc || !c.route.empty() || c.info.model == "helicopter";  // its NPC drives it (a helicopter: always as told)
        if (c.driver && (!c.bike || !c.info.npc)) DropDriver(c);
        // Not this one any more (the mode changed the model; someone else's model is left by them: it
        // stands again from where the server says).
        if (c.bike && c.bike != mounted && (c.model != c.info.model || (c.following && !others && !npc))) { DropDriver(c); DropBlip(c); Destroy(c.bike); c.following = false; }
        if (!want) { if (c.bike && c.bike != mounted) { DropDriver(c); DropBlip(c); Destroy(c.bike); } continue; }
        if (!c.bike) {
            if (now < c.nextTry || Dist2(at, {c.info.state.x, c.info.state.y, c.info.state.z}) > 40000.f * 40000.f) continue;  // 400 m
            c.nextTry = now + 3;
            c.bike = Spawn(pawn, c.info.state, ModelOf(c.info.model));
            c.model = c.info.model; c.following = false;
            c.placed = c.info.state;
            c.repairHidden = false;
            c.markers.clear(); c.scanned = false; c.marked = -1; c.blip = nullptr; c.repair = nullptr; c.nextAdvert = now + 1; c.advert = -1;
            if (!c.bike) continue;
        }
        if (others) {
            if (!c.following) { Follow(c.bike); c.following = true; }  // moved by mp (DrivesModel)
        } else if (npc) {
            if (!c.following) { Follow(c.bike); c.following = true; }
            Drive(c, Clock());
            if (c.info.npc) { SeatDriver(c); PoseDriver(c); }
            if (!c.repairHidden) {  // no repairing or refuelling it, no icon above it
                c.repairHidden = true;
                if (UObject* r = RepairActor(c.bike)) {
                    if (auto f = Fn(r, "SetActorHiddenInGame")) { Params p(f); p.SetBool("bNewHidden", true); Call(r, "SetActorHiddenInGame", &p); }
                    if (auto f = Fn(r, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(r, "SetActorEnableCollision", &p); }
                    if (auto f = Fn(r, "SetActorTickEnabled")) { Params p(f); p.SetBool("bEnabled", false); Call(r, "SetActorTickEnabled", &p); }
                }
            }
        } else if (c.info.driver == 0xFFFF && c.bike != mounted) {
            // Moved by the server (the mode, or where someone else left it): only then, the bike may
            // settle, tip over or roll where it stands.
            const net::VehicleState& server = c.info.state;
            if (Dist2({c.placed.x, c.placed.y, c.placed.z}, {server.x, server.y, server.z}) > 150.f * 150.f) {
                c.placed = server;
                if (Dist2(Location(c.bike), {server.x, server.y, server.z}) > 150.f * 150.f) { Place(c.bike, server); Park(c.bike); }
            }
        }
        Mark(c);
        Advert(c, now);
    }
    // A server vehicle got on without the server (the game's own E): off again, and ask.
    // (Not at once: getting on and off the game reports the bike now and then for a moment.)
    uint16_t foreign = 0;
    if (mounted && IsServerBike(mounted)) for (auto& [vid, c] : list) if (c.bike == mounted && vid != riding && vid != boarding) foreign = vid;
    if (!foreign) foreignSince = 0;
    else if (!foreignSince) foreignSince = now;
    else if (now - foreignSince > 0.7) { foreignSince = 0; GetOff(pawn); client.SendVehicle(net::Msg::VehicleEnter, foreign); }
    // Allowed: onto it (the mode may have put the player on a bike far away: next to it first).
    if (boarding) {
        auto it = list.find(boarding);
        UObject* bike = it != list.end() ? it->second.bike : nullptr;
        if (now > boardUntil || it == list.end()) { dg::Log("Vehicles: could not get on %u", boarding); boarding = 0; }
        else if (mounted && mounted == bike) {
            riding = boarding; boarding = 0; onIt = true; nextReport = 0; offSince = 0;
            dg::Log("Vehicles: riding %u", riding);
        } else if (IsValid(bike) && now >= nextBoardTry) {
            nextBoardTry = now + 1.5;
            if (boardTries == 0) ApplyState(bike, it->second.info.state);  // its fuel and health
            const FVector b = Location(bike);
            if (Dist2(at, b) > 1500.f * 1500.f) if (auto f = Fn(pawn, "K2_SetActorLocation")) {
                Params p(f); p.Set("NewLocation", FVector{b.X + 150.f, b.Y, b.Z + 100.f}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(pawn, "K2_SetActorLocation", &p);
            }
            // The game's own getting on (with its animation); forced only when that does not happen.
            // (The models and the passenger seat: forced at once, the game does not get the player there itself.)
            const bool force = boardTries >= 2 || !it->second.info.model.empty() || behind;
            if (auto f = Fn(pawn, "TryEnterVehicle")) {
                Params e(f); e.Set("VehiclePawn", bike); e.SetBool("bForce", force); e.Set<float>("MaxDistance", 2000.f);
                if (behind) e.Set("SeatName", FindName("PassengerSeat"));
                Call(pawn, "TryEnterVehicle", &e);
            }
            ++boardTries;
        }
    }
    // Riding: where it is, about every second; got off: where it was left.
    if (riding) {
        auto it = list.find(riding);
        UObject* bike = it != list.end() ? it->second.bike : nullptr;
        const bool on = IsValid(bike) && mounted == bike;
        if (on && !behind && now >= nextReport) { nextReport = now + 1; const auto s = StateOf(bike); client.SendVehicle(net::Msg::VehicleState, riding, &s); }
        if (on) offSince = 0; else if (!offSince) offSince = now;
        if (!on && onIt && now - offSince > 1.5) {
            net::VehicleState s = IsValid(bike) ? StateOf(bike) : it != list.end() ? it->second.info.state : net::VehicleState{};
            client.SendVehicle(net::Msg::VehicleExit, riding, &s);
            if (it != list.end() && !behind) it->second.info.state = s;
            dg::Log("Vehicles: off %u", riding);
            riding = 0; onIt = false;
        }
        if (on) onIt = true;
    }
    // A helicopter seat: into it once its copy is here; out when the server says.
    if (heliPending) {
        auto it = list.find(heliPending);
        if (it == list.end()) heliPending = 0;
        else if (IsValid(it->second.bike)) { SitIn(pawn, controller, it->second.bike, heliSeatIndex); heliSeat = heliPending; heliPending = 0; }
    }
    if (heliStandUp) {
        heliStandUp = false;
        if (heliSeat) { auto it = list.find(heliSeat); StandUp(pawn, controller, it != list.end() ? it->second.bike : nullptr); heliSeat = 0; }
    }
    if (heliSeat && list.find(heliSeat) == list.end()) { StandUp(pawn, controller, nullptr); heliSeat = 0; }
    // A passenger cannot punch from its seat (the game lets it).
    const bool wantMeleeOff = (behind && riding && mounted) || heliSeat;
    if (wantMeleeOff != meleeOff) {
        meleeOff = wantMeleeOff;
        if (auto f = Fn(pawn, "SetCanMeleeAttack")) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), !meleeOff); Call(pawn, "SetCanMeleeAttack", &p); }
        SetBoolProp(pawn, "CanDoMelee", !meleeOff);
    }
    if (!riding && !boarding) behind = false;
    // On foot next to a parked one: "[E] Get on"; next to a ridden one (by a player or its NPC) with a free
    // seat behind: "[E] Ride along" (the key goes to the server, not to the game).
    uint16_t nearest = 0; std::string text;
    if (heliSeat) {  // seated: out while it stands
        auto it = list.find(heliSeat);
        if (it != list.end() && it->second.route.empty()) { nearest = heliSeat; text = lang::Pick("Выйти", "Get out"); }
    } else if (!mounted && !boarding && !heliPending) {
        float best = 1e30f;
        for (auto& [id, c] : list) {
            const bool heli = c.info.model == "helicopter";
            const bool ridden = !heli && ((c.info.driver != 0xFFFF && c.info.driver != me) || c.info.npc);
            if (heli ? !IsValid(c.bike) || !c.route.empty() || c.info.riders >= 4 : ridden ? c.info.passenger != 0xFFFF : !IsValid(c.bike)) continue;
            const float reach = ModelOf(c.info.model).reach;
            const FVector where = IsValid(c.bike) ? Location(c.bike) : FVector{c.info.state.x, c.info.state.y, c.info.state.z};
            const float d = Dist2(at, where);
            if (d >= reach * reach || d >= best) continue;
            best = d; nearest = id;
            text = heli ? lang::Pick("Сесть", "Board") : ridden ? lang::Pick("Сесть сзади", "Ride along") : c.info.locked && !c.info.yours ? lang::Pick("Заперто", "Locked") : lang::Pick("Сесть", "Get on");
        }
    }
    mpui::SetVehiclePrompt(nearest, text);
}

}  // namespace vehicles
