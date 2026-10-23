#pragma once
// Multiplayer overlay over the game's client area: chat (T to type), centre banners, status line.
// Nameplates are not here: they are 3D text in the world (see mp.cpp).
// Drawing runs on its own thread; the game thread publishes state and reads submitted chat lines.
#include <cstdint>
#include <string>
#include <vector>

namespace hud {

void Start();
void SetVisible(bool on);         // whole overlay (hidden in menus / loading)
void SetChatAllowed(bool on);     // T opens chat only in gameplay
void SetStatus(const std::wstring& text);                                // top-right line
void SetBanner(const std::wstring& title, const std::wstring& text);     // persistent centre banner ("" clears)
void ShowHint(const std::wstring& title, const std::wstring& text, double seconds);
void AddMessage(uint32_t rgba, const std::wstring& text);
bool ChatOpen();
std::vector<std::wstring> TakeSubmitted();  // lines typed by the player since the last call
void Submit(const std::wstring& line);      // a line as if the player typed it (dev console "say")

}  // namespace hud
