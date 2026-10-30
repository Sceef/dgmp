#include "playeranim.h"
#include "log.h"
#include <windows.h>

using namespace ue;

namespace playeranim {
namespace {
struct Want { bool pending = false; std::string path; uint8_t flags = 0; float rate = 1; } want;
struct Playing { bool on = false; std::string path; uint8_t flags = 0; UObject* montage = nullptr; UObject* instance = nullptr; bool frozen = false; } now;

UObject* AnimInstance(UObject* pawn) {
    UObject* mesh = IsValid(pawn) ? Field<UObject*>(pawn, 0x3B0) : nullptr;  // Character.Mesh (CharacterMesh0)
    auto f = IsValid(mesh) ? Fn(mesh, "GetAnimInstance") : nullptr; if (!f) return nullptr;
    Params p(f); Call(mesh, "GetAnimInstance", &p);
    UObject* a = p.Get<UObject*>("ReturnValue");
    return IsValid(a) ? a : nullptr;
}
void Freeze(UObject* controller, bool on) {
    if (auto f = IsValid(controller) ? Fn(controller, "SetIgnoreMoveInput") : nullptr) { Params p(f); p.SetBool("bNewMoveInput", on); Call(controller, "SetIgnoreMoveInput", &p); }
}
// Ends what plays; `reason` for the server (0 done, 1 moved); -1: the server asked, nothing to report.
void Stop(UObject* controller, net::Client* client, int reason) {
    if (!now.on) return;
    if (IsValid(now.instance)) if (auto f = Fn(now.instance, "StopSlotAnimation")) {
        Params p(f); p.Set<float>("InBlendOutTime", 0.25f); p.Set("SlotNodeName", FindName("FullBody")); Call(now.instance, "StopSlotAnimation", &p);
    }
    if (now.frozen) Freeze(controller, false);
    if (client && reason >= 0) client->SendAnimEnd(uint8_t(reason));
    dg::Log("Player animation ended (%d): %s", reason, now.path.c_str());
    now = Playing{};
}
bool MoveKeyDown() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    for (int k : {int('W'), int('A'), int('S'), int('D'), VK_SPACE}) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}
}  // namespace

void OnPlay(const std::string& path, uint8_t flags, float rate) { want = {true, path, flags, rate > 0 ? rate : 1.f}; }

void Reset() { want = Want{}; now = Playing{}; }

bool IsPlaying(const std::string& path) { return (want.pending && want.path == path) || (now.on && now.path == path); }

void Update(UObject* pawn, UObject* controller, net::Client& client, bool canAct, bool dead) {
    if (dead) { if (now.on) Stop(controller, &client, 0); want.pending = false; return; }
    if (want.pending && IsValid(pawn)) {
        want.pending = false;
        Stop(controller, nullptr, -1);
        if (want.path.empty()) return;
        UObject* asset = FindObject(want.path.c_str());
        if (!asset) asset = LoadAsset(want.path.c_str());
        UObject* instance = AnimInstance(pawn);
        if (!asset || !instance) { dg::Log("Player animation: cannot play %s", want.path.c_str()); client.SendAnimEnd(0); return; }
        AddToRoot(asset);  // kept: a collected animation under a playing montage would crash
        const bool loop = want.flags & net::AnimLoop;
        if (auto f = Fn(instance, "PlaySlotAnimationAsDynamicMontage")) {
            Params p(f);
            p.Set("Asset", asset); p.Set("SlotNodeName", FindName("FullBody"));
            p.Set<float>("BlendInTime", 0.25f); p.Set<float>("BlendOutTime", 0.25f); p.Set<float>("InPlayRate", want.rate);
            p.Set<int>("LoopCount", loop ? 100000 : 1); p.Set<float>("BlendOutTriggerTime", -1.f);
            Call(instance, "PlaySlotAnimationAsDynamicMontage", &p);
            now.montage = p.Get<UObject*>("ReturnValue");
        }
        if (!IsValid(now.montage)) { dg::Log("Player animation: no montage for %s", want.path.c_str()); client.SendAnimEnd(0); now = Playing{}; return; }
        now.on = true; now.path = want.path; now.flags = want.flags; now.instance = instance;
        if (want.flags & net::AnimFreeze) { Freeze(controller, true); now.frozen = true; }
        dg::Log("Player animation: %s%s", want.path.c_str(), loop ? " (loop)" : "");
        return;
    }
    if (!now.on) return;
    // Over by itself (played once), or the player walks off.
    bool playing = false;
    if (IsValid(now.instance)) if (auto f = Fn(now.instance, "Montage_IsPlaying")) { Params p(f); p.Set("Montage", now.montage); Call(now.instance, "Montage_IsPlaying", &p); playing = p.GetBool("ReturnValue"); }
    if (!playing) { const bool frozen = now.frozen; now.instance = nullptr; if (frozen) Freeze(controller, false); now.frozen = false; client.SendAnimEnd(0); dg::Log("Player animation done: %s", now.path.c_str()); now = Playing{}; return; }
    if ((now.flags & net::AnimStopOnMove) && canAct && MoveKeyDown()) Stop(controller, &client, 1);
}

}  // namespace playeranim
