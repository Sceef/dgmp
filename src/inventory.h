#pragma once
// Player inventory and bike control used by the server's Lua API (give/take items, weapons, bike)
// and the inventory snapshot reported back to the server.
#include "ue.h"
#include "shared/protocol.h"
#include <string>
#include <utility>
#include <vector>

namespace inventory {
using Snapshot = std::vector<std::pair<std::string, int>>;  // item key -> amount, sorted by key

// Game thread. Item keys are the game's KeyName, e.g. kWeaponID_AK47, kAmmoID_AK47, kConsumableID_Bandage_Small.
void Apply(net::ItemAction action, const std::string& key, int amount);
// Frozen: no moving; on a bike no throttle, brake or getting off (race countdowns).
void SetFrozen(bool on);
void RequestBike(net::BikeAction action, const ue::FVector& pos, float yaw);  // executed by Update
void Update(ue::UObject* pawn, ue::UObject* controller);
bool OwnsBike();
// Fills `out` and returns true when the inventory differs from the last reported snapshot.
bool TakeSnapshot(Snapshot& out, double now);
void Reset();  // world reload: forget cached objects
ue::UObject* FindItem(const std::string& key);
// All item definitions (KeyName -> InventoryItem), empty until the game's item manager exists.
std::vector<std::pair<std::string, ue::UObject*>> AllItems();  // InventoryItem definition by KeyName (case-insensitive), or null
}
