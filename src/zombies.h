#pragma once
// The game's freakers in multiplayer (docs/zombies.md). The server decides where they are; for each one
// a single client (its owner: the player it is near) runs the game's own AI on a real pawn, and every
// other client shows the same pawn as a puppet: AI stopped, walked by movement input along the owner's
// reports. Game thread only.
#include "ue.h"
#include "shared/netclient.h"
#include <string>

namespace zombies {

// Research and building blocks (dev console "z ...").
// A freaker of this faction (Swarmer, Newt, ...: EAIFactionType names the ambient generator has loaded)
// spawned by the game's arcade spawner at the point. The pawn arrives a few frames later (Update).
bool Spawn(const std::string& faction, const ue::FVector& at, float yaw, bool chase, std::string* error = nullptr,
           int count = 1, ue::UObject* crowdGroup = nullptr);  // crowdGroup: a BendCrowdGroup to join (hordes)
void SetBrain(ue::UObject* pawn, bool on);                        // its behaviour tree runs / is stopped
void Drive(ue::UObject* pawn, const ue::FVector& to, float yaw);  // a puppet walks there (Update moves it)
void Mirror(ue::UObject* source, ue::UObject* puppet, float delay, const ue::FVector& offset);  // local network test
void Release(ue::UObject* pawn);                                  // no longer driven
ue::UObject* LastSpawned();                                       // the newest pawn Spawn produced
float Health(ue::UObject* pawn);                                  // -1: not a pawn with health
void Kill(ue::UObject* pawn);                                     // lethal damage: it dies the game's way
std::string Describe();                                           // what is driven now

void Update(double now, float dt, ue::UObject* localPawn);

// Multiplayer: the server's zombies (Msg::Zombie / Msg::ZombieMoves) and, every frame while connected,
// spawning them near this player, running the owned ones and reporting them, walking the puppets.
void OnMessage(net::Msg op, const uint8_t* data, size_t size);
void Net(double now, ue::UObject* localPawn, net::Client& client, int world);
bool IsOurs(ue::UObject* pawn);  // a server zombie's pawn here (the AI sweep leaves it alone)
// A watched pawn took point or radial damage (policy's damage watcher); byUs: this player caused it.
void OnDamage(ue::UObject* actor, float damage, bool byUs, ue::UObject* causer);
bool Active();                   // the server has zombies: the arcade spawner must stay usable
void Reset();                    // disconnected: they go

}  // namespace zombies
