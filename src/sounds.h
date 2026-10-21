#pragma once
// Sounds: the mode's (playSound, docs/API.md "Звуки"), and the shots of other players. The local
// player's shots (the pawn's OnWeaponFired_Event) go to the server, which hands them to the players
// nearby; they hear the game's NPC fire sound of that weapon where the shooter stands. Game thread only.
#include "ue.h"
#include "shared/netclient.h"
#include <string>

namespace sounds {

// path: a SoundBase asset; at null: 2D (no position).
void Play(const std::string& path, const ue::FVector* at, float volume, float pitch);
void OnPlayerShot(uint16_t id, const std::string& weaponClass, const ue::FVector& at);
void OnPawnEvent(ue::UObject* object, const std::string& name);  // observed events (gameevents)
void Update(ue::UObject* pawn, net::Client& client);             // every frame while connected
void Reset();

}  // namespace sounds
