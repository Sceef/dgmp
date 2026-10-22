#pragma once
// The mode's HTML UI (docs/webui.md): pages of modes/<mode>/ui shown over the game, like FiveM's NUI.
// A WebView2 runs off screen on its own thread (visual hosting); Windows.Graphics.Capture turns its
// visual into frames that the overlay draws. The page and the server's Lua talk through dgmp.send /
// dgmp.on (Msg::UiMessage / Msg::Ui).
#include "shared/netclient.h"
#include <string>
#include <utility>
#include <vector>

namespace webui {

// Game thread: the mode's files (path, sha256): the ones not in cache/ui are asked for; their parts arrive.
void OnList(const std::vector<std::pair<std::string, std::string>>& files, net::Client& client);
void OnData(const std::string& path, int part, int parts, const std::string& bytes);
// Game thread: the server opens, closes, focuses the page or sends it a message.
void OnUi(net::UiOp op, const std::string& name, const std::string& text, bool focus);
void Reset();  // disconnected: the page closes (the browser stays for the next server)
// Game thread, every frame: the page's messages to the server; the cursor while the page has the focus.
void Update(net::Client& client);

// The page has the mouse and keyboard (the game none).
bool Focused();

// Hooks the page into the overlay (drawing, input).
void Install();

}  // namespace webui
