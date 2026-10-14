#include "ue.h"
#include "log.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <windows.h>

namespace ue {

namespace {

constexpr uintptr_t kRva_GUObjectArray = 0x4C12290;
constexpr uintptr_t kRva_GNames = 0x4C0DC48;
constexpr uintptr_t kRva_ProcessEvent = 0x1AC2A20; // UObject vtable slot 60
constexpr uintptr_t kRva_RegisterComponent = 0x278EDA0; // called by AActor::AddComponent
constexpr uintptr_t kRva_StaticConstructObject = 0x1AC6D30; // verified from UGameplayStatics::SpawnObject
constexpr uintptr_t kRva_StaticLoadObject = 0x1AC83A0;      // logs "Failed to find object" on failure

constexpr int32_t kItemFlag_Unreachable = 1 << 28;
constexpr int32_t kItemFlag_PendingKill = 1 << 29;

struct FUObjectItem {
    UObject* Object;
    int32_t Flags;
    int32_t SerialNumber;
};

uintptr_t g_base = 0;
FUObjectItem** g_objects = nullptr;  // &GUObjectArray.ObjObjects.Objects
int32_t* g_numObjects = nullptr;
uint8_t*** g_names = nullptr;        // TNameEntryArray* (chunk table)

using ProcessEventFn = void(__fastcall*)(UObject*, UObject*, void*);
ProcessEventFn g_processEvent = nullptr;

std::unordered_map<std::string, UObject*> g_classCache;
std::unordered_map<uint64_t, UObject*> g_fnCache;

const FUObjectItem* ItemAt(int32_t index)
{
    if (index < 0 || index >= *g_numObjects) return nullptr;
    return &(*g_objects)[index];
}

} // namespace

uintptr_t Base() { return g_base; }

bool Init()
{
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExpectedTimeDateStamp) {
        dg::Log("Unsupported DaysGone.exe build (TimeDateStamp %u, expected %u). Offsets must be re-derived.",
                nt->FileHeader.TimeDateStamp, kExpectedTimeDateStamp);
        return false;
    }
    g_objects = reinterpret_cast<FUObjectItem**>(g_base + kRva_GUObjectArray + 0x10);
    g_numObjects = reinterpret_cast<int32_t*>(g_base + kRva_GUObjectArray + 0x1C);
    g_names = reinterpret_cast<uint8_t***>(g_base + kRva_GNames);
    g_processEvent = reinterpret_cast<ProcessEventFn>(g_base + kRva_ProcessEvent);
    return true;
}

int32_t NumObjects() { return (g_objects && *g_objects) ? *g_numObjects : 0; }

UObject* ObjectAt(int32_t index)
{
    const FUObjectItem* it = ItemAt(index);
    return it ? it->Object : nullptr;
}

int32_t SerialAt(int32_t index)
{
    const FUObjectItem* it = ItemAt(index);
    return it ? it->SerialNumber : 0;
}

bool IsValid(const UObject* o)
{
    if (!o) return false;
    const FUObjectItem* it = ItemAt(o->InternalIndex);
    if (!it || it->Object != o) return false;
    return (it->Flags & (kItemFlag_PendingKill | kItemFlag_Unreachable)) == 0;
}

UObject* Resolve(const FWeakObjectPtr& w)
{
    const FUObjectItem* it = ItemAt(w.ObjectIndex);
    if (!it || !it->Object || it->SerialNumber != w.SerialNumber) return nullptr;
    return IsValid(it->Object) ? it->Object : nullptr;
}

std::string NameOf(FName n)
{
    uint8_t** chunks = *g_names;
    if (!chunks || n.Index < 0) return {};
    uint8_t** chunk = reinterpret_cast<uint8_t**>(chunks[n.Index >> 14]);
    if (!chunk) return {};
    uint8_t* entry = chunk[n.Index & 0x3FFF];
    if (!entry) return {};
    std::string s;
    // Bend fork: string data starts at +0x12, bit 0 of the header marks a wide entry.
    if (*reinterpret_cast<int32_t*>(entry) & 1) {
        for (auto w = reinterpret_cast<const wchar_t*>(entry + 0x12); *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
    } else {
        s = reinterpret_cast<const char*>(entry + 0x12);
    }
    if (n.Number) s += "_" + std::to_string(n.Number - 1);
    return s;
}

std::vector<std::string> SearchNames(const char* fragment, int max)
{
    std::vector<std::string> out;
    std::string want(fragment); for (auto& c : want) c = char(tolower(static_cast<unsigned char>(c)));
    uint8_t** chunks = *g_names;
    const int32_t count = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(chunks) + 0x400);
    for (int32_t i = 0; i < count && int(out.size()) < max; ++i) {
        uint8_t** chunk = reinterpret_cast<uint8_t**>(chunks[i >> 14]);
        uint8_t* e = chunk ? chunk[i & 0x3FFF] : nullptr;
        if (!e || (*reinterpret_cast<int32_t*>(e) & 1)) continue;
        std::string name(reinterpret_cast<const char*>(e + 0x12)), low = name;
        for (auto& c : low) c = char(tolower(static_cast<unsigned char>(c)));
        if (low.find(want) != std::string::npos) out.push_back(name);
    }
    return out;
}

