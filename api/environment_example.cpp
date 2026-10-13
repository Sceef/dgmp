// Example for a future client-side mode plugin, not a standalone launcher.
// Pass the HMODULE of DGMP's private version.dll, not the Windows system DLL.
#include <windows.h>
#include "dgmp.h"

bool QueueDeveloperEnvironment(HMODULE dgmp,float hours,uint32_t weatherId,bool freeze) {
    if(!dgmp) return false;
    auto setTime=reinterpret_cast<DGMP_SetTimeOfDayFn>(GetProcAddress(dgmp,"DGMP_SetTimeOfDay"));
    auto setPause=reinterpret_cast<DGMP_SetTimePausedFn>(GetProcAddress(dgmp,"DGMP_SetTimePaused"));
    auto setWeather=reinterpret_cast<DGMP_SetWeatherFn>(GetProcAddress(dgmp,"DGMP_SetWeather"));
    if(!setTime || !setPause || !setWeather) return false;
    // Independent queued requests; this is not an atomic transaction.
    const int timeAccepted=setTime(hours);
    const int pauseAccepted=setPause(freeze?1:0);
    const int weatherAccepted=setWeather(weatherId);
    return timeAccepted && pauseAccepted && weatherAccepted;
}

bool ReadDeveloperEnvironment(HMODULE dgmp,DGMP_EnvironmentState& state) {
    if(!dgmp) return false;
    auto get=reinterpret_cast<DGMP_GetEnvironmentStateFn>(GetProcAddress(dgmp,"DGMP_GetEnvironmentState"));
    state={}; state.structSize=sizeof(state);
    return get && get(&state) && state.available;
}

bool ReadWeatherPreset(HMODULE dgmp,uint32_t catalogIndex,DGMP_WeatherInfo& info) {
    if(!dgmp) return false;
    auto get=reinterpret_cast<DGMP_GetWeatherInfoFn>(GetProcAddress(dgmp,"DGMP_GetWeatherInfo"));
    info={}; info.structSize=sizeof(info);
    return get && get(catalogIndex,&info);
}
// ReadWeatherPreset(module,index,preset) -> pass preset.id to the setter.
// QueueDeveloperEnvironment(module,18.5f,preset.id,true) requests 18:30,
// frozen time, and that preset. Check processedRevision/lastRequestResult later.
