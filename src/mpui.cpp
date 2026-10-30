#include "mpui.h"
#include "overlay.h"
#include "images.h"
#include "skins.h"
#include "webui.h"
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
// ---------------------------------------------------------------- the mode's item window
// One or two panes of items (an inventory and a trunk); the server decides what any click does.
struct WinItem { std::string id, name, text, icon; int count = 1; std::vector<std::pair<std::string, std::string>> actions; };  // (id, label)
struct WinPane { std::string id, title, info, empty; std::vector<WinItem> items; };
struct ItemWin {
    bool open = false;
    uint16_t id = 0;
    std::string title, hint;
    std::vector<WinPane> panes;
    int pane = 0, item = -1;  // the selection
} itemWin;
std::vector<std::pair<uint16_t, std::string>> itemActions;  // to send (window, json)
void ItemAct(json::Value v) { itemActions.emplace_back(itemWin.id, v.dump()); }
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
struct Spot { net::Client::Events::InteractionInfo info; double sightAt = -1; bool sight = true; };  // sight: checked then
std::map<uint16_t, Spot> spots;
std::set<uint8_t> watched;
int world = 0;
struct Prompt { uint16_t id = 0; uint8_t key = 0; std::string text; bool vehicle = false, carry = false; float progress = 0; int player = -1; } prompt;  // id 0: none; a player's: id 0xFFFE
struct PlayerMenu { bool on = false; uint8_t key = 'F'; std::string label; float range = 250; } playerMenu;
std::vector<PlayerSpot> playerSpots;
int aimedPlayer = -1;
struct Question { uint16_t id = 0; std::string text; double until = 0; } question;  // id 0: none
struct DownedHud { bool on = false; float bleed = 0, giveUp = 0, hold = 0; bool help = false; } downedHud;
constexpr uint16_t kPlayerPrompt = 0xFFFE;
std::map<uint16_t, Spot> playerSight;  // sight checks of aimed players
struct CarryPrompt { std::string text; float progress = 0; bool inHands = false; } carryPrompt;
struct VehiclePrompt { uint16_t id = 0; std::string text; } vehiclePrompt;
bool owned = false;  // the overlay is shown for a dialog
View view;
uint16_t aimedSpot = 0;  // the interaction picked by the aim last frame (it keeps a wider cone)
constexpr float kAimCone = 25, kAimKeep = 35, kLoneCone = 90;  // degrees
// How far off the middle of the view a sphere of `radius` at `p` is, in degrees (0: the view's centre
// line passes through it); 180 when the view is unknown.
float AimAngle(const FVector& p, float radius) {
    if (!view.valid) return 180;
    const float dx = p.X - view.cam.X, dy = p.Y - view.cam.Y, dz = p.Z - view.cam.Z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d < 1) return 0;
    const float c = std::clamp((dx * view.fwd.X + dy * view.fwd.Y + dz * view.fwd.Z) / d, -1.f, 1.f);
    const float off = std::acos(c) * 57.2957795f - std::asin(std::min(1.f, radius / d)) * 57.2957795f;
    return std::max(0.f, off);
}
// Nothing between the camera and the spot's aim point (checked five times a second per spot).
bool InSight(Spot& s, const FVector& p, double now) {
    if (!view.clear) return true;
    if (now - s.sightAt < 0.2) return s.sight;
    s.sightAt = now; s.sight = view.clear(view.cam, p);
    return s.sight;
}

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
void OnItemWindow(uint16_t id, const std::string& text) {
    std::lock_guard g(lock);
    if (text.empty()) { if (itemWin.id == id) itemWin.open = false; return; }
    json::Value v;
    if (!json::Value::Parse(text, v) || !v.isObject()) return;
    const bool same = itemWin.open && itemWin.id == id;
    ItemWin w; w.open = true; w.id = id;
    w.title = v.str("title"); w.hint = v.str("hint");
    for (auto& p : v["panes"].elements()) {
        if (w.panes.size() >= 2) break;
        WinPane pane; pane.id = p.str("id"); pane.title = p.str("title"); pane.info = p.str("info"); pane.empty = p.str("empty");
        for (auto& it : p["items"].elements()) {
            WinItem item; item.id = it.str("id"); item.name = it.str("name", item.id); item.text = it.str("text"); item.icon = it.str("icon");
            item.count = it["count"].isNumber() ? it["count"].asInt() : 1;
            for (auto& a : it["actions"].elements()) if (a.isObject()) item.actions.emplace_back(a.str("id"), a.str("label", a.str("id")));
            if (pane.items.size() < 500) pane.items.push_back(std::move(item));
        }
        w.panes.push_back(std::move(pane));
    }
    if (same) {  // an update: the selection stays where it was (on a list that got shorter, the last one)
        w.pane = std::min(itemWin.pane, std::max(0, int(w.panes.size()) - 1)); w.item = itemWin.item;
        if (w.pane < int(w.panes.size())) w.item = std::min(w.item, int(w.panes[size_t(w.pane)].items.size()) - 1);
    }
    itemWin = std::move(w);
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
    dialog = Dialog{}; answer = Answer{}; itemWin = ItemWin{}; itemActions.clear(); hud.clear(); spots.clear(); watched.clear(); prompt = Prompt{};
    playerMenu = PlayerMenu{}; playerSpots.clear(); playerSight.clear(); question = Question{}; aimedSpot = 0; aimedPlayer = -1; downedHud = DownedHud{}; world = 0; vehiclePrompt = VehiclePrompt{}; carryPrompt = CarryPrompt{};
}
bool DialogOpen() { std::lock_guard g(lock); return dialog.open || itemWin.open; }
void SetWorld(int w) { std::lock_guard g(lock); world = w; }
void SetView(View v) { std::lock_guard g(lock); view = std::move(v); }
void SetPlayerSpots(std::vector<PlayerSpot> list) { std::lock_guard g(lock); playerSpots = std::move(list); }
void OnPlayerMenu(bool on, uint8_t key, const std::string& label, float range) { std::lock_guard g(lock); playerMenu = {on, key, label, range}; }
void OnAsk(uint16_t id, const std::string& text, float seconds) {
    std::lock_guard g(lock);
    if (text.empty()) { if (question.id == id) question = Question{}; return; }
    question = {id, text, GetTickCount64() / 1000.0 + seconds};
}
bool AskPending() { std::lock_guard g(lock); return question.id != 0; }
void SetDownedHud(bool on, float bleed, float giveUp, float hold, bool helpCalled) { std::lock_guard g(lock); downedHud = {on, bleed, giveUp, hold, helpCalled}; }
void SetIcons(std::vector<WorldIcon> list) { std::lock_guard g(lock); icons = std::move(list); }
void SetCarryPrompt(const std::string& text, float progress, bool inHands) { std::lock_guard g(lock); carryPrompt = {text, progress, inHands}; }
bool CarryPromptShown() { std::lock_guard g(lock); return prompt.carry; }
void SetVehiclePrompt(uint16_t id, const std::string& text) { std::lock_guard g(lock); vehiclePrompt = {id, text}; }

