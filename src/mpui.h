#pragma once
// Multiplayer UI the server's mode drives (docs/API.md "Dialogs", "HUD", "Interactions", "Keys"):
// dialogs (message, input, password, list), HUD elements (text and bars), the "[E] ..." prompt at
// interactions and the presses of keys the mode watches.
//
// Threads: the On* functions and Update run on the game thread; DrawWindows and DrawHud are overlay
// callbacks on the frame thread.
#include "ue.h"
#include "shared/netclient.h"
#include <string>
#include <vector>

namespace mpui {

void OnDialog(const net::Client::Events::DialogInfo& d);
void OnHud(const std::string& id, const std::string& json);
void OnInteraction(uint16_t id, const net::Client::Events::InteractionInfo* info);
void OnWatchKeys(const std::vector<uint8_t>& keys);
void Reset();  // the connection is gone: nothing of the server stays on screen
void SetWorld(int world);  // interactions of other virtual worlds are not there
// A server vehicle the player stands at: "[E] text"; E asks the server to get on it (id 0: none).
void SetVehiclePrompt(uint16_t id, const std::string& text);
// The mode's icons in the world where they are on screen this frame (game thread): the bottom centre of
// the picture at x, y; scale shrinks it with distance.
struct WorldIcon { float x, y, size, scale; unsigned color; std::string image, text; };
void SetIcons(std::vector<WorldIcon> list);

// Every frame while connected. `at`: the player (actor root); `canAct`: it plays (no menu, chat or
// death). Sends the dialog answer, interaction uses and watched key presses.
void Update(net::Client& client, const ue::FVector& at, bool canAct, bool hudShown);
bool DialogOpen();

// Overlay callbacks: the windows (the dialog and the character creator) and the HUD layer.
void DrawWindows();
void DrawHud();

}  // namespace mpui
