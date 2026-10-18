#include "inventory.h"
#include "vehicles.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <map>

using namespace ue;

namespace inventory {
namespace {
UObject *pawn = nullptr, *controller = nullptr;
UObject* manager = nullptr;               // BP_InventoryItemMgr_C: owns one InventoryItem per key
std::map<std::string, UObject*> items;    // KeyName -> item
std::map<uint8_t, UObject*> ammoById;     // EInventoryAmmoID -> ammo item
Snapshot reported;
double nextSnapshot = 0;
bool bikeTaken = false;                   // the server took the bike: keep it gone (fast travel respawns it)
struct BikeRequest { bool pending = false; net::BikeAction action{}; FVector pos{}; float yaw = 0; } bikeRequest;
UObject* bikeClass = nullptr;             // remembered so the bike can be given back after it was destroyed
// GiveReplace: weapons the new one displaces are dropped by the game as pickups; remove those.
std::map<UObject*, int32_t> pickupsBefore;
double sweepDropsUntil = 0;

UObject* Component() {
    static UObject* cached = nullptr;
    if (IsValid(cached) && cached->Outer == controller) return cached;
    cached = nullptr;
    UObject* cls = FindClass("PlayerInventoryComponent");
    for (int32_t i = 0, n = NumObjects(); i < n && cls && IsValid(controller); ++i) {
        UObject* o = ObjectAt(i);
        if (o && o->Outer == controller && IsValid(o) && IsA(o, cls)) { cached = o; break; }
    }
    return cached;
}

bool IndexItems() {
    if (IsValid(manager) && !items.empty()) return true;
    manager = FindRuntimeInstance("BP_InventoryItemMgr_C");
    items.clear(); ammoById.clear();
    if (!IsValid(manager)) return false;
    UObject* itemCls = FindClass("InventoryItem");
    UObject* ammoCls = FindClass("InventoryItemAmmo");
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o->Outer != manager || !IsValid(o) || !IsA(o, itemCls)) continue;
        const auto key = NameOf(Field<FName>(o, 0x28));  // InventoryItem.KeyName
        items[key] = o;
        if (ammoCls && IsA(o, ammoCls)) if (auto f = Fn(o, "GetAmmoID")) { Params p(f); Call(o, "GetAmmoID", &p); ammoById[p.Get<uint8_t>("ReturnValue")] = o; }
    }
    dg::Log("Inventory: %zu item definitions, %zu ammo types", items.size(), ammoById.size());
    return !items.empty();
}

UObject* Item(const std::string& key) {
    if (!IndexItems()) return nullptr;
    auto it = items.find(key);
    if (it == items.end()) {  // case-insensitive fallback for script authors
        for (auto& [k, v] : items) if (_stricmp(k.c_str(), key.c_str()) == 0) return v;
        return nullptr;
    }
    return it->second;
}

int Count(UObject* inv, UObject* item) {
    auto f = Fn(inv, "GetItemCount"); if (!f) return 0;
    Params p(f); p.Set("Item", item); Call(inv, "GetItemCount", &p); return p.Get<int32_t>("ReturnValue");
}
int Change(UObject* inv, UObject* item, int amount) {
    if (amount == 0) return 0;
    const char* fn = amount > 0 ? "AddItem" : "RemoveItem";
    auto f = Fn(inv, fn); if (!f) return 0;
    Params p(f); p.Set("Item", item); p.Set<int32_t>("ItemCount", std::abs(amount)); Call(inv, fn, &p);
    return p.Get<int32_t>("ReturnValue");
}

// Story-style disarm: the only call that removes the built-in knife / rock / taser. It stashes the
// whole inventory, so everything except the chosen tools is put straight back.
void StripAll(UObject* inv, uint8_t tools = net::ToolKnife | net::ToolRock | net::ToolTaser) {
    auto f = Fn(inv, "StoreAndRemoveAllInventory"); if (!f) { Call(inv, "RemoveWeapons"); return; }
    Params p(f); p.SetBool("RemoveRock", tools & net::ToolRock); p.SetBool("RemoveKnife", tools & net::ToolKnife); p.SetBool("RemoveTaser", tools & net::ToolTaser);
    Call(inv, "StoreAndRemoveAllInventory", &p);
}
void StripTools(UObject* inv, uint8_t tools) {
    if (!tools) return;
    Snapshot keep;
    // The removed tools are inventory items too (kWeaponID_SmallRock, kAmmoID_Rock ...): don't put those back.
    auto removed = [tools](const std::string& k) {
        return ((tools & net::ToolRock) && k.find("Rock") != std::string::npos) || ((tools & net::ToolKnife) && k.find("Knife") != std::string::npos)
            || ((tools & net::ToolTaser) && k.find("Taser") != std::string::npos && k.rfind("kRecipeID_", 0) != 0);
    };
    if (IndexItems()) for (auto& [k, item] : items) if (const int c = Count(inv, item); c > 0 && !removed(k)) keep.emplace_back(k, c);
    StripAll(inv, tools);
    for (auto& [k, c] : keep) if (UObject* item = Item(k); item && Count(inv, item) < c) Change(inv, item, c - Count(inv, item));
}

