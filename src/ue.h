#pragma once
// Minimal reflection layer over Days Gone's UE 4.11 (Bend fork).
// Offsets come from DGCoop/notes/OFFSETS.md and are only valid for exe TimeDateStamp 1752517958.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ue {

constexpr uint32_t kExpectedTimeDateStamp = 1752517958;

struct FName { int32_t Index; int32_t Number; };
struct FString { const wchar_t* Data; int32_t Num; int32_t Max; };
struct FVector { float X, Y, Z; };
struct FRotator { float Pitch, Yaw, Roll; };
struct FQuat { float X, Y, Z, W; };
struct alignas(16) FTransform {
    FQuat Rotation{0, 0, 0, 1};
    FVector Translation{0, 0, 0}; float pad0 = 0;
    FVector Scale3D{1, 1, 1}; float pad1 = 0;
};
struct FWeakObjectPtr { int32_t ObjectIndex; int32_t SerialNumber; };

struct UObject {
    void** VTable;
    int32_t ObjectFlags;
    int32_t InternalIndex;
    UObject* Class;
    FName Name;
    UObject* Outer;
};

// Field offsets (see notes/OFFSETS.md and notes/sdk_dump.txt)
namespace off {
constexpr int UStruct_Super = 0x30, UStruct_Children = 0x38, UStruct_Size = 0x40;
constexpr int UField_Next = 0x28;
constexpr int UProperty_ElementSize = 0x34, UProperty_Offset = 0x50;
constexpr int BoolProperty_ByteOffset = 0x79, BoolProperty_FieldMask = 0x7B;
constexpr int Actor_RootComponent = 0x168;
constexpr int Scene_AttachParent = 0xD0, Scene_ComponentVelocity = 0x1E0;
constexpr int Scene_C2W_Rot = 0x120, Scene_C2W_Trans = 0x130;  // native FTransform ComponentToWorld
constexpr int Skinned_SkeletalMesh = 0x688, Skinned_MasterPose = 0x690;
constexpr int Pawn_Controller = 0x380;
constexpr int PC_CameraManager = 0x3D8;
constexpr int CamMgr_POV_Location = 0x3C0 + 0x10;
constexpr int Ride_VehiclePawn = 0xD0;
constexpr int SkelMeshActor_Component = 0x358;
constexpr int TextRenderActor_Component = 0x348;
}

bool Init();
uintptr_t Base();

int32_t NumObjects();
UObject* ObjectAt(int32_t index);
int32_t SerialAt(int32_t index);
bool IsValid(const UObject* o);
UObject* Resolve(const FWeakObjectPtr& w);

std::string NameOf(FName n);
// Looks an existing name up in GNames (no new names are added). Index -1 if absent.
FName FindName(const char* str);
// Names in the name table containing `fragment` (case-insensitive), for research tools.
std::vector<std::string> SearchNames(const char* fragment, int max);
// Reads a (possibly bitfield) bool UPROPERTY by name from obj's class chain.
bool SetBoolProp(UObject* obj, const char* name, bool value);
// First parameter property of a function (for calls whose parameter names we don't know).
UObject* FirstParam(UObject* fn);
bool GetBoolProp(const UObject* obj, const char* name, bool* found = nullptr);
std::string GetName(const UObject* o);
std::string FullName(const UObject* o);

inline UObject* ClassOf(const UObject* o) { return o->Class; }
inline UObject* SuperOf(const UObject* s) { return *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(s) + off::UStruct_Super); }
bool IsA(const UObject* o, const UObject* cls);
bool IsAByName(const UObject* o, const char* className);

UObject* FindClass(const char* shortName);
UObject* FindRuntimeInstance(const char* className);
UObject* FindObject(const char* fullPath);
UObject* FindFunction(const UObject* cls, const char* name);
UObject* FindProperty(const UObject* strct, const char* name);
// Uses Kismet's native generic Array_Remove, including destruction of FText/struct entries.
bool RemoveArrayEntry(UObject* object, const char* propertyName, int32_t index);

template <typename T>
inline T& Field(const void* o, int offset) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(o) + offset); }

void ProcessEvent(UObject* obj, UObject* fn, void* params);
// Native UActorComponent::RegisterComponent (creates render/physics state in the owner's world).
void RegisterComponent(UObject* component);
// Native StaticConstructObject_Internal, called exactly like UGameplayStatics::SpawnObject does
// (which itself refuses actors and components).
UObject* NewObject(UObject* cls, UObject* outer);
// Same, with a given name (the name must exist in the name table).
UObject* NewObjectNamed(UObject* cls, UObject* outer, FName name);
// Keeps an object we created alive (GC root).
void AddToRoot(UObject* o);
// Finds an asset by full path ("/Game/Dir/Asset.Asset"), loading it from disk if it isn't in memory.
UObject* LoadAsset(const char* fullPath);

// Parameter frame for a UFunction, addressed by parameter name.
class Params {
public:
    explicit Params(UObject* fn) : fn_(fn) { std::memset(buf_, 0, sizeof(buf_)); }
    UObject* Function() const { return fn_; }
    void* Data() { return buf_; }

    template <typename T>
    bool Set(const char* name, const T& v)
    {
        UObject* p = FindProperty(fn_, name);
        if (!p) { MissingParam(name); return false; }
        std::memcpy(buf_ + Field<int32_t>(p, off::UProperty_Offset), &v, sizeof(T));
        return true;
    }
    bool SetBool(const char* name, bool v);

    template <typename T>
    T Get(const char* name)
    {
        T v{};
        if (UObject* p = FindProperty(fn_, name))
            std::memcpy(&v, buf_ + Field<int32_t>(p, off::UProperty_Offset), sizeof(T));
        return v;
    }
    bool GetBool(const char* name);

private:
    void MissingParam(const char* name) const;
    alignas(16) uint8_t buf_[0x400];
    UObject* fn_;
};

// Looks up `fnName` on the object's class chain and calls it. Returns false if not found.
bool Call(UObject* obj, const char* fnName, Params* p = nullptr);
UObject* Fn(UObject* obj, const char* fnName);

// Math helpers
FRotator QuatToRotator(const FQuat& q);
FQuat RotatorToQuat(const FRotator& r);
FQuat Slerp(const FQuat& a, FQuat b, float t);

} // namespace ue
