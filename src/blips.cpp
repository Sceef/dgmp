#include "blips.h"
#include "policy.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <map>

using namespace ue;

namespace blips {
namespace {
struct Blip {
    net::Client::Events::BlipInfo info;
    UObject* holder = nullptr;   // hidden actor the marker takes its position from
    UObject* marker = nullptr;   // MiniMapLocationComponent
    bool dirty = true;           // (re)made next Update
    bool moved = false;          // only its place changed (a blip that follows a player): the holder moves
};
std::map<uint16_t, Blip> blips;
int myWorld = 0;

UObject* Statics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }

// Construct FText through the engine, which owns its shared text data.
void SetText(UObject* object, const char* field, const std::string& text) {
    auto lib = FindObject("/Script/Engine.Default__KismetTextLibrary");
    auto fn = lib ? Fn(lib,"Conv_StringToText") : nullptr;
    if (!fn) return;
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring value(count,L'\0');
    if (count) MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),value.data(),count);
    Params p(fn); p.Set("InString",FString{value.c_str(),int32_t(value.size()+1),int32_t(value.size()+1)});
    Call(lib,"Conv_StringToText",&p);
    struct EngineText { uint8_t data[0x18]; };
    for (auto cls=object->Class; cls; cls=SuperOf(cls)) if (auto prop=FindProperty(cls,field)) {
        Field<EngineText>(object,Field<int>(prop,off::UProperty_Offset))=p.Get<EngineText>("ReturnValue");
        break;
    }
}

UObject* SpawnHolder(const FVector& at, UObject* context) {
    UObject* cls = FindClass("StaticMeshActor");
    if (!cls || !Statics()) return nullptr;
    FTransform t; t.Translation = at;
    Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject", context); begin.Set("ActorClass", cls); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
    Call(Statics(), "BeginSpawningActorFromClass", &begin);
    UObject* actor = begin.Get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    Params finish(Fn(Statics(), "FinishSpawningActor"));
    finish.Set("Actor", actor); finish.Set("SpawnTransform", t);
    Call(Statics(), "FinishSpawningActor", &finish);
    // Not hidden: the radar skips locations of hidden actors (it has no mesh, nothing is drawn).
    if (auto f = Fn(actor, "SetActorEnableCollision")) { Params p(f); p.SetBool("bNewActorEnableCollision", false); Call(actor, "SetActorEnableCollision", &p); }
    return actor;
}

UObject* MakeMarker(UObject* holder, const net::Client::Events::BlipInfo& b) {
    static UObject* cls = FindClass("MiniMapLocationComponent");
    UObject* m = cls ? NewObject(cls, holder) : nullptr;
    if (!m) return nullptr;
    auto prop = [&](const char* name) -> uint8_t* {
        for (auto k = m->Class; k; k = SuperOf(k)) if (auto pr = FindProperty(k, name)) return reinterpret_cast<uint8_t*>(m) + Field<int>(pr, off::UProperty_Offset);
        return nullptr;
    };
    std::string path = b.icon;
    if (path.empty()) path = "MAP_player_marker";
    if (path[0] != '/') { if (path.rfind("MAP_", 0) != 0 && path.rfind("map_", 0) != 0) path = "MAP_" + path; path = "/Game/Libraries/UI/Textures/" + path + "." + path; }
    UObject* icon = FindObject(path.c_str());
    if (!icon) icon = LoadAsset(path.c_str());
    if (!icon) dg::Log("Blips: no icon %s", path.c_str());
    else AddToRoot(icon);
    if (auto pr = prop("MapIcon")) *reinterpret_cast<UObject**>(pr) = icon;
    if (auto pr = prop("LocationType")) *pr = 0;   // takes colour and size
    if (auto pr = prop("RollOverType")) *pr = 7;   // EMapRolloverType::Basic; default Objective shows a story mission card
    SetBoolProp(m, "bUseSimpleMapInfoDisplay", true);
    SetText(m,"DisplayName",b.title);
    SetText(m,"DisplayDescription",b.description);
    if (auto pr = prop("DisplayRule")) *pr = 0;    // AlwaysVisible
    // Edge display on, or the radar never draws a Player-type location; how far it stays on the rim:
    // always (edge) or only just outside the radar.
    if (auto pr = prop("EdgeDisplay")) *pr = 1;
    if (auto pr = prop("EdgeDistance")) *pr = b.edge ? 0 : 4;  // Infinite / Smallest
    if (auto pr = prop("IconScale")) *reinterpret_cast<float*>(pr) = 0.5f * b.scale;
    const uint32_t c = b.color ? b.color : 0xFFFFFFFFu;
    const float color[4] = {float((c >> 24) & 0xFF) / 255.f, float((c >> 16) & 0xFF) / 255.f, float((c >> 8) & 0xFF) / 255.f, float(c & 0xFF) / 255.f};
    if (auto pr = prop("MinimapColor")) std::memcpy(pr, color, sizeof(color));
    if (auto pr = prop("MarkerColor")) std::memcpy(pr, color, sizeof(color));
    SetBoolProp(m, "bGetPositionFromOwner", true);
    RegisterComponent(m);
    if (auto f = Fn(m, "SetVisibility")) { Params p(f); p.SetBool("VisibleOnMap", b.map); p.SetBool("VisibleOnMiniMap", b.radar); Call(m, "SetVisibility", &p); }
    policy::AllowMarker(m, true);
    policy::RadarOnly(m, !b.map);
    return m;
}