UObject* AssignedBike() {
    if (!IsValid(controller)) return nullptr;
    auto f = Fn(controller, "GetAssignedVehicle"); if (!f) return nullptr;
    Params p(f); Call(controller, "GetAssignedVehicle", &p);
    UObject* bike = p.Get<UObject*>("ReturnValue");
    if (IsValid(bike)) bikeClass = bike->Class;
    // A server vehicle the game assigned on getting on it is not the player's own bike.
    return IsValid(bike) && !vehicles::IsServerBike(bike) ? bike : nullptr;
}
bool InVehicle() {
    auto f = IsValid(pawn) ? Fn(pawn, "IsInVehicle") : nullptr; if (!f) return false;
    Params p(f); Call(pawn, "IsInVehicle", &p); return p.GetBool("ReturnValue");
}

void TakeBike() {
    UObject* bike = AssignedBike();
    if (!bike) return;
    if (InVehicle()) {
        // Off the seat first; the bike is destroyed on a later frame once the rider is clear.
        if (auto f = Fn(pawn, "TryExitVehicle")) { Params p(f); p.SetBool("bForce", true); Call(pawn, "TryExitVehicle", &p); }
        return;
    }
    Call(bike, "K2_DestroyActor");
    dg::Log("Inventory: bike taken");
}

void GiveBike(const FVector& at, float yaw) {
    if (AssignedBike()) return;
    if (!bikeClass) bikeClass = FindClass("BP_bike04Drifter_C");
    auto f = IsValid(controller) && bikeClass ? Fn(controller, "SpawnFirstPlayerVehicle") : nullptr;
    if (!f) { dg::Log("Inventory: cannot give a bike (class %p)", bikeClass); return; }
    FVector pos = at;
    if (pos.X == 0 && pos.Y == 0 && pos.Z == 0) {
        // Next to the player, to the right of where they look.
        UObject* root = IsValid(pawn) ? Field<UObject*>(pawn, off::Actor_RootComponent) : nullptr;
        if (!IsValid(root)) return;
        const FVector p = Field<FVector>(root, off::Scene_C2W_Trans);
        const float r = yaw * 0.0174532925f;
        pos = {p.X - std::sin(r) * 300.f, p.Y + std::cos(r) * 300.f, p.Z};
    }
    FTransform t; t.Translation = pos; t.Rotation = RotatorToQuat({0, yaw, 0});
    Params p(f); p.Set("Class", bikeClass); p.Set("SpawnTransform", t); p.SetBool("SpawnEvenIfColliding", true);
    Call(controller, "SpawnFirstPlayerVehicle", &p);
    // The call returns null even when it works; the new bike shows up as the assigned vehicle.
    UObject* bike = AssignedBike();
    dg::Log("Inventory: bike given %s at %.0f %.0f %.0f", bike ? GetName(bike).c_str() : "(failed)", pos.X, pos.Y, pos.Z);
}
}  // namespace

FVector PlayerPos() {
    UObject* root = IsValid(pawn) ? Field<UObject*>(pawn, off::Actor_RootComponent) : nullptr;
    return IsValid(root) ? Field<FVector>(root, off::Scene_C2W_Trans) : FVector{};
}
template <typename F> void ForNearbyPickups(float radius, F&& fn) {
    UObject* cls = FindClass("BendInventoryPickup"); if (!cls) return;
    const FVector p = PlayerPos();
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || (o->ObjectFlags & 0x10) || !IsA(o, cls) || !IsValid(o)) continue;
        UObject* root = Field<UObject*>(o, off::Actor_RootComponent); if (!IsValid(root)) continue;
        const FVector q = Field<FVector>(root, off::Scene_C2W_Trans);
        const float dx = q.X - p.X, dy = q.Y - p.Y, dz = q.Z - p.Z;
        if (dx * dx + dy * dy + dz * dz <= radius * radius) fn(o);
    }
}
void SweepDroppedWeapons(double now) {
    if (now > sweepDropsUntil) return;
    std::vector<UObject*> drop;
    ForNearbyPickups(1500.f, [&](UObject* o) {
        auto it = pickupsBefore.find(o);
        const bool isNew = it == pickupsBefore.end() || it->second != SerialAt(o->InternalIndex);
        if (isNew && Field<uint8_t>(o, 0x3D0) == 4) drop.push_back(o);  // InventoryItemBaseType == kBaseType_Weapon
    });
    for (auto o : drop) { dg::Log("Inventory: replaced weapon removed (%s)", GetName(o).c_str()); Call(o, "K2_DestroyActor"); }
}

