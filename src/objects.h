#pragma once
// The mode's objects (createObject on the server, docs/API.md "Объекты"): props of the game spawned
// movable near the player, in its virtual world; a move (moveObject) slides them to the target in the
// time the server gave, the same on every client. Game thread only.
#include "ue.h"
#include "shared/netclient.h"

namespace objects {

void OnObject(uint16_t id, const net::Client::Events::ObjectInfo* info);  // info null = removed
void OnMove(uint16_t id, const float to[6], float seconds, float elapsed);
void SetWorld(int world);
void Reset();                              // disconnected: every object goes
void Update(double now, ue::UObject* pawn);  // every frame while connected

}  // namespace objects