FName FindName(const char* str)
{
    static std::unordered_map<std::string, int32_t> cache;
    if (auto it = cache.find(str); it != cache.end() && it->second>=0) return {it->second, 0};
    uint8_t** chunks = *g_names;
    const int32_t count = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(chunks) + 0x400);
    for (int32_t i = 0; i < count; ++i) {
        uint8_t** chunk = reinterpret_cast<uint8_t**>(chunks[i >> 14]);
        uint8_t* e = chunk ? chunk[i & 0x3FFF] : nullptr;
        if (!e || (*reinterpret_cast<int32_t*>(e) & 1)) continue;
        if (_stricmp(reinterpret_cast<const char*>(e + 0x12), str) == 0) {
            cache[str] = i;
            return {i, 0};
        }
    }
    cache[str] = -1;
    return {-1, 0};
}

bool GetBoolProp(const UObject* obj, const char* name, bool* found)
{
    if (found) *found = false;
    if (!obj) return false;
    for (const UObject* c = obj->Class; c; c = SuperOf(c)) {
        if (UObject* p = FindProperty(c, name)) {
            if (found) *found = true;
            const uint8_t* byte = reinterpret_cast<const uint8_t*>(obj) + Field<int32_t>(p, off::UProperty_Offset) +
                                  Field<uint8_t>(p, off::BoolProperty_ByteOffset);
            return (*byte & Field<uint8_t>(p, off::BoolProperty_FieldMask)) != 0;
        }
    }
    return false;
}

bool SetBoolProp(UObject* obj, const char* name, bool value)
{
    if (!obj) return false;
    for (const UObject* c = obj->Class; c; c = SuperOf(c)) {
        if (UObject* p = FindProperty(c, name)) {
            uint8_t* byte = reinterpret_cast<uint8_t*>(obj) + Field<int32_t>(p, off::UProperty_Offset) +
                            Field<uint8_t>(p, off::BoolProperty_ByteOffset);
            const uint8_t mask = Field<uint8_t>(p, off::BoolProperty_FieldMask);
            *byte = value ? (*byte | mask) : (*byte & ~mask);
            return true;
        }
    }
    return false;
}

UObject* FirstParam(UObject* fn)
{
    return fn ? Field<UObject*>(fn, 0x38) : nullptr; // UStruct::Children (parameters come first)
}

std::string GetName(const UObject* o) { return o ? NameOf(o->Name) : std::string("None"); }

std::string FullName(const UObject* o)
{
    std::string path;
    for (const UObject* it = o; it; it = it->Outer)
        path = path.empty() ? GetName(it) : GetName(it) + "." + path;
    return path;
}

bool IsA(const UObject* o, const UObject* cls)
{
    for (const UObject* c = o ? o->Class : nullptr; c; c = SuperOf(c))
        if (c == cls) return true;
    return false;
}

bool IsAByName(const UObject* o, const char* className)
{
    for (const UObject* c = o ? o->Class : nullptr; c; c = SuperOf(c))
        if (GetName(c) == className) return true;
    return false;
}

UObject* FindClass(const char* shortName)
{
    static std::unordered_map<std::string, uint64_t> missUntil;
    auto it = g_classCache.find(shortName);
    if (it != g_classCache.end() && IsValid(it->second)) return it->second;
    const uint64_t nowMs = GetTickCount64();
    if (auto m = missUntil.find(shortName); m != missUntil.end() && nowMs < m->second) return nullptr;
    const auto wanted = FindName(shortName);
    for (int32_t i = 0, n = NumObjects(); wanted.Index >= 0 && i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || !o->Class) continue;
        if (o->Name.Index != wanted.Index || o->Name.Number != 0) continue;
        std::string cn = GetName(o->Class);
        if (cn == "Class" || cn == "BlueprintGeneratedClass" || cn == "WidgetBlueprintGeneratedClass") {
            g_classCache[shortName] = o;
            return o;
        }
    }
    missUntil[shortName] = nowMs + 5000; // not loaded (yet); don't rescan every frame
    return nullptr;
}

