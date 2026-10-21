#include "sounds.h"
#include "log.h"
#include <algorithm>
#include <map>
#include <vector>

using namespace ue;

namespace sounds {
namespace {
UObject* localPawn = nullptr;
struct Shot { std::string cls; FVector at; };
std::vector<Shot> shots;          // fired since the last Update
double lastShotLog = 0;

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

void OnPlayerShot(uint16_t id, const std::string& weaponClass, const FVector& at) {
    const std::string cue = FireSound(weaponClass);
    if (cue.empty()) { dg::Log("Sounds: no fire sound for %s (player %u)", weaponClass.c_str(), id); return; }
    Play(cue, &at, 1.f, 1.f);
}

void OnPawnEvent(UObject* object, const std::string& name) {
    if (object != localPawn || name != "OnWeaponFired_Event") return;
    UObject* weapon = ObjectProp(object, "EquippedWeaponRef");
    if (!IsValid(weapon)) return;
    Shot s; s.cls = GetName(weapon->Class);
    if (auto f = Fn(weapon, "K2_GetActorLocation")) { Params p(f); Call(weapon, "K2_GetActorLocation", &p); s.at = p.Get<FVector>("ReturnValue"); }
    if (shots.size() < 32) shots.push_back(std::move(s));
}

void Update(UObject* pawn, net::Client& client) {
    localPawn = pawn;
    for (auto& s : shots) client.SendShot(s.cls, s.at.X, s.at.Y, s.at.Z);
    if (!shots.empty()) {
        const double now = GetTickCount64() / 1000.0;
        if (now - lastShotLog > 5) { lastShotLog = now; dg::Log("Shot: %s (%zu)", shots.back().cls.c_str(), shots.size()); }
    }
    shots.clear();
}

void Reset() { shots.clear(); }

}  // namespace sounds
