#pragma once
// The mode's own images (docs/API.md "Images"): PNG, JPG or GIF files of modes/<mode>/images, sent by
// the server and kept in cache/images by their hash. Drawn by the overlay (HUD, dialogs, icons in the
// world), not by the game's own interface.
#include "shared/netclient.h"
#include <string>
#include <utility>
#include <vector>

namespace images {

// Game thread: the server's list (name, sha256): asks for the ones not cached; their parts arrive.
void OnList(const std::vector<std::pair<std::string, std::string>>& list, net::Client& client);
void OnData(const std::string& name, int part, int parts, const std::string& bytes);
void Reset();  // disconnected: the names are forgotten (the textures stay for the next session)

// Frame thread (inside an overlay callback): the image's texture, made on its first use; empty while
// it has not arrived. w, h: its size in pixels.
struct View { void* texture = nullptr; float w = 0, h = 0; };
View Get(const std::string& name);

}  // namespace images
