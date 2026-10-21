#pragma once
// The mode's own icons on the big map and the radar (createBlip, docs/API.md "Значки на карте"): an
// invisible holder actor at the spot with the game's MiniMapLocationComponent, an icon of the game's
// map (MAP_*) tinted in the blip's colour. Game thread only.
#include "ue.h"
#include "shared/netclient.h"

namespace blips {

void OnBlip(uint16_t id, const net::Client::Events::BlipInfo* info);  // info null = removed
void SetWorld(int world);
void Reset();
void Update(ue::UObject* pawn);  // every frame while connected (spawns what is new; worlds)

}  // namespace blips