// Orders that arrive before the player has an inventory (joining while the world loads) wait here.
struct Order { net::ItemAction action; std::string key; int amount; };
std::vector<Order> waiting;
void Apply(net::ItemAction action, const std::string& key, int amount) {
    UObject* inv = Component();
    if (!inv) {
        if (waiting.size() < 256) waiting.push_back({action, key, amount});
        dg::Log("Inventory: no inventory component yet, %s waits", key.empty() ? "order" : key.c_str());
        return;
    }
    switch (action) {
    case net::ItemAction::Give: case net::ItemAction::Take: case net::ItemAction::GiveReplace: {
        UObject* item = Item(key);
        if (!item) { dg::Log("Inventory: unknown item %s", key.c_str()); return; }
        if (action == net::ItemAction::GiveReplace) {
            pickupsBefore.clear();
            ForNearbyPickups(1500.f, [](UObject* o) { pickupsBefore[o] = SerialAt(o->InternalIndex); });
            sweepDropsUntil = GetTickCount64() / 1000.0 + 2.0;
        }
        const int done = Change(inv, item, action == net::ItemAction::Take ? -amount : amount);
        dg::Log("Inventory: %s %s x%d -> %d (now %d)", action == net::ItemAction::Take ? "take" : "give", key.c_str(), amount, done, Count(inv, item));
        break;
    }
    case net::ItemAction::SetAmmo: {
        // Accepts an ammo key or a weapon key (sets that weapon's ammo type).
        UObject* item = Item(key);
        if (item && IsAByName(item, "InventoryWeaponItem")) { auto it = ammoById.find(Field<uint8_t>(item, 0x131)); item = it != ammoById.end() ? it->second : nullptr; }
        if (!item) { dg::Log("Inventory: unknown ammo %s", key.c_str()); return; }
        Change(inv, item, amount - Count(inv, item));
        break;
    }
    case net::ItemAction::TakeWeapons:
        Call(inv, "RemoveWeapons");
        StripTools(inv, uint8_t(amount));
        dg::Log("Inventory: weapons taken (tools mask %d)", amount);
        break;
    case net::ItemAction::TakeTools:
        StripTools(inv, uint8_t(amount));
        dg::Log("Inventory: tools taken (mask %d)", amount);
        break;
    case net::ItemAction::Clear: {
        StripAll(inv);
        if (IndexItems()) for (auto& [k, item] : items) if (const int c = Count(inv, item); c > 0) Change(inv, item, -c);
        dg::Log("Inventory: cleared");
        break;
    }
    }
    nextSnapshot = 0;  // report the result promptly
}

void RequestBike(net::BikeAction action, const FVector& pos, float yaw) { bikeRequest = {true, action, pos, yaw}; }

