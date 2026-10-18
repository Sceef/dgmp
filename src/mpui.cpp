#include "mpui.h"
#include "overlay.h"
#include "images.h"
#include "skins.h"
#include "lang.h"
#include "shared/json.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

using namespace ue;

namespace mpui {
namespace {
std::mutex lock;  // everything below: written on the game thread, drawn on the frame thread

// ---------------------------------------------------------------- text with {RRGGBB} colour codes
// SA-MP style: "{FF0000}red {FFFFFF}white". Lines split at '\n'.
struct Segment { std::string text; ImU32 color; std::string image; };  // image: {img:name} (text empty)
bool Hex(char c, int& v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
    return false;
}
bool CodeAt(const std::string& s, size_t i, ImU32& color) {
    if (i + 7 >= s.size() || s[i] != '{' || s[i + 7] != '}') return false;
    int rgb[6];
    for (int k = 0; k < 6; ++k) if (!Hex(s[i + 1 + size_t(k)], rgb[k])) return false;
    color = IM_COL32(rgb[0] * 16 + rgb[1], rgb[2] * 16 + rgb[3], rgb[4] * 16 + rgb[5], (color >> IM_COL32_A_SHIFT) & 0xFF);
    return true;
}
std::vector<std::vector<Segment>> Lines(const std::string& s, ImU32 base) {
    std::vector<std::vector<Segment>> lines(1);
    ImU32 color = base;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) lines.back().push_back({cur, color}); cur.clear(); };
    for (size_t i = 0; i < s.size(); ++i) {
        ImU32 next = color;
        if (s[i] == '{' && CodeAt(s, i, next)) { flush(); color = next; i += 7; continue; }
        if (s.compare(i, 5, "{img:") == 0) {  // an image of the mode, as tall as the text
            const size_t end = s.find('}', i + 5);
            if (end != std::string::npos && end - i - 5 <= 100) { flush(); lines.back().push_back({"", color, s.substr(i + 5, end - i - 5)}); i = end; continue; }
        }
        if (s[i] == '\n') { flush(); lines.emplace_back(); continue; }
        if (s[i] == '\r') continue;
        cur += s[i];
    }
    flush();
    return lines;
}
std::string Plain(const std::string& s) {
    std::string out;
    for (auto& line : Lines(s, 0)) { if (!out.empty()) out += '\n'; for (auto& seg : line) out += seg.text; }
    return out;
}
float ImageWidth(const std::string& name, float height) {
    const images::View v = images::Get(name);
    return v.texture && v.h > 0 ? height * v.w / v.h : height;
}
ImVec2 LineSize(ImFont* font, float size, const std::vector<Segment>& line) {
    float w = 0;
    for (auto& seg : line) w += seg.image.empty() ? font->CalcTextSizeA(size, FLT_MAX, 0, seg.text.c_str()).x : ImageWidth(seg.image, size) + 2;
    return ImVec2(w, size);
}
void DrawLine(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const std::vector<Segment>& line, bool shadow, float alpha = 1) {
    for (auto& seg : line) {
        if (!seg.image.empty()) {
            const images::View v = images::Get(seg.image);
            const float w = ImageWidth(seg.image, size);
            if (v.texture) dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(v.texture)), at, ImVec2(at.x + w, at.y + size), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, int(255 * alpha)));
            at.x += w + 2;
            continue;
        }
        const ImU32 a = ImU32(float((seg.color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        if (shadow) dl->AddText(font, size, ImVec2(at.x + 1.5f, at.y + 1.5f), IM_COL32(0, 0, 0, a * 3 / 4), seg.text.c_str());
        dl->AddText(font, size, at, (seg.color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT), seg.text.c_str());
        at.x += font->CalcTextSizeA(size, FLT_MAX, 0, seg.text.c_str()).x;
    }
}
// Coloured text as an ImGui item (inside a window).
void ColoredText(const std::string& s) {
    ImFont* font = ImGui::GetFont(); const float size = ImGui::GetFontSize();
    const ImU32 base = ImGui::GetColorU32(ImGuiCol_Text);
    for (auto& line : Lines(s, base)) {
        const ImVec2 at = ImGui::GetCursorScreenPos(), box = LineSize(font, size, line);
        DrawLine(ImGui::GetWindowDrawList(), font, size, at, line, false);
        ImGui::Dummy(ImVec2(box.x, size));
    }
}
ImU32 ParseColor(const json::Value& v, ImU32 fallback) {
    if (!v.isString()) return fallback;
    const std::string s = v.asString();
    if (s.size() != 7 && s.size() != 9) return fallback;
    if (s[0] != '#') return fallback;
    int d[8] = {15, 15, 15, 15, 15, 15, 15, 15};
    for (size_t k = 1; k < s.size(); ++k) if (!Hex(s[k], d[k - 1])) return fallback;
    return IM_COL32(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5], d[6] * 16 + d[7]);
}

