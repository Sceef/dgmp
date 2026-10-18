#pragma once
#include "ue.h"
#include <cstdint>
#include <vector>
namespace policy {
struct State {
    bool photoDisabled=false;
    bool mapRevealed=false;
    bool fogMaterialOverridden=false;
    bool allRegions=false;
    bool mapOnly=false;
    bool menuActive=false;
    int mapEnum=-1;
    unsigned int blockedPhoto=0,blockedCombat=0,blockedMenus=0,hiddenControls=0;
    unsigned int blockedGameplay=0,hiddenMarkers=0,visibleFuelMarkers=0,disabledZones=0,hiddenHudControls=0;
    bool playerVODisabled=false,openWorldVOPaused=false,zoneOverlayHidden=false;
    bool trackingCleared=false;
    unsigned int activeNativeJobs=0,pausedNativeJobs=0;
    bool nativeMissionActive=false;
    bool savingBlocked=false;
    unsigned int blockedSaveRequests=0,hiddenSaveControls=0;
    unsigned int removedPauseControls=0,blockedPauseRequests=0;
    unsigned int blockedTutorials=0,dismissedTutorials=0;
    unsigned int nestObjectsHidden=0,storyBlockersDisabled=0,doorsUnlocked=0,zoneParametersCleared=0;
    uint32_t markerMask=(1u<<0)|(1u<<1)|(1u<<7);
};
void ConfigureMarkers(uint32_t mask);
// Map markers created by DGMP itself (other players): always shown, whatever the category mask.
void AllowMarker(ue::UObject* component,bool allowed);
// A marker of the game kept off the map and radar whatever its category (a bike that is not yours).
void DenyMarker(ue::UObject* component,bool denied);
// Shown on the radar (minimap) only, not on the big map.
void RadarOnly(ue::UObject* component,bool radarOnly);
// While true the game's mission waypoint and its road route belong to a server marker: the story
// suppression stops hiding them.
void SetRouteOwned(bool owned);
// While a server quest is shown, the objective HUD (panel, popups, meter) is not hidden or blocked.
void SetQuestHud(bool active);
// The completion/reward screens of a server quest pause the game and expect a game mission to
// unpause it: for a while, a pause with nothing on screen is lifted.
void WatchRewardScreens();
bool RewardPanelOpen();  // the quest reward panel is up
bool TakeDeath();  // the local player died since the last call
void SetEditorSession(bool on);
// The game never pauses when its window loses focus (editor and multiplayer). Once, after ue::Init.
void DisableFocusPause(ue::UObject* engine);
// Multiplayer: the game's menus (pause menu, map) open without pausing the world.
void DisableMenuPause();  // single-player editor: no pause when the window loses focus
// Multiplayer: a hit that would kill the local player is cancelled (ModifyDamage returns 0) and
// reported here instead; the mode's death (body, respawn) follows. Set by mp.
void SetInterceptLethalHits(bool on);
bool TakeLethalHit();
void SetBlockDeathReload(bool on);  // multiplayer: the game's checkpoint reload after a death is skipped
bool TakeDeathReloadBlocked();     // it was skipped since the last call (the player lies dead)
// Server quest pickups: offered as takeable even with a full backpack.
void SetTakeable(ue::UObject* pickup,bool takeable);
// Dev: logs every distinct event the local pawn, controller and their components receive for
// `seconds` (finding the game functions behind looting, pickups, interactions...). Returns hooked count.
// `extra`: also these objects and their components.
// Point and radial damage taken by watched actors (remote players' bodies): the actor, the hit
// component (null for radial damage), the damage, the instigating controller and the damage causer.
using DamageWatcher = void (*)(ue::UObject* actor, ue::UObject* component, float damage, ue::UObject* instigator, ue::UObject* causer);
void SetDamageWatcher(DamageWatcher watcher);
void WatchDamage(ue::UObject* actor);
int TraceEvents(double seconds, const std::vector<ue::UObject*>& extra = {});
// Game events (gameevents.cpp): every event of an observed object is passed to the observer.
void SetObserver(void(*observer)(ue::UObject* object,const std::string& name));
void Observe(ue::UObject* object);
// Shows the game's own tutorial popup (title, text, optional Texture2D asset path) for a mode/server hint.
// Empty title = survival-tip line at the bottom, closed automatically after `seconds` (default 5).
bool ShowNativeHint(ue::UObject* controller,const std::wstring& title,const std::wstring& text,const std::string& image,bool pause,float seconds=0);
void Update(ue::UObject* pawn,ue::UObject* controller,ue::UObject* gameMode,bool editor,bool reveal,bool mapOnly,bool suppressStory);
State GetState();
void OpenMap();
void CloseMap();
void TestEvent(const char* event);
}
