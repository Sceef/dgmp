#include "players.h"
#include "voice.h"
#include "log.h"
#include "lang.h"
#include "overlay.h"
#include "profile.h"
#include "mpui.h"
#include "imgui.h"
#include "shared/json.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace players {
namespace {

struct Row { uint16_t id = 0; std::string name, key; int ping = 0; bool admin = false, bot = false, talking = false; std::vector<std::string> cells; };
struct List { bool voice = true; int max = 0; std::vector<std::string> columns; std::vector<Row> rows; };

// Shared with the render thread (lock): what the window shows and what it asked for.
std::mutex lock;
bool open = false;
List list;
uint16_t self = 0xFFFF;
std::map<std::string, bool> muted;   // player key -> muted here (DGMP.ini [Players])
std::map<std::string, int> volumes;  // player key -> its voice volume, % (100 is not stored)
bool changed = false;                // the window changed a mute or a volume
bool openVoice = false;              // the window's "voice settings" button
bool closeAsked = false;             // the window's close button

// Game thread.
bool loaded = false, ownsOverlay = false;
double nextRequest = 0;
std::vector<uint16_t> sentIgnore;
bool ignoreSent = false;

double Clock() { return GetTickCount64() / 1000.0; }
std::wstring IniPath() { return profile::Root() + L"\\DGMP.ini"; }
// An ini key for a player key: "s:<SteamID>" stays readable, a nickname is written in hex (any characters).
std::wstring IniKey(const std::string& key) {
    if (key.rfind("s:", 0) == 0) return std::wstring(key.begin(), key.end());
    std::wstring out = L"n:";
    static const wchar_t* hex = L"0123456789abcdef";
    for (size_t i = 2; i < key.size(); ++i) { const auto c = static_cast<unsigned char>(key[i]); out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}
std::string FromIniKey(const std::wstring& k) {
    if (k.rfind(L"s:", 0) == 0) return std::string(k.begin(), k.end());
    if (k.rfind(L"n:", 0) != 0 || k.size() % 2) return {};
    std::string out = "n:";
    for (size_t i = 2; i + 1 < k.size(); i += 2) out += char(std::stoi(std::wstring(k.substr(i, 2)), nullptr, 16));
    return out;
}
void Load() {
    if (loaded) return;
    loaded = true;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(L"Players", buf.data(), DWORD(buf.size()), IniPath().c_str());
    std::lock_guard g(lock);
    for (const wchar_t* at = buf.data(); at < buf.data() + n && *at; at += wcslen(at) + 1) {
        std::wstring line = at; const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        // "<key>.mute=1", "<key>.volume=60"
        const std::wstring name = line.substr(0, eq), value = line.substr(eq + 1);
        const auto dot = name.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        const std::string key = FromIniKey(name.substr(0, dot));
        if (key.empty()) continue;
        if (name.substr(dot + 1) == L"mute" && value == L"1") muted[key] = true;
        if (name.substr(dot + 1) == L"volume") volumes[key] = std::clamp(_wtoi(value.c_str()), 0, 200);
    }
}
void Save() {
    std::wstring section;
    {
        std::lock_guard g(lock);
        for (auto& [key, on] : muted) if (on) section += IniKey(key) + L".mute=1" + L'\0';
        for (auto& [key, v] : volumes) if (v != 100) section += IniKey(key) + L".volume=" + std::to_wstring(v) + L'\0';
    }
    section += L'\0';
    WritePrivateProfileSectionW(L"Players", section.c_str(), IniPath().c_str());
}

// The voice side of the mutes and volumes: the server stops sending muted voices, the client drops them too
// (frames already on the way) and plays the others at their volume.
void Apply(net::Client& client) {
    std::vector<uint16_t> ignore;
    std::vector<std::tuple<uint16_t, bool, int>> audio;
    {
        std::lock_guard g(lock);
        for (auto& r : list.rows) {
            if (r.id == self) continue;
            const bool m = muted.count(r.key) && muted[r.key];
            const int v = volumes.count(r.key) ? volumes[r.key] : 100;
            if (m) ignore.push_back(r.id);
            audio.emplace_back(r.id, m, v);
        }
    }
    voice::ClearPlayerAudio();
    for (auto& [id, m, v] : audio) voice::SetPlayerAudio(id, m, v);
    std::sort(ignore.begin(), ignore.end());
    if (!ignoreSent || ignore != sentIgnore) { client.SendVoiceIgnore(ignore); sentIgnore = ignore; ignoreSent = true; }
}

}  // namespace

void OnList(const std::string& text) {
    json::Value v;
    if (!json::Value::Parse(text, v) || !v.isObject()) return;
    List l;
    l.voice = v["voice"].asBool(true); l.max = v["max"].asInt();
    for (auto& c : v["columns"].elements()) l.columns.push_back(c.asString());
    for (auto& r : v["rows"].elements()) {
        Row row; row.id = uint16_t(r["id"].asInt()); row.name = r.str("name"); row.key = r.str("key"); row.ping = r["ping"].asInt();
        row.admin = r["admin"].asBool(); row.bot = r["bot"].asBool();
        for (auto& c : r["cells"].elements()) row.cells.push_back(c.isString() ? c.asString() : std::string());
        l.rows.push_back(std::move(row));
    }
    std::sort(l.rows.begin(), l.rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
    std::lock_guard g(lock);
    list = std::move(l);
}

void Update(net::Client& client, bool canOpen) {
    Load();
    const double now = Clock();
    self = client.localId();
    // Tab: open / close (the overlay takes it from the game while connected).
    const bool tab = overlay::TakeTabPress();
    bool isOpen, voiceAsked, edited;
    {
        std::lock_guard g(lock);
        if (tab && (open || canOpen)) open = !open;
        if (open && (closeAsked || mpui::DialogOpen())) open = false;
        if (openVoice) open = false;
        voiceAsked = openVoice; openVoice = false; closeAsked = false;
        edited = changed; changed = false;
        isOpen = open;
        for (auto& r : list.rows) r.talking = voice::Talking(r.id);
    }
    if (isOpen && !ownsOverlay) { ownsOverlay = true; overlay::SetVisible(true); overlay::SetCursorMode(true); nextRequest = 0; }
    else if (!isOpen && ownsOverlay) { ownsOverlay = false; if (!mpui::DialogOpen()) { overlay::SetCursorMode(false); overlay::SetVisible(false); } }
    else if (isOpen && !overlay::CursorMode()) overlay::SetCursorMode(true);
    if (voiceAsked) voice::OpenSettings();
    // The list: once a second while open, and every 10 s anyway (mutes follow players who join later).
    if (now >= nextRequest) { nextRequest = now + (isOpen ? 1.0 : 10.0); client.SendListRequest(); }
    if (edited) Save();
    Apply(client);
}

bool Open() { std::lock_guard g(lock); return open; }

void Reset() {
    {
        std::lock_guard g(lock);
        open = false; list = List{};
    }
    if (ownsOverlay) { ownsOverlay = false; overlay::SetCursorMode(false); overlay::SetVisible(false); }
    sentIgnore.clear(); ignoreSent = false; nextRequest = 0;
    voice::ClearPlayerAudio();
}

void Draw() {
    std::lock_guard g(lock);
    if (!open) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.f, io.DisplaySize.x - 40), std::min(560.f, io.DisplaySize.y - 80)), ImGuiCond_Appearing);
    bool keep = true;
    const std::string title = std::string(lang::Pick("Игроки", "Players")) + "  " + std::to_string(list.rows.size()) + (list.max ? " / " + std::to_string(list.max) : "") + "###dgmpplayers";
    ImGui::Begin(title.c_str(), &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (!list.voice) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1), "%s", lang::Pick("Голосовой чат на сервере выключен", "Voice chat is off on this server"));
    if (ImGui::Button(lang::Pick("Настройки голоса", "Voice settings"))) openVoice = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", lang::Pick("Tab — закрыть. «Заглушить» — только у вас, сохраняется.", "Tab: close. Mute is yours only and is kept."));

    auto muteButton = [&](const Row& r) {
        bool& m = muted[r.key];
        if (ImGui::SmallButton(m ? lang::Pick("Вернуть звук", "Unmute") : lang::Pick("Заглушить", "Mute"))) { m = !m; changed = true; }
    };
    // Who talks now: the quickest way to find the one to mute.
    std::vector<const Row*> talking;
    for (auto& r : list.rows) if (r.talking && r.id != self) talking.push_back(&r);
    if (!talking.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(lang::Pick("Говорят сейчас:", "Talking now:"));
        for (auto* r : talking) {
            ImGui::PushID(int(r->id) + 100000);
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%u  %s", r->id, r->name.c_str());
            ImGui::SameLine(); muteButton(*r);
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    const int columns = 4 + int(list.columns.size()) + 2;
    if (ImGui::BeginTable("##players", columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn(lang::Pick("Ник", "Name"), ImGuiTableColumnFlags_WidthStretch);
        for (auto& c : list.columns) ImGui::TableSetupColumn(c.c_str());
        ImGui::TableSetupColumn(lang::Pick("Пинг", "Ping"));
        ImGui::TableSetupColumn(" ");
        ImGui::TableSetupColumn(lang::Pick("Громкость", "Volume"));
        ImGui::TableSetupColumn(" ");
        ImGui::TableHeadersRow();
        for (auto& r : list.rows) {
            ImGui::PushID(int(r.id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%u", r.id);
            ImGui::TableSetColumnIndex(1);
            const ImVec4 color = r.id == self ? ImVec4(0.95f, 0.85f, 0.4f, 1) : r.admin ? ImVec4(0.55f, 0.75f, 1, 1) : ImVec4(0.92f, 0.92f, 0.92f, 1);
            ImGui::TextColored(color, "%s%s%s", r.name.c_str(), r.id == self ? lang::Pick("  (вы)", "  (you)") : "", r.bot ? "  [bot]" : "");
            for (size_t c = 0; c < list.columns.size(); ++c) {
                ImGui::TableSetColumnIndex(int(2 + c));
                ImGui::TextUnformatted(c < r.cells.size() ? r.cells[c].c_str() : "");
            }
            const int base = 2 + int(list.columns.size());
            ImGui::TableSetColumnIndex(base);
            ImGui::TextColored(r.ping > 200 ? ImVec4(0.95f, 0.5f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%d", r.ping);
            ImGui::TableSetColumnIndex(base + 1);
            if (r.talking) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", lang::Pick("говорит", "talking"));
            if (r.id != self && !r.bot) {
                ImGui::TableSetColumnIndex(base + 2);
                int v = volumes.count(r.key) ? volumes[r.key] : 100;
                ImGui::SetNextItemWidth(110);
                if (ImGui::SliderInt("##vol", &v, 0, 200, "%d%%")) { volumes[r.key] = v; changed = true; }
                ImGui::TableSetColumnIndex(base + 3);
                muteButton(r);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!keep) closeAsked = true;
}

}  // namespace players