// ---------------------------------------------------------------- dialog
struct Dialog {
    bool open = false;
    net::Client::Events::DialogInfo info;
    char input[257] = "";
    int selected = 0;
    bool focus = true, scroll = false;
    bool reveal = false;  // a password shown as typed (the eye next to it)
} dialog;
struct Answer { bool pending = false; uint16_t id = 0; bool ok = false; int item = -1; std::string text; } answer;

void Respond(bool ok) {
    answer = {true, dialog.info.id, ok, dialog.info.style == net::DialogStyle::List ? dialog.selected : -1,
              dialog.info.style == net::DialogStyle::Input || dialog.info.style == net::DialogStyle::Password ? std::string(dialog.input) : std::string()};
    dialog.open = false;
}
std::vector<std::string> Cells(const std::string& row) {
    std::vector<std::string> out; size_t at = 0;
    for (size_t k; (k = row.find('\t', at)) != std::string::npos; at = k + 1) out.push_back(row.substr(at, k - at));
    out.push_back(row.substr(at));
    return out;
}
void DrawDialog() {
    if (!dialog.open) return;
    const auto& d = dialog.info;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(380, 0), ImVec2(io.DisplaySize.x * 0.7f, io.DisplaySize.y * 0.85f));
    const std::string title = Plain(d.title) + "###dgmpdialog" + std::to_string(d.id);
    ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!d.text.empty()) { ColoredText(d.text); ImGui::Spacing(); }
    bool enter = false;
    if (d.style == net::DialogStyle::Input || d.style == net::DialogStyle::Password) {
        const bool password = d.style == net::DialogStyle::Password;
        const float eye = password ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0;
        if (dialog.focus) { ImGui::SetKeyboardFocusHere(); dialog.focus = false; }
        ImGui::SetNextItemWidth(std::max(340.f, ImGui::GetContentRegionAvail().x) - eye);
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (password && !dialog.reveal ? ImGuiInputTextFlags_Password : 0);
        if (ImGui::InputText("##input", dialog.input, sizeof dialog.input, flags)) enter = true;
        if (password) {  // the eye: shows what was typed (crossed out while hidden)
            ImGui::SameLine();
            const float h = ImGui::GetFrameHeight();
            if (ImGui::InvisibleButton("##eye", ImVec2(h, h))) dialog.reveal = !dialog.reveal;
            const ImVec2 a = ImGui::GetItemRectMin(), c(a.x + h / 2, a.y + h / 2);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            dl->AddRectFilled(a, ImVec2(a.x + h, a.y + h), ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
            dl->AddEllipse(c, ImVec2(h * 0.36f, h * 0.22f), col, 0, 24, 1.6f);
            dl->AddCircleFilled(c, h * 0.11f, col);
            if (!dialog.reveal) dl->AddLine(ImVec2(c.x - h * 0.34f, c.y + h * 0.28f), ImVec2(c.x + h * 0.34f, c.y - h * 0.28f), col, 1.6f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dialog.reveal ? lang::Pick("Скрыть", "Hide") : lang::Pick("Показать", "Show"));
        }
    } else if (d.style == net::DialogStyle::List) {
        const int n = int(d.items.size());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { dialog.selected = std::min(n - 1, dialog.selected + 1); dialog.scroll = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { dialog.selected = std::max(0, dialog.selected - 1); dialog.scroll = true; }
        size_t columns = d.headers.empty() ? 1 : Cells(d.headers).size();
        for (auto& it : d.items) columns = std::max(columns, Cells(it).size());
        columns = std::min<size_t>(columns, 8);
        const float row = ImGui::GetTextLineHeightWithSpacing() + 2;
        const float height = std::min(row * float(n + (d.headers.empty() ? 0 : 1)) + 6, io.DisplaySize.y * 0.55f);
        if (ImGui::BeginTable("##list", int(columns), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | (columns > 1 ? ImGuiTableFlags_BordersInnerV : 0),
                              ImVec2(std::max(340.f, 150.f * float(columns)), height))) {
            if (!d.headers.empty()) {
                ImGui::TableSetupScrollFreeze(0, 1);
                for (auto& h : Cells(d.headers)) ImGui::TableSetupColumn(Plain(h).c_str());
                for (size_t c = Cells(d.headers).size(); c < columns; ++c) ImGui::TableSetupColumn("");
                ImGui::TableHeadersRow();
            }
            for (int i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                const auto cells = Cells(d.items[size_t(i)]);
                for (size_t c = 0; c < columns; ++c) {
                    ImGui::TableSetColumnIndex(int(c));
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    const std::string cell = c < cells.size() ? cells[c] : std::string();
                    const auto line = Lines(cell, ImGui::GetColorU32(ImGuiCol_Text))[0];
                    const ImVec2 box(std::max(LineSize(ImGui::GetFont(), ImGui::GetFontSize(), line).x, 20.f), ImGui::GetTextLineHeight());
                    if (c == 0) {  // the row's selectable spans every column; the text is drawn over it
                        ImGui::PushID(i);
                        // Width 0: the frame spans the whole row; the dummy gives the column its width.
                        if (ImGui::Selectable("##row", dialog.selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, box.y))) {
                            dialog.selected = i;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) enter = true;
                        }
                        ImGui::SameLine(0, 0); ImGui::Dummy(box);
                        if (dialog.scroll && dialog.selected == i) { ImGui::SetScrollHereY(); dialog.scroll = false; }
                        ImGui::PopID();
                    } else ImGui::Dummy(box);
                    DrawLine(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), at, line, false);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    const float bw = 120;
    const float total = d.button2.empty() ? bw : bw * 2 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - total) * 0.5f));
    if (ImGui::Button((Plain(d.button1) + "##b1").c_str(), ImVec2(bw, 0))) enter = true;
    bool cancel = false;
    if (!d.button2.empty()) { ImGui::SameLine(); if (ImGui::Button((Plain(d.button2) + "##b2").c_str(), ImVec2(bw, 0))) cancel = true; }
    if (d.style != net::DialogStyle::Input && d.style != net::DialogStyle::Password && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) enter = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
    ImGui::End();
    if (enter) Respond(true);
    else if (cancel) Respond(false);
}