void Update(net::Client& client, const FVector& at, bool canAct, bool shown) {
    bool open, actNow = false;
    std::vector<uint8_t> keys;
    {
        std::lock_guard g(lock);
        hudShown = shown;
        if (answer.pending) { answer.pending = false; client.SendDialogResponse(answer.id, answer.ok, answer.item, answer.text); }
        for (auto& [id, json] : itemActions) client.SendItemAction(id, json);
        itemActions.clear();
        open = dialog.open || itemWin.open;
        // The interaction the player stands at (the nearest).
        prompt = Prompt{};
        const bool act = canAct && !open && !skins::CreatorOpen() && !overlay::CursorMode();
        if (act && !downedHud.on) {
            // Of the interactions in range: the aimed one in sight nearest the middle of the view; else, when only
            // one is in range and roughly in front, that one; else the nearest of those that need no aim.
            const double now = GetTickCount64() / 1000.0;
            struct Cand { uint16_t id; Spot* s; FVector p; float score; int player = -1; };
            std::vector<Cand> aimed;
            Cand lone{}, loose{}; int inRange = 0; float looseD2 = 1e30f; bool haveLone = false;
            for (auto& [id, s] : spots) {
                const float dx = at.X - s.info.x, dy = at.Y - s.info.y, dz = at.Z - s.info.z, d2 = dx * dx + dy * dy;
                if ((s.info.world >= 0 && s.info.world != world) || d2 > s.info.radius * s.info.radius || std::fabs(dz) > 300.f) continue;
                ++inRange;
                const FVector p{s.info.x, s.info.y, s.info.z + s.info.aimHeight};
                if (!s.info.aim || !view.valid) { if (d2 < looseD2) { looseD2 = d2; loose = {id, &s, p, 0}; } continue; }
                const float off = AimAngle(p, s.info.aimRadius);
                if (off <= (id == aimedSpot ? kAimKeep : kAimCone)) aimed.push_back({id, &s, p, off - s.info.priority * 2.f + std::sqrt(d2) * 0.002f});
                else if (off <= kLoneCone) { lone = {id, &s, p, off}; haveLone = true; }
            }
            // Other players, for the player menu: aimed only (a capsule from the feet up), in range.
            if (playerMenu.on && view.valid) for (auto& ps : playerSpots) {
                const float dx = at.X - ps.feet.X, dy = at.Y - ps.feet.Y, dz = at.Z - ps.feet.Z;
                if (dx * dx + dy * dy > playerMenu.range * playerMenu.range || std::fabs(dz) > 250.f) continue;
                // The point of the body's axis (40..150 above the feet) nearest the view's centre line.
                const float t = std::clamp((view.cam.Z + view.fwd.Z * std::sqrt((ps.feet.X - view.cam.X) * (ps.feet.X - view.cam.X) + (ps.feet.Y - view.cam.Y) * (ps.feet.Y - view.cam.Y))) - ps.feet.Z,
                                           ps.downed ? 10.f : 40.f, ps.downed ? 50.f : 150.f);  // a wounded one lies on the ground
                const FVector p{ps.feet.X, ps.feet.Y, ps.feet.Z + t};
                const float off = AimAngle(p, 40);
                if (off <= (int(ps.id) == aimedPlayer ? kAimKeep : kAimCone)) aimed.push_back({kPlayerPrompt, nullptr, p, off + std::sqrt(dx * dx + dy * dy) * 0.002f, int(ps.id)});
            }
            std::sort(aimed.begin(), aimed.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
            const Cand* pick = nullptr;
            int traced = 0;
            for (auto& c : aimed) {
                if (traced++ >= 3) break;
                if (InSight(c.s ? *c.s : playerSight[uint16_t(c.player)], c.p, now)) { pick = &c; break; }
            }
            if (!pick && inRange == 1 && haveLone) pick = &lone;
            if (!pick && loose.s) pick = &loose;
            aimedSpot = pick && pick->s ? pick->id : 0;
            aimedPlayer = pick && pick->player >= 0 ? pick->player : -1;
            if (pick && pick->player >= 0) { prompt = {kPlayerPrompt, playerMenu.key, playerMenu.label}; prompt.player = pick->player; }
            else if (pick) prompt = {pick->id, pick->s->info.key, pick->s->info.text};
            if (!prompt.id && vehiclePrompt.id) prompt = {vehiclePrompt.id, 'E', vehiclePrompt.text, true};
            if (!prompt.id && !carryPrompt.text.empty()) prompt = {0xFFFF, 'F', carryPrompt.text, false, true, carryPrompt.progress};
            // Keep ownership during pickup/put-down acknowledgements too: a held F must not
            // open the NPC dialog in the frame between the animation and the server reply.
            if (carryPrompt.inHands) prompt = {0xFFFF, 'F', carryPrompt.text, false, true, carryPrompt.progress};
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
    else if (owned && !skins::CreatorOpen()) { owned = false; if (!webui::Focused()) overlay::SetVisible(false); }  // (a focused page keeps the cursor)
    // Key presses (edges), only while the game window has the focus.
    static bool down[256] = {};
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::set<uint8_t> want(keys.begin(), keys.end());
    for (int vk = 0; vk < 256; ++vk) {
        const bool now = focused && want.count(uint8_t(vk)) && (GetAsyncKeyState(vk) & 0x8000);
        if (now && !down[vk] && actNow && !downedHud.on) {  // (lying wounded: only Y / N, H and E of the wound)
            uint16_t use = 0; bool watchedKey, vehicle = false; int player = -1;
            { std::lock_guard g(lock); if (prompt.id && !prompt.carry && prompt.key == vk) { use = prompt.id; vehicle = prompt.vehicle; player = prompt.player; } watchedKey = watched.count(uint8_t(vk)) != 0 && !(prompt.carry && prompt.key == vk); }
            if (player >= 0) client.SendPlayerInteract(uint16_t(player));
            else if (use && vehicle) client.SendVehicle(net::Msg::VehicleEnter, use);
            else if (use) client.SendUse(use);
            else if (watchedKey) client.SendKey(uint8_t(vk));
        }
        down[vk] = now;
    }
    // The mode's yes/no question: Y / N (not the game's: they are blocked meanwhile), or it runs out.
    uint16_t asked = 0;
    { std::lock_guard g(lock); if (question.id && GetTickCount64() / 1000.0 > question.until + 1) question = Question{}; asked = question.id; }
    overlay::SetAskKeysBlocked(asked != 0);
    static bool yDown = false, nDown = false;
    const bool y = focused && (GetAsyncKeyState('Y') & 0x8000), n = focused && (GetAsyncKeyState('N') & 0x8000);
    if (asked && actNow && !overlay::Typing() && ((y && !yDown) || (n && !nDown))) {
        client.SendAskAnswer(asked, y && !yDown);
        std::lock_guard g(lock); if (question.id == asked) question = Question{};
    }
    yDown = y; nDown = n;
}

// The item window: the panes side by side, the chosen item's actions and moving it under them.
void DrawItemWindow() {
    if (!itemWin.open) return;
    ImGuiIO& io = ImGui::GetIO();
    const int panes = std::max(1, int(itemWin.panes.size()));
    const float paneW = std::min(420.f, io.DisplaySize.x * 0.4f), listH = std::min(420.f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const std::string title = Plain(itemWin.title.empty() ? std::string(lang::Pick("Предметы", "Items")) : itemWin.title) + "###dgmpitems";
    bool keep = true;
    ImGui::Begin(title.c_str(), &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    const float icon = 36;
    for (int p = 0; p < int(itemWin.panes.size()); ++p) {
        WinPane& pane = itemWin.panes[size_t(p)];
        if (p) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(p);
        ColoredText(pane.title.empty() ? std::string(" ") : pane.title);
        if (!pane.info.empty()) { ImGui::SameLine(paneW - ImGui::CalcTextSize(Plain(pane.info).c_str()).x); ColoredText(pane.info); }
        ImGui::BeginChild("##list", ImVec2(paneW, listH), true);
        if (pane.items.empty()) ImGui::TextDisabled("%s", pane.empty.empty() ? lang::Pick("Пусто", "Empty") : Plain(pane.empty).c_str());
        for (int i = 0; i < int(pane.items.size()); ++i) {
            const WinItem& it = pane.items[size_t(i)];
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##item", itemWin.pane == p && itemWin.item == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, icon))) {
                itemWin.pane = p; itemWin.item = i;
                // A double click moves all of it to the other pane.
                if (ImGui::IsMouseDoubleClicked(0) && panes == 2) ItemAct(json::Value::Object().set("type", "move").set("pane", p + 1).set("item", i + 1).set("count", it.count));
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const images::View v = images::Get(it.icon);
            if (v.texture) dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(v.texture)), at, ImVec2(at.x + icon, at.y + icon));
            else dl->AddRect(at, ImVec2(at.x + icon, at.y + icon), ImGui::GetColorU32(ImGuiCol_Border), 4);
            const float tx = at.x + icon + 8;
            const auto name = Lines(it.name, ImGui::GetColorU32(ImGuiCol_Text))[0];
            DrawLine(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + 1), name, false);
            if (!it.text.empty()) {
                const auto sub = Lines(it.text, ImGui::GetColorU32(ImGuiCol_TextDisabled))[0];
                DrawLine(dl, ImGui::GetFont(), ImGui::GetFontSize() * 0.9f, ImVec2(tx, at.y + icon / 2 + 1), sub, false);
            }
            if (it.count != 1) {
                const std::string n = "x" + std::to_string(it.count);
                const float w = ImGui::CalcTextSize(n.c_str()).x;
                dl->AddText(ImVec2(at.x + ImGui::GetContentRegionAvail().x - w - 4, at.y + (icon - ImGui::GetFontSize()) / 2), ImGui::GetColorU32(ImGuiCol_Text), n.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::EndGroup();
    }
    // The chosen item: what it can do, and moving it.
    const WinItem* sel = itemWin.pane < int(itemWin.panes.size()) && itemWin.item >= 0 && itemWin.item < int(itemWin.panes[size_t(itemWin.pane)].items.size())
        ? &itemWin.panes[size_t(itemWin.pane)].items[size_t(itemWin.item)] : nullptr;
    if (sel) {
        const int p = itemWin.pane + 1, i = itemWin.item + 1;
        for (size_t a = 0; a < sel->actions.size(); ++a) {
            if (a) ImGui::SameLine();
            ImGui::PushID(int(a));
            if (ImGui::Button(Plain(sel->actions[a].second).c_str())) ItemAct(json::Value::Object().set("type", "action").set("pane", p).set("item", i).set("id", sel->actions[a].first));
            ImGui::PopID();
        }
        if (panes == 2) {
            if (!sel->actions.empty()) ImGui::SameLine();
            const char* all = itemWin.pane == 0 ? lang::Pick("Переложить  >>", "Move  >>") : lang::Pick("<<  Переложить", "<<  Move");
            if (ImGui::Button(all)) ItemAct(json::Value::Object().set("type", "move").set("pane", p).set("item", i).set("count", sel->count));
            if (sel->count > 1) {
                ImGui::SameLine();
                if (ImGui::Button(lang::Pick("1 шт.", "1 only"))) ItemAct(json::Value::Object().set("type", "move").set("pane", p).set("item", i).set("count", 1));
            }
        }
    } else ImGui::TextDisabled("%s", lang::Pick("Выберите предмет", "Pick an item"));
    if (!itemWin.hint.empty()) ColoredText(itemWin.hint);
    if (ImGui::Button(lang::Pick("Закрыть", "Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) keep = false;
    ImGui::End();
    if (!keep) { ItemAct(json::Value::Object().set("type", "close")); itemWin.open = false; }
}

void DrawWindows() {
    {
        std::lock_guard g(lock);
        DrawDialog();
        DrawItemWindow();
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
    // Lying wounded: a red edge, the time left and the keys.
    if (downedHud.on) {
        const float pulse = 0.5f + 0.5f * std::sin(float(GetTickCount64() % 100000) / 1000.f * 3.f);
        const ImU32 edge = IM_COL32(150, 0, 0, int(90 + 60 * pulse)), clear = IM_COL32(150, 0, 0, 0);
        const float b = screen.y * 0.18f;
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(screen.x, b), edge, edge, clear, clear);
        dl->AddRectFilledMultiColor(ImVec2(0, screen.y - b), screen, clear, clear, edge, edge);
        ImFont* big = static_cast<ImFont*>(overlay::LargeFont()); if (!big) big = small;
        const int left = std::max(0, int(downedHud.bleed));
        char line[128];
        snprintf(line, sizeof line, "%s  %d:%02d", lang::Pick("Вы тяжело ранены", "You are badly wounded"), left / 60, left % 60);
        const float size = 30, w = big->CalcTextSizeA(size, FLT_MAX, 0, line).x;
        dl->AddText(big, size, ImVec2((screen.x - w) / 2 + 2, screen.y * 0.30f + 2), IM_COL32(0, 0, 0, 200), line);
        dl->AddText(big, size, ImVec2((screen.x - w) / 2, screen.y * 0.30f), IM_COL32(235, 90, 80, 255), line);
        std::string keys = downedHud.help ? std::string(lang::Pick("Помощь вызвана", "Help is called")) : std::string("[H] ") + lang::Pick("Позвать помощь", "Call for help");
        if (downedHud.giveUp <= 0) keys += std::string("     ") + lang::Pick("Удерживайте [E] — сдаться", "Hold [E]: give up");
        else { char t[64]; snprintf(t, sizeof t, "     %s %d", lang::Pick("Сдаться можно через", "Give up in"), int(downedHud.giveUp + 0.99f)); keys += t; }
        const float ks = 22, kw = small->CalcTextSizeA(ks, FLT_MAX, 0, keys.c_str()).x;
        const ImVec2 kp((screen.x - kw) / 2, screen.y * 0.30f + 42);
        dl->AddRectFilled(ImVec2(kp.x - 12, kp.y - 6), ImVec2(kp.x + kw + 12, kp.y + ks + 8), IM_COL32(0, 0, 0, 150), 6);
        dl->AddText(small, ks, kp, IM_COL32(240, 240, 240, 255), keys.c_str());
        if (downedHud.hold > 0) dl->AddRectFilled(ImVec2(kp.x - 12, kp.y + ks + 5), ImVec2(kp.x - 12 + (kw + 24) * downedHud.hold, kp.y + ks + 8), IM_COL32(235, 90, 80, 255), 2);
    }
    // The mode's yes/no question, over the prompt: "text   [Y] Yes  [N] No  12".
    if (question.id) {
        const float size = 21;
        ImFont* font = small;
        const auto lines = Lines(question.text, IM_COL32(240, 240, 240, 255));
        const int left = std::max(0, int(question.until - GetTickCount64() / 1000.0 + 0.99));
        const std::string keys = std::string("[Y] ") + lang::Pick("Да", "Yes") + "    [N] " + lang::Pick("Нет", "No") + "    " + std::to_string(left);
        float w = font->CalcTextSizeA(size, FLT_MAX, 0, keys.c_str()).x;
        for (auto& l : lines) w = std::max(w, LineSize(font, size, l).x);
        const float h = (lines.size() + 1) * (size + 4);
        const float x = (screen.x - w) / 2, y = screen.y * 0.74f - h - 26;
        dl->AddRectFilled(ImVec2(x - 14, y - 10), ImVec2(x + w + 14, y + h + 8), IM_COL32(0, 0, 0, 170), 6);
        float ly = y;
        for (auto& l : lines) { DrawLine(dl, font, size, ImVec2(x, ly), l, true); ly += size + 4; }
        dl->AddText(font, size, ImVec2(x, ly), IM_COL32(242, 209, 107, 255), keys.c_str());
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
        // A key held down (carrying): how long it still has to be held.
        if (prompt.carry && prompt.progress > 0)
            dl->AddRectFilled(ImVec2(x - 12, y + 30 + 5), ImVec2(x - 12 + (total + 24) * prompt.progress, y + 30 + 8), IM_COL32(235, 235, 235, 230), 2);
    }
}

}  // namespace mpui
