#include "sounds.h"
#include "log.h"
#include "policy.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace ue;

namespace sounds {
namespace {
UObject* localPawn = nullptr;
UObject* observedWeapon = nullptr;
struct Shot { std::string cls; FVector at, direction; };
std::vector<Shot> shots;          // fired since the last Update
double lastShotLog = 0;
double lastQueuedShot = 0;

UObject* Statics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }

UObject* Asset(const std::string& path) {
    static std::map<std::string, UObject*> cache;
    auto it = cache.find(path);
    if (it != cache.end() && IsValid(it->second)) return it->second;
    UObject* a = FindObject(path.c_str());
    if (!a) a = LoadAsset(path.c_str());
    if (a) AddToRoot(a);  // a sound collected while it plays would crash
    cache[path] = a;
    return a;
}

UObject* ObjectProp(UObject* o, const char* name) {
    for (auto c = IsValid(o) ? o->Class : nullptr; c; c = SuperOf(c)) if (auto p = FindProperty(c, name)) return Field<UObject*>(o, Field<int>(p, off::UProperty_Offset));
    return nullptr;
}

UObject* CurrentWeapon(UObject* pawn) {
    if (auto f = IsValid(pawn) ? Fn(pawn, "GetEquippedItemCurrentlyInHand") : nullptr) {
        Params p(f); Call(pawn, "GetEquippedItemCurrentlyInHand", &p);
        UObject* weapon = p.Get<UObject*>("ReturnValue");
        if (IsValid(weapon)) return weapon;
    }
    return ObjectProp(pawn, "EquippedWeaponRef");
}

// The game's NPC fire sound for a weapon class (Bend_WpnAK47_C -> cc_wpn_ak47_npc).
std::string FireSound(const std::string& weaponClass) {
    std::string c; for (char ch : weaponClass) if (ch != '_') c += char(std::tolower(static_cast<unsigned char>(ch)));
    static const std::pair<const char*, const char*> kinds[] = {
        {"ak47", "ak47/cc_wpn_ak47_npc"}, {"deserteagle", "pistol/cc_wpn_desert_eagle_npc"}, {"fn57", "pistol/cc_wpn_fn57_npc"},
        {"glock", "pistol/cc_wpn_glock41_npc"}, {"danwesson", "pistol/cc_wpn_dan_wesson_npc"}, {"sig", "pistol/cc_wpn_sig226_npc"},
        {"mayor", "pistol/cc_wpn_mayor_npc"}, {"stubby", "pistol/cc_wpn_snubnose_npc"}, {"snub", "pistol/cc_wpn_snubnose_npc"},
        {"m9", "pistol/cc_wpn_m9_npc"}, {"henry", "rifle/cc_wpn_henry_npc"}, {"as50", "rifle/cc_wpn_as50_npc"},
        {"m14", "rifle/cc_wpn_m14_npc"}, {"m40", "rifle/cc_wpn_m40_npc"}, {"dragunov", "rifle/cc_wpn_dragunov_npc"},
        {"msg90", "rifle/cc_wpn_msg90_npc"}, {"sako", "rifle/cc_wpn_sako_npc"}, {"marlin", "rifle/cc_wpn_marlin1985_npc"},
        {"ruger", "rifle/cc_wpn_ruger1022_npc"}, {"colt", "rifle/cc_wpn_colt_rifle_npc"}, {"honeybadger", "assault_rifle/cc_wpn_honey_badger_npc"},
        {"1887", "shotgun/cc_wpn_1887_npc"}, {"benelli", "shotgun/cc_wpn_benelli_nova_npc"}, {"dp12", "shotgun/cc_wpn_dp12_npc"},
        {"mossberg", "shotgun/cc_wpn_mossberg590_npc"}, {"toz", "shotgun/cc_wpn_toz194_npc"}, {"mp5", "machine_gun/cc_wpn_mp5n_npc"},
        {"ppsh", "machine_gun/cc_wpn_ppsh_41_npc"}, {"panzerfaust", "special/cc_wpn_panzerfaust_fire_npc"},
        // by kind, when the model has no own sound
        {"shotgun", "shotgun/cc_wpn_shotgun_npc"}, {"sniper", "sniper/cc_wpn_rifle_npc"}, {"rifle", "rifle/cc_wpn_henry_npc"},
        {"smg", "machine_gun/cc_wpn_mp5n_npc"}, {"mg", "machine_gun/cc_wpn_mp5n_npc"}, {"pistol", "pistol/cc_wpn_m9_npc"},
    };
    for (auto& [key, cue] : kinds) {
        if (c.find(key) == std::string::npos) continue;
        const std::string name = std::string(cue).substr(std::string(cue).rfind('/') + 1);
        return std::string("/Game/Audio/weapon/") + cue + "." + name;
    }
    return "";
}

UObject* ImpactFactory() {
    static UObject* factory = nullptr;
    if (!IsValid(factory)) factory = FindObject("/Script/BendGame.Default__BendImpactEffect");
    return IsValid(factory) ? factory : nullptr;
}

FRotator DirectionRotation(const FVector& direction) {
    const float horizontal = std::sqrt(direction.X * direction.X + direction.Y * direction.Y);
    return FRotator{std::atan2(direction.Z, horizontal) * 57.2957795f, std::atan2(direction.Y, direction.X) * 57.2957795f, 0};
}

FVector RotationDirection(const FRotator& rotation) {
    const float pitch = rotation.Pitch * 0.0174532925f, yaw = rotation.Yaw * 0.0174532925f, horizontal = std::cos(pitch);
    return FVector{horizontal * std::cos(yaw), horizontal * std::sin(yaw), std::sin(pitch)};
}

UObject* BulletImpactTemplate(const std::string& weaponClass) {
    std::string lower; for (char ch : weaponClass) lower += char(std::tolower(static_cast<unsigned char>(ch)));
    const char* preferred = lower.find("shotgun") != std::string::npos || lower.find("1887") != std::string::npos || lower.find("benelli") != std::string::npos || lower.find("dp12") != std::string::npos || lower.find("mossberg") != std::string::npos || lower.find("toz") != std::string::npos ? "Bend_BulletImpact_Shotgun_C"
        : lower.find("sniper") != std::string::npos || lower.find("as50") != std::string::npos || lower.find("m40") != std::string::npos || lower.find("dragunov") != std::string::npos || lower.find("msg90") != std::string::npos ? "Bend_BulletImpact_Sniper_C"
        : lower.find("pistol") != std::string::npos || lower.find("glock") != std::string::npos || lower.find("m9") != std::string::npos || lower.find("fn57") != std::string::npos || lower.find("mayor") != std::string::npos || lower.find("stubby") != std::string::npos || lower.find("snub") != std::string::npos || lower.find("sig") != std::string::npos || lower.find("danwesson") != std::string::npos || lower.find("deserteagle") != std::string::npos ? "Bend_BulletImpact_Pistol_C"
        : lower.find("rifle") != std::string::npos || lower.find("henry") != std::string::npos || lower.find("marlin") != std::string::npos || lower.find("ruger") != std::string::npos ? "Bend_BulletImpact_Rifle_C" : "Bend_BulletImpact_Assault_C";
    if (UObject* impact = FindClass(preferred)) return impact;
    static UObject* fallback = nullptr;
    if (!IsValid(fallback)) fallback = LoadAsset("/Game/Weapons/Projectile_Weapons/AssaultRifles/Projectiles/Bend_BulletImpact_Assault.Bend_BulletImpact_Assault_C");
    return IsValid(fallback) ? fallback : nullptr;
}

void PlayShotImpact(const std::string& weaponClass, const FVector& origin, FVector direction) {
    const float length = std::sqrt(direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z);
    if (!IsValid(localPawn) || length < 0.1f) return;
    direction.X /= length; direction.Y /= length; direction.Z /= length;
    static UObject* kismet = nullptr; static UObject* statics = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if (!IsValid(statics)) statics = FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr;
    auto breakHit = statics ? Fn(statics, "BreakHitResult") : nullptr;
    UObject* factory = ImpactFactory();
    auto create = factory ? Fn(factory, "CreateImpactEffectBP") : nullptr;
    UObject* impactTemplate = BulletImpactTemplate(weaponClass);
    if (!trace || !breakHit || !create || !IsValid(impactTemplate)) return;
    const FVector start{origin.X + direction.X * 5.f, origin.Y + direction.Y * 5.f, origin.Z + direction.Z * 5.f};
    const FVector end{start.X + direction.X * 40000.f, start.Y + direction.Y * 40000.f, start.Z + direction.Z * 40000.f};
    Params ray(trace); ray.Set("WorldContextObject", localPawn); ray.Set("Start", start); ray.Set("End", end);
    ray.Set<uint8_t>("TraceChannel", 0); ray.SetBool("bTraceComplex", true); ray.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &ray);
    auto hitProperty = FindProperty(trace, "OutHit"), inputProperty = FindProperty(breakHit, "Hit");
    if (!ray.GetBool("ReturnValue") || !hitProperty || !inputProperty || Field<int>(hitProperty, off::UProperty_ElementSize) != 136 || Field<int>(inputProperty, off::UProperty_ElementSize) != 136) return;
    Params hit(breakHit);
    std::memcpy(static_cast<uint8_t*>(hit.Data()) + Field<int>(inputProperty, off::UProperty_Offset), static_cast<uint8_t*>(ray.Data()) + Field<int>(hitProperty, off::UProperty_Offset), 136);
    Call(statics, "BreakHitResult", &hit);
    const FVector location = hit.Get<FVector>("ImpactPoint"), normal = hit.Get<FVector>("ImpactNormal");
    Params p(create); p.Set("ImpactTemplate", impactTemplate);
    p.Set("Location", location); p.Set("Rotation", DirectionRotation(direction)); p.Set("SurfaceNormal", normal);
    p.Set("ParticleRotation", DirectionRotation(normal)); p.Set("ShootDir", direction);
    p.Set("InstigatorCtrl", Field<UObject*>(localPawn, off::Pawn_Controller)); p.SetBool("DestructionOnly", false);
    Call(factory, "CreateImpactEffectBP", &p);
}

