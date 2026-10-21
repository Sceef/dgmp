#pragma once
// A mode's own edits of the game world (modes/<mode>/world.json, docs/world.md): objects of the game
// removed or moved, and props and lights added (a light is an added entry with asset "light:point"
// or "light:spot" and its settings in "light"). Applied by the multiplayer client (sent by the server on join)
// and by the editor (live preview while editing). Game thread only.
#include "ue.h"
#include "shared/json.h"
#include <string>
#include <vector>

namespace worldedit {

// Replaces the edits. `preview` (editor): removed objects are only hidden, so that undoing a removal
// can bring them back; in a game session they are destroyed.
void Set(const json::Value& edits, bool preview);
void Clear();

// Every frame: finds world objects as levels stream in and applies the edits; spawns added props
// near the player. `pawn` is the world context.
void Update(ue::UObject* pawn);

// A static mesh (or a blueprint class "..._C") of the game spawned movable at that spot: the mode's
// objects of createObject (src/objects.cpp). Null: the asset does not load.
ue::UObject* SpawnMesh(const std::string& asset, const ue::FVector& at, const ue::FRotator& rot, float scale, ue::UObject* context);
void PlaceActor(ue::UObject* actor, const ue::FVector& at, const ue::FRotator& rot, float scale);

// The stable name of a world object: "<level>/<actor>", e.g. "03-12_Pop/BP_Interact_SearchCarTrunk21".
std::string KeyOf(ue::UObject* actor);
// Merged mesh collections (BendStaticMeshCollection): the piece of the collection actor at a world
// point (-1: none), and Detach, which replaces the collection by separate actors, one per piece, and
// returns the piece's actor. Pieces are keyed "<level>/<collection>#<index>".
// False for collections of roads and curbs (spline pieces cannot be recreated yet).
bool Editable(ue::UObject* collectionActor);
int PieceAt(ue::UObject* collectionActor, const ue::FVector& at);
// `part`: return the piece itself instead of its object's leading piece.
ue::UObject* Detach(ue::UObject* collectionActor, int piece, ue::UObject* context, bool part = false);
// A part of such an object has its own key "<object key>:<index>" (empty for other actors).
std::string PartKey(ue::UObject* actor);
// The actor whose transform stands for a selection: an object's leading piece, a part, a world object.
ue::UObject* Primary(const std::string& key);
// Actors that make up a selected world object (several for an object of a collection).
std::vector<ue::UObject*> Members(const std::string& key);
// Actor of an added prop (by its uid) or of a world object (by key), if it is loaded.
ue::UObject* FindAdded(const std::string& uid);
ue::UObject* FindWorld(const std::string& key);

}  // namespace worldedit
