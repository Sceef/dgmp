#pragma once
// Development console for reverse engineering in a running game. Active only when the folder
// <DGMP root>/dev exists: write "<seq>\n<command>" to dev/cmd.txt, read the answer in dev/out.txt.
#include "ue.h"

namespace devcmd {
void Update(ue::UObject* pawn, ue::UObject* controller, ue::UObject* gameMode);  // game thread
}
