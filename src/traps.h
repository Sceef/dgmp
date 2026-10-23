#pragma once
// The mode's traps (createTrap on the server, docs/API.md "Ловушки"): the game's own mine, IED and bear trap
// actors (their own triggers and pickups switched off: the server decides when they go off), a tripwire drawn
// between its two ends with an IED at the first one, or nothing for an invisible zone. Fire: an explosive
// explodes the game's way (it hurts this player and the zombies this game runs), a bear trap catches its
// victim on the victim's screen and snaps shut on the others. Game thread only.
#include "ue.h"
#include <cstddef>
#include <cstdint>

namespace traps {

void OnMessage(const uint8_t* data, size_t size);  // Msg::Trap without the op byte
void SetWorld(int world);
void Reset();                                       // disconnected: every trap goes
void Update(ue::UObject* pawn, uint16_t localId);   // every frame while connected
// This player is held by a bear trap (the game's mash-F escape runs): the mode's keys and prompts must not take F.
bool Caught();
// This player died (the multiplayer death keeps the same body): the bear trap holding it lets go and goes.
void OnLocalDeath(ue::UObject* pawn);
// A lethal bear trap bled this player out: the multiplayer death now (consumed).
bool TakeBledOut();
// Overlay HUD layer (frame thread): the red of bleeding in a bear trap.
void Draw();

}  // namespace traps
