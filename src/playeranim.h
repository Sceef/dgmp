#pragma once
// Animations the mode plays on the local player (player:playAnim, docs/API.md "Анимации игрока"): an
// animation of the game as a dynamic montage in the FullBody slot of Deacon's animation blueprint. The
// others see it through the streamed bone pose. Optionally the player cannot walk meanwhile, and a
// move key ends it (reported to the server: animEnd). Game thread only.
#include "ue.h"
#include "shared/netclient.h"
#include <string>

namespace playeranim {

void OnPlay(const std::string& path, uint8_t flags, float rate);  // path "": stop (net::AnimFlags)
void Reset();                                                      // disconnected
bool IsPlaying(const std::string& path);                             // this animation plays now (or is about to)
// Every frame while connected. `canAct`: the player plays (no menu, chat, dialog or death).
void Update(ue::UObject* pawn, ue::UObject* controller, net::Client& client, bool canAct, bool dead);

}  // namespace playeranim
