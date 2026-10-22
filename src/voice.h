#pragma once
// Voice chat (docs/API.md "Голосовой чат"). Push-to-talk capture goes through the game's own Steam
// (ISteamUser voice: the microphone chosen in Steam, Opus inside); the server relays the frames to the
// players who hear them. Other players' voices play through XAudio2: near ones at their bodies (3D, the
// listener at the game camera), radio and phone ones without a position, band-limited.
// Keys and volumes: DGMP.ini [Voice] (TalkKey N, RadioKey K, Volume, MicVolume, Microphone, OpenMic), set in /voice.
#include "ue.h"
#include "shared/netclient.h"
#include <cstdint>
#include <string>

namespace voice {

void OnSetup(uint8_t flags, const std::string& radio, const std::string& call);  // net::VoiceFlags
void OnFrame(uint16_t id, net::VoiceRoute route, net::VoiceCodec codec, int gain, float range, const uint8_t* frame, size_t size);  // gain: the speaker's microphone %
// Game thread, every frame while connected. canTalk false (chat or a dialog open, a menu) lets the keys go.
void Update(net::Client& client, bool canTalk, const ue::FVector& camPos, const ue::FRotator& camRot);
bool Talking(uint16_t id);  // that player's voice is heard right now (the mark at its health bar)
void DrawHud();             // render thread (overlay HUD layer): the microphone mark, who talks on the radio
// The settings window (chat command /voice, handled here, never sent): microphone and voices volume, a
// microphone test (hear yourself, level meter), keys, always-on microphone. Saved to DGMP.ini [Voice].
void OpenSettings();        // game thread
void DrawWindow();          // render thread (overlay windows)
void Reset();               // the connection is gone: silence, no recording

}  // namespace voice
