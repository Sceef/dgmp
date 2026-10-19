#include "gameevents.h"
#include "lang.h"
#include "policy.h"
#include "inventory.h"
#include "hud.h"
#include "carry.h"
#include <windows.h>
#include "log.h"
#include <cmath>
#include <map>
#include <set>

using namespace ue;

namespace gameevents {
namespace {
// Classes whose instances are hooked (with subclasses). Their events, as seen in the game:
//   BendInventoryPickup::OnPickupAccepted              item taken (carried objects like gas cans: Carry)
//   BP_Interact_Search*::CompleteJimmy                 car trunk / hood / ambulance forced open
//   Bend_SearchBody_*::Complete                        body searched
//   BendBasicInteract::SetInteractionFinished          any other interact done (signs, collectibles...)
//   BP_Door_C::*OpenFinished__EventFunc                door opened or kicked open
//   Explosive_C::ExplodeDelayed                        barrel / propane tank exploded
//   Bend_ThrownProj_C::OnExploded                      thrown grenade, pipe bomb... exploded
//   gas can pickups: ExplodePickupDelayed (shot, also after being thrown), CreateFireDelayed (its fire)
// Fire has no event: Molotov fire areas (Bend_Molotov_Impact_*) are spotted as they appear.
const char* kHooked[] = {"BendInventoryPickup", "BendBasicInteract", "BendDoor", "Explosive_C", "Bend_ThrownProj_C"};  // thrown only: bullets are projectiles too
// Items carried in both hands instead of going into the backpack.
const char* kCarried[] = {"kWeaponID_Gas_Can", "kWeaponID_Oxygen_Tank", "kWeaponID_CoolerNBE", "kWeaponID_CoolerNFS", "kWeaponID_MicrocentrifugeRFS4", "kWeaponID_DynamiteCrateMBU3", "kWeaponID_Lantern"};
FVector lastBlast{}; double lastBlastAt = -1;  // one explosion can be reported by the barrel and the projectile
std::vector<UObject*> classes;
int cursor = 0;
std::set<UObject*> fires;            // fire actors already reported
std::vector<Event> pending;
struct Placed { std::string key; int count = 1; FVector pos{}; float yaw = 0; UObject* actor = nullptr; bool taken = false, inRange = false; };
std::map<uint16_t, Placed> placed;  // server world items

// classPath: a pickup class to spawn instead of the generic one (loaded when needed); interact: the
// game's own interaction stays on (the player takes it the native way).
UObject* SpawnPickup(const Placed& p, UObject* context, const char* classPath = nullptr, bool interact = false) {
    UObject* item = inventory::FindItem(p.key);
    static UObject* statics = nullptr;
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    UObject* cls = nullptr;
    if (classPath) { cls = FindObject(classPath); if (!cls) cls = LoadAsset(classPath); }
    else cls = FindClass(p.key == "kWeaponID_Gas_Can" ? "BP_BendInventoryPickup_GasCan_C" : "BP_BendInventoryPickup_C");
    if (!item || !statics || !cls) return nullptr;
    FTransform t; t.Translation = p.pos; t.Rotation = FQuat{0, 0, std::sin(p.yaw * 0.00872664626f), std::cos(p.yaw * 0.00872664626f)};
    Params begin(Fn(statics, "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(statics, "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    // What a placed world pickup holds: BendInventoryItem {vtable, KeyHash}, the base type and count.
    auto field = [&](const char* name) -> uint8_t* {
        for (auto c = actor->Class; c; c = SuperOf(c)) if (auto prop = FindProperty(c, name)) return reinterpret_cast<uint8_t*>(actor) + Field<int>(prop, off::UProperty_Offset);
        return nullptr;
    };
    if (auto f = field("PickupItem")) *reinterpret_cast<int32_t*>(f + 8) = Field<int32_t>(item, 0x30);  // InventoryItem.KeyHash
    if (auto f = field("InventoryItemBaseType")) *f = Field<uint8_t>(item, 0xA0);                       // InventoryItem.BaseType
    if (auto f = field("ItemCount")) *reinterpret_cast<int32_t*>(f) = p.count;
    Params finish(Fn(statics, "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(statics, "FinishSpawningActor", &finish);
    // The mesh is set at begin play; its materials come with the item's assets.
    if (Fn(actor, "UpdateInventoryItem")) Call(actor, "UpdateInventoryItem");
    if (Fn(actor, "BeginAsyncItemAssetCache")) Call(actor, "BeginAsyncItemAssetCache");
    if (auto f = Fn(actor, "SnapToGround")) { Params snap(f); snap.Set("ActorToIgnore", context); Call(actor, "SnapToGround", &snap); }
    // Quest items have their own pickup (E, works with a full backpack): the game's interaction is off.
    if (auto f = Fn(actor, "EnableDisableInteract"); f && !interact) { Params e(f); e.SetBool("enable", false); e.SetBool("HideItem", false); Call(actor, "EnableDisableInteract", &e); }
    policy::Observe(actor);
    dg::Log("World item spawned: %s x%d at %.0f %.0f %.0f", p.key.c_str(), p.count, p.pos.X, p.pos.Y, p.pos.Z);
    return actor;
}
std::map<std::pair<UObject*, int>, double> lastSent;  // per object and type: the game repeats some events

FVector Location(UObject* actor) {
    auto f = Fn(actor, "K2_GetActorLocation"); if (!f) return {};
    Params p(f); Call(actor, "K2_GetActorLocation", &p); return p.Get<FVector>("ReturnValue");
}
std::string ItemKey(UObject* pickup) {
    auto f = Fn(pickup, "GetInventoryItem"); if (!f) return {};
    Params p(f); Call(pickup, "GetInventoryItem", &p);
    UObject* item = p.Get<UObject*>("ReturnValue");
    return IsValid(item) ? NameOf(Field<FName>(item, 0x28)) : std::string();  // InventoryItem.KeyName
}
bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void Push(UObject* object, net::GameEventType type, std::string detail = {}) {
    const double now = GetTickCount64() / 1000.0;
    auto& last = lastSent[{object, int(type)}];
    if (now - last < 2.0) return;
    last = now;
    Event e{type, GetName(object->Class), std::move(detail), GetName(object), Location(object)};
    for (auto& [id, p] : placed) if (p.actor == object) { e.id = "dgmp_item_" + std::to_string(id); if (type == net::GameEventType::Pickup || type == net::GameEventType::Carry) p.taken = true; }
    dg::Log("Game event %d: %s %s %s", int(type), e.cls.c_str(), e.id.c_str(), e.detail.c_str());
    pending.push_back(std::move(e));
}

void Observe(UObject* object, const std::string& name) {
    if (!IsValid(object)) return;
    const std::string cls = GetName(object->Class);
    carry::OnPawnEvent(object, name);
    if (carry::IsBoxActor(object)) return;  // the server's boxes report themselves (carryPick / carryDrop)
    if (name == "OnPickupAccepted") {
        auto key = ItemKey(object);
        bool carried = false; for (const char* k : kCarried) carried |= key == k;
        Push(object, carried ? net::GameEventType::Carry : net::GameEventType::Pickup, std::move(key));
    }
    else if (name == "CompleteJimmy") Push(object, net::GameEventType::Search);
    else if (name == "Complete" && Contains(cls, "SearchBody")) Push(object, net::GameEventType::Body);
    else if (name == "SetInteractionFinished") Push(object, net::GameEventType::Interact);
    else if (Contains(name, "OpenFinished__EventFunc") && Contains(cls, "Door")) Push(object, net::GameEventType::Door);
    else if (name == "CreateFireDelayed") Push(object, net::GameEventType::Fire);  // exploding gas can sets fire
    else if (name == "ExplodeDelayed" || name == "OnExploded" || name == "ExplodePickupDelayed") {
        const FVector at = Location(object);
        const double now = GetTickCount64() / 1000.0;
        const float dx = at.X - lastBlast.X, dy = at.Y - lastBlast.Y, dz = at.Z - lastBlast.Z;
        if (now - lastBlastAt < 1.5 && dx * dx + dy * dy + dz * dz < 500.f * 500.f) return;
        lastBlast = at; lastBlastAt = now;
        Push(object, net::GameEventType::Explosion);
    }
}
}  // namespace

void Update(UObject* pawn, double, std::vector<Event>& out) {
    if (classes.empty()) {
        for (const char* c : kHooked) if (auto cls = FindClass(c)) classes.push_back(cls);
        if (classes.size() != std::size(kHooked)) { classes.clear(); return; }  // not loaded yet
        policy::SetObserver(&Observe);
    }
    // Objects stream in all the time: walk the object table in slices (a full pass takes ~1 s).
    static std::vector<UObject*> fireClasses;
    if (fireClasses.empty()) for (const char* c : {"Bend_Molotov_Impact_C", "Bend_Molotov_Impact_Small_C"}) if (auto cls = FindClass(c)) fireClasses.push_back(cls);
    const int n = NumObjects();
    if (cursor >= n) {
        cursor = 0;
        for (auto it = fires.begin(); it != fires.end();) it = IsValid(*it) ? std::next(it) : fires.erase(it);
        if (lastSent.size() > 4096) lastSent.clear();
    }
    for (const int end = std::min(n, cursor + 8192); cursor < end; ++cursor) {
        UObject* o = ObjectAt(cursor);
        if (!o || (o->ObjectFlags & 0x10) || !IsValid(o)) continue;  // default objects
        bool hooked = false;
        for (auto cls : classes) if (IsA(o, cls)) { policy::Observe(o); hooked = true; break; }
        if (hooked || !IsValid(pawn)) continue;
        for (auto cls : fireClasses) if (IsA(o, cls)) { if (fires.insert(o).second) Push(o, net::GameEventType::Fire); break; }
    }
    // Server world items appear when the player is within 200 m (the ground there is loaded).
    if (IsValid(pawn)) {
        const FVector here = Location(pawn);
        static bool eWas = false;
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool eDown = pid == GetCurrentProcessId() && !hud::ChatOpen() && (GetAsyncKeyState('E') & 0x8000);
        bool takePressed = eDown && !eWas;
        eWas = eDown;
        for (auto& [id, p] : placed) {
            if (!p.taken && p.actor && IsValid(p.actor)) {
                const FVector at = Location(p.actor);
                const float ax = at.X - here.X, ay = at.Y - here.Y, az = at.Z - here.Z;
                const bool inRange = ax * ax + ay * ay + az * az < 220.f * 220.f;
                if (inRange && !p.inRange) {
                    UObject* controller = nullptr;
                    for (auto c = pawn->Class; c && !controller; c = SuperOf(c)) if (auto prop = FindProperty(c, "Controller")) controller = Field<UObject*>(pawn, Field<int>(prop, off::UProperty_Offset));
                    if (!policy::ShowNativeHint(controller, L"", lang::Pick(L"Нажмите E, чтобы взять",L"Press E to take"), "", false, 3)) hud::ShowHint(L"", lang::Pick(L"[E] Взять",L"[E] Take"), 3.0);
                }
                p.inRange = inRange;
                if (inRange && takePressed) {
                    Push(p.actor, net::GameEventType::Pickup, p.key);  // marks it taken
                    inventory::Apply(net::ItemAction::Give, p.key, p.count);  // added when there is room
                    Call(p.actor, "K2_DestroyActor"); p.actor = nullptr;
                    takePressed = false;
                }
                continue;
            }
            if (p.taken || (p.actor && IsValid(p.actor))) continue;
            const float dx = p.pos.X - here.X, dy = p.pos.Y - here.Y;
            if (dx * dx + dy * dy < 20000.f * 20000.f) p.actor = SpawnPickup(p, pawn);
        }
    }
    out.insert(out.end(), std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
    pending.clear();
}

void Reset() { cursor = 0; fires.clear(); lastSent.clear(); pending.clear(); for (auto& [id, p] : placed) p.actor = nullptr; }

UObject* SpawnPreviewItem(const std::string& key, const FVector& pos, float yaw, UObject* context) {
    Placed p; p.key = key; p.pos = pos; p.yaw = yaw;
    return SpawnPickup(p, context);
}

UObject* SpawnCarryPickup(const std::string& key, const FVector& pos, float yaw, UObject* context, const char* classPath) {
    Placed p; p.key = key; p.pos = pos; p.yaw = yaw;
    return SpawnPickup(p, context, classPath, false);
}

UObject* SpawnResearchPickup(const std::string& key, int count, const FVector& pos, float yaw, UObject* context, const char* classPath) {
    Placed p; p.key = key; p.count = count; p.pos = pos; p.yaw = yaw;
    return SpawnPickup(p, context, classPath, true);
}

void SetWorldItem(uint16_t id, const std::string* key, int count, const FVector& pos, float yaw) {
    auto it = placed.find(id);
    if (it != placed.end() && IsValid(it->second.actor)) Call(it->second.actor, "K2_DestroyActor");
    if (!key) { if (it != placed.end()) placed.erase(it); return; }
    placed[id] = Placed{*key, count, pos, yaw};
}
}  // namespace gameevents
