#include "voice.h"
#include "log.h"
#include "lang.h"
#include "overlay.h"
#include "profile.h"
#include "imgui.h"
#include <windows.h>
#include <xaudio2.h>
#include <x3daudio.h>
#pragma comment(lib, "xaudio2.lib")  // xaudio2_9.dll (Windows 10+): XAudio2 and X3DAudio
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <type_traits>
#include <vector>

using namespace ue;

namespace voice {
namespace {
constexpr uint32_t kRate = 24000;         // what Steam decompresses to for us (mono, 16 bit)
constexpr double kSendEvery = 0.05;       // seconds of voice in one network frame
constexpr double kPrebuffer = 0.08;       // a talk starts once this much arrived (network jitter)
constexpr double kTalkingFor = 0.3;       // a voice counts as talking this long after its last frame

double Clock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- settings (DGMP.ini [Voice])
struct Settings { int talkKey = 'N', radioKey = 'K'; float volume = 1.f; bool microphone = true, openMic = false; bool loaded = false; } settings;

int ParseKey(std::wstring k, int fallback) {
    for (auto& ch : k) ch = wchar_t(towupper(ch));
    if (k.size() == 1 && ((k[0] >= L'A' && k[0] <= L'Z') || (k[0] >= L'0' && k[0] <= L'9'))) return int(k[0]);
    if (k.size() >= 2 && k[0] == L'F' && iswdigit(k[1])) { const int n = _wtoi(k.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    static const std::pair<const wchar_t*, int> names[] = {
        {L"CAPSLOCK", VK_CAPITAL}, {L"CAPS", VK_CAPITAL}, {L"MOUSE4", VK_XBUTTON1}, {L"MOUSE5", VK_XBUTTON2}, {L"MOUSE3", VK_MBUTTON},
        {L"LALT", VK_LMENU}, {L"RALT", VK_RMENU}, {L"LCTRL", VK_LCONTROL}, {L"RCTRL", VK_RCONTROL}, {L"LSHIFT", VK_LSHIFT}, {L"RSHIFT", VK_RSHIFT},
        {L"TILDE", VK_OEM_3}, {L"`", VK_OEM_3}, {L"NUMPAD0", VK_NUMPAD0}, {L"INSERT", VK_INSERT}, {L"HOME", VK_HOME}, {L"END", VK_END},
        {L"PAGEUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT}, {L"NONE", 0}};
    for (auto& [name, vk] : names) if (k == name) return vk;
    if (k.rfind(L"0X", 0) == 0) return int(wcstol(k.c_str(), nullptr, 16));
    return fallback;
}
std::string KeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
    case VK_CAPITAL: return "Caps Lock"; case VK_XBUTTON1: return "Mouse 4"; case VK_XBUTTON2: return "Mouse 5"; case VK_MBUTTON: return "Mouse 3";
    case VK_LMENU: return "Left Alt"; case VK_RMENU: return "Right Alt"; case VK_LCONTROL: return "Left Ctrl"; case VK_RCONTROL: return "Right Ctrl";
    case VK_OEM_3: return "~"; default: return "?";
    }
}
void LoadSettings() {
    if (settings.loaded) return;
    settings.loaded = true;
    const std::wstring ini = profile::Root() + L"\\DGMP.ini";
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Voice", L"TalkKey", L"N", buf, 64, ini.c_str()); settings.talkKey = ParseKey(buf, 'N');
    GetPrivateProfileStringW(L"Voice", L"RadioKey", L"K", buf, 64, ini.c_str()); settings.radioKey = ParseKey(buf, 'K');
    settings.volume = std::clamp(int(GetPrivateProfileIntW(L"Voice", L"Volume", 100, ini.c_str())), 0, 200) / 100.f;
    settings.microphone = GetPrivateProfileIntW(L"Voice", L"Microphone", 1, ini.c_str()) != 0;
    settings.openMic = GetPrivateProfileIntW(L"Voice", L"OpenMic", 0, ini.c_str()) != 0;
    dg::Log("Voice: talk %s, radio %s, volume %.0f%%, microphone %s", KeyName(settings.talkKey).c_str(), KeyName(settings.radioKey).c_str(),
            settings.volume * 100, !settings.microphone ? "off" : settings.openMic ? "always on" : "on");
}

// ---------------------------------------------------------------- Steam voice (the game's steam_api64.dll, flat API 1.48)
enum EVoiceResult { kVoiceOK = 0, kVoiceNotRecording = 2, kVoiceNoData = 3, kVoiceBufferTooSmall = 4 };
struct SteamVoice {
    void* (__cdecl* user)() = nullptr;
    void (__cdecl* start)(void*) = nullptr;
    void (__cdecl* stop)(void*) = nullptr;
    int (__cdecl* available)(void*, uint32_t*, uint32_t*, uint32_t) = nullptr;
    int (__cdecl* get)(void*, bool, void*, uint32_t, uint32_t*, bool, void*, uint32_t, uint32_t*, uint32_t) = nullptr;
    int (__cdecl* decompress)(void*, const void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t) = nullptr;
    bool tried = false;
    bool Ready() {
        if (!tried) {
            HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
            if (!dll) return false;  // not loaded yet: try again later
            tried = true;
            auto load = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name)); return fn != nullptr; };
            const bool ok = load(user, "SteamAPI_SteamUser_v020") & load(start, "SteamAPI_ISteamUser_StartVoiceRecording") & load(stop, "SteamAPI_ISteamUser_StopVoiceRecording")
                          & load(available, "SteamAPI_ISteamUser_GetAvailableVoice") & load(get, "SteamAPI_ISteamUser_GetVoice")
                          & load(decompress, "SteamAPI_ISteamUser_DecompressVoice");
            if (!ok) { user = nullptr; dg::Log("Voice: Steam voice functions missing"); }
        }
        return user && user();
    }
} steamVoice;

// ---------------------------------------------------------------- capture
struct Capture {
    bool recording = false;   // StartVoiceRecording called
    double drainUntil = 0;    // after StopVoiceRecording, Steam still hands out the last of it
    int key = 0;              // net::VoiceKey of this talk
    double nextPoll = 0;
    bool warned = false;
    double since = 0; int frames = 0; size_t bytes = 0;  // this talk, for the log
} cap;
void LogTalk(double now) {
    if (cap.frames) dg::Log("Voice: said %.1f s, %d frames, %.1f KB", now - cap.since, cap.frames, cap.bytes / 1024.0);
    cap.since = now; cap.frames = 0; cap.bytes = 0;
}
std::vector<uint8_t> captureBuf(8192);

void StopCapture(double now) {
    if (!cap.recording) return;
    if (steamVoice.Ready()) steamVoice.stop(steamVoice.user());
    cap.recording = false; cap.drainUntil = now + 0.5;
    LogTalk(now);
}

void PollCapture(net::Client& client, double now) {
    if ((!cap.recording && now >= cap.drainUntil) || now < cap.nextPoll || !steamVoice.Ready()) return;
    cap.nextPoll = now + kSendEvery;
    void* user = steamVoice.user();
    uint32_t compressed = 0;
    int r = steamVoice.available(user, &compressed, nullptr, 0);
    if (r == kVoiceNotRecording && !cap.recording) { cap.drainUntil = 0; return; }
    if (r != kVoiceOK || !compressed) return;
    if (compressed > captureBuf.size()) captureBuf.resize(compressed);
    uint32_t written = 0;
    r = steamVoice.get(user, true, captureBuf.data(), uint32_t(captureBuf.size()), &written, false, nullptr, 0, nullptr, 0);
    if (r != kVoiceOK || !written) return;
    if (written > net::kVoiceFrameMax) { dg::Log("Voice: frame of %u bytes dropped", written); return; }
    client.SendVoice(net::VoiceKey(cap.key), net::VoiceCodec::Steam, captureBuf.data(), written);
    ++cap.frames; cap.bytes += written;
    if (cap.recording && now - cap.since >= 30) LogTalk(now);  // an open microphone: now and then
}

// ---------------------------------------------------------------- playback
IXAudio2* xa = nullptr;
IXAudio2MasteringVoice* master = nullptr;
X3DAUDIO_HANDLE x3d{};
UINT32 outChannels = 2;
bool audioTried = false;

bool AudioReady() {
    if (xa) return true;
    if (audioTried) return false;
    audioTried = true;
    if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { dg::Log("Voice: XAudio2 unavailable"); xa = nullptr; return false; }
    if (FAILED(xa->CreateMasteringVoice(&master))) { dg::Log("Voice: no audio output"); xa->Release(); xa = nullptr; return false; }
    DWORD mask = 0; master->GetChannelMask(&mask);
    XAUDIO2_VOICE_DETAILS details{}; master->GetVoiceDetails(&details); outChannels = details.InputChannels;
    X3DAudioInitialize(mask ? mask : SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, x3d);
    dg::Log("Voice: audio out %u channels", outChannels);
    return true;
}

// Band-limited, a little driven: a radio (300..3000 Hz) or a phone (300..3400 Hz).
struct Filter {
    float hpPrev = 0, hpOut = 0, lp1 = 0, lp2 = 0;
    void Run(int16_t* s, size_t n, bool radio) {
        const float hpA = 1.f / (1.f + 2.f * 3.14159f * 300.f / kRate);
        const float lpA = 1.f - std::exp(-2.f * 3.14159f * (radio ? 3000.f : 3400.f) / kRate);
        for (size_t i = 0; i < n; ++i) {
            const float x = s[i] / 32768.f;
            hpOut = hpA * (hpOut + x - hpPrev); hpPrev = x;
            lp1 += lpA * (hpOut - lp1); lp2 += lpA * (lp1 - lp2);
            float y = lp2;
            if (radio) y = std::tanh(y * 2.2f) / std::tanh(2.2f) * 0.9f;
            s[i] = int16_t(std::clamp(y, -1.f, 1.f) * 32767.f);
        }
    }
};

struct Speaker {
    IXAudio2SourceVoice* src = nullptr;
    std::deque<std::vector<int16_t>> queued;  // submitted buffers, oldest first (XAudio2 reads them)
    std::vector<int16_t> pending;             // waiting for the prebuffer
    double pendingSince = 0, last = 0;
    net::VoiceRoute route = net::VoiceRoute::Near;
    float range = 2500;
    Filter filter;
    double since = 0; int frames = 0; size_t bytes = 0; int peak = 0; uint64_t playedAt = 0;  // this talk, for the log
};
void LogHeard(uint16_t id, Speaker& s, double now) {
    if (!s.frames) return;
    XAUDIO2_VOICE_STATE st{}; s.src->GetState(&st);
    dg::Log("Voice: heard %u for %.1f s (route %d): %d frames, %.1f KB, peak %d%%, %.1f s played", id, s.last - s.since, int(s.route), s.frames,
            s.bytes / 1024.0, s.peak * 100 / 32768, double(st.SamplesPlayed - s.playedAt) / kRate);
    s.frames = 0; s.bytes = 0; s.peak = 0; s.playedAt = st.SamplesPlayed; s.since = now;
}
std::map<uint16_t, Speaker> speakers;
std::vector<int16_t> pcm(kRate);  // one second

void Submit(Speaker& s, std::vector<int16_t>&& samples) {
    s.queued.push_back(std::move(samples));
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(s.queued.back().size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(s.queued.back().data());
    if (FAILED(s.src->SubmitSourceBuffer(&b))) s.queued.pop_back();
}

// ---------------------------------------------------------------- state shared with the HUD
uint8_t setupFlags = 0;
std::string radioName, callName;
struct HudState { bool talking = false, radio = false, muted = false, off = false; int radioKey = 0; std::string channel; std::vector<std::string> onRadio; } hudNext;
std::mutex hudLock;
HudState hud;
std::map<uint16_t, double> heardAt;  // game thread
}  // namespace

void OnSetup(uint8_t flags, const std::string& radio, const std::string& call) {
    setupFlags = flags; radioName = radio; callName = call;
    dg::Log("Voice: %s%s%s%s", flags & net::VoiceOn ? "on" : "off", flags & net::VoiceMuted ? ", muted" : "",
            radio.empty() ? "" : (", radio " + radio).c_str(), call.empty() ? "" : (", call " + call).c_str());
}

void OnFrame(uint16_t id, net::VoiceRoute route, net::VoiceCodec codec, float range, const uint8_t* frame, size_t size) {
    if (codec != net::VoiceCodec::Steam || !steamVoice.Ready() || !AudioReady()) return;
    uint32_t written = 0;
    const int r = steamVoice.decompress(steamVoice.user(), frame, uint32_t(size), pcm.data(), uint32_t(pcm.size() * sizeof(int16_t)), &written, kRate);
    if (r != kVoiceOK || !written) return;
    const double now = Clock();
    heardAt[id] = now;
    Speaker& s = speakers[id];
    if (!s.src) {
        WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = kRate; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = kRate * 2;
        if (FAILED(xa->CreateSourceVoice(&s.src, &wf))) { speakers.erase(id); return; }
        s.src->Start();
    }
    if (route != s.route) s.filter = {};
    if (!s.frames) s.since = now;
    s.route = route; s.range = range; s.last = now;
    ++s.frames; s.bytes += size;
    for (uint32_t i = 0; i < written / sizeof(int16_t); ++i) s.peak = std::max(s.peak, std::abs(int(pcm[i])));
    std::vector<int16_t> samples(pcm.begin(), pcm.begin() + written / sizeof(int16_t));
    if (route != net::VoiceRoute::Near) s.filter.Run(samples.data(), samples.size(), route == net::VoiceRoute::Radio);
    XAUDIO2_VOICE_STATE st{}; s.src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (s.queued.size() > st.BuffersQueued) s.queued.pop_front();
    if (st.BuffersQueued > 16) return;  // far behind (a burst after a stall): drop rather than lag
    if (st.BuffersQueued == 0) {
        // Silence before: gather a little first, so the next frames arrive before this one has played.
        if (s.pending.empty()) s.pendingSince = now;
        s.pending.insert(s.pending.end(), samples.begin(), samples.end());
        if (s.pending.size() < size_t(kPrebuffer * kRate) && now - s.pendingSince < kPrebuffer * 1.5) return;
        samples = std::move(s.pending); s.pending.clear();
    }
    Submit(s, std::move(samples));
}

bool Talking(uint16_t id) {
    auto it = heardAt.find(id);
    return it != heardAt.end() && Clock() - it->second < kTalkingFor;
}

void Update(net::Client& client, bool canTalk, const FVector& camPos, const FRotator& camRot) {
    LoadSettings();
    const double now = Clock();
    // Push-to-talk: in the game window only, never while typing in the chat or a dialog.
    const bool focus = [] { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }();
    auto down = [&](int vk) { return vk && focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool on = (setupFlags & net::VoiceOn) && settings.microphone && canTalk && !overlay::Typing();
    const bool radioHeld = on && down(settings.radioKey) && (setupFlags & net::VoiceRadio);
    const bool talkHeld = on && (down(settings.talkKey) || radioHeld || settings.openMic);
    const int key = radioHeld ? int(net::VoiceKey::Radio) : int(net::VoiceKey::Talk);
    if (talkHeld && !(setupFlags & net::VoiceMuted) && steamVoice.Ready()) {
        if (!cap.recording) {
            steamVoice.start(steamVoice.user());
            cap.recording = true; cap.drainUntil = 0; cap.nextPoll = now + kSendEvery; cap.since = now;
        }
        cap.key = key;
    } else StopCapture(now);
    if (talkHeld && !steamVoice.Ready() && !cap.warned) { cap.warned = true; dg::Log("Voice: Steam is not running, the microphone is unavailable"); }
    PollCapture(client, now);

    // Voices: near ones from where their players are, seen from the camera; the rest straight ahead.
    if (xa) {
        const float cp = camRot.Pitch * 3.14159265f / 180.f, cy = camRot.Yaw * 3.14159265f / 180.f;
        X3DAUDIO_LISTENER listener{};
        listener.OrientFront = {std::cos(cp) * std::sin(cy), std::sin(cp), std::cos(cp) * std::cos(cy)};  // UE (x fwd, y right, z up) -> X3D (y, z, x)
        listener.OrientTop = {-std::sin(cp) * std::sin(cy), std::cos(cp), -std::sin(cp) * std::cos(cy)};
        float matrix[8] = {};
        X3DAUDIO_DSP_SETTINGS dsp{}; dsp.SrcChannelCount = 1; dsp.DstChannelCount = std::min<UINT32>(outChannels, 8); dsp.pMatrixCoefficients = matrix;
        for (auto it = speakers.begin(); it != speakers.end();) {
            Speaker& s = it->second;
            XAUDIO2_VOICE_STATE st{}; s.src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            while (s.queued.size() > st.BuffersQueued) s.queued.pop_front();
            // Gathered too little and nothing more came: play what there is.
            if (!s.pending.empty() && now - s.pendingSince >= kPrebuffer * 1.5) { auto p = std::move(s.pending); s.pending.clear(); Submit(s, std::move(p)); }
            if (s.frames && now - s.last > 1) LogHeard(it->first, s, now);
            if (now - s.last > 5 && !st.BuffersQueued) { s.src->DestroyVoice(); it = speakers.erase(it); continue; }
            float gain = settings.volume;
            X3DAUDIO_EMITTER emitter{}; emitter.ChannelCount = 1; emitter.CurveDistanceScaler = 1; emitter.OrientFront = {0, 0, 1}; emitter.OrientTop = {0, 1, 0};
            emitter.Position = listener.OrientFront;  // radio and phone: in front, no distance
            auto p = client.players().find(it->first);
            if (s.route == net::VoiceRoute::Near && p != client.players().end()) {
                const auto& ps = p->second.state;
                const float dx = ps.x - camPos.X, dy = ps.y - camPos.Y, dz = ps.z + 60.f - camPos.Z;  // its head, about
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                // Full voice up close, fading to nothing at the speaker's range.
                const float inner = s.range * 0.15f, fade = std::clamp(1.f - (d - inner) / std::max(1.f, s.range - inner), 0.f, 1.f);
                gain *= d <= inner ? 1.f : fade * fade;
                if (d > 1.f) emitter.Position = {dy / d, dz / d, dx / d};  // one metre away in its direction: X3DAudio only pans
            }
            X3DAudioCalculate(x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
            s.src->SetOutputMatrix(master, 1, dsp.DstChannelCount, matrix);
            s.src->SetVolume(gain);
            ++it;
        }
    }
    for (auto it = heardAt.begin(); it != heardAt.end();) it = now - it->second > 10 ? heardAt.erase(it) : std::next(it);

    HudState h;
    h.off = !(setupFlags & net::VoiceOn);
    h.talking = cap.recording;
    h.radio = cap.recording && cap.key == int(net::VoiceKey::Radio);
    h.muted = talkHeld && (setupFlags & net::VoiceMuted);
    h.radioKey = (setupFlags & net::VoiceRadio) ? settings.radioKey : 0;
    h.channel = h.radio ? radioName : callName;
    for (auto& [id, s] : speakers) {
        if (s.route == net::VoiceRoute::Near || now - s.last > kTalkingFor) continue;
        auto p = client.players().find(id);
        h.onRadio.push_back((s.route == net::VoiceRoute::Radio ? "~ " : "@ ") + (p != client.players().end() ? p->second.nick : "#" + std::to_string(id)));
    }
    std::lock_guard g(hudLock);
    hud = std::move(h);
}

void DrawHud() {
    HudState h;
    { std::lock_guard g(hudLock); h = hud; }
    if (h.off) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 18.f, x = 28.f;
    float y = ImGui::GetIO().DisplaySize.y * 0.66f;
    auto line = [&](ImU32 dot, const std::string& text) {
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + 22 + ts.x + 8, y + ts.y + 4), IM_COL32(0, 0, 0, 130), 5);
        dl->AddCircleFilled(ImVec2(x + 6, y + ts.y / 2), 6, dot);
        dl->AddText(font, size, ImVec2(x + 20, y), IM_COL32(240, 240, 240, 255), text.c_str());
        y += ts.y + 12;
    };
    if (h.muted) line(IM_COL32(200, 80, 70, 255), lang::Pick("Микрофон выключен сервером", "Your microphone is muted"));
    else if (h.talking) {
        std::string text = h.radio ? lang::Pick("Рация", "Radio") : lang::Pick("Говорите", "Talking");
        if (!h.channel.empty()) text += ": " + h.channel;
        line(h.radio ? IM_COL32(240, 180, 60, 255) : IM_COL32(110, 210, 110, 255), text);
    }
    for (auto& who : h.onRadio) line(IM_COL32(240, 180, 60, 200), who);
}

void Reset() {
    StopCapture(Clock()); cap.drainUntil = 0;
    for (auto& [id, s] : speakers) if (s.src) s.src->DestroyVoice();
    speakers.clear(); heardAt.clear();
    setupFlags = 0; radioName.clear(); callName.clear();
    std::lock_guard g(hudLock);
    hud = {};
}

}  // namespace voice