// ---------------------------------------------------------------- HUD elements
struct Element {
    std::string text, image;
    float iw = 0, ih = 0;  // an image's width / height, parts of the screen (0: from the other, or its own size)
    float x = 0.5f, y = 0.5f, size = 1, bar = -1, w = 0.12f, h = 0.014f;
    int align = 0;  // 0 left, 1 centre, 2 right
    ImU32 color = IM_COL32(255, 255, 255, 255), bg = 0, barColor = IM_COL32(220, 220, 220, 255);
    bool shadow = true;
};
std::map<std::string, Element> hud;
std::vector<WorldIcon> icons;  // createIcon pictures on screen this frame
bool hudShown = false;

// ---------------------------------------------------------------- interactions and keys
struct Spot { net::Client::Events::InteractionInfo info; };
std::map<uint16_t, Spot> spots;
std::set<uint8_t> watched;
int world = 0;
struct Prompt { uint16_t id = 0; uint8_t key = 0; std::string text; bool vehicle = false; } prompt;  // id 0: none
struct VehiclePrompt { uint16_t id = 0; std::string text; } vehiclePrompt;
bool owned = false;  // the overlay is shown for a dialog

std::string KeyLabel(uint8_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x6F);
    return "?";
}
}  // namespace

void OnDialog(const net::Client::Events::DialogInfo& d) {
    std::lock_guard g(lock);
    if (d.style == net::DialogStyle::Close) { if (dialog.open && dialog.info.id == d.id) dialog.open = false; return; }
    dialog = Dialog{};
    dialog.open = true; dialog.info = d;
    snprintf(dialog.input, sizeof dialog.input, "%s", d.defaultText.c_str());
}
void OnHud(const std::string& id, const std::string& text) {
    std::lock_guard g(lock);
    if (text.empty()) { if (id.empty()) hud.clear(); else hud.erase(id); return; }
    json::Value v;
    if (!json::Value::Parse(text, v) || !v.isObject()) return;
    Element e;
    e.text = v["text"].isString() ? v["text"].asString() : v["text"].isNumber() ? std::to_string(int(v["text"].asNumber())) : std::string();
    e.x = float(v["x"].asNumber(0.5)); e.y = float(v["y"].asNumber(0.5));
    e.size = std::clamp(float(v["size"].asNumber(1)), 0.4f, 4.f);
    const std::string align = v["align"].asString();
    e.align = align == "center" || align == "centre" ? 1 : align == "right" ? 2 : 0;
    e.color = ParseColor(v["color"], e.color);
    e.bg = ParseColor(v["bg"], 0);
    e.shadow = v["shadow"].asBool(true);
    e.image = v["image"].asString();
    e.iw = float(v["w"].asNumber(0)); e.ih = float(v["h"].asNumber(0));
    if (v["bar"].isNumber()) {
        e.bar = std::clamp(float(v["bar"].asNumber()), 0.f, 1.f);
        e.w = float(v["w"].asNumber(0.12)); e.h = float(v["h"].asNumber(0.014));
        e.barColor = ParseColor(v["barColor"], e.barColor);
        if (!e.bg) e.bg = IM_COL32(0, 0, 0, 140);
    }
    if (hud.size() < 128 || hud.count(id)) hud[id] = e;
}
void OnInteraction(uint16_t id, const net::Client::Events::InteractionInfo* info) {
    std::lock_guard g(lock);
    if (info) spots[id] = {*info}; else spots.erase(id);
}
void OnWatchKeys(const std::vector<uint8_t>& keys) {
    std::lock_guard g(lock);
    watched = std::set<uint8_t>(keys.begin(), keys.end());
}
void Reset() {
    overlay::SetBlockedKey(0);
    std::lock_guard g(lock);
    icons.clear();
    dialog = Dialog{}; answer = Answer{}; hud.clear(); spots.clear(); watched.clear(); prompt = Prompt{}; world = 0; vehiclePrompt = VehiclePrompt{};
}
bool DialogOpen() { std::lock_guard g(lock); return dialog.open; }
void SetWorld(int w) { std::lock_guard g(lock); world = w; }
void SetIcons(std::vector<WorldIcon> list) { std::lock_guard g(lock); icons = std::move(list); }
void SetVehiclePrompt(uint16_t id, const std::string& text) { std::lock_guard g(lock); vehiclePrompt = {id, text}; }

