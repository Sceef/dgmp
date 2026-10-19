#include "carry.h"
#include "gameevents.h"
#include "mpui.h"
#include "lang.h"
#include "policy.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace ue;

namespace carry {
namespace {
// What a model is in the game: the item, the pickup that lies on the ground, the weapon held in the
// hands, the pickup the game itself leaves when it drops it, and whether the game drops it in a dodge roll
// (the coolers do, the crate stays in the hands). (The server's list: Carry::KnownModel.)
struct Model { const char* name; const char* ru; const char* en; const char* item; const char* pickup; const char* weapon; const char* dropped; bool rollDrops; };
const Model kModels[] = {
    {"crate", "Ящик динамита", "Dynamite crate", "kWeaponID_DynamiteCrateMBU3",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/BP_BendInventoryPickup_Dynamite_Crate.BP_BendInventoryPickup_Dynamite_Crate_C",
     "Bend_WpnMelee_Crate_MBU3_C",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/BP_BendInventoryPickup_Dynamite_Crate_SpawnedIn.BP_BendInventoryPickup_Dynamite_Crate_SpawnedIn_C", false},
    // The hand cooler (NBE).
    {"cooler", "Переносной холодильник", "Hand cooler", "kWeaponID_CoolerNBE",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/DoNotPopulate/BP_BendInventoryPickup_NBECoolerSpawnedIn.BP_BendInventoryPickup_NBECoolerSpawnedIn_C",
     "Bend_WpnMelee_Cooler_NBE_C",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/DoNotPopulate/BP_BendInventoryPickup_NBECoolerSpawnedIn.BP_BendInventoryPickup_NBECoolerSpawnedIn_C", true},
    // The antibiotics case (NFS "cooler").
    {"medcase", "Кейс с антибиотиками", "Antibiotics case", "kWeaponID_CoolerNFS",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/BP_BendInventoryPickup_NFSCoolerStatic.BP_BendInventoryPickup_NFSCoolerStatic_C",
     "Bend_WpnMelee_Cooler_NFS_C",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/DoNotPopulate/BP_BendInventoryPickup_NFSCoolerSpawnedIn.BP_BendInventoryPickup_NFSCoolerSpawnedIn_C", true},
    // The microcentrifuge (RFS4): no pickup of its own, the base one takes the item's mesh; dropped, the game
    // leaves an NBE cooler pickup with that mesh.
    {"centrifuge", "Микроцентрифуга", "Microcentrifuge", "kWeaponID_MicrocentrifugeRFS4",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/DoNotPopulate/BP_CarryWeaponPickupBase.BP_CarryWeaponPickupBase_C",
     "Bend_WpnMelee_Microcentrifuge_RFS4_C",
     "/Game/Libraries/Blueprints/Interacts/InventoryPickup/DoNotPopulate/BP_BendInventoryPickup_NBECoolerSpawnedIn.BP_BendInventoryPickup_NBECoolerSpawnedIn_C", true},
};
const Model* ModelOf(const std::string& name) {
    for (auto& m : kModels) if (name == m.name) return &m;
    return nullptr;
}

constexpr float kReach = 200.f;          // 2 m: close enough to pick one up
constexpr float kShowRange = 20000.f;    // 200 m: boxes are placed when the player is this close
constexpr float kHideRange = 30000.f;    // and taken away beyond this
constexpr double kHoldTime = 0.5;        // seconds F is held to pick up / set down

struct Box { net::Client::Events::CarryInfo info; UObject* actor = nullptr; };
std::map<uint16_t, Box> boxes;
int world = 0;
uint16_t me = 0xFFFF;

// The box in this player's hands, as far as this client knows.
enum class Hands { Empty, Picking, Holding, Releasing, Lost };
Hands hands = Hands::Empty;
uint16_t heldId = 0;              // the box the server put into the hands (Picking, Holding) or took away (Releasing, Lost)
const Model* heldModel = nullptr;
double handsSince = 0;
UObject* weapon = nullptr;        // the game's carried item while holding
double requestedAt = -10;         // a pick / put request is on its way (no new one, no prompt for a moment)
double holdStart = -1;            // F held since
bool restricted = false;          // punching is off
bool rolled = false;              // the player started a dodge roll (OnPawnEvent)
UObject* observedPawn = nullptr;  // the pawn whose events come to OnPawnEvent
net::CarryOp lostOp = net::CarryOp::Lost;  // how the box left the hands when the game dropped it (Lost, or Roll)
uint16_t brokenId = 0; double brokenAt = -10;  // a box broken by a roll: not picked up again until the server removes it

FVector Location(UObject* actor) {
    auto f = Fn(actor, "K2_GetActorLocation"); if (!f) return {};
    Params p(f); Call(actor, "K2_GetActorLocation", &p); return p.Get<FVector>("ReturnValue");
}
float Yaw(UObject* actor) {
    auto f = Fn(actor, "K2_GetActorRotation"); if (!f) return 0;
    Params p(f); Call(actor, "K2_GetActorRotation", &p); return p.Get<FRotator>("ReturnValue").Yaw;
}
float Dist2(const FVector& a, float x, float y) { const float dx = a.X - x, dy = a.Y - y; return dx * dx + dy * dy; }
void Destroy(UObject*& actor) { if (actor && IsValid(actor)) Call(actor, "K2_DestroyActor"); actor = nullptr; }

struct RawList { UObject** data; int num, max; };
// The model's carried item in the pawn's hands, or null.
UObject* WeaponInHands(UObject* pawn, const Model* m) {
    auto f = m && IsValid(pawn) ? Fn(pawn, "GetAttachedActors") : nullptr;
    if (!f) return nullptr;
    Params p(f); Call(pawn, "GetAttachedActors", &p);
    const RawList list = p.Get<RawList>("OutActors");
    for (int i = 0; i < list.num; ++i) {
        UObject* a = list.data[i];
        if (IsValid(a) && GetName(a->Class) == m->weapon) return a;  // (hidden too: the game hides it for a moment on ladders...)
    }
    return nullptr;
}
// Pickups the game left itself when it dropped the model's item (none of ours), the nearest to `around`
// within `range`, or null.
UObject* DroppedByGame(UObject* context, const Model* m, const FVector& around, float range) {
    static UObject* statics = nullptr;
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    UObject* cls = FindObject(m->dropped);
    if (!cls) cls = LoadAsset(m->dropped);
    auto f = statics && cls ? Fn(statics, "GetAllActorsOfClass") : nullptr;
    if (!f) return nullptr;
    Params p(f); p.Set("WorldContextObject", context); p.Set("ActorClass", cls);
    Call(statics, "GetAllActorsOfClass", &p);
    const RawList list = p.Get<RawList>("OutActors");
    UObject* best = nullptr; float bestD = range * range;
    for (int i = 0; i < list.num; ++i) {
        UObject* a = list.data[i];
        if (!IsValid(a) || IsBoxActor(a) || GetBoolProp(a, "bHidden")) continue;
        if (auto off = Fn(a, "EnableDisableInteract")) { Params e(off); e.SetBool("enable", false); e.SetBool("HideItem", false); Call(a, "EnableDisableInteract", &e); }  // no "[R] Take" while it falls
        const FVector at = Location(a);
        const float d = Dist2(at, around.X, around.Y) + (at.Z - around.Z) * (at.Z - around.Z);
        if (d < bestD) { bestD = d; best = a; }
    }
    return best;
}
void CallBool(UObject* o, const char* fn, bool v) {
    if (auto f = Fn(o, fn)) { Params p(f); p.SetBool(GetName(FirstParam(f)).c_str(), v); Call(o, fn, &p); }
}
// The model's pickup lying at `at`. SnapToGround misses some surfaces (roads are merged meshes: it lands
// on the terrain under them, out of sight): one that sank well below the spot goes back up to it (the
// spot is where the player's feet were).
UObject* PlaceBox(const Model* m, const FVector& at, float yaw, UObject* pawn) {
    UObject* a = gameevents::SpawnCarryPickup(m->item, at, yaw, pawn, m->pickup);
    if (!IsValid(a)) return a;
    // Some pickups are physics objects (the hand cooler): kicked by walking into it, it would roll away from
    // where the server has it (each client its own way) and could not be picked up there. It stands still.
    static UObject* primitive = FindClass("PrimitiveComponent");
    if (auto f = primitive ? Fn(a, "GetComponentsByClass") : nullptr) {
        Params q(f); q.Set("ComponentClass", primitive); Call(a, "GetComponentsByClass", &q);
        const RawList comps = q.Get<RawList>("ReturnValue");
        for (int i = 0; i < comps.num; ++i)
            if (UObject* c = comps.data[i]; IsValid(c) && GetBoolProp(c, "bSimulatePhysics")) CallBool(c, "SetSimulatePhysics", false);
    }
    if (Location(a).Z < at.Z - 25.f)
        if (auto f = Fn(a, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", at); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(a, "K2_SetActorLocation", &p); }
    return a;
}
// No punching while carrying (the crate swings like a melee weapon); aiming is blocked in policy. The dodge roll stays: the box's
// `roll` decides what it does to the box.
void Restrict(UObject* pawn, bool on) {
    if (!IsValid(pawn)) { restricted = false; return; }
    CallBool(pawn, "SetCanMeleeAttack", !on);
    SetBoolProp(pawn, "CanDoMelee", !on);
    restricted = on;
}
// The game's weapon HUD offers "[Space] Drop" for some carried items (the antibiotics case) and takes it
// away only after its own drop: hidden while a box is in the hands and after it left them.
void HideDropPrompt() {
    UObject* w = FindRuntimeInstance("UI_HudWeapon_C");
    if (!IsValid(w) || !GetBoolProp(w, "WeaponDropPromptVisible")) return;
    if (auto f = Fn(w, "OnWeaponDropped")) { Params p(f); Call(w, "OnWeaponDropped", &p); }
}
// Some carried items drop themselves: the antibiotics case's DropWeaponComponent throws it out of the
// hands when the window loses focus, and the item's own input (held Space) drops it. Both go while held.
void DisarmDrop(UObject* item, UObject* pawn) {
    if (UObject* controller = IsValid(item) && IsValid(pawn) ? Field<UObject*>(pawn, off::Pawn_Controller) : nullptr; IsValid(controller))
        if (auto f = Fn(item, "DisableInput")) { Params p(f); p.Set("PlayerController", controller); Call(item, "DisableInput", &p); }
    static UObject* dropClass = nullptr;
    if (!IsValid(dropClass)) dropClass = FindClass("DropWeaponComponent_C");
    auto f = IsValid(item) && dropClass ? Fn(item, "GetComponentsByClass") : nullptr;
    if (!f) return;
    Params q(f); q.Set("ComponentClass", dropClass); Call(item, "GetComponentsByClass", &q);
    const RawList comps = q.Get<RawList>("ReturnValue");
    std::vector<UObject*> list(comps.data, comps.data + comps.num);  // the array changes as they go
    for (UObject* c : list)
        if (auto d = IsValid(c) ? Fn(c, "K2_DestroyComponent") : nullptr) { Params p(d); p.Set("Object", c); Call(c, "K2_DestroyComponent", &p); dg::Log("Carry: the item's own drop is off"); }
}
void SetHands(Hands h, double now) { hands = h; handsSince = now; }
// Out of the hands without a trace: the game drops it, and the pickup it leaves goes (the box is the server's).
void LetGo(UObject* pawn, double now) {
    if (IsValid(weapon) && IsValid(pawn))
        if (auto f = Fn(pawn, "DropWeapon")) { Params p(f); p.Set("Weapon", weapon); p.SetBool("BrokenWeaponOnly", false); Call(pawn, "DropWeapon", &p); }
    weapon = nullptr;
    SetHands(Hands::Releasing, now);
}
}  // namespace

const ModelInfo* Models(int& count) {
    static const std::vector<ModelInfo> list = [] { std::vector<ModelInfo> v; for (auto& m : kModels) v.push_back({m.name, m.ru, m.en}); return v; }();
    count = int(list.size());
    return list.data();
}

UObject* SpawnPreview(const std::string& model, const FVector& at, float yaw, UObject* context) {
    const Model* m = ModelOf(model);
    return m ? PlaceBox(m, at, yaw, context) : nullptr;
}

void OnPawnEvent(UObject* object, const std::string& name) {
    if (object == observedPawn && name == "OnDodgeRollStartRingCheck") rolled = true;
}

bool IsBoxActor(UObject* actor) {
    if (!actor) return false;
    for (auto& [id, b] : boxes) if (b.actor == actor) return true;
    return false;
}

void OnCarry(uint16_t id, const net::Client::Events::CarryInfo* info) {
    auto it = boxes.find(id);
    if (!info) {
        if (it != boxes.end()) { Destroy(it->second.actor); boxes.erase(it); }
        return;
    }
    Box& b = boxes[id];
    const bool moved = b.info.carrier != info->carrier || b.info.x != info->x || b.info.y != info->y || b.info.z != info->z || b.info.model != info->model;
    // Picked up by someone else, or put somewhere else: the copy here goes (placed again below if it lies).
    // The one this player gets keeps its copy: the pick-up animation takes it from there.
    if (moved && info->carrier != me) Destroy(b.actor);
    b.info = *info;
}

void SetWorld(int w) { world = w; }

void Reset() {
    for (auto& [id, b] : boxes) Destroy(b.actor);
    boxes.clear();
    weapon = nullptr; heldId = 0; heldModel = nullptr; hands = Hands::Empty; holdStart = -1;
    mpui::SetCarryPrompt("", 0);
    policy::SetCarryInputBlock(false);
    policy::SetCarryRollBlock(false);
}

void Update(double now, UObject* pawn, net::Client& client, bool canAct) {
    me = client.localId();
    if (!IsValid(pawn)) return;
    const FVector here = Location(pawn);
    if (pawn != observedPawn) { policy::Observe(pawn); observedPawn = pawn; rolled = false; }  // its dodge rolls (OnPawnEvent)

    // ---- the hands: what the server says against what the game shows
    auto mine = std::find_if(boxes.begin(), boxes.end(), [](auto& kv) { return kv.second.info.carrier == me; });
    const uint16_t want = mine != boxes.end() ? mine->first : 0;
    if (heldModel && (hands == Hands::Holding || hands == Hands::Picking)) {
        UObject* inHands = WeaponInHands(pawn, heldModel);
        if (inHands) weapon = inHands;
        if (want != heldId) {  // put down (the server agreed), taken by the mode, or given to someone else
            dg::Log("Carry: box %u leaves the hands", heldId);
            LetGo(pawn, now);
        } else if (hands == Hands::Picking) {
            if (inHands) {
                SetHands(Hands::Holding, now);
                DisarmDrop(weapon, pawn);
                for (auto& [id, b] : boxes) if (id == heldId) Destroy(b.actor);  // the game hides the pickup it took ("TAKEN")
                dg::Log("Carry: holding box %u", heldId);
            } else if (now - handsSince > 10) {  // the game would not take it (from a step away the player first walks up to it): it lies where it is
                dg::Log("Carry: box %u was not picked up", heldId);
                auto it = boxes.find(heldId);
                const FVector at = it != boxes.end() && IsValid(it->second.actor) ? Location(it->second.actor) : here;
                client.SendCarry(heldId, net::CarryOp::Lost, at.X, at.Y, at.Z, Yaw(pawn));
                SetHands(Hands::Lost, now);
            }
        } else if (!inHands) {  // the game took it out of the hands: a hit, the bike, a ladder...
            SetHands(Hands::Lost, now);
            weapon = nullptr;
            dg::Log("Carry: box %u dropped by the game", heldId);
        }
    }
    // A dodge roll with the box: it stays in the hands, falls (reported where it lies, like a drop by the
    // game) or breaks (it goes; the server removes it), as the box's `roll` says.
    if (rolled) {
        rolled = false;
        auto it = boxes.find(heldId);
        const bool justLost = hands == Hands::Lost && now - handsSince < 0.6;  // the game dropped it as the roll began (the coolers)
        const uint8_t roll = (hands == Hands::Holding || justLost) && it != boxes.end() ? it->second.info.roll : 0;
        if (roll == 1 && justLost) lostOp = net::CarryOp::Roll;
        else if (roll == 1) {
            if (IsValid(weapon)) if (auto f = Fn(pawn, "DropWeapon")) { Params p(f); p.Set("Weapon", weapon); p.SetBool("BrokenWeaponOnly", false); Call(pawn, "DropWeapon", &p); }
            weapon = nullptr; lostOp = net::CarryOp::Roll;
            SetHands(Hands::Lost, now);
            dg::Log("Carry: box %u falls in a roll", heldId);
        } else if (roll == 2) {
            client.SendCarry(heldId, net::CarryOp::Break, here.X, here.Y, here.Z - 90.f, Yaw(pawn));
            brokenId = heldId; brokenAt = now;
            LetGo(pawn, now);  // (dropped already: the pickup the game left goes while releasing)
            dg::Log("Carry: box %u breaks in a roll", heldId);
        }
    }
    if (hands == Hands::Lost) {
        // Report where it fell (the pickup the game left; it goes, the server's box takes its place) once
        // it lies still: a thrown one flies for a moment. Without one, where the player stands.
        static FVector lastAt{}; static double stillSince = 0;
        UObject* dropped = heldModel ? DroppedByGame(pawn, heldModel, here, 1500.f) : nullptr;
        bool settled = false;
        if (dropped) {
            const FVector at = Location(dropped);
            const float dx = at.X - lastAt.X, dy = at.Y - lastAt.Y, dz = at.Z - lastAt.Z;
            if (dx * dx + dy * dy + dz * dz > 4.f) { lastAt = at; stillSince = now; }
            settled = now - stillSince > 0.4 || now - handsSince > 4.0;
        }
        if (settled || (!dropped && now - handsSince > 1.5)) {
            const FVector at = dropped ? Location(dropped) : FVector{here.X, here.Y, here.Z - 90.f};
            const float yaw = dropped ? Yaw(dropped) : Yaw(pawn);
            Destroy(dropped);
            if (want == heldId) client.SendCarry(heldId, lostOp, at.X, at.Y, at.Z, yaw);
            lostOp = net::CarryOp::Lost;
            SetHands(Hands::Releasing, now);
        }
    }
    if (hands == Hands::Releasing) {
        // The pickup the game leaves after DropWeapon comes a moment later.
        if (UObject* dropped = heldModel ? DroppedByGame(pawn, heldModel, here, 600.f) : nullptr) Destroy(dropped);
        if (now - handsSince > 1.5) { hands = Hands::Empty; heldId = 0; heldModel = nullptr; }
    }
    // An item of a model in the hands that the server gave to nobody here (a pick-up that finished after
    // it was given up): out of the hands again, without a trace.
    static double nextStrayCheck = 0;
    if (hands == Hands::Empty && !want && now >= nextStrayCheck) {
        nextStrayCheck = now + 0.5;
        for (auto& m : kModels)
            if (UObject* stray = WeaponInHands(pawn, &m)) {
                dg::Log("Carry: %s in the hands that the server did not give: let go", m.name);
                heldModel = &m; heldId = 0; weapon = stray;
                LetGo(pawn, now);
                break;
            }
    }
    if (hands == Hands::Empty && want && !(want == brokenId && now - brokenAt < 3)) {  // into the hands: the native pick-up of the copy here (one at the feet if there is none)
        Box& b = boxes[want];
        heldModel = ModelOf(b.info.model);
        heldId = want;
        if (!heldModel) { client.SendCarry(want, net::CarryOp::Lost, here.X, here.Y, here.Z - 90.f, Yaw(pawn)); SetHands(Hands::Lost, now); }
        else {
            if (!IsValid(b.actor) || Dist2(Location(b.actor), here.X, here.Y) > 400.f * 400.f) {
                Destroy(b.actor);
                b.actor = PlaceBox(heldModel, {here.X, here.Y, here.Z - 90.f}, Yaw(pawn), pawn);
            }
            if (IsValid(b.actor)) Call(b.actor, "ForcePickup");  // (its interaction stays off: no "[R] Take" flashing)
            SetHands(Hands::Picking, now);
            dg::Log("Carry: picking up box %u", want);
        }
    }
    // Punching and rolling are off while the box is in the hands (also when something turned them back on).
    const bool carrying = hands == Hands::Holding || hands == Hands::Picking;
    static double nextAssert = 0;
    if (carrying != restricted || (carrying && now >= nextAssert && GetBoolProp(pawn, "CanDoMelee"))) Restrict(pawn, carrying);
    if (carrying && now >= nextAssert) nextAssert = now + 0.5;
    policy::SetCarryInputBlock(carrying);  // no aiming / weapon switch: the game would holster the item, i.e. throw it away
    // A box that stays in the hands in a roll ("keep") of a model the game drops in one: no roll at all.
    {
        auto it = boxes.find(heldId);
        policy::SetCarryRollBlock(carrying && heldModel && heldModel->rollDrops && it != boxes.end() && it->second.info.roll == 0);
    }
    static double nextPromptCheck = 0;
    if (hands != Hands::Empty && now >= nextPromptCheck) { nextPromptCheck = now + 0.25; HideDropPrompt(); }

    // ---- the boxes on the ground: placed near the player, in its world
    for (auto& [id, b] : boxes) {
        const bool lies = b.info.carrier == 0xFFFF && (b.info.world < 0 || b.info.world == world);
        const float d = Dist2(here, b.info.x, b.info.y);
        if (b.info.carrier == me) continue;  // its copy is the one being picked up
        if (!lies || d > kHideRange * kHideRange) { Destroy(b.actor); continue; }
        if (IsValid(b.actor) && GetBoolProp(b.actor, "bHidden")) Destroy(b.actor);  // a pickup the game took and hid ("TAKEN")
        if (IsValid(b.actor) || d > kShowRange * kShowRange) continue;
        const Model* m = ModelOf(b.info.model);
        if (m) b.actor = PlaceBox(m, {b.info.x, b.info.y, b.info.z}, b.info.yaw, pawn);
    }

    // ---- hold F: pick the nearest box up, or set the carried one down in front
    std::string prompt;
    uint16_t target = 0;
    const bool waiting = now - requestedAt < 2.0;
    if (canAct && !waiting) {
        if (hands == Hands::Holding) { target = heldId; prompt = lang::Pick("Положить (удерживать)", "Put down (hold)"); }
        else if (hands == Hands::Empty) {
            float best = kReach * kReach;
            for (auto& [id, b] : boxes) {
                if (b.info.carrier != 0xFFFF || !IsValid(b.actor)) continue;
                const FVector at = Location(b.actor);
                const float d = Dist2(here, at.X, at.Y);
                if (d < best && std::fabs(at.Z - here.Z) < 200.f) { best = d; target = id; }
            }
            if (target) prompt = lang::Pick("Поднять (удерживать)", "Pick up (hold)");
        }
    }
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool fDown = pid == GetCurrentProcessId() && (GetAsyncKeyState('F') & 0x8000);
    // F belongs to the box only while its prompt is the one shown (an interaction of the mode comes first).
    // holdStart: -1 not held, -2 used (until F is let go).
    const bool active = target && mpui::CarryPromptShown();
    if (!fDown) holdStart = -1;
    else if (active && holdStart == -1) holdStart = now;
    else if (!active && holdStart >= 0) holdStart = -1;
    if (active && holdStart >= 0 && now - holdStart >= kHoldTime) {
        holdStart = -2;
        requestedAt = now;
        if (hands == Hands::Holding) {
            const float yaw = Yaw(pawn), r = yaw * 0.0174533f;
            client.SendCarry(target, net::CarryOp::Put, here.X + std::cos(r) * 70.f, here.Y + std::sin(r) * 70.f, here.Z - 90.f, yaw);
        } else client.SendCarry(target, net::CarryOp::Pick, here.X, here.Y, here.Z, Yaw(pawn));
        prompt.clear(); target = 0;
    }
    const float progress = target && holdStart >= 0 ? float(std::min(1.0, (now - holdStart) / kHoldTime)) : 0.f;
    mpui::SetCarryPrompt(prompt, progress);
}

}  // namespace carry
