#pragma once
// Boxes the players carry in both hands (docs/API.md "Переноска"), the story's dynamite crate: the game's
// own carried item (Bend_WpnMelee_Crate_MBU3, its pick-up animation and carry walk). A box on the ground
// is a pickup of the game on every client with its own interaction off; hold F at it to ask the server,
// which puts it into this player's hands (ForcePickup: the native animation). Hold F with it to ask to
// set it down; the game taking it away (a hit, the bike, parkour) is reported with the spot where it fell.
// While carrying there is no punching; a dodge roll keeps, drops or breaks the box (its `roll`). The
// others see the box in the carrier's hands through its gear. Game thread only.
#include "ue.h"
#include "shared/netclient.h"
#include <string>

namespace carry {

void OnCarry(uint16_t id, const net::Client::Events::CarryInfo* info);  // info null = removed
void SetWorld(int world);
void Reset();  // disconnected: the boxes go, the one in the hands too

// Every frame while connected. `canAct`: the player plays (no menu, chat, dialog or death).
void Update(double now, ue::UObject* pawn, net::Client& client, bool canAct);
bool IsBoxActor(ue::UObject* actor);  // a pickup this module placed (its game events are not reported)
// Events of observed objects (gameevents' observer passes them on): the local pawn's dodge roll.
void OnPawnEvent(ue::UObject* object, const std::string& name);

// The models a box can be (Carry::KnownModel on the server): "crate", ...
struct ModelInfo { const char* name; const char* ru; const char* en; };
const ModelInfo* Models(int& count);
// The editor's look at a box: the model's pickup lying there, the game's interaction off (null: unknown model).
ue::UObject* SpawnPreview(const std::string& model, const ue::FVector& at, float yaw, ue::UObject* context);

}  // namespace carry