UObject* FindRuntimeInstance(const char* className) {
    struct Entry { FWeakObjectPtr object{-1,0}; uint64_t retry=0; };
    static thread_local std::unordered_map<std::string,Entry> cache;
    auto& entry=cache[className];
    if(auto o=Resolve(entry.object);IsValid(o) && IsAByName(o,className)) return o;
    const auto now=GetTickCount64(); if(now<entry.retry) return nullptr;
    entry.retry=now+1000;
    auto cls=FindClass(className); if(!cls) return nullptr;
    for(int i=0,n=NumObjects();i<n;++i) {
        auto o=ObjectAt(i); if(!IsValid(o) || !IsA(o,cls)) continue;
        bool runtime=true;
        for(auto outer=o;outer;outer=outer->Outer) {
            if(outer->ObjectFlags&0x30) { runtime=false; break; }
            const auto type=GetName(outer->Class);
            if(type=="Class" || type=="BlueprintGeneratedClass" || type=="WidgetBlueprintGeneratedClass") { runtime=false; break; }
        }
        if(runtime) { entry.object={i,SerialAt(i)}; return o; }
    }
    return nullptr;
}

UObject* FindObject(const char* fullPath)
{
    static std::unordered_map<std::string, UObject*> cache;
    if (auto it = cache.find(fullPath); it != cache.end() && IsValid(it->second)) return it->second;

    // Compare name indices instead of strings: resolving the short name once makes the scan cheap.
    const char* dot = strrchr(fullPath, '.');
    std::string shortName = dot ? dot + 1 : fullPath;
    FName want = FindName(shortName.c_str());
    if (want.Index < 0) {
        const size_t us = shortName.find_last_of('_'); // "Name_3" is FName{Name, 4}
        if (us == std::string::npos || shortName.find_first_not_of("0123456789", us + 1) != std::string::npos) return nullptr;
        want = FindName(shortName.substr(0, us).c_str());
        if (want.Index < 0) return nullptr;
        want.Number = atoi(shortName.c_str() + us + 1) + 1;
    }
    for (int32_t i = 0, n = NumObjects(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o->Name.Index != want.Index || o->Name.Number != want.Number) continue;
        if (FullName(o) == fullPath && IsValid(o)) { cache[fullPath] = o; return o; }
    }
    return nullptr;
}

UObject* FindFunction(const UObject* cls, const char* name)
{
    uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(cls)) << 16) ^ std::hash<std::string>{}(name);
    if (auto it = g_fnCache.find(key); it != g_fnCache.end()) return it->second;
    for (const UObject* c = cls; c; c = SuperOf(c)) {
        for (UObject* f = Field<UObject*>(c, off::UStruct_Children); f; f = Field<UObject*>(f, off::UField_Next)) {
            if (GetName(f) == name && GetName(f->Class) == "Function") {
                g_fnCache[key] = f;
                return f;
            }
        }
    }
    g_fnCache[key] = nullptr;
    return nullptr;
}

UObject* FindProperty(const UObject* strct, const char* name)
{
    if (!IsValid(strct)) return nullptr;
    struct Entry { int serial=0; std::unordered_map<std::string,UObject*> fields; };
    static thread_local std::unordered_map<const UObject*,Entry> cache;
    auto& entry=cache[strct];
    const int serial=SerialAt(strct->InternalIndex);
    if(entry.serial!=serial || entry.fields.empty()) {
        entry.serial=serial; entry.fields.clear();
        for(auto p=Field<UObject*>(strct,off::UStruct_Children);p;p=Field<UObject*>(p,off::UField_Next))
            entry.fields.emplace(GetName(p),p);
    }
    const auto it=entry.fields.find(name);
    return it==entry.fields.end()?nullptr:it->second;
}

void ProcessEvent(UObject* obj, UObject* fn, void* params) { g_processEvent(obj, fn, params); }

