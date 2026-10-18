#include "skins.h"
#include "log.h"
#include "profile.h"
#include "overlay.h"
#include "lang.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <cstring>
#include <sstream>

using namespace ue;
namespace fs = std::filesystem;

namespace skins {
namespace {
// ---------------------------------------------------------------- catalogs (data/looks.txt, data/parts.txt)
struct Variant { int id; std::string family, slot, mesh, source; std::vector<std::string> materials; std::string label; };
std::vector<Variant> variants;                          // by id
std::map<std::string, std::string> lookPaths;           // look name -> asset path
std::map<std::string, std::vector<std::string>> presets; // family -> look names
std::map<std::string, std::vector<int>> lookParts;       // look name -> its part ids
bool loaded = false;

// The body a look belongs to: "male", "female", "deacon", or "story:<character>" (each story
// character has its own parts); "" for freakers and dead bodies.
std::string FamilyOf(const std::string& path) {
    if (path.find("/Characters/npc_male/") != std::string::npos) return "male";
    if (path.find("/Characters/npc_female/") != std::string::npos) return "female";
    if (path.find("/Characters/Deacon/") != std::string::npos) return "deacon";
    const size_t at = path.find("/Characters/");
    if (at == std::string::npos) return {};
    const std::string who = path.substr(at + 12, path.find('/', at + 12) - at - 12);
    if (who == "Freaker" || who == "deadbody") return {};
    return "story:" + who;
}
// The creator's choice of body: story characters are one choice.
std::string UiFamily(const std::string& family) { return family.rfind("story:", 0) == 0 ? "story" : family; }
std::string SlotOf(const std::string& meshPath) {
    const size_t at = meshPath.find("/parts/");
    if (at == std::string::npos) return {};
    const size_t end = meshPath.find('/', at + 7);
    return end == std::string::npos ? "up" : meshPath.substr(at + 7, end - at - 7);
}
std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out; size_t at = 0;
    for (size_t k; (k = s.find(sep, at)) != std::string::npos; at = k + 1) out.push_back(s.substr(at, k - at));
    out.push_back(s.substr(at));
    return out;
}
std::string Short(const std::string& path) { const size_t dot = path.rfind('.'); return dot == std::string::npos ? path : path.substr(dot + 1); }
void Load() {
    if (loaded) return;
    loaded = true;
    const fs::path data = fs::path(profile::Root()) / L"data";
    std::ifstream looks(data / L"looks.txt");
    for (std::string line; std::getline(looks, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = Split(line, '\t');
        if (f.size() < 2) continue;
        lookPaths[f[0]] = f[1];
        const std::string family = FamilyOf(f[1]);
        if (!family.empty()) presets[UiFamily(family)].push_back(f[0]);
    }
    std::ifstream parts(data / L"parts.txt");
    for (std::string line; std::getline(parts, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = Split(line, '\t');
        if (f.size() < 6) continue;
        Variant v; v.id = std::atoi(f[0].c_str()); v.family = f[1]; v.slot = f[2]; v.mesh = f[3]; v.source = f[5];
        if (!f[4].empty()) v.materials = Split(f[4], '|');
        // Faces differ by their material: name a head after its face material.
        std::string name = Short(v.mesh);
        if (v.slot == "head") for (auto& m : v.materials) if (m.find("/head/mi_") != std::string::npos && m.find("morph") == std::string::npos) { name = Short(m); break; }
        for (const char* prefix : {"mi_", "m_mod_", "f_mod_", "m_sur_", "f_sur_", "m_npc_", "f_npc_", "m_", "f_", "deacon_"}) if (name.rfind(prefix, 0) == 0) { name = name.substr(strlen(prefix)); break; }
        v.label = name;
        if (v.id >= int(variants.size())) variants.resize(size_t(v.id) + 1);
        variants[size_t(v.id)] = v;
    }
    std::ifstream lp(data / L"lookparts.txt");
    for (std::string line; std::getline(lp, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = Split(line, '\t');
        if (f.size() < 2) continue;
        for (auto& id : Split(f[1], ',')) if (!id.empty()) lookParts[f[0]].push_back(std::atoi(id.c_str()));
    }
    dg::Log("Skins: %zu looks, %zu part variants", lookPaths.size(), variants.size());
}
const Variant* VariantOf(int id) { Load(); return id >= 0 && id < int(variants.size()) && !variants[size_t(id)].mesh.empty() ? &variants[size_t(id)] : nullptr; }

// ---------------------------------------------------------------- wearing
struct Parsed { std::string base; std::vector<std::pair<std::string, int>> parts; };  // part -1: none
Parsed Parse(const std::string& skin) {
    Parsed p;
    const auto f = Split(skin.substr(5), ';');  // after "skin:"
    p.base = f.empty() ? std::string() : f[0];
    for (size_t i = 1; i < f.size(); ++i) {
        const size_t eq = f[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string value = f[i].substr(eq + 1);
        p.parts.push_back({f[i].substr(0, eq), value == "-" ? -1 : std::atoi(value.c_str())});
    }
    return p;
}
std::string Compose(const std::string& base, const std::map<std::string, int>& parts) {
    std::string s = "skin:" + base;
    for (auto& [slot, id] : parts) s += ";" + slot + "=" + (id < 0 ? std::string("-") : std::to_string(id));
    return s;
}
UObject* Asset(const std::string& path) {
    static std::map<std::string, UObject*> cache;
    auto& slot = cache[path];
    if (!IsValid(slot)) { slot = FindObject(path.c_str()); if (!slot) slot = LoadAsset(path.c_str()); }
    return slot;
}
bool WearAsset(UObject* comp, const std::string& path) {
    UObject* look = IsValid(comp) ? Asset(path) : nullptr;
    auto f = look ? Fn(comp, "SetLook") : nullptr;
    if (!f) { dg::Log("Look %s could not be applied", path.c_str()); return false; }
    SetBoolProp(comp, "bDisableClothSimulation", true);
    Params p(f); p.Set("NewLook", look); p.SetBool("bNeedsToWorkWithRagdoll", false); Call(comp, "SetLook", &p);
    return true;
}
std::vector<UObject*> CreatedParts(UObject* comp) {
    struct Arr { UObject** data; int32_t num, max; };
    std::vector<UObject*> out;
    for (auto cls = comp->Class; cls; cls = SuperOf(cls)) if (auto prop = FindProperty(cls, "CreatedLookParts")) {
        const auto a = Field<Arr>(comp, Field<int>(prop, off::UProperty_Offset));
        for (int i = 0; a.data && i < a.num && i < 64; ++i) if (IsValid(a.data[i])) out.push_back(a.data[i]);
        break;
    }
    return out;
}
void SetPart(UObject* comp, const Variant& v) {
    UObject* mesh = Asset(v.mesh);
    if (!mesh) return;
    SetBoolProp(comp, "bDisableClothSimulation", true);
    if (auto f = Fn(comp, "SetSkeletalMesh")) { Params p(f); p.Set("NewMesh", mesh); Call(comp, "SetSkeletalMesh", &p); }
    if (auto f = Fn(comp, "SetMaterial"))
        for (int i = 0; i < int(v.materials.size()); ++i) {
            if (v.materials[size_t(i)].empty()) continue;
            UObject* m = Asset(v.materials[size_t(i)]);
            if (!m) continue;
            Params p(f); p.Set<int>("ElementIndex", i); p.Set("Material", m); Call(comp, "SetMaterial", &p);
        }
}
std::map<UObject*, std::vector<UObject*>> extras;  // mesh -> parts added for slots its look lacks
void DropExtras(UObject* comp) {
    auto it = extras.find(comp);
    if (it == extras.end()) return;
    for (UObject* c : it->second) if (IsValid(c)) if (auto f = Fn(c, "K2_DestroyComponent")) { Params p(f); p.Set("Object", c); Call(c, "K2_DestroyComponent", &p); }
    extras.erase(it);
}
}  // namespace

bool IsSkin(const std::string& look) { return look.rfind("skin:", 0) == 0; }
std::string LookPath(const std::string& look) {
    if (look.empty() || IsSkin(look) || look[0] == '/') return look;
    Load();
    auto it = lookPaths.find(look);
    return it == lookPaths.end() ? std::string() : it->second;
}
std::vector<std::string> LookNames() {
    Load();
    std::vector<std::string> out;
    for (auto& [name, path] : lookPaths) if (!FamilyOf(path).empty()) out.push_back(name);  // people only
    return out;
}
std::string BaseLook(const std::string& look) {
    if (!IsSkin(look)) return look;
    Load();
    auto it = lookPaths.find(Parse(look).base);
    return it == lookPaths.end() ? std::string() : it->second;
}

std::vector<UObject*> ExtraParts(UObject* mesh) {
    std::vector<UObject*> out;
    if (auto it = extras.find(mesh); it != extras.end()) for (UObject* c : it->second) if (IsValid(c)) out.push_back(c);
    return out;
}

bool Wear(UObject* comp, const std::string& look) {
    if (!IsValid(comp)) return false;
    DropExtras(comp);
    if (!IsSkin(look)) return WearAsset(comp, look);
    Load();
    const Parsed p = Parse(look);
    auto base = lookPaths.find(p.base);
    if (base == lookPaths.end() || !WearAsset(comp, base->second)) { dg::Log("Skin: unknown look %s", p.base.c_str()); return false; }
    std::vector<UObject*> parts = CreatedParts(comp);
    for (auto& [slot, id] : p.parts) {
        const Variant* v = VariantOf(id);
        if (id >= 0 && (!v || v->slot != slot)) continue;
        if (slot == "head") { if (v) SetPart(comp, *v); continue; }  // the look's master part: the mesh itself
        UObject* part = nullptr;
        for (UObject* c : parts) {
            UObject* mesh = Field<UObject*>(c, off::Skinned_SkeletalMesh);
            if (IsValid(mesh) && SlotOf(FullName(mesh)) == slot) { part = c; break; }
        }
        if (!v) {  // slot taken off
            if (part) if (auto f = Fn(part, "SetVisibility")) { Params q(f); q.SetBool("bNewVisibility", false); Call(part, "SetVisibility", &q); }
            continue;
        }
        if (!part) {  // the look has nothing there: a part of our own, posed by the mesh
            UObject* owner = comp->Outer;
            UObject* cls = parts.empty() ? FindClass("SkeletalMeshComponent") : parts[0]->Class;
            part = cls && owner ? NewObject(cls, owner) : nullptr;
            if (!part) continue;
            SetPart(part, *v);
            RegisterComponent(part);
            if (auto f = Fn(part, "K2_AttachTo")) { Params q(f); q.Set("InParent", comp); q.Set<uint8_t>("AttachType", 2); Call(part, "K2_AttachTo", &q); }
            if (auto f = Fn(part, "SetMasterPoseComponent")) { Params q(f); q.Set("NewMasterBoneComponent", comp); Call(part, "SetMasterPoseComponent", &q); }
            extras[comp].push_back(part);
            continue;
        }
        SetPart(part, *v);
    }
    return true;
}

// ---------------------------------------------------------------- the character creator
namespace {
const char* kSlots[] = {"head", "hair", "beard", "hat", "face", "up", "torso", "hand", "lo", "feet", "pack", "holster", "tact"};
bool Optional(const std::string& slot) { return slot != "head"; }  // anything but the face can be taken off
const char* SlotName(const std::string& slot) {
    static const std::map<std::string, std::pair<const char*, const char*>> names = {
        {"head", {"Лицо", "Face"}}, {"hair", {"Волосы", "Hair"}}, {"beard", {"Борода", "Beard"}}, {"hat", {"Головной убор", "Headwear"}},
        {"up", {"Верх", "Top"}}, {"torso", {"Руки", "Arms"}}, {"hand", {"Кисти", "Hands"}}, {"lo", {"Низ", "Bottoms"}},
        {"feet", {"Обувь", "Shoes"}}, {"face", {"Очки, маска", "Glasses, mask"}}, {"holster", {"Кобура", "Holster"}}, {"pack", {"Рюкзак, снаряжение", "Pack, gear"}},
        {"tact", {"Снаряжение", "Gear"}}};
    auto it = names.find(slot);
    return it == names.end() ? slot.c_str() : lang::Pick(it->second.first, it->second.second);
}
struct Creator {
    bool open = false, started = false;
    Options opt;
    std::string family, base;
    std::map<std::string, int> parts;       // slot -> part id, -1 none (unset: the starting look's own)
    std::string applied;                    // what the body wears now
    bool finished = false, ok = false;
    char filter[64] = "";
    int choice = 0;                         // the picker's look (Options::choices)
    // camera
    UObject* cam = nullptr;
    UObject* previousView = nullptr;  // the view target to give back
    float yaw = 0, turn = 0, pitch = -5, dist = 260, faceDist = 90;  // yaw: the body's at the start; turn: the player turned it
    bool head = false;
    bool hideWhenBlocking = true;     // the body's setting before the creator
    FRotator meshTurn{};              // the mesh's own rotation in its actor before the creator
} c;
// The game fades a body standing between the camera and the player: not the one being dressed.
void SetFade(UObject* mesh, bool fade) {
    SetBoolProp(mesh, "bHideWhenBlockingCamera", fade);
    for (UObject* part : CreatedParts(mesh)) SetBoolProp(part, "bHideWhenBlockingCamera", fade);
    for (UObject* part : ExtraParts(mesh)) SetBoolProp(part, "bHideWhenBlockingCamera", fade);
}
std::mutex lock;  // c: drawn on the render thread, applied on the game thread

std::vector<std::string> Families() {
    std::vector<std::string> out;
    for (const char* f : {"male", "female", "deacon", "story"}) {
        if (!c.opt.families.empty() && std::find(c.opt.families.begin(), c.opt.families.end(), f) == c.opt.families.end()) continue;
        if (presets.count(f)) out.push_back(f);
    }
    return out;
}
std::vector<const Variant*> PartsFor(const std::string& uiFamily, const std::string& slot) {
    // A story character's parts only: their bodies differ.
    const std::string family = uiFamily == "story" && lookPaths.count(c.base) ? FamilyOf(lookPaths[c.base]) : uiFamily;
    std::vector<const Variant*> out;
    for (auto& v : variants) if (!v.mesh.empty() && v.family == family && v.slot == slot) out.push_back(&v);
    return out;
}
// A top comes with the arms and hands of the look it is from: sleeves and bare arms fit.
void PickTop(int id) {
    c.parts["up"] = id;
    const Variant* v = VariantOf(id);
    if (!v) return;
    c.parts.erase("torso"); c.parts.erase("hand");
    for (int part : lookParts[v->source]) if (const Variant* w = VariantOf(part)) if (w->slot == "torso" || w->slot == "hand") c.parts[w->slot] = part;
}
void Randomize() {
    static std::mt19937 rng{std::random_device{}()};
    auto& list = presets[c.family];
    if (!list.empty()) c.base = list[rng() % list.size()];
    c.parts.clear();
    for (const char* slot : kSlots) {
        auto opts = PartsFor(c.family, slot);
        if (opts.empty()) continue;
        if (std::string(slot) == "torso" || std::string(slot) == "hand") continue;  // they come with the top
        if (Optional(slot) && rng() % 3 == 0) { c.parts[slot] = -1; continue; }
        const int id = opts[rng() % opts.size()]->id;
        if (std::string(slot) == "up") PickTop(id); else c.parts[slot] = id;
    }
    if (c.parts.count("hat") && c.parts["hat"] >= 0) c.parts["hair"] = -1;  // hair would show through a hat
}
void CameraControls() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::TextUnformatted(lang::Pick("Камера:", "Camera:")); ImGui::SameLine();
    if (ImGui::RadioButton(lang::Pick("Общий план", "Full body"), !c.head)) c.head = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(lang::Pick("Лицо", "Face"), c.head)) c.head = true;
    if (ImGui::Button(lang::Pick("Готово", "Done"), ImVec2(140, 0))) { c.finished = true; c.ok = true; }
    if (c.opt.cancel) { ImGui::SameLine(); if (ImGui::Button(lang::Pick("Отмена", "Cancel"), ImVec2(140, 0))) { c.finished = true; c.ok = false; } }
    ImGui::TextDisabled("%s", lang::Pick("ПКМ — повернуть персонажа, колесо — ближе/дальше", "RMB — turn the character, wheel — zoom"));
    // Outside the window: RMB turns the body, the wheel zooms.
    if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) c.turn += io.MouseDelta.x * 0.5f;
        if (io.MouseWheel != 0) {
            float& d = c.head ? c.faceDist : c.dist;
            d = c.head ? std::clamp(d - io.MouseWheel * 10.f, 40.f, 160.f) : std::clamp(d - io.MouseWheel * 25.f, 80.f, 450.f);
        }
    }
}
// The picker: only the looks the mode offers, stepped through with arrows or chosen from the list.
void DrawPicker() {
    ImGuiIO& io = ImGui::GetIO();
    const int n = int(c.opt.choices.size());
    ImGui::SetNextWindowPos(ImVec2(30, 60), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(380, std::min(io.DisplaySize.y - 120, 190.f + 26.f * float(n))), ImGuiCond_Appearing);
    ImGui::Begin(c.opt.title.empty() ? lang::Pick("Персонаж", "Character") : c.opt.title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) c.choice = (c.choice - 1 + n) % n;
    ImGui::SameLine();
    const std::string shown = c.opt.choices[size_t(c.choice)].name + "   " + std::to_string(c.choice + 1) + " / " + std::to_string(n);
    ImGui::Button(shown.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) c.choice = (c.choice + 1) % n;
    ImGui::Separator();
    if (ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3 - 4))) {
        for (int k = 0; k < n; ++k) if (ImGui::Selectable((c.opt.choices[size_t(k)].name + "##" + std::to_string(k)).c_str(), k == c.choice)) c.choice = k;
    }
    ImGui::EndChild();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) c.choice = (c.choice - 1 + n) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) c.choice = (c.choice + 1) % n;
    }
    CameraControls();
    ImGui::End();
}
void Draw() {
    std::lock_guard g(lock);
    if (!c.open) return;
    if (!c.opt.choices.empty()) { DrawPicker(); return; }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(30, 60), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(470, io.DisplaySize.y - 120), ImGuiCond_Appearing);
    ImGui::Begin(c.opt.title.empty() ? lang::Pick("Персонаж", "Character") : c.opt.title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
    const auto fams = Families();
    if (fams.size() > 1) {
        for (size_t i = 0; i < fams.size(); ++i) {
            const char* label = fams[i] == "male" ? lang::Pick("Мужчина", "Man") : fams[i] == "female" ? lang::Pick("Женщина", "Woman")
                              : fams[i] == "deacon" ? lang::Pick("Дикон", "Deacon") : lang::Pick("Сюжетные", "Story");
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(label, c.family == fams[i]) && c.family != fams[i]) { c.family = fams[i]; c.base = presets[c.family].front(); c.parts.clear(); }
        }
    }
    // Starting look: arrows step through the looks of the body.
    {
        auto& list = presets[c.family];
        const int n = int(list.size());
        int at = int(std::find(list.begin(), list.end(), c.base) - list.begin());
        if (ImGui::ArrowButton("##prevlook", ImGuiDir_Left) && n) { c.base = list[size_t((at - 1 + n) % n)]; c.parts.clear(); }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##nextlook", ImGuiDir_Right) && n) { c.base = list[size_t((at + 1) % n)]; c.parts.clear(); }
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(-150);
    if (ImGui::BeginCombo(lang::Pick("Основа", "Preset"), c.base.c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##flt", lang::Pick("Поиск", "Search"), c.filter, sizeof c.filter);
        for (auto& name : presets[c.family]) {
            if (c.filter[0] && name.find(c.filter) == std::string::npos) continue;
            if (ImGui::Selectable(name.c_str(), name == c.base)) { c.base = name; c.parts.clear(); }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); if (ImGui::Button(lang::Pick("Случайно", "Random"))) Randomize();
    ImGui::Separator();
    for (const char* slot : kSlots) {
        auto opts = PartsFor(c.family, slot);
        if (opts.empty()) continue;
        ImGui::PushID(slot);
        auto it = c.parts.find(slot);
        int at = -2;  // -2: the starting look's own, -1: none
        if (it != c.parts.end()) { at = -1; for (size_t k = 0; k < opts.size(); ++k) if (opts[k]->id == it->second) at = int(k); }
        const bool canNone = Optional(slot);
        auto set = [&](int k) { if (k < 0) c.parts[slot] = -1; else if (std::string(slot) == "up") PickTop(opts[size_t(k)]->id); else c.parts[slot] = opts[size_t(k)]->id; };
        const int n = int(opts.size());
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(SlotName(slot)); ImGui::SameLine(130);
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { int k = at < 0 ? 0 : at - 1; if (k < (canNone ? -1 : 0)) k = n - 1; set(k); }
        ImGui::SameLine();
        const std::string shown = at == -2 ? lang::Pick("(как в основе)", "(preset)") : at == -1 ? lang::Pick("нет", "none") : opts[size_t(at)]->label + "  " + std::to_string(at + 1) + "/" + std::to_string(n);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##pick", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable(lang::Pick("(как в основе)", "(preset)"), at == -2)) c.parts.erase(slot);
            if (canNone && ImGui::Selectable(lang::Pick("нет", "none"), at == -1)) set(-1);
            for (int k = 0; k < n; ++k) if (ImGui::Selectable((opts[size_t(k)]->label + "##" + std::to_string(k)).c_str(), k == at)) set(k);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { int k = at < 0 ? 0 : at + 1; if (k >= n) k = canNone ? -1 : 0; set(k); }
        if (at != -2) {  // back to the preset's own part
            ImGui::SameLine();
            if (ImGui::SmallButton("↺")) c.parts.erase(slot);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lang::Pick("Как в основе", "As in the preset"));
        }
        ImGui::PopID();
    }
    if (ImGui::Button(lang::Pick("Сбросить всё", "Reset all"))) c.parts.clear();
    ImGui::Separator();
    CameraControls();
    ImGui::End();
}
UObject* Statics() { static UObject* s = nullptr; if (!IsValid(s)) s = FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
// The camera stays in front of walls between it and the body (a small room).
FVector Unblocked(UObject* self, const FVector& from, const FVector& to) {
    static UObject* kismet = nullptr;
    if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
    auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr, brk = Statics() ? Fn(Statics(), "BreakHitResult") : nullptr;
    if (!trace || !brk) return to;
    Params t(trace); t.Set("WorldContextObject", self); t.Set("Start", from); t.Set("End", to);
    t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", false); t.SetBool("bIgnoreSelf", true);
    Call(kismet, "LineTraceSingle_NEW", &t);
    if (!t.GetBool("ReturnValue")) return to;
    Params b(brk);
    auto hitProp = FindProperty(trace, "OutHit"), inProp = FindProperty(brk, "Hit");
    if (!hitProp || !inProp) return to;
    std::memcpy(static_cast<uint8_t*>(b.Data()) + Field<int>(inProp, off::UProperty_Offset), static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset), 136);
    Call(Statics(), "BreakHitResult", &b);
    const FVector hit = b.Get<FVector>("Location");
    const float dx = from.X - hit.X, dy = from.Y - hit.Y, dz = from.Z - hit.Z, len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1) return hit;
    const float back = std::min(15.f, len * 0.5f);
    return {hit.X + dx / len * back, hit.Y + dy / len * back, hit.Z + dz / len * back};
}
void ViewThrough(UObject* controller, UObject* target) {
    if (auto f = IsValid(controller) && IsValid(target) ? Fn(controller, "SetViewTargetWithBlend") : nullptr) { Params p(f); p.Set("NewViewTarget", target); p.Set<float>("BlendTime", 0.f); Call(controller, "SetViewTargetWithBlend", &p); }
}
}  // namespace