void Update(net::Client& client, const FVector& at, bool canAct, bool shown) {
    bool open, actNow = false;
    std::vector<uint8_t> keys;
    {
        std::lock_guard g(lock);
        hudShown = shown;
        if (answer.pending) { answer.pending = false; client.SendDialogResponse(answer.id, answer.ok, answer.item, answer.text); }
        open = dialog.open;
        // The interaction the player stands at (the nearest).
        prompt = Prompt{};
        const bool act = canAct && !open && !skins::CreatorOpen() && !overlay::CursorMode();
        if (act) {
            float best = 1e30f;
            for (auto& [id, s] : spots) {
                const float dx = at.X - s.info.x, dy = at.Y - s.info.y, dz = at.Z - s.info.z, d2 = dx * dx + dy * dy;
                if ((s.info.world >= 0 && s.info.world != world) || d2 > s.info.radius * s.info.radius || std::fabs(dz) > 300.f || d2 >= best) continue;
                best = d2; prompt = {id, s.info.key, s.info.text};
            }
            if (!prompt.id && vehiclePrompt.id) prompt = {vehiclePrompt.id, 'E', vehiclePrompt.text, true};
        }
        // Every key that may act is followed (also while it may not, so that a key still held
        // when a dialog closes is no new press).
        keys.assign(watched.begin(), watched.end());
        for (auto& [id, s] : spots) keys.push_back(s.info.key);
        keys.push_back('E');  // vehicles
        actNow = act;
    }
    {
        std::lock_guard g(lock);
        overlay::SetBlockedKey(prompt.id ? prompt.key : 0);  // E at an interaction is ours, not the game's (no bike mounting)
    }
    // The overlay belongs to the dialog while it is open.
    if (open) { if (!overlay::Visible()) overlay::SetVisible(true); if (!overlay::CursorMode()) overlay::SetCursorMode(true); owned = true; }
    else if (owned && !skins::CreatorOpen()) { owned = false; overlay::SetVisible(false); }
    // Key presses (edges), only while the game window has the focus.
    static bool down[256] = {};
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::set<uint8_t> want(keys.begin(), keys.end());
    for (int vk = 0; vk < 256; ++vk) {
        const bool now = focused && want.count(uint8_t(vk)) && (GetAsyncKeyState(vk) & 0x8000);
        if (now && !down[vk] && actNow) {
            uint16_t use = 0; bool watchedKey, vehicle = false;
            { std::lock_guard g(lock); if (prompt.id && prompt.key == vk) { use = prompt.id; vehicle = prompt.vehicle; } watchedKey = watched.count(uint8_t(vk)) != 0; }
            if (use && vehicle) client.SendVehicle(net::Msg::VehicleEnter, use);
            else if (use) client.SendUse(use);
            else if (watchedKey) client.SendKey(uint8_t(vk));
        }
        down[vk] = now;
    }
}

