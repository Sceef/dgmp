#include "audio.h"
#include "log.h"
#include "ue.h"
#include <windows.h>

using namespace ue;

namespace audio {
namespace {
bool muted = false;       // what we last applied
ULONGLONG nextApply = 0;

float Setting(const char* name) {
    // Engine.GameUserSettings holds the player's volumes (0..1).
    static UObject* engine = nullptr;
    if (!IsValid(engine)) engine = FindRuntimeInstance("GameEngine");
    UObject* settings = nullptr;
    if (IsValid(engine))
        for (auto cls = engine->Class; cls && !settings; cls = SuperOf(cls))
            if (auto p = FindProperty(cls, "GameUserSettings")) settings = Field<UObject*>(engine, Field<int>(p, off::UProperty_Offset));
    if (!IsValid(settings)) return 1.f;
    for (auto cls = settings->Class; cls; cls = SuperOf(cls))
        if (auto p = FindProperty(cls, name)) return Field<float>(settings, Field<int>(p, off::UProperty_Offset));
    return 1.f;
}

bool Apply(bool mute) {
    UObject* manager = FindRuntimeInstance("BendAudioManager");
    if (!IsValid(manager)) return false;
    const float fade = mute ? 0.f : 1.5f;
    const std::pair<const char*, const char*> buses[] = {{"SetSFXAudioVolume", "SfxVolume"}, {"SetMusicAudioVolume", "MusicVolume"}, {"SetVoiceAudioVolume", "SpeechVolume"}};
    for (auto& [fn, setting] : buses) {
        auto f = Fn(manager, fn); if (!f) continue;
        Params p(f); p.Set("NewVolumeLevel", mute ? 0.f : Setting(setting)); p.Set("FadeTime", fade);
        if (FindProperty(f, "calledFromSlider")) p.SetBool("calledFromSlider", false);
        Call(manager, fn, &p);
    }
    return true;
}
}  // namespace

void Update(bool wantMuted) {
    const ULONGLONG now = GetTickCount64();
    if (wantMuted) {
        // Re-applied while booting: the menu and the loaded world each bring their own audio manager.
        if (now < nextApply) return;
        nextApply = now + 500;
        if (Apply(true) && !muted) { muted = true; dg::Log("Audio muted while the world boots"); }
    } else if (muted) {
        if (Apply(false)) { muted = false; dg::Log("Audio restored to the player's settings"); }
    }
}

}  // namespace audio