void OpenCreator(const Options& options) {
    Load();
    std::lock_guard g(lock);
    c = Creator{};
    c.opt = options;
    c.open = true;
    for (size_t k = 0; k < options.choices.size(); ++k) if (options.choices[k].look == options.current) c.choice = int(k);
    const auto fams = Families();
    // Start from what the player wears (a skin, or a look of an allowed family).
    std::string start = options.current;
    if (IsSkin(start)) { const Parsed p = Parse(start); c.base = p.base; for (auto& [s, id] : p.parts) c.parts[s] = id; }
    else for (auto& [name, path] : lookPaths) if (path == start) { c.base = name; break; }
    c.family = lookPaths.count(c.base) ? UiFamily(FamilyOf(lookPaths[c.base])) : std::string();
    if (std::find(fams.begin(), fams.end(), c.family) == fams.end()) {
        c.family = fams.empty() ? "male" : fams.front();
        c.base = presets[c.family].empty() ? std::string() : presets[c.family].front();
        c.parts.clear();
    }
    overlay::SetVisible(true);
    overlay::SetCursorMode(true);
}
bool CreatorOpen() { std::lock_guard g(lock); return c.open; }
void CloseCreator() { std::lock_guard g(lock); if (c.open) { c.finished = true; c.ok = false; } }
void DrawCreator() { Draw(); }

