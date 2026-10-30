#pragma once
// The Tab panel (docs/API.md "Список игроков"): everyone on the server with its ping, who talks now and
// the mode's columns (setScoreboard); muting a player for yourself and its voice volume. Mutes and
// volumes are kept in DGMP.ini [Players] under the player's Steam account (or nickname) and sent to the
// server, which then does not send that voice at all (Msg::VoiceIgnore).
#include "shared/netclient.h"
#include <string>

namespace players {

void OnList(const std::string& json);  // Msg::PlayerList (game thread)
// Game thread, every frame while connected: Tab opens and closes it (canOpen: playing, no chat or dialog).
void Update(net::Client& client, bool canOpen);
void Draw();    // render thread (overlay windows)
bool Open();
void Reset();   // the connection is gone

}  // namespace players