void Drop(Blip& b) {
    if (IsValid(b.marker)) {
        policy::AllowMarker(b.marker, false); policy::RadarOnly(b.marker, false);
        if (auto f = Fn(b.marker, "K2_DestroyComponent")) { Params p(f); p.Set("Object", b.marker); Call(b.marker, "K2_DestroyComponent", &p); }
    }
    if (IsValid(b.holder)) Call(b.holder, "K2_DestroyActor");
    b.marker = b.holder = nullptr;
}
}  // namespace

void OnBlip(uint16_t id, const net::Client::Events::BlipInfo* info) {
    if (!info) { auto it = blips.find(id); if (it != blips.end()) { Drop(it->second); blips.erase(it); } return; }
    Blip& b = blips[id];
    const auto& o = b.info;
    const bool same = b.marker && o.icon == info->icon && o.color == info->color && o.scale == info->scale && o.map == info->map && o.radar == info->radar
        && o.edge == info->edge && o.world == info->world && o.title == info->title && o.description == info->description;
    b.info = *info;
    if (same) b.moved = true; else b.dirty = true;
}

void SetWorld(int world) {
    if (world == myWorld) return;
    myWorld = world;
    for (auto& [id, b] : blips) b.dirty = true;
}

void Reset() {
    for (auto& [id, b] : blips) Drop(b);
    blips.clear();
}

void Update(UObject* pawn) {
    if (!IsValid(pawn)) return;
    for (auto& [id, b] : blips) {
        if ((b.holder && !IsValid(b.holder)) || (b.marker && !IsValid(b.marker))) b.dirty = true;
        if (!b.dirty && b.moved && IsValid(b.holder)) {
            b.moved = false;
            if (auto f = Fn(b.holder, "K2_SetActorLocation")) { Params p(f); p.Set("NewLocation", FVector{b.info.x, b.info.y, b.info.z}); p.SetBool("bSweep", false); p.SetBool("bTeleport", true); Call(b.holder, "K2_SetActorLocation", &p); }
            continue;
        }
        if (!b.dirty) continue;
        b.dirty = false; b.moved = false;
        Drop(b);
        if (b.info.world >= 0 && b.info.world != myWorld) continue;
        b.holder = SpawnHolder(FVector{b.info.x, b.info.y, b.info.z}, pawn);
        if (b.holder) b.marker = MakeMarker(b.holder, b.info);
        if (!b.marker) dg::Log("Blips: cannot make %u", id);
    }
}

}  // namespace blips