void QueueShot(UObject* weapon) {
    if (!IsValid(weapon)) return;
    const double now = GetTickCount64() / 1000.0;
    if (now - lastQueuedShot < 0.02) return;
    lastQueuedShot = now;
    Shot s; s.cls = GetName(weapon->Class);
    if (auto f = Fn(weapon, "K2_GetActorLocation")) { Params p(f); Call(weapon, "K2_GetActorLocation", &p); s.at = p.Get<FVector>("ReturnValue"); }
    UObject* controller = IsValid(localPawn) ? Field<UObject*>(localPawn, off::Pawn_Controller) : nullptr;
    if (auto f = IsValid(controller) ? Fn(controller, "GetControlRotation") : nullptr) { Params p(f); Call(controller, "GetControlRotation", &p); s.direction = RotationDirection(p.Get<FRotator>("ReturnValue")); }
    else if (auto forward = Fn(weapon, "GetActorForwardVector")) { Params p(forward); Call(weapon, "GetActorForwardVector", &p); s.direction = p.Get<FVector>("ReturnValue"); }
    if (shots.size() < 32) shots.push_back(std::move(s));
}

}  // namespace

void Play(const std::string& path, const FVector* at, float volume, float pitch) {
    UObject* sound = Asset(path);
    if (!sound || !Statics() || !IsValid(localPawn)) { if (!sound) dg::Log("Sounds: cannot load %s", path.c_str()); return; }
    if (at) {
        Params p(Fn(Statics(), "SpawnSoundAtLocation"));
        p.Set("WorldContextObject", localPawn); p.Set("Sound", sound); p.Set("Location", *at);
        p.Set<float>("VolumeMultiplier", volume); p.Set<float>("PitchMultiplier", pitch);
        Call(Statics(), "SpawnSoundAtLocation", &p);
    } else {
        Params p(Fn(Statics(), "PlaySound2D"));
        p.Set("WorldContextObject", localPawn); p.Set("Sound", sound);
        p.Set<float>("VolumeMultiplier", volume); p.Set<float>("PitchMultiplier", pitch);
        Call(Statics(), "PlaySound2D", &p);
    }
}

