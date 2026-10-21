#pragma once
// The Steam account the game runs under, through the game's own steam_api64.dll (Steamworks 1.48 flat
// API). The server checks the auth session ticket with the Steam Web API, so a player cannot claim
// another player's SteamID (admins and bans are kept by SteamID).
#include <cstdint>
#include <string>

namespace steam {
bool Identity(uint64_t& steamId);   // false: Steam is not running in this process
bool NewTicket(std::string& hex);   // a fresh ticket for one connection (the previous one is cancelled)
void CancelTicket();                // once the server let us in, or the connection is gone
}
