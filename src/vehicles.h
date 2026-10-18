#pragma once
// The mode's vehicles (docs/API.md "Vehicles"): every parked server vehicle is a real vehicle of the game
// (a bike, or a model: the golf cart, the trucks) on every client. A player gets on one only when the
// server allows it (lock, owner, the mode); while someone rides a bike the others see that player's bike
// instead; a model stays and follows its driver. Where it is left it stands again for everyone. Game
// thread only.
#include "ue.h"
#include "shared/netclient.h"

namespace vehicles {

void OnVehicle(uint16_t id, const net::Client::Events::VehicleInfo* info);  // info null = removed
void OnAllow(uint16_t id, int seat);  // get on it (seat 0: at the wheel; 1: behind the driver; a helicopter: 1-4)
void OnEject(uint16_t id);  // get off it
void OnRoute(uint16_t id, const std::vector<float>& points, float speed, float elapsed, bool loop);  // its NPC's way
void SetWorld(int world);
void Reset();  // disconnected: the bikes go

// Every frame while connected: the bikes, the "[E] Get on" prompt, getting on, reports of the ridden one.
void Update(double now, ue::UObject* pawn, ue::UObject* controller, net::Client& client);
bool IsServerBike(ue::UObject* bike);  // a server vehicle's bike on this client
ue::UObject* AnyBike();                // some bike standing here (its meshes draw other players' bikes)
// That player drives a model (not a bike), or a vehicle this player rides behind them on: its vehicle
// here, which follows the player (set its place from the player's state), or null while there is none
// here yet. False: on no such vehicle (the player's bike is drawn as theirs).
bool DrivesModel(uint16_t playerId, ue::UObject*& vehicle);
bool RidingBehind(ue::UObject* vehicle);
ue::UObject* HelicopterSeat();             // the helicopter this player sits in (null: none)  // this player is on it as a passenger (its rider is someone else)
bool IsModel(ue::UObject* vehicle);  // a server vehicle that is not a bike

}  // namespace vehicles
