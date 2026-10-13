#pragma once
#include "ue.h"
#include <cstdint>
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
void Update(ue::UObject* pawn,ue::UObject* controller,ue::UObject* gameMode,bool editor,bool reveal,bool mapOnly,bool suppressStory);
State GetState();
void OpenMap();
void CloseMap();
void TestEvent(const char* event);
}