bool RemoveArrayEntry(UObject* object, const char* propertyName, int32_t index)
{
    if (!IsValid(object)) return false;
    UObject* property = nullptr;
    for (auto cls = object->Class; cls && !property; cls = SuperOf(cls)) property = FindProperty(cls, propertyName);
    if (!property || GetName(property->Class) != "ArrayProperty") return false;
    auto inner = Field<UObject*>(property, 0x78);
    if (!IsValid(inner)) return false;
    struct Array { void* data; int32_t num, max; };
    auto& array = Field<Array>(object, Field<int32_t>(property, off::UProperty_Offset));
    if (!array.data || index < 0 || index >= array.num || array.num > array.max || array.max > 32768) return false;
    // Verified from execArray_Remove at RVA 0x2E29260 for the supported executable:
    // RCX=array address, RDX=UArrayProperty, R8D=index. It calls FScriptArrayHelper::RemoveValues.
    constexpr uintptr_t rva = 0x2917F70;
    constexpr uint8_t signature[]{0x48, 0x85, 0xC9, 0x0F, 0x84};
    if (memcmp(reinterpret_cast<void*>(Base() + rva), signature, sizeof(signature)) != 0) return false;
    const int32_t before = array.num;
    using RemoveFn = void(__fastcall*)(void*, UObject*, int32_t);
    reinterpret_cast<RemoveFn>(Base() + rva)(&array, property, index);
    return array.num == before - 1;
}

UObject* NewObject(UObject* cls, UObject* outer)
{
    // Bend fork signature: (Class, Outer, Name, Flags, InternalFlags, Template, bCopyTransients, InstanceGraph, bAssumeArchetype)
    using Fn = UObject*(__fastcall*)(UObject*, UObject*, FName, int32_t, int32_t, UObject*, bool, void*, bool);
    return reinterpret_cast<Fn>(g_base + kRva_StaticConstructObject)(cls, outer, FName{0, 0}, 0x1000000, 0, nullptr, false, nullptr, true);
}

UObject* NewObjectNamed(UObject* cls, UObject* outer, FName name)
{
    using Fn = UObject*(__fastcall*)(UObject*, UObject*, FName, int32_t, int32_t, UObject*, bool, void*, bool);
    return reinterpret_cast<Fn>(g_base + kRva_StaticConstructObject)(cls, outer, name, 0x1000000, 0, nullptr, false, nullptr, true);
}

void AddToRoot(UObject* o)
{
    if (!o) return;
    if (const FUObjectItem* it = ItemAt(o->InternalIndex)) const_cast<FUObjectItem*>(it)->Flags |= (1 << 30); // RootSet
}

UObject* LoadAsset(const char* fullPath)
{
    if (!fullPath || !*fullPath) return nullptr;
    static std::unordered_map<std::string, UObject*> loaded;
    if (auto it = loaded.find(fullPath); it != loaded.end() && IsValid(it->second)) return it->second;
    if (UObject* o = FindObject(fullPath)) return loaded[fullPath] = o;
    static std::unordered_map<std::string, uint64_t> failedUntil;
    const uint64_t now = GetTickCount64();
    if (auto it = failedUntil.find(fullPath); it != failedUntil.end() && now < it->second) return nullptr;
    // UObject* StaticLoadObject(UClass*, UObject* Outer, const TCHAR* Name, const TCHAR* Filename,
    //                           uint32 LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation)
    using Fn = UObject*(__fastcall*)(UObject*, UObject*, const wchar_t*, const wchar_t*, uint32_t, void*, bool);
    static UObject* objectClass = nullptr;
    if (!objectClass) objectClass = FindObject("/Script/CoreUObject.Object");
    if (!objectClass) return nullptr;
    std::wstring w(fullPath, fullPath + strlen(fullPath));
    UObject* o = reinterpret_cast<Fn>(g_base + kRva_StaticLoadObject)(objectClass, nullptr, w.c_str(), nullptr, 0, nullptr, true);
    if (o) { dg::Log("Loaded asset %s", fullPath); loaded[fullPath] = o; }
    else { dg::Log("Could not load asset %s", fullPath); failedUntil[fullPath] = now + 30000; }
    return o;
}

void RegisterComponent(UObject* component)
{
    reinterpret_cast<void(__fastcall*)(UObject*)>(g_base + kRva_RegisterComponent)(component);
}

void Params::MissingParam(const char* name) const
{
    dg::Log("Param '%s' not found on %s", name, fn_ ? GetName(fn_).c_str() : "<null function>");
}

