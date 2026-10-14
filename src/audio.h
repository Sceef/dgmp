#pragma once
// Silences the game's own mixer (SFX / music / voice buses of BendAudioManager) while DGMP boots the
// world, so menu music and NPC chatter from the template save are never heard. Volumes return to the
// player's settings (GameUserSettings) afterwards. Nothing is changed in the Windows mixer.
namespace audio {
void Update(bool muted);  // game thread, every frame
}
