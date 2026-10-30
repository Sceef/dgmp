#pragma once
// Multiplayer client session inside the game: connection to dgmp-server, local state/pose upload,
// remote players (ghost bodies, bikes, 3D nameplates), chat and server commands.
// Enabled when the launcher passes DGMP_CONNECT / DGMP_NICK (/ DGMP_PASSWORD).
#include "ue.h"
#include <string>
#include <vector>

namespace mp {

enum class Phase { Off, Connecting, Joined, Failed };

bool Init();      // reads the environment; true = this game runs as a multiplayer client
bool Enabled();
Phase GetPhase();
const std::wstring& FailReason();

// Game thread, every frame. `inWorld`: world loaded, player spawned, not in the main menu.
// `gameplay`: additionally no menu/map/loading cover (chat may open).
void Update(double now, float dt, ue::UObject* pawn, ue::UObject* controller, bool inWorld, bool gameplay);

// A teleport the server asked for (spawn after joining, /tp ...). Consumed by the caller.
// The player died: the loading cover stays up until the server's respawn teleport.
bool Respawning();
bool TakeTeleport(ue::FVector& pos, float& yaw, bool& initial);

// Editor (no server): shows the quest open in the in-game quest editor (quest_preview.json) as
// quest markers in the world, the selected step highlighted. Game thread, every frame.
void UpdateEditorPreview(double now, ue::UObject* pawn);

// Bodies of the remote players (research tools).
std::vector<ue::UObject*> GhostActors();
// The remote players' bodies, for the dev console (ghosts): where, what is shown, the pose and its twins.
std::string DescribeGhosts();
// The remote players drawn here now: id, where the body stands (feet), alive (not dead or hidden).
struct RemoteBody { uint16_t id; ue::FVector pos; bool alive; };
std::vector<RemoteBody> RemoteBodies();
// The remote player whose body (or bike) this actor is: its id, 0 none.
uint16_t PlayerOfActor(ue::UObject* actor);

void Shutdown();

}  // namespace mp
