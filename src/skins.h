#pragma once
// Character skins: a worn look is either a look asset ("/Game/Characters/.../looks/x.x") or a skin made
// in the character creator, "skin:<look name>;<slot>=<part id>;..." - a look of the game to start
// from, with some of its parts swapped for other parts of the same body (data/parts.txt ids; "-" takes
// the slot off). Skins fit in the look messages and are worn the same way on the own body, on other
// players' bodies and on corpses.
#include "ue.h"
#include <string>
#include <vector>

namespace skins {

bool IsSkin(const std::string& look);
// The look asset a look or skin starts from (its path), "" if unknown.
std::string BaseLook(const std::string& look);
// A look name of data/looks.txt as its asset path (paths and skins as they are; "" if unknown), and
// the names of the people's looks.
std::string LookPath(const std::string& look);
std::vector<std::string> LookNames();
// Wears a look or a skin on a skeletal mesh component (SetLook, then the part swaps).
bool Wear(ue::UObject* mesh, const std::string& look);
// Parts this module added to a mesh (slots its base look does not have); posed by the mesh.
std::vector<ue::UObject*> ExtraParts(ue::UObject* mesh);

// The character creator: the player picks a body, a starting look and parts, seen live on itself.
// With `choices` it is a picker instead: only those looks (a mode's skins made in the editor).
struct Choice { std::string name, look; };
struct Options {
    std::string title;
    bool cancel = true;                  // the player may close it without choosing
    std::vector<std::string> families;   // "male", "female", "deacon", "story"; empty: all
    std::string current;                 // look or skin to start from
    std::vector<Choice> choices;         // the picker's looks; empty: the full creator
    bool embedded = false;               // drawn by the editor (DrawCreator), the editor's input and windows stay
    float rootHeight = 0;                // the body's root above its feet minus a character's (a mannequin: -90)
    bool hasYaw = false; float yaw = 0;  // the body turns this way first (a choosing place's direction)
};
void OpenCreator(const Options& options);
bool CreatorOpen();
// Asks an open creator to close (cancelled); UpdateCreator finishes it.
void CloseCreator();
// The creator's window (the editor's windows or the multiplayer ones draw it); the overlay's frame thread.
void DrawCreator();
// Game thread, every frame while connected. When the player finishes, returns true with `ok`
// (false: cancelled) and the chosen skin.
bool UpdateCreator(ue::UObject* pawn, ue::UObject* controller, ue::UObject* mesh, bool& ok, std::string& skin);

}  // namespace skins