void DrawWindows() {
    {
        std::lock_guard g(lock);
        DrawDialog();
    }
    if (skins::CreatorOpen()) skins::DrawCreator();
}

void DrawHud() {
    std::lock_guard g(lock);
    if (!hudShown) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImFont* small = ImGui::GetFont();
    ImFont* large = static_cast<ImFont*>(overlay::LargeFont());
    // Icons at points of the world (createIcon): their screen points come from the game thread.
    for (auto& ic : icons) {
        const images::View v = images::Get(ic.image);
        const float size = ic.size * ic.scale, w = v.texture && v.h > 0 ? size * v.w / v.h : size;
        if (v.texture) dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(v.texture)), ImVec2(ic.x - w / 2, ic.y - size), ImVec2(ic.x + w / 2, ic.y), ImVec2(0, 0), ImVec2(1, 1), ic.color);
        if (!ic.text.empty()) {
            const auto lines = Lines(ic.text, IM_COL32(240, 240, 240, 255));
            const float ts = 17 * std::max(ic.scale, 0.7f);
            const ImVec2 box = LineSize(small, ts, lines[0]);
            DrawLine(dl, small, ts, ImVec2(ic.x - box.x / 2, ic.y + 2), lines[0], true);
        }
    }
    for (auto& [id, e] : hud) {
        const float px = e.x * screen.x, py = e.y * screen.y;
        if (!e.image.empty()) {  // a picture: its size from w and/or h (keeping its shape), else its own pixels x size
            const images::View v = images::Get(e.image);
            if (!v.texture) continue;
            float w = e.iw * screen.x, h = e.ih * screen.y;
            if (w <= 0 && h <= 0) { w = v.w * e.size; h = v.h * e.size; }
            else if (w <= 0) w = h * v.w / v.h;
            else if (h <= 0) h = w * v.h / v.w;
            const float x = e.align == 1 ? px - w / 2 : e.align == 2 ? px - w : px;
            dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(v.texture)), ImVec2(x, py), ImVec2(x + w, py + h), ImVec2(0, 0), ImVec2(1, 1), e.color);
            continue;
        }
        if (e.bar >= 0) {
            const float w = e.w * screen.x, h = std::max(2.f, e.h * screen.y);
            const float x = e.align == 1 ? px - w / 2 : e.align == 2 ? px - w : px;
            dl->AddRectFilled(ImVec2(x - 2, py - 2), ImVec2(x + w + 2, py + h + 2), e.bg, 3);
            dl->AddRectFilled(ImVec2(x, py), ImVec2(x + w * e.bar, py + h), e.barColor, 2);
            if (!e.text.empty()) {
                const float size = 16 * e.size;
                ImFont* font = size > 24 && large ? large : small;
                const auto lines = Lines(e.text, e.color);
                const ImVec2 box = LineSize(font, size, lines[0]);
                DrawLine(dl, font, size, ImVec2(x, py - box.y - 3), lines[0], e.shadow);
            }
            continue;
        }
        const float size = 19 * e.size;
        ImFont* font = size > 24 && large ? large : small;
        const auto lines = Lines(e.text, e.color);
        float width = 0;
        for (auto& line : lines) width = std::max(width, LineSize(font, size, line).x);
        const float height = size * float(lines.size());
        if (e.bg) {
            const float bx = e.align == 1 ? px - width / 2 : e.align == 2 ? px - width : px;
            dl->AddRectFilled(ImVec2(bx - 8, py - 5), ImVec2(bx + width + 8, py + height + 5), e.bg, 5);
        }
        float y = py;
        for (auto& line : lines) {
            const float w = LineSize(font, size, line).x;
            const float x = e.align == 1 ? px - w / 2 : e.align == 2 ? px - w : px;
            DrawLine(dl, font, size, ImVec2(x, y), line, e.shadow);
            y += size;
        }
    }
    // The prompt of the interaction the player stands at: "[E] Enter the house".
    if (prompt.id) {
        const float size = 22;
        ImFont* font = small;
        const std::string key = KeyLabel(prompt.key);
        const auto lines = Lines(prompt.text, IM_COL32(240, 240, 240, 255));
        const ImVec2 textBox = LineSize(font, size, lines[0]);
        const float keyW = std::max(30.f, font->CalcTextSizeA(size, FLT_MAX, 0, key.c_str()).x + 14);
        const float total = keyW + 10 + textBox.x;
        const float x = (screen.x - total) / 2, y = screen.y * 0.74f;
        dl->AddRectFilled(ImVec2(x - 12, y - 8), ImVec2(x + total + 12, y + 30 + 8), IM_COL32(0, 0, 0, 150), 6);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + keyW, y + 30), IM_COL32(235, 235, 235, 255), 4);
        const float kw = font->CalcTextSizeA(size, FLT_MAX, 0, key.c_str()).x;
        dl->AddText(font, size, ImVec2(x + (keyW - kw) / 2, y + 4), IM_COL32(20, 20, 20, 255), key.c_str());
        DrawLine(dl, font, size, ImVec2(x + keyW + 10, y + 4), lines[0], true);
    }
}

}  // namespace mpui
