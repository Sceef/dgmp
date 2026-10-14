#pragma once
// The editor's in-game tool windows (Dear ImGui through overlay): the item library and the quest
// editor (quests/<id>.json, docs/quests.md). Single-player editor sessions only.
#include "ue.h"
#include <string>

namespace editorui {
void Start();  // once the game window exists
// Game thread, every frame: item catalog, quest files, clicks into the world, preview for the game.
void Update(double now, ue::UObject* pawn, ue::UObject* controller);
// The editor status text (position, fly speed, keys), shown folded in the bottom-left corner.
void SetStatus(const std::wstring& text);
}
