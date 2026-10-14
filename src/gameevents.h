#pragma once
// Gameplay events of the local player reported to the server for mode scripts and quest objectives:
// picked up an item, searched a car / a body, used an interactable, opened a door, an explosive
// blew up, something was set on fire. Found by hooking the world objects the player acts on.
#include "ue.h"
#include "shared/protocol.h"
#include <string>
#include <vector>

namespace gameevents {
struct Event {
    net::GameEventType type;
    std::string cls;     // actor class, e.g. BP_Interact_SearchCarTrunk_C
    std::string detail;  // pickup: item key (kConsumableID_Molotov ...)
    std::string id;      // actor name, stable for placed world objects
    ue::FVector at{};
};
// Game thread: hooks newly loaded objects and collects events. Returns the events since last call.
void Update(ue::UObject* pawn, double now, std::vector<Event>& out);
void Reset();
// Pickups placed by the server (quest items): spawned near the player, reported as pickups with
// id "dgmp_item_<id>". key null removes it.
void SetWorldItem(uint16_t id, const std::string* key, int count, const ue::FVector& pos);
}
