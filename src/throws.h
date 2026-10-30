#pragma once
// Thrown rocks others see: this player's rock, as it leaves the hand, is reported (its mesh, where and how
// fast); another player's rock flies here as a physics copy of that mesh, then lies for a while. The copy
// hurts no one: the thrower's own game hits and reports, as with bullets. Game thread only.
#include "ue.h"
#include <cstdint>
#include <string>

namespace net { class Client; }

namespace throws {

// Every frame in a session: watches the rock weapon, reports new throws, retires old copies.
void Update(double now, ue::UObject* pawn, net::Client& client);
// A script event of an observed object (policy observer): the rock weapon's OnFired.
void OnEvent(ue::UObject* object, const std::string& name);
// Another player threw: a copy of `mesh` (scale `scale`) from `at` with velocity `vel`.
void OnThrow(uint16_t player, const std::string& mesh, float scale, const ue::FVector& at, const ue::FVector& vel);
void Reset();  // disconnected: the copies go

}  // namespace throws
