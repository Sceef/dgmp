#pragma once
#include <stdint.h>
#if defined(DGMP_BUILD) && defined(_WIN32)
#define DGMP_API __declspec(dllexport)
#else
#define DGMP_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Early local client API. Resolve these exports from the DGMP version.dll.
   Calls enqueue policy changes; Unreal objects are updated on the game thread.
   This is a local client API. It does not implement server persistence or replication. */
enum DGMP_MapCategory {
    DGMP_MAP_PLAYER=0, DGMP_MAP_MOTORCYCLE=1, DGMP_MAP_ENEMY=2,
    DGMP_MAP_NEST=3, DGMP_MAP_OBJECTIVE=4, DGMP_MAP_SIDE_OBJECTIVE=5,
    DGMP_MAP_SAFEHOUSE=6, DGMP_MAP_FUEL=7, DGMP_MAP_MERCHANT=8,
    DGMP_MAP_WAYPOINT=9, DGMP_MAP_CUSTOM=10, DGMP_MAP_OBJECTIVE_REGION=11,
    DGMP_MAP_DEAD_FRIEND=12, DGMP_MAP_NEST_ZONE=13, DGMP_MAP_MISSION=14,
    DGMP_MAP_JOB=15, DGMP_MAP_JOB_OBJECTIVE=16, DGMP_MAP_BUNKER=17,
    DGMP_MAP_ENCAMPMENT=18, DGMP_MAP_CAMPSITE=19, DGMP_MAP_HISTORICAL=20,
    DGMP_MAP_NERO=21, DGMP_MAP_AMBUSH_CAMP=22, DGMP_MAP_PLANT=23,
    DGMP_MAP_DEAD_BODY=24, DGMP_MAP_AMBUSH_ZONE=25, DGMP_MAP_COLLECTIBLE=26,
    DGMP_MAP_TRACKING_CLUE=27, DGMP_MAP_JOB_GIVER=28, DGMP_MAP_TRAP=29
};
typedef int (*DGMP_SetMapCategoryVisibleFn)(uint32_t category,int visible);
typedef uint32_t (*DGMP_GetMapMarkerMaskFn)(void);
/* Return 0 for invalid category/visibility; 1 for an accepted request. */
DGMP_API int DGMP_SetMapCategoryVisible(uint32_t category,int visible);
DGMP_API uint32_t DGMP_GetMapMarkerMask(void);

/* Local environment API, version 1. Setters return 1 for an enqueued request,
   0 for invalid input or a full queue. They do not synchronously modify Unreal.
   Before initialization, valid requests wait for the world to become ready.
   Getters copy a thread-safe snapshot; initialize structSize before calling.
   Weather IDs are discovered from this game's enum; never assume an index is an ID. */
typedef struct DGMP_WeatherInfo {
    uint32_t structSize;
    uint32_t id;
    char name[64];
} DGMP_WeatherInfo;
typedef struct DGMP_EnvironmentState {
    uint32_t structSize;
    uint32_t available;
    float hours;                 /* [0,24): 18.5 = 18:30 */
    uint32_t weatherId;
    uint32_t timePaused;
    uint32_t weatherAutomatic;
    uint64_t requestedRevision;
    uint64_t processedRevision;
    int32_t lastRequestResult;   /* 0: none, 1: applied, -1: rejected by engine */
    uint32_t reserved;
} DGMP_EnvironmentState;
DGMP_API int DGMP_SetTimeOfDay(float hours);
DGMP_API int DGMP_SetTimePaused(int paused);
DGMP_API int DGMP_SetWeather(uint32_t weatherId); /* Select preset; disables ambient weather changes. */
DGMP_API int DGMP_SetWeatherAutomatic(int enabled);
DGMP_API int DGMP_GetEnvironmentState(DGMP_EnvironmentState* result);
DGMP_API uint32_t DGMP_GetWeatherCount(void);
DGMP_API int DGMP_GetWeatherInfo(uint32_t index,DGMP_WeatherInfo* result);
DGMP_API int DGMP_SetDevMenuVisible(int visible);
DGMP_API int DGMP_IsDevMenuVisible(void);
typedef int (*DGMP_SetTimeOfDayFn)(float);
typedef int (*DGMP_SetTimePausedFn)(int);
typedef int (*DGMP_SetWeatherFn)(uint32_t);
typedef int (*DGMP_SetWeatherAutomaticFn)(int);
typedef int (*DGMP_GetEnvironmentStateFn)(DGMP_EnvironmentState*);
typedef uint32_t (*DGMP_GetWeatherCountFn)(void);
typedef int (*DGMP_GetWeatherInfoFn)(uint32_t,DGMP_WeatherInfo*);
typedef int (*DGMP_SetDevMenuVisibleFn)(int);
typedef int (*DGMP_IsDevMenuVisibleFn)(void);
#ifdef __cplusplus
}
#endif