// The shooter's own fire sound of that gun (wpn_m9_fire_plr, wpn_ak47_fire_auto_plr) next to the NPC cue,
// or "" when the game has none.
std::string NearFireSound(const std::string& npcCue) {
    const size_t slash = npcCue.rfind('/'), dot = npcCue.rfind('.');
    if (slash == std::string::npos || dot == std::string::npos) return "";
    std::string name = npcCue.substr(slash + 1, dot - slash - 1);  // cc_wpn_m9_npc
    if (name.rfind("cc_wpn_", 0) != 0 || name.size() < 12 || name.compare(name.size() - 4, 4, "_npc") != 0) return "";
    const std::string gun = name.substr(7, name.size() - 11), dir = npcCue.substr(0, slash + 1);
    for (const char* kind : {"_fire_plr", "_fire_auto_plr"}) {
        const std::string n = "wpn_" + gun + kind, path = dir + n + "." + n;
        if (Asset(path)) return path;
    }
    return "";
}

void OnPlayerShot(uint16_t id, const std::string& weaponClass, const FVector& at, const FVector& direction) {
    std::string cue = FireSound(weaponClass);
    if (cue.empty()) { cue = "/Game/Audio/weapon/rifle/cc_wpn_henry_npc.cc_wpn_henry_npc"; dg::Log("Sounds: generic fire sound for %s (player %u)", weaponClass.c_str(), id); }
    // The NPC cues are made for enemies far off: a gun fired next to this player sounded distant. Within
    // 25 m, the shooter's own sound of that gun, where it was fired.
    FVector me{};
    if (auto f = IsValid(localPawn) ? Fn(localPawn, "K2_GetActorLocation") : nullptr) { Params p(f); Call(localPawn, "K2_GetActorLocation", &p); me = p.Get<FVector>("ReturnValue"); }
    const float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
    if (!cue.empty() && dx * dx + dy * dy + dz * dz < 2500.f * 2500.f) {
        static std::map<std::string, std::string> nearCues;
        auto it = nearCues.find(cue);
        if (it == nearCues.end()) it = nearCues.emplace(cue, NearFireSound(cue)).first;
        if (!it->second.empty()) Play(it->second, &at, 1.f, 1.f);
        else Play(cue, &at, 1.f, 1.f);
    } else if (!cue.empty()) {
        Play(cue, &at, 1.f, 1.f);
    }
    PlayShotImpact(weaponClass, at, direction);
}

void OnPawnEvent(UObject* object, const std::string& name) {
    UObject* weapon = object == localPawn && name == "OnWeaponFired_Event" ? CurrentWeapon(object) : object == observedWeapon && name == "OnFired" ? object : nullptr;
    QueueShot(weapon);
}

void Update(UObject* pawn, net::Client& client) {
    localPawn = pawn;
    UObject* weapon = CurrentWeapon(pawn);
    if (weapon != observedWeapon) {
        observedWeapon = IsValid(weapon) ? weapon : nullptr;
        if (observedWeapon) policy::Observe(observedWeapon);
    }
    for (auto& s : shots) client.SendShot(s.cls, s.at.X, s.at.Y, s.at.Z, s.direction.X, s.direction.Y, s.direction.Z);
    if (!shots.empty()) {
        const double now = GetTickCount64() / 1000.0;
        if (now - lastShotLog > 5) { lastShotLog = now; dg::Log("Shot: %s (%zu)", shots.back().cls.c_str(), shots.size()); }
    }
    shots.clear();
}

void Reset() { shots.clear(); observedWeapon = nullptr; lastQueuedShot = 0; }

}  // namespace sounds
