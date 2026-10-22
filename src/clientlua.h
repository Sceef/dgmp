#pragma once
// The mode's client scripts (docs/client-lua.md): Lua files of modes/<mode>/client, sent by the server
// like the HTML UI's files (cache/scripts/<sha256>) and run here in a sandbox: no files, no system, no
// bytecode, a time limit per call and a memory limit. They draw over the game, answer keys and chat
// commands at once, and talk to the server's Lua (send / onServer) and the mode's page (uiSend / onUi).
//
// Threads: everything runs on the game thread except Draw (an overlay callback on the frame thread).
#include "ue.h"
#include "shared/netclient.h"
#include <string>
#include <utility>
#include <vector>

namespace clientlua {

// The server's list (path, sha256): the files not cached are asked for; their parts arrive. A new list
// (the mode restarted) stops the scripts; they start again once every file is here.
void OnList(const std::vector<std::pair<std::string, std::string>>& files, net::Client& client);
void OnData(const std::string& path, int part, int parts, const std::string& bytes);
// The server's Lua sent the scripts a message (player:clientSend): onServer(name, fn).
void OnServer(const std::string& name, const std::string& json);
void Reset();  // disconnected: the scripts stop

// What the scripts see of this frame.
struct Frame {
    float dt = 0;
    ue::UObject* pawn = nullptr;
    ue::UObject* controller = nullptr;
    bool hudShown = false;   // their drawing is shown (no menu, map or loading)
    bool canAct = false;     // their keys work (playing; no chat, dialog, focused page or death)
    uint16_t id = 0;
    std::string name;
    int world = 0;
    bool dead = false;
    bool haveState = false;
    net::PlayerState state;  // the player (actor root), its flags and health
    ue::FVector cam{};
    ue::FRotator camRot{};
};
// Every frame while connected and in the world: timers, keys, on("frame") and what they drew.
void Update(const Frame& frame, net::Client& client);

// A chat line the player typed: true if a script's command("name", fn) took it (it is not sent).
bool OnChat(const std::string& line);
// A message of the mode's page (dgmp.send): true if a script's onUi(name, fn) took it (it is not sent).
bool OnPageMessage(const std::string& name, const std::string& json);

// Overlay HUD layer: what the scripts drew this frame.
void Draw();

}  // namespace clientlua