// ---------------------------------------------------------------- getting on the bike, freezing
double mountUntil = 0, nextMountTry = 0;
bool frozen = false, frozenApplied = false, frozenOnBike = false;
UObject* MountedVehicle() {
    auto f = IsValid(controller) ? Fn(controller, "GetMountedVehicle") : nullptr; if (!f) return nullptr;
    Params p(f); Call(controller, "GetMountedVehicle", &p);
    UObject* v = p.Get<UObject*>("ReturnValue");
    return IsValid(v) ? v : nullptr;
}
// The player's bike to the spot (the existing one moved, else a new one); then onto it (UpdateMount).
void MountBike(const FVector& at, float yaw) {
    UObject* bike = AssignedBike();
    if (bike) {  // a rider on it goes along
        if (auto f = Fn(bike, "K2_SetActorLocationAndRotation")) {
            Params p(f); p.Set("NewLocation", FVector{at.X, at.Y, at.Z + 60.f}); p.Set("NewRotation", FRotator{0, yaw, 0}); p.SetBool("bTeleport", true);
            Call(bike, "K2_SetActorLocationAndRotation", &p);
        }
        dg::Log("Inventory: bike moved to %.0f %.0f %.0f", at.X, at.Y, at.Z);
    } else GiveBike(at, yaw);
    mountUntil = GetTickCount64() / 1000.0 + 30; nextMountTry = 0;
}
void UpdateMount(double now) {
    if (!mountUntil) return;
    if (now > mountUntil) { mountUntil = 0; dg::Log("Inventory: could not get on the bike"); return; }
    UObject* bike = AssignedBike();
    if (!bike || now < nextMountTry) return;
    if (MountedVehicle() == bike) { mountUntil = 0; dg::Log("Inventory: on the bike"); return; }
    UObject* a = Field<UObject*>(bike, off::Actor_RootComponent), *b = Field<UObject*>(pawn, off::Actor_RootComponent);
    if (!IsValid(a) || !IsValid(b)) return;
    const FVector p = Field<FVector>(a, off::Scene_C2W_Trans), q = Field<FVector>(b, off::Scene_C2W_Trans);
    if ((p.X - q.X) * (p.X - q.X) + (p.Y - q.Y) * (p.Y - q.Y) + (p.Z - q.Z) * (p.Z - q.Z) > 1500.f * 1500.f) return;  // still travelling there
    nextMountTry = now + 1.0;
    if (auto f = Fn(pawn, "TryEnterVehicle")) {
        Params e(f); e.Set("VehiclePawn", bike); e.SetBool("bForce", true); e.Set<float>("MaxDistance", 2000.f);
        Call(pawn, "TryEnterVehicle", &e);
    }
}
void SetFrozen(bool on) { frozen = on; }
void UpdateFreeze() {
    UObject* ride = nullptr;
    for (auto c = pawn->Class; c && !ride; c = SuperOf(c)) if (auto prop = FindProperty(c, "VehicleRideComponent")) ride = Field<UObject*>(pawn, Field<int>(prop, off::UProperty_Offset));
    auto canExit = [&](bool v) { if (auto f = IsValid(ride) ? Fn(ride, "SetCanExitVehicle") : nullptr) { Params p(f); p.SetBool("bCanExit", v); Call(ride, "SetCanExitVehicle", &p); } };
    auto ignoreMove = [&](bool v) { if (auto f = Fn(controller, "SetIgnoreMoveInput")) { Params p(f); p.SetBool("bNewMoveInput", v); Call(controller, "SetIgnoreMoveInput", &p); } };
    const bool onBike = MountedVehicle() != nullptr;
    if (frozen && (!frozenApplied || onBike != frozenOnBike)) {
        if (!frozenApplied) ignoreMove(true);  // a counter in the engine: once per freeze
        if (onBike) { Call(controller, "SuppressThrottleBrake"); canExit(false); }
        frozenApplied = true; frozenOnBike = onBike;
        dg::Log("Frozen%s", onBike ? " on the bike" : "");
    } else if (!frozen && frozenApplied) {
        ignoreMove(false);
        Call(controller, "UnSuppressThrottleBrake"); canExit(true);
        frozenApplied = false;
        dg::Log("Unfrozen");
    }
}

void Update(UObject* p, UObject* c) {
    pawn = p; controller = c;
    if (!IsValid(pawn) || !IsValid(controller)) return;
    if (!waiting.empty() && Component()) {
        auto orders = std::move(waiting); waiting.clear();
        for (auto& o : orders) Apply(o.action, o.key, o.amount);
    }
    if (bikeRequest.pending) {
        bikeTaken = bikeRequest.action == net::BikeAction::Take;
        if (bikeTaken) TakeBike();
        else if (bikeRequest.action == net::BikeAction::Mount) MountBike(bikeRequest.pos, bikeRequest.yaw);
        else GiveBike(bikeRequest.pos, bikeRequest.yaw);
        bikeRequest.pending = false;
        nextSnapshot = 0;
    }
    static double nextEnforce = 0;
    const double now = GetTickCount64() / 1000.0;
    if (bikeTaken && now >= nextEnforce) { nextEnforce = now + 0.5; TakeBike(); }
    UpdateMount(now);
    UpdateFreeze();
    SweepDroppedWeapons(now);
}

bool OwnsBike() { return AssignedBike() != nullptr; }

bool TakeSnapshot(Snapshot& out, double now) {
    if (now < nextSnapshot) return false;
    nextSnapshot = now + 2.0;
    UObject* inv = Component();
    if (!inv || !IndexItems()) return false;
    Snapshot snap;
    for (auto& [key, item] : items) if (const int c = Count(inv, item); c > 0) snap.emplace_back(key, c);
    if (snap == reported) return false;
    reported = snap; out = std::move(snap);
    return true;
}

UObject* FindItem(const std::string& key) { return Item(key); }
std::vector<std::pair<std::string, UObject*>> AllItems() {
    std::vector<std::pair<std::string, UObject*>> out;
    if (IndexItems()) for (auto& [k, v] : items) out.emplace_back(k, v);
    return out;
}

void Reset() { manager = nullptr; items.clear(); ammoById.clear(); reported.clear(); nextSnapshot = 0; }

}  // namespace inventory