bool UpdateCreator(UObject* pawn, UObject* controller, UObject* mesh, bool& ok, std::string& skin) {
    std::unique_lock g(lock);
    if (!c.open) return false;
    if (!IsValid(pawn) || !IsValid(mesh)) {
        if (!c.finished) return false;
        // Closed without a body (the editor's mannequin is gone): only the view goes back.
        ok = false; skin.clear();
        c.open = false;
        if (IsValid(c.previousView)) ViewThrough(controller, c.previousView);
        if (IsValid(c.cam)) Call(c.cam, "K2_DestroyActor");
        c.cam = nullptr;
        if (!c.opt.embedded) overlay::SetVisible(false);
        return true;
    }
    if (!c.started) {
        c.started = true;
        if (auto f = Fn(pawn, "K2_GetActorRotation")) { Params p(f); Call(pawn, "K2_GetActorRotation", &p); c.yaw = p.Get<FRotator>("ReturnValue").Yaw; }
        if (c.opt.hasYaw) {  // facing the way the choosing place says
            c.yaw = c.opt.yaw;
            if (auto f = Fn(pawn, "K2_SetActorRotation")) { Params p(f); p.Set("NewRotation", FRotator{0, c.yaw, 0}); Call(pawn, "K2_SetActorRotation", &p); }
            if (auto f = IsValid(controller) ? Fn(controller, "SetControlRotation") : nullptr) { Params p(f); p.Set("NewRotation", FRotator{0, c.yaw, 0}); Call(controller, "SetControlRotation", &p); }
        }
        if (auto f = Fn(controller, "GetViewTarget")) { Params p(f); Call(controller, "GetViewTarget", &p); c.previousView = p.Get<UObject*>("ReturnValue"); }
        if (Statics()) {
            FTransform t;
            Params begin(Fn(Statics(), "BeginSpawningActorFromClass"));
            begin.Set("WorldContextObject", pawn); begin.Set("ActorClass", FindClass("CameraActor")); begin.Set("SpawnTransform", t); begin.SetBool("bNoCollisionFail", true);
            Call(Statics(), "BeginSpawningActorFromClass", &begin);
            c.cam = begin.Get<UObject*>("ReturnValue");
            if (c.cam) {
                Params finish(Fn(Statics(), "FinishSpawningActor")); finish.Set("Actor", c.cam); finish.Set("SpawnTransform", t); Call(Statics(), "FinishSpawningActor", &finish);
                // Fill the whole window: a letterboxed view target leaves the screen broken afterwards.
                for (auto k = c.cam->Class; k; k = SuperOf(k)) if (auto prop = FindProperty(k, "CameraComponent")) {
                    UObject* comp = Field<UObject*>(c.cam, Field<int>(prop, off::UProperty_Offset));
                    if (IsValid(comp)) if (auto f = Fn(comp, "SetConstraintAspectRatio")) { Params a(f); a.SetBool("bInConstrainAspectRatio", false); Call(comp, "SetConstraintAspectRatio", &a); }
                    break;
                }
                ViewThrough(controller, c.cam);
            }
        }
        if (!c.opt.embedded) if (auto f = Fn(controller, "SetIgnoreMoveInput")) { Params p(f); p.SetBool("bNewMoveInput", true); Call(controller, "SetIgnoreMoveInput", &p); }
        bool found = false;
        c.hideWhenBlocking = GetBoolProp(mesh, "bHideWhenBlockingCamera", &found) || !found;
        c.meshTurn = Field<FRotator>(mesh, 0x17C);  // SceneComponent.RelativeRotation
    }
    if (!overlay::Visible()) overlay::SetVisible(true);
    if (!overlay::CursorMode()) overlay::SetCursorMode(true);
    // The body wears what is picked.
    const std::string want = c.opt.choices.empty() ? Compose(c.base, c.parts) : c.opt.choices[size_t(c.choice)].look;
    if (want != c.applied) { c.applied = want; g.unlock(); Wear(mesh, want); g.lock(); }
    SetFade(mesh, false);  // new parts come with the game's setting
    // The player turns the body; the camera stays in front of where it first faced, a little to
    // its side so the body stands right of the window (the face when a head row is under the mouse).
    // Turned as a picture only: the game keeps turning the character itself, not its mesh.
    if (auto f = Fn(mesh, "K2_SetRelativeRotation")) { Params p(f); p.Set("NewRotation", FRotator{c.meshTurn.Pitch, c.meshTurn.Yaw + c.turn, c.meshTurn.Roll}); Call(mesh, "K2_SetRelativeRotation", &p); }
    UObject* root = Field<UObject*>(pawn, off::Actor_RootComponent);
    FVector body = IsValid(root) ? Field<FVector>(root, off::Scene_C2W_Trans) : FVector{};
    body.Z += (c.head ? 70.f : 10.f) - c.opt.rootHeight;
    const float dist = c.head ? c.faceDist : c.dist;
    const float view = c.yaw + 180.f;  // looking back at the body's front
    const float rad = 0.0174532925f, cp = std::cos(c.pitch * rad), sp = std::sin(c.pitch * rad), cy = std::cos(view * rad), sy = std::sin(view * rad);
    const float side = dist * 0.35f;   // the view's centre moves to the body's right: the body shows on the right
    const FVector target{body.X + sy * side, body.Y - cy * side, body.Z};
    const FVector eye = Unblocked(pawn, body, FVector{target.X - cp * cy * dist, target.Y - cp * sy * dist, target.Z - sp * dist});
    if (IsValid(c.cam)) {
        if (auto f = Fn(c.cam, "K2_SetActorLocationAndRotation")) { Params p(f); p.Set("NewLocation", eye); p.Set("NewRotation", FRotator{c.pitch, view, 0}); p.SetBool("bTeleport", true); Call(c.cam, "K2_SetActorLocationAndRotation", &p); }
    }
    if (!c.finished) return false;
    // Done: back to the player's own view.
    ok = c.ok; skin = c.applied;
    c.open = false;
    SetFade(mesh, c.hideWhenBlocking);
    if (auto f = Fn(mesh, "K2_SetRelativeRotation")) { Params p(f); p.Set("NewRotation", c.meshTurn); Call(mesh, "K2_SetRelativeRotation", &p); }
    ViewThrough(controller, IsValid(c.previousView) ? c.previousView : pawn);
    if (IsValid(c.cam)) Call(c.cam, "K2_DestroyActor");
    c.cam = nullptr;
    if (c.opt.embedded) return true;
    if (auto f = Fn(controller, "SetIgnoreMoveInput")) { Params p(f); p.SetBool("bNewMoveInput", false); Call(controller, "SetIgnoreMoveInput", &p); }
    overlay::SetVisible(false);  // the multiplayer windows (mpui) take it back if a dialog is open
    return true;
}

}  // namespace skins
