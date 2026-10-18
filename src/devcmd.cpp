#include "devcmd.h"
#include "profile.h"
#include "log.h"
#include "policy.h"
#include "worldedit.h"
#include "mp.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ue;
namespace fs = std::filesystem;

// Commands (one per request):
//   find <Class> [max]                 live instances of a class (default objects excluded)
//   names <substring> [max]            entries of the name table (input actions, sockets, ...)
//   objs <substring> [max]             objects whose full name contains substring
//   fns <obj> [filter]                 functions of the object's class chain
//   fn <obj> <Function>                parameters of one function
//   props <obj> [filter]               properties with current values
//   array <obj> <ArrayProperty> [max]  elements of an array property
//   set <obj> <Property> <value>
//   trace [seconds]                   log distinct events of the local pawn/controller and their components
//   refs <obj> [bytes]                 raw scan: offsets holding pointers to live objects (unreflected fields)
//   call <obj> <Function> [Param=value ...]   values: numbers, true/false, enum short names,
//                                       names, strings, objects (see <obj>), vectors "x,y,z"
// <obj>: pawn | controller | gamemode | comp:<Class> (component of pawn/controller) |
//        cdo:<Class> | /Full/Path.Object | @0xADDRESS | <ClassName> (first live instance) |
//        name:<ObjectName>
namespace devcmd {
namespace {
UObject *ctxPawn = nullptr, *ctxController = nullptr, *ctxGameMode = nullptr;
fs::path dir;
int lastSeq = 0;
ULONGLONG nextPoll = 0;
std::vector<std::unique_ptr<std::wstring>> keepAlive;  // FString arguments must outlive the call

constexpr int kProp_Inner = 0x78;  // ArrayProperty::Inner / ObjectProperty::PropertyClass / StructProperty::Struct / ByteProperty::Enum

std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; }
bool Contains(const std::string& hay, const std::string& needle) { return needle.empty() || Lower(hay).find(Lower(needle)) != std::string::npos; }
std::string Hex(uint64_t v) { char b[32]; std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v)); return b; }
std::string Narrow(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0'); WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string TypeOf(UObject* p) { return GetName(p->Class); }
int OffsetOf(UObject* p) { return Field<int>(p, off::UProperty_Offset); }
int SizeOf(UObject* p) { return Field<int>(p, off::UProperty_ElementSize); }

std::string EnumName(UObject* enumObj, uint8_t value) {
    if (!IsValid(enumObj)) return {};
    struct Item { FName name; uint8_t value; uint8_t pad[7]; };
    struct Items { Item* data; int32_t num, max; };
    const auto items = Field<Items>(enumObj, 0x40);
    if (!items.data || items.num <= 0 || items.num > 1024) return {};
    for (int i = 0; i < items.num; ++i) if (items.data[i].value == value) {
        auto n = NameOf(items.data[i].name); const auto colons = n.find("::");
        return colons == std::string::npos ? n : n.substr(colons + 2);
    }
    return {};
}
bool EnumValue(UObject* enumObj, const std::string& name, uint8_t& out) {
    if (!IsValid(enumObj)) return false;
    struct Item { FName name; uint8_t value; uint8_t pad[7]; };
    struct Items { Item* data; int32_t num, max; };
    const auto items = Field<Items>(enumObj, 0x40);
    for (int i = 0; items.data && i < items.num && i < 1024; ++i) {
        auto n = NameOf(items.data[i].name); const auto colons = n.find("::");
        const auto shortName = colons == std::string::npos ? n : n.substr(colons + 2);
        if (_stricmp(shortName.c_str(), name.c_str()) == 0 || _stricmp(n.c_str(), name.c_str()) == 0) { out = items.data[i].value; return true; }
    }
    return false;
}

std::string Format(UObject* p, const uint8_t* base) {
    const auto type = TypeOf(p);
    const uint8_t* at = base + OffsetOf(p);
    if (type == "IntProperty") return std::to_string(*reinterpret_cast<const int32_t*>(at));
    if (type == "Int64Property") return std::to_string(*reinterpret_cast<const int64_t*>(at));
    if (type == "UInt32Property") return std::to_string(*reinterpret_cast<const uint32_t*>(at));
    if (type == "FloatProperty") { char b[32]; std::snprintf(b, sizeof(b), "%g", *reinterpret_cast<const float*>(at)); return b; }
    if (type == "DoubleProperty") { char b[32]; std::snprintf(b, sizeof(b), "%g", *reinterpret_cast<const double*>(at)); return b; }
    if (type == "BoolProperty") return (at[Field<uint8_t>(p, off::BoolProperty_ByteOffset)] & Field<uint8_t>(p, off::BoolProperty_FieldMask)) ? "true" : "false";
    if (type == "ByteProperty") { const auto v = *at; const auto e = EnumName(Field<UObject*>(p, kProp_Inner), v); return std::to_string(v) + (e.empty() ? "" : " (" + e + ")"); }
    if (type == "NameProperty") return NameOf(*reinterpret_cast<const FName*>(at));
    if (type == "StrProperty") { auto s = reinterpret_cast<const FString*>(at); return "\"" + Narrow(s->Data, s->Num) + "\""; }
    if (type == "ObjectProperty" || type == "ClassProperty") { auto o = *reinterpret_cast<UObject* const*>(at); return o ? FullName(o) : "null"; }
    if (type == "WeakObjectProperty") { auto o = Resolve(*reinterpret_cast<const FWeakObjectPtr*>(at)); return o ? FullName(o) : "null"; }
    if (type == "ArrayProperty") { struct A { void* d; int32_t n, m; }; auto a = reinterpret_cast<const A*>(at); return "[" + std::to_string(a->n) + " x " + TypeOf(Field<UObject*>(p, kProp_Inner)) + "]"; }
    if (type == "StructProperty") {
        const auto sname = GetName(Field<UObject*>(p, kProp_Inner));
        if (sname == "Vector" || sname == "Rotator") { auto f = reinterpret_cast<const float*>(at); char b[96]; std::snprintf(b, sizeof(b), "(%g, %g, %g)", f[0], f[1], f[2]); return sname + b; }
        std::string out = sname + " {";
        for (int i = 0; i < std::min(SizeOf(p), 32); ++i) { char b[4]; std::snprintf(b, sizeof(b), "%02X", at[i]); out += b; }
        return out + (SizeOf(p) > 32 ? "...}" : "}");
    }
    return "<" + type + " size " + std::to_string(SizeOf(p)) + ">";
}

UObject* FindClassDefault(const std::string& cls) {
    const std::string want = "Default__" + cls;
    for (int i = 0, n = NumObjects(); i < n; ++i) { auto o = ObjectAt(i); if (o && IsValid(o) && GetName(o) == want) return o; }
    return nullptr;
}
UObject* Resolve(const std::string& spec) {
    if (spec == "pawn") return ctxPawn;
    if (spec == "controller") return ctxController;
    if (spec == "gamemode") return ctxGameMode;
    if (spec.rfind("@", 0) == 0) return reinterpret_cast<UObject*>(std::stoull(spec.substr(1), nullptr, 16));
    if (spec.rfind("/", 0) == 0) return FindObject(spec.c_str());
    if (spec.rfind("cdo:", 0) == 0) return FindClassDefault(spec.substr(4));
    if (spec.rfind("name:", 0) == 0) {
        const auto want = spec.substr(5);
        for (int i = 0, n = NumObjects(); i < n; ++i) { auto o = ObjectAt(i); if (o && IsValid(o) && !(o->ObjectFlags & 0x10) && GetName(o) == want) return o; }
        return nullptr;
    }
    if (spec.rfind("comp:", 0) == 0) {
        UObject* cls = FindClass(spec.substr(5).c_str());
        for (int i = 0, n = NumObjects(); i < n && cls; ++i) {
            auto o = ObjectAt(i);
            if (o && IsValid(o) && (o->Outer == ctxPawn || o->Outer == ctxController) && IsA(o, cls)) return o;
        }
        return nullptr;
    }
    return FindRuntimeInstance(spec.c_str());
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out; std::string cur; bool quoted = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ' ' && !quoted) { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Writes a textual value into the memory of property p at base. Returns an error or "".
std::string Assign(UObject* p, uint8_t* base, const std::string& value) {
    const auto type = TypeOf(p);
    uint8_t* at = base + OffsetOf(p);
    try {
        if (type == "IntProperty") { *reinterpret_cast<int32_t*>(at) = std::stoi(value); return ""; }
        if (type == "Int64Property") { *reinterpret_cast<int64_t*>(at) = std::stoll(value); return ""; }
        if (type == "FloatProperty") { *reinterpret_cast<float*>(at) = std::stof(value); return ""; }
        if (type == "BoolProperty") {
            const bool v = value == "1" || Lower(value) == "true";
            auto& b = at[Field<uint8_t>(p, off::BoolProperty_ByteOffset)]; const auto mask = Field<uint8_t>(p, off::BoolProperty_FieldMask);
            b = v ? (b | mask) : (b & ~mask); return "";
        }
        if (type == "ByteProperty") {
            uint8_t v;
            if (!value.empty() && std::isdigit(static_cast<unsigned char>(value[0]))) v = uint8_t(std::stoi(value));
            else if (!EnumValue(Field<UObject*>(p, kProp_Inner), value, v)) return "unknown enum value " + value;
            *at = v; return "";
        }
        if (type == "NameProperty") {
            const auto n = FindName(value.c_str());
            if (n.Index < 0) return "name not in the name table: " + value;
            *reinterpret_cast<FName*>(at) = n; return "";
        }
        if (type == "StrProperty") {
            auto w = std::make_unique<std::wstring>(value.begin(), value.end());
            const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
            w->resize(len); MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, w->data(), len);
            *reinterpret_cast<FString*>(at) = FString{w->c_str(), int32_t(w->size()), int32_t(w->size())};
            keepAlive.push_back(std::move(w)); return "";
        }
        if (type == "ObjectProperty" || type == "ClassProperty") {
            UObject* o = value == "null" ? nullptr : Resolve(value);
            if (!o && value != "null") { o = FindClass(value.c_str()); }
            if (!o && value != "null") return "object not found: " + value;
            *reinterpret_cast<UObject**>(at) = o; return "";
        }
        if (type == "TextProperty") {
            // FText from a string through the engine (the reference lives in the never-destructed frame).
            auto lib = FindObject("/Script/Engine.Default__KismetTextLibrary");
            auto conv = lib ? Fn(lib, "Conv_StringToText") : nullptr;
            if (!conv) return "Conv_StringToText not found";
            auto w = std::make_unique<std::wstring>();
            const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
            w->resize(len); MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, w->data(), len);
            static std::vector<std::unique_ptr<Params>> frames;
            frames.push_back(std::make_unique<Params>(conv));
            frames.back()->Set("InString", FString{w->c_str(), int32_t(w->size()), int32_t(w->size())});
            ProcessEvent(lib, conv, frames.back()->Data());
            std::memcpy(at, static_cast<uint8_t*>(frames.back()->Data()) + Field<int>(FindProperty(conv, "ReturnValue"), off::UProperty_Offset), 24);
            keepAlive.push_back(std::move(w)); return "";
        }
        if (type == "StructProperty") {
            float f[4] = {}; const int n = sscanf_s(value.c_str(), "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
            if (n < 2 || n * 4 > SizeOf(p)) return "struct values are written as x,y,z";
            std::memcpy(at, f, size_t(n) * 4); return "";
        }
    } catch (...) { return "bad value " + value; }
    return "cannot assign " + type;
}

UObject* FunctionOf(UObject* o, const std::string& name) {
    for (auto cls = o->Class; cls; cls = SuperOf(cls))
        for (auto c = Field<UObject*>(cls, off::UStruct_Children); c; c = Field<UObject*>(c, off::UField_Next))
            if (GetName(c->Class) == "Function" && _stricmp(GetName(c).c_str(), name.c_str()) == 0) return c;
    return nullptr;
}

// Offset of the first 8-byte slot in [o, o+bytes) holding `target`, or -1 (also when unreadable).
int FindPointer(const UObject* o, size_t bytes, const UObject* target) {
    __try {
        const auto base = reinterpret_cast<const uint8_t*>(o);
        for (size_t at = 0; at + 8 <= bytes; at += 8) if (*reinterpret_cast<const UObject* const*>(base + at) == target) return int(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

std::string Run(const std::string& line) {
    keepAlive.clear();
    const auto a = Split(line);
    if (a.empty()) return "empty command";
    std::ostringstream out;
    const auto& cmd = a[0];
    if (cmd == "find" || cmd == "objs") {
        if (a.size() < 2) return "usage: find <Class> [max] | objs <substring> [max]";
        const int max = a.size() > 2 ? std::stoi(a[2]) : 40;
        UObject* cls = cmd == "find" ? FindClass(a[1].c_str()) : nullptr;
        if (cmd == "find" && !cls) return "class not found";
        int shown = 0;
        for (int i = 0, n = NumObjects(); i < n && shown < max; ++i) {
            auto o = ObjectAt(i); if (!o || !IsValid(o)) continue;
            if (cmd == "find" ? (IsA(o, cls) && !(o->ObjectFlags & 0x10)) : Contains(FullName(o), a[1])) {
                out << Hex(reinterpret_cast<uintptr_t>(o)) << "  " << GetName(o->Class) << "  " << FullName(o) << "\n"; ++shown;
            }
        }
        out << shown << " shown";
        return out.str();
    }
    if (cmd == "enum") {
        if (a.size() < 2) return "usage: enum <EnumName>";
        for (int i = 0, n = NumObjects(); i < n; ++i) {
            auto e = ObjectAt(i);
            if (!e || !IsValid(e) || GetName(e) != a[1] || GetName(e->Class) != "Enum") continue;
            struct Item { FName name; uint8_t value; uint8_t pad[7]; };
            struct Items { Item* data; int32_t num, max; };
            const auto items = Field<Items>(e, 0x40);
            for (int k = 0; items.data && k < items.num && k < 1024; ++k) out << "  " << int(items.data[k].value) << " " << NameOf(items.data[k].name) << "\n";
            return out.str();
        }
        return "enum not found";
    }
    if (cmd == "names") {
        if (a.size() < 2) return "usage: names <substring> [max]";
        for (auto& n : SearchNames(a[1].c_str(), a.size() > 2 ? std::stoi(a[2]) : 80)) out << "  " << n << "\n";
        return out.str();
    }
    if (cmd == "trace") {
        const double seconds = a.size() > 1 ? std::stod(a[1]) : 60;
        std::vector<UObject*> extra;  // trace <seconds> ghosts | <obj>: also those objects
        if (a.size() > 2) { if (a[2] == "ghosts") extra = mp::GhostActors(); else if (UObject* o = Resolve(a[2])) extra.push_back(o); }
        return "tracing " + std::to_string(policy::TraceEvents(seconds, extra)) + " objects (see DGMP.log, lines TRACE)";
    }
    if (cmd == "ray" && a.size() >= 4) {  // ray x y z [complex]: vertical trace through the point, prints the hit
        static UObject* kismet = nullptr;
        if (!IsValid(kismet)) kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary");
        auto trace = kismet ? Fn(kismet, "LineTraceSingle_NEW") : nullptr;
        if (!trace || !ctxPawn) return "no trace";
        const FVector p{std::stof(a[1]), std::stof(a[2]), std::stof(a[3])};
        Params t(trace); t.Set("WorldContextObject", ctxPawn); t.Set("Start", FVector{p.X, p.Y, p.Z + 600}); t.Set("End", FVector{p.X, p.Y, p.Z - 600});
        t.Set<uint8_t>("TraceChannel", 0); t.SetBool("bTraceComplex", a.size() > 4); t.SetBool("bIgnoreSelf", true);
        Call(kismet, "LineTraceSingle_NEW", &t);
        if (!t.GetBool("ReturnValue")) return "no hit";
        auto hitProp = FindProperty(trace, "OutHit");
        const uint8_t* h = static_cast<uint8_t*>(t.Data()) + Field<int>(hitProp, off::UProperty_Offset);
        auto weak = [](const uint8_t* w) -> UObject* { const int idx = *reinterpret_cast<const int*>(w); return idx >= 0 && idx < NumObjects() ? ObjectAt(idx) : nullptr; };
        UObject* actor = weak(h + 0x64); UObject* comp = weak(h + 0x6C);
        const float* at = reinterpret_cast<const float*>(h + 0x18);
        out << "at " << at[0] << " " << at[1] << " " << at[2] << "  item " << *reinterpret_cast<const int*>(h + 0x58) << " face " << *reinterpret_cast<const int*>(h + 0x80) << "\n";
        out << "actor " << (actor ? FullName(actor) : "?") << "\ncomp " << (comp ? FullName(comp) : "?") << "\n";
        if (comp) { char b[64]; snprintf(b, sizeof b, "comp @%p  +0x640 %d\n", (void*)comp, *reinterpret_cast<const int*>(reinterpret_cast<uint8_t*>(comp) + 0x640)); out << b; }
        if (a.back() == "detach" && actor) {  // ray x y z detach: replace the hit collection, print the piece's actor
            const int piece = worldedit::PieceAt(actor, FVector{at[0], at[1], at[2]});
            UObject* detached = piece >= 0 ? worldedit::Detach(actor, piece, ctxPawn) : nullptr;
            char b[96]; snprintf(b, sizeof b, "piece %d actor @%p\n", piece, (void*)detached); out << b;
            if (detached) out << worldedit::KeyOf(detached) << "\n";
        }
        return out.str();
    }
    if (cmd == "attached" && a.size() > 1) {  // attached <obj>: actors attached to it, their socket, visibility and meshes
        UObject* o = Resolve(a[1]);
        auto f = o ? Fn(o, "GetAttachedActors") : nullptr;
        if (!f) return "no GetAttachedActors";
        Params p(f); Call(o, "GetAttachedActors", &p);
        struct Raw { UObject** data; int num, max; };
        const Raw list = p.Get<Raw>("OutActors");
        static UObject* meshCls = FindClass("MeshComponent");
        for (int i = 0; i < list.num; ++i) {
            UObject* act = list.data[i];
            if (!IsValid(act)) continue;
            UObject* root = Field<UObject*>(act, off::Actor_RootComponent);
            out << GetName(act) << " (" << GetName(act->Class) << ")" << (GetBoolProp(act, "bHidden") ? " hidden" : "")
                << " socket=" << (root ? NameOf(Field<FName>(root, 0xE8)) : std::string("?")) << "\n";
            if (auto g = Fn(act, "GetComponentsByClass")) {
                Params q(g); q.Set("ComponentClass", meshCls); Call(act, "GetComponentsByClass", &q);
                const Raw comps = q.Get<Raw>("ReturnValue");
                for (int k = 0; k < comps.num; ++k) if (IsValid(comps.data[k])) {
                    UObject* c = comps.data[k];
                    UObject* mesh = nullptr;
                    for (auto cl = c->Class; cl && !mesh; cl = SuperOf(cl)) for (const char* n : {"StaticMesh", "SkeletalMesh"}) if (auto prop = FindProperty(cl, n)) { mesh = Field<UObject*>(c, Field<int>(prop, off::UProperty_Offset)); break; }
                    out << "    " << GetName(c) << " (" << GetName(c->Class) << ") vis=" << GetBoolProp(c, "bVisible") << " hiddenInGame=" << GetBoolProp(c, "bHiddenInGame")
                        << " socket=" << NameOf(Field<FName>(c, 0xE8)) << " mesh=" << (mesh ? FullName(mesh) : std::string("-")) << "\n";
                }
            }
        }
        return out.str();
    }
    if (cmd == "poke") {  // poke <hexaddr> <float> [float ...]: write floats (research only)
        float* at = reinterpret_cast<float*>(std::stoull(a[1], nullptr, 16));
        for (size_t i = 2; i < a.size(); ++i) at[i - 2] = std::stof(a[i]);
        return "written " + std::to_string(a.size() - 2) + " floats";
    }
    // Bike research: bikespawn [statics|controller] [distance] - another player bike in front of the
    // pawn (GameplayStatics spawn, or the controller's SpawnPlayerVehicle), optionally of another class [Class_C]; bikeassign <obj>; bikeenter <obj>.
    if (cmd == "bikespawn" || cmd == "bikeassign" || cmd == "bikeenter") {
        if (!ctxPawn || !ctxController) return "no pawn";
        if (cmd == "bikeassign" || cmd == "bikeenter") {
            if (a.size() < 2) return "usage: " + cmd + " <obj>";
            UObject* bike = Resolve(a[1]);
            if (!bike || !IsValid(bike)) return "object not found";
            if (cmd == "bikeassign") { auto f = FunctionOf(ctxController, "SetAssignedVehicle"); if (!f) return "no SetAssignedVehicle"; Params p(f); p.Set("InAssignedVehicle", bike); ProcessEvent(ctxController, f, p.Data()); }
            else { auto f = FunctionOf(ctxPawn, "TryEnterVehicle"); if (!f) return "no TryEnterVehicle"; Params p(f); p.Set("VehiclePawn", bike); p.SetBool("bForce", true); p.Set<float>("MaxDistance", 2000.f); ProcessEvent(ctxPawn, f, p.Data()); out << "returned " << Format(FindProperty(f, "ReturnValue"), static_cast<const uint8_t*>(p.Data())) << "\n"; }
            auto g = FunctionOf(ctxController, "GetAssignedVehicle"); Params q(g); ProcessEvent(ctxController, g, q.Data());
            out << "assigned now: " << (q.Get<UObject*>("ReturnValue") ? FullName(q.Get<UObject*>("ReturnValue")) : std::string("none")) << "\n";
            return out.str();
        }
        const bool viaController = a.size() > 1 && a[1] == "controller";
        const float dist = a.size() > 2 ? std::stof(a[2]) : 400.f;
        UObject* cls = FindClass(a.size() > 3 ? a[3].c_str() : "BP_bike04Drifter_C"); if (!cls) return "class not loaded (load <path> first)";
        auto loc = FunctionOf(ctxPawn, "K2_GetActorLocation"); Params l(loc); ProcessEvent(ctxPawn, loc, l.Data());
        auto rot = FunctionOf(ctxController, "GetControlRotation"); Params r(rot); ProcessEvent(ctxController, rot, r.Data());
        const FVector at = l.Get<FVector>("ReturnValue"); const float yaw = r.Get<FRotator>("ReturnValue").Yaw, rad = yaw * 0.0174532925f;
        FTransform t; t.Translation = {at.X + std::cos(rad) * dist, at.Y + std::sin(rad) * dist, at.Z};
        auto g = FunctionOf(ctxController, "GetAssignedVehicle"); Params before(g); ProcessEvent(ctxController, g, before.Data());
        UObject* bike = nullptr;
        if (viaController) {
            auto f = FunctionOf(ctxController, "SpawnPlayerVehicle"); if (!f) return "no SpawnPlayerVehicle";
            Params p(f); p.Set<int>("PlayerIndex", 0); p.Set("Class", cls); p.Set("SpawnTransform", t); p.SetBool("SpawnEvenIfColliding", true);
            ProcessEvent(ctxController, f, p.Data());
            bike = p.Get<UObject*>("ReturnValue");
        } else {
            UObject* statics = FindObject("/Script/Engine.Default__GameplayStatics");
            auto b = FunctionOf(statics, "BeginSpawningActorFromClass"), fin = FunctionOf(statics, "FinishSpawningActor");
            Params p(b); p.Set("WorldContextObject", ctxPawn); p.Set("ActorClass", cls); p.Set("SpawnTransform", t); p.SetBool("bNoCollisionFail", true);
            ProcessEvent(statics, b, p.Data());
            bike = p.Get<UObject*>("ReturnValue");
            if (bike) { Params q(fin); q.Set("Actor", bike); q.Set("SpawnTransform", t); ProcessEvent(statics, fin, q.Data()); }
        }
        Params after(g); ProcessEvent(ctxController, g, after.Data());
        auto name = [](UObject* o) { return o ? FullName(o) : std::string("none"); };
        out << "spawned: " << name(bike) << "\nassigned before: " << name(before.Get<UObject*>("ReturnValue")) << "\nassigned after: " << name(after.Get<UObject*>("ReturnValue")) << "\n";
        return out.str();
    }
    if (cmd == "coll") {  // coll [radius] [mesh filter]: merged-mesh collection instances near the pawn (64-byte entries at +0x768)
        const float radius = a.size() > 1 ? std::stof(a[1]) : 1500.f;
        const std::string filter = a.size() > 2 ? a[2] : "";
        FVector me{};
        if (ctxPawn) if (auto f = Fn(ctxPawn, "K2_GetActorLocation")) { Params l(f); Call(ctxPawn, "K2_GetActorLocation", &l); me = l.Get<FVector>("ReturnValue"); }
        UObject* cls = FindClass("BendStaticMeshCollectionComponent");
        int shown = 0;
        for (int i = 0, n = NumObjects(); i < n; ++i) {
            UObject* o = ObjectAt(i);
            if (!o || (o->ObjectFlags & 0x10) || !IsValid(o) || !IsA(o, cls)) continue;
            const auto base = reinterpret_cast<uint8_t*>(o);
            const auto inst = *reinterpret_cast<uint8_t**>(base + 0x768); const int count = *reinterpret_cast<int*>(base + 0x770);
            const auto meshes = *reinterpret_cast<UObject***>(base + 0x5E0); const int meshCount = *reinterpret_cast<int*>(base + 0x5E8);
            for (int k = 0; k < count && inst; ++k) {
                const uint8_t* e = inst + size_t(k) * 64;
                const float* t = reinterpret_cast<const float*>(e + 16);
                const float dx = t[0] - me.X, dy = t[1] - me.Y, dz = t[2] - me.Z;
                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                const uint16_t* m = reinterpret_cast<const uint16_t*>(e + 48);
                const UObject* mesh = m[0] < meshCount && meshes ? meshes[m[0]] : nullptr;
                if (!filter.empty() && !(mesh && Contains(GetName(mesh), filter))) continue;
                if (shown >= 400) continue;
                char row[400];
                snprintf(row, sizeof row, "%p #%d %s  at %.0f %.0f %.0f  s %.2f  meta %04X %04X %04X %04X %04X %04X %04X %04X  comp %p\n", (void*)e, k, mesh ? GetName(mesh).c_str() : "?",
                         t[0], t[1], t[2], t[4], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], (void*)o);
                out << row; ++shown;
            }
        }
        out << shown << " instances within " << radius << " of " << me.X << " " << me.Y << " " << me.Z << "\n";
        return out.str();
    }
    if (cmd == "mem") {  // mem <hexaddr> [bytes]: raw memory as qwords and floats (no object needed)
        const uint8_t* at = reinterpret_cast<const uint8_t*>(std::stoull(a[1], nullptr, 16));
        const size_t bytes = a.size() > 2 ? std::stoul(a[2], nullptr, 0) : 0x80;
        for (size_t i = 0; i + 16 <= bytes; i += 16) {
            uint64_t q[2]; float f[4];
            std::memcpy(q, at + i, 16); std::memcpy(f, at + i, 16);
            char row[160]; snprintf(row, sizeof row, "  +0x%04zX %016llX %016llX  %12.2f %12.2f %12.2f %12.2f\n", i, (unsigned long long)q[0], (unsigned long long)q[1], f[0], f[1], f[2], f[3]);
            out << row;
        }
        return out.str();
    }
    if (cmd == "load" && a.size() > 1) {  // load </Game/Path.Asset_C>: loads an asset (a class to spawn)
        UObject* x = FindObject(a[1].c_str()); if (!x) x = LoadAsset(a[1].c_str());
        return x ? "loaded " + FullName(x) : std::string("not found");
    }
    if (a.size() < 2) return "missing object";
    UObject* o = Resolve(a[1]);
    if (!o || !IsValid(o)) return "object not found: " + a[1];
    out << FullName(o) << " (" << GetName(o->Class) << ")\n";
    if (cmd == "fns") {
        const std::string filter = a.size() > 2 ? a[2] : "";
        for (auto cls = o->Class; cls; cls = SuperOf(cls))
            for (auto c = Field<UObject*>(cls, off::UStruct_Children); c; c = Field<UObject*>(c, off::UField_Next))
                if (GetName(c->Class) == "Function" && Contains(GetName(c), filter)) out << "  " << GetName(cls) << "::" << GetName(c) << "\n";
        return out.str();
    }
    if (cmd == "fn") {
        if (a.size() < 3) return "usage: fn <obj> <Function>";
        auto f = FunctionOf(o, a[2]); if (!f) return "function not found";
        for (auto p = Field<UObject*>(f, off::UStruct_Children); p; p = Field<UObject*>(p, off::UField_Next)) {
            out << "  " << TypeOf(p);
            if (auto inner = Field<UObject*>(p, kProp_Inner); (TypeOf(p) == "StructProperty" || TypeOf(p) == "ByteProperty" || TypeOf(p) == "ObjectProperty" || TypeOf(p) == "ArrayProperty") && inner) {
                out << "<" << GetName(inner);
                // Arrays: show the element property type and what it points at (struct / class).
                if (TypeOf(p) == "ArrayProperty") { out << ":" << TypeOf(inner); if (auto what = Field<UObject*>(inner, kProp_Inner)) out << "<" << GetName(what) << ">"; out << " size " << SizeOf(inner); }
                out << ">";
            }
            out << " " << GetName(p) << " @" << OffsetOf(p) << " size " << SizeOf(p) << "\n";
        }
        return out.str();
    }
    if (cmd == "props") {
        const std::string filter = a.size() > 2 ? a[2] : "";
        for (auto cls = o->Class; cls; cls = SuperOf(cls))
            for (auto p = Field<UObject*>(cls, off::UStruct_Children); p; p = Field<UObject*>(p, off::UField_Next))
                if (GetName(p->Class) != "Function" && Contains(GetName(p), filter))
                    out << "  " << Hex(OffsetOf(p)) << " " << GetName(p) << " = " << Format(p, reinterpret_cast<const uint8_t*>(o)) << "\n";
        return out.str();
    }
    if (cmd == "array") {
        if (a.size() < 3) return "usage: array <obj> <Property> [max]";
        UObject* p = nullptr; for (auto cls = o->Class; cls && !p; cls = SuperOf(cls)) p = FindProperty(cls, a[2].c_str());
        if (!p || TypeOf(p) != "ArrayProperty") return "not an array property";
        struct A { uint8_t* d; int32_t n, m; }; auto arr = Field<A>(o, OffsetOf(p));
        UObject* inner = Field<UObject*>(p, kProp_Inner);
        const int max = a.size() > 3 ? std::stoi(a[3]) : 100, size = SizeOf(inner);
        out << arr.n << " elements of " << TypeOf(inner) << " size " << size << "\n";
        for (int i = 0; i < arr.n && i < max; ++i) {
            // Format reads at base + offset; inner properties have offset 0.
            out << "  [" << i << "] " << Format(inner, arr.d + i * size) << "\n";
        }
        return out.str();
    }
    if (cmd == "set") {
        if (a.size() < 4) return "usage: set <obj> <Property> <value>";
        UObject* p = nullptr; for (auto cls = o->Class; cls && !p; cls = SuperOf(cls)) p = FindProperty(cls, a[2].c_str());
        if (!p) return "property not found";
        // Strings and texts allocated here would later be freed by the engine's allocator: params only.
        if (TypeOf(p) == "StrProperty" || TypeOf(p) == "TextProperty") return "set: string/text properties of objects are not supported (use call)";
        auto err = Assign(p, reinterpret_cast<uint8_t*>(o), a[3]);
        return err.empty() ? out.str() + "  " + GetName(p) + " = " + Format(p, reinterpret_cast<const uint8_t*>(o)) : err;
    }
    if (cmd == "whoref") {  // objects whose first bytes hold a pointer to <obj>
        const size_t bytes = a.size() > 2 ? std::stoul(a[2], nullptr, 0) : 0x600;
        int shown = 0;
        for (int i = 0, n = NumObjects(); i < n && shown < 60; ++i) {
            UObject* x = ObjectAt(i);
            if (!x || x == o) continue;
            const int at = FindPointer(x, bytes, o);
            if (at >= 0) { char hex[24]; snprintf(hex, sizeof hex, "  +0x%X ", at); out << hex << FullName(x) << " (" << GetName(x->Class) << ")\n"; ++shown; }
        }
        return out.str();
    }
    if (cmd == "ncall") {  // ncall <obj> <rva>: call DaysGone.exe+rva(obj) (one-argument native), print rax
        const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        using Fn1 = uint64_t(__fastcall*)(UObject*);
        const auto fn = reinterpret_cast<Fn1>(exe + std::stoull(a[2], nullptr, 16));
        const uint64_t r = fn(o);
        char row[64]; snprintf(row, sizeof row, "  rax = %016llX\n",(unsigned long long)r); out << row;
        return out.str();
    }
    if (cmd == "hex") {  // raw qwords of the object, with DaysGone.exe RVAs marked
        const size_t bytes = a.size() > 2 ? std::stoul(a[2], nullptr, 0) : 0x100;
        const auto base = reinterpret_cast<const uint64_t*>(o);
        const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        for (size_t i = 0; i * 8 < bytes; ++i) {
            const uint64_t v = base[i];
            char row[96];
            if (v >= exe && v < exe + 0x8000000) snprintf(row, sizeof row, "  +0x%03zX %016llX  exe+%llX\n", i * 8, (unsigned long long)v, (unsigned long long)(v - exe));
            else snprintf(row, sizeof row, "  +0x%03zX %016llX\n", i * 8, (unsigned long long)v);
            out << row;
        }
        return out.str();
    }
    if (cmd == "refs") {
        const size_t bytes = a.size() > 2 ? std::stoul(a[2], nullptr, 0) : 0x800;
        std::unordered_set<const void*> live;
        for (int i = 0, n = NumObjects(); i < n; ++i) if (auto x = ObjectAt(i)) live.insert(x);
        const auto base = reinterpret_cast<const uint8_t*>(o);
        for (size_t at = 0; at + 8 <= bytes; at += 8) {
            const auto v = *reinterpret_cast<UObject* const*>(base + at);
            if (v && v != o && live.count(v)) { char hex[32]; snprintf(hex, sizeof hex, "  0x%zX ", at); out << hex << FullName(v) << "\n"; }
        }
        return out.str();
    }
    if (cmd == "call") {
        if (a.size() < 3) return "usage: call <obj> <Function> [Param=value ...]";
        auto f = FunctionOf(o, a[2]); if (!f) return "function not found";
        Params params(f);
        for (size_t i = 3; i < a.size(); ++i) {
            const auto eq = a[i].find('=');
            if (eq == std::string::npos) return "argument without '=': " + a[i];
            auto p = FindProperty(f, a[i].substr(0, eq).c_str());
            if (!p) return "no parameter " + a[i].substr(0, eq);
            auto err = Assign(p, static_cast<uint8_t*>(params.Data()), a[i].substr(eq + 1));
            if (!err.empty()) return err;
        }
        ProcessEvent(o, f, params.Data());
        for (auto p = Field<UObject*>(f, off::UStruct_Children); p; p = Field<UObject*>(p, off::UField_Next))
            out << "  " << GetName(p) << " = " << Format(p, static_cast<const uint8_t*>(params.Data())) << "\n";
        return out.str();
    }
    return "unknown command " + cmd;
}

void RunInto(const std::string* line, std::string* result) { *result = Run(*line); }
int SafeRun(const std::string& line, std::string& result) {
    __try { RunInto(&line, &result); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return int(GetExceptionCode()); }
}
}  // namespace

void Update(UObject* pawn, UObject* controller, UObject* gameMode) {
    ctxPawn = pawn; ctxController = controller; ctxGameMode = gameMode;
    if (GetTickCount64() < nextPoll) return;
    nextPoll = GetTickCount64() + 150;
    if (dir.empty()) dir = fs::path(profile::Root()) / L"dev";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::ifstream in(dir / L"cmd.txt");
    int seq = 0; std::string line;
    if (!(in >> seq) || seq <= lastSeq) return;
    std::getline(in, line); std::getline(in, line);
    lastSeq = seq;
    std::string result;
    if (const int code = SafeRun(line, result)) { char b[64]; std::snprintf(b, sizeof(b), "EXCEPTION 0x%08X", code); result = b; }
    std::ofstream o(dir / L"out.tmp", std::ios::trunc);
    o << seq << "\n> " << line << "\n" << result << "\n";
    o.close();
    MoveFileExW((dir / L"out.tmp").c_str(), (dir / L"out.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace devcmd