bool Params::SetBool(const char* name, bool v)
{
    UObject* p = FindProperty(fn_, name);
    if (!p) { MissingParam(name); return false; }
    uint8_t* byte = buf_ + Field<int32_t>(p, off::UProperty_Offset) + Field<uint8_t>(p, off::BoolProperty_ByteOffset);
    uint8_t mask = Field<uint8_t>(p, off::BoolProperty_FieldMask);
    *byte = v ? (*byte | mask) : (*byte & ~mask);
    return true;
}

bool Params::GetBool(const char* name)
{
    UObject* p = FindProperty(fn_, name);
    if (!p) return false;
    uint8_t* byte = buf_ + Field<int32_t>(p, off::UProperty_Offset) + Field<uint8_t>(p, off::BoolProperty_ByteOffset);
    return (*byte & Field<uint8_t>(p, off::BoolProperty_FieldMask)) != 0;
}

UObject* Fn(UObject* obj, const char* fnName)
{
    return obj ? FindFunction(obj->Class, fnName) : nullptr;
}

bool Call(UObject* obj, const char* fnName, Params* p)
{
    if (!IsValid(obj)) return false;
    UObject* fn = p ? p->Function() : Fn(obj, fnName);
    if (!fn) {
        dg::Log("Call: %s not found on %s", fnName, GetName(obj->Class).c_str());
        return false;
    }
    alignas(16) uint8_t empty[16] = {};
    ProcessEvent(obj, fn, p ? p->Data() : empty);
    return true;
}

// ---- math ----

constexpr float kRadToDeg = 57.2957795f;
constexpr float kDegToRad = 0.0174532925f;

FRotator QuatToRotator(const FQuat& q)
{
    // Same convention as FQuat::Rotator() in UE4.
    const float singularity = q.Z * q.X - q.W * q.Y;
    const float yawY = 2.f * (q.W * q.Z + q.X * q.Y);
    const float yawX = 1.f - 2.f * (q.Y * q.Y + q.Z * q.Z);
    FRotator r;
    if (singularity < -0.4999995f) {
        r.Pitch = -90.f;
        r.Yaw = std::atan2(yawY, yawX) * kRadToDeg;
        r.Roll = -r.Yaw - 2.f * std::atan2(q.X, q.W) * kRadToDeg;
    } else if (singularity > 0.4999995f) {
        r.Pitch = 90.f;
        r.Yaw = std::atan2(yawY, yawX) * kRadToDeg;
        r.Roll = r.Yaw - 2.f * std::atan2(q.X, q.W) * kRadToDeg;
    } else {
        r.Pitch = std::asin(2.f * singularity) * kRadToDeg;
        r.Yaw = std::atan2(yawY, yawX) * kRadToDeg;
        r.Roll = std::atan2(-2.f * (q.W * q.X + q.Y * q.Z), 1.f - 2.f * (q.X * q.X + q.Y * q.Y)) * kRadToDeg;
    }
    return r;
}

FQuat RotatorToQuat(const FRotator& r)
{
    const float p = r.Pitch * kDegToRad * 0.5f, y = r.Yaw * kDegToRad * 0.5f, ro = r.Roll * kDegToRad * 0.5f;
    const float SP = std::sin(p), CP = std::cos(p), SY = std::sin(y), CY = std::cos(y), SR = std::sin(ro), CR = std::cos(ro);
    return { CR * SP * SY - SR * CP * CY, -CR * SP * CY - SR * CP * SY, CR * CP * SY - SR * SP * CY, CR * CP * CY + SR * SP * SY };
}

FQuat Slerp(const FQuat& a, FQuat b, float t)
{
    float cosom = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    if (cosom < 0.f) { cosom = -cosom; b = {-b.X, -b.Y, -b.Z, -b.W}; }
    float s0 = 1.f - t, s1 = t;
    if (cosom < 0.9999f) {
        const float omega = std::acos(cosom), sinom = 1.f / std::sin(omega);
        s0 = std::sin((1.f - t) * omega) * sinom;
        s1 = std::sin(t * omega) * sinom;
    }
    FQuat r{ s0 * a.X + s1 * b.X, s0 * a.Y + s1 * b.Y, s0 * a.Z + s1 * b.Z, s0 * a.W + s1 * b.W };
    const float len = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z + r.W * r.W);
    if (len > 0.f) { r.X /= len; r.Y /= len; r.Z /= len; r.W /= len; }
    return r;
}

} // namespace ue
