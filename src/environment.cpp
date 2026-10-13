#include "environment.h"
#include "ue.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
using namespace ue;
namespace environment {
namespace {
enum class Kind { Time, Pause, Weather, Automatic };
struct Request { Kind kind; float value; uint64_t revision; };
std::mutex mutex;
Snapshot published;
std::deque<Request> requests;
uint64_t revision=0,processed=0;
UObject* manager=nullptr;
float resumeRate=1;
bool paused=false,automatic=true,automaticOverride=false;
std::string message="Waiting for world";
int lastResult=0;
double nextPublish=0;
double nextManagerScan=0,nextCatalogScan=0;
std::vector<DGMP_WeatherInfo> catalog;
template<typename T> T Read(UObject* o,const char* key,T fallback={}) {
    for(auto cls=o?o->Class:nullptr;cls;cls=SuperOf(cls)) if(auto p=FindProperty(cls,key)) return Field<T>(o,Field<int>(p,off::UProperty_Offset));
    return fallback;
}
UObject* FindManager() {
    auto cls=FindClass("BendWorldManager"); if(!cls) return nullptr;
    for(int i=0,n=NumObjects();i<n;++i) if(auto o=ObjectAt(i);o && !(o->ObjectFlags&0x30) && IsValid(o) && IsA(o,cls)) return o;
    return nullptr;
}
bool OneFloat(const char* name,float value) {
    auto f=Fn(manager,name); auto first=FirstParam(f);
    if(!first || GetName(first->Class)!="FloatProperty") return false;
    Params p(f); return p.Set(GetName(first).c_str(),value) && Call(manager,name,&p);
}
float FloatResult(const char* name) {
    auto f=Fn(manager,name); if(!f) return 0;
    Params p(f); Call(manager,name,&p); return p.Get<float>("ReturnValue");
}
void DiscoverWeather() {
    if(!catalog.empty()) return;
    auto enumeration=FindObject("/Script/BendGame.EWeatherType");
    if(!IsValid(enumeration) || GetName(enumeration->Class)!="Enum") return;
    struct Item { FName name; uint8_t value; uint8_t padding[7]; };
    struct Items { Item* data; int32_t num,max; };
    const auto names=Field<Items>(enumeration,0x40);
    if(!names.data || names.num<=0 || names.num>128) return;
    for(int i=0;i<names.num;++i) {
        std::string name=NameOf(names.data[i].name);
        if(name.find("_MAX")!=std::string::npos || name.find("::Num")!=std::string::npos || name.find("::Invalid")!=std::string::npos || name.find("::None")!=std::string::npos) continue;
        if(auto colon=name.rfind("::");colon!=std::string::npos) name=name.substr(colon+2);
        DGMP_WeatherInfo info{}; info.structSize=sizeof(info); info.id=uint32_t(names.data[i].value);
        snprintf(info.name,sizeof(info.name),"%s",name.c_str()); catalog.push_back(info);
        dg::Log("Weather catalog: %u %s",info.id,info.name);
    }
}
bool Enqueue(Kind kind,float value) {
    std::lock_guard lock(mutex);
    if(requests.size()>=64) return false;
    requests.push_back({kind,value,++revision}); return true;
}
bool Apply(const Request& r) {
    if(r.kind==Kind::Time) {
        auto f=Fn(manager,"SetWorldTimeOfDay"); if(!f) return false;
        Params p(f);
        return p.Set("TimeOfDay",r.value) && p.SetBool("CanAdvanceDay",false) && Call(manager,"SetWorldTimeOfDay",&p);
    }
    if(r.kind==Kind::Pause) {
        const bool value=r.value!=0;
        if(value && !paused) { const float rate=Read<float>(manager,"WorldTimeRate",1); if(std::isfinite(rate) && rate>0) resumeRate=rate; }
        if(!OneFloat("SetWorldTimeRate",value?0.f:resumeRate)) return false;
        paused=value; return true;
    }
    if(r.kind==Kind::Automatic) {
        automatic=r.value!=0; automaticOverride=true;
        return SetBoolProp(manager,"EnableAmbientWeatherChange",automatic);
    }
    const auto id=uint32_t(r.value);
    if(std::none_of(catalog.begin(),catalog.end(),[id](const auto& info){return info.id==id;})) return false;
    auto f=Fn(manager,"SelectWorldWeather"); if(!f) return false;
    Params p(f);
    if(!p.Set<uint8_t>("NewWeather",uint8_t(id)) || !p.Set<uint8_t>("NewWeatherTransitionRate",1) || !p.Set<float>("RelativeTime",0)) return false;
    if(!Call(manager,"SelectWorldWeather",&p)) return false;
    automatic=false; automaticOverride=true;
    SetBoolProp(manager,"EnableAmbientWeatherChange",false); return true;
}
}
Snapshot GetSnapshot() { std::lock_guard lock(mutex); return published; }
void Update(bool ready) {
    const double now=GetTickCount64()/1000.0;
    if(ready && !IsValid(manager) && now>=nextManagerScan) {
        nextManagerScan=now+1;
        manager=FindManager();
        if(IsValid(manager)) {
            automatic=GetBoolProp(manager,"EnableAmbientWeatherChange");
            dg::Log("Environment manager acquired: %s",GetName(manager).c_str());
        }
    }
    const bool available=ready && IsValid(manager);
    if(available) {
        if(catalog.empty() && now>=nextCatalogScan) { nextCatalogScan=now+1; DiscoverWeather(); }
        std::deque<Request> pending;
        { std::lock_guard lock(mutex); pending.swap(requests); }
        if(!pending.empty()) nextPublish=0;
        for(const auto& r:pending) {
            const bool applied=Apply(r); processed=r.revision; lastResult=applied?1:-1;
            message=applied?"Applied":"Engine rejected environment request";
            dg::Log("Environment request %llu kind=%u value=%.3f applied=%d",static_cast<unsigned long long>(r.revision),unsigned(r.kind),r.value,applied);
        }
        if(paused) OneFloat("SetWorldTimeRate",0);
        if(automaticOverride) SetBoolProp(manager,"EnableAmbientWeatherChange",automatic);
    }
    if(now<nextPublish) return;
    nextPublish=now+0.1;
    Snapshot snapshot;
    snapshot.weather=catalog; snapshot.message=available?message:"Waiting for world";
    auto& state=snapshot.state; state.structSize=sizeof(state); state.available=available; state.processedRevision=processed; state.lastRequestResult=lastResult;
    if(available) {
        state.hours=FloatResult("GetWorldTimeOfDay");
        state.weatherId=Read<uint8_t>(manager,"WorldWeather");
        state.weatherAutomatic=GetBoolProp(manager,"EnableAmbientWeatherChange"); state.timePaused=paused;
    }
    std::lock_guard lock(mutex); state.requestedRevision=revision; published=std::move(snapshot);
}
}
using namespace environment;
extern "C" __declspec(dllexport) int DGMP_SetTimeOfDay(float hours) {
    return std::isfinite(hours) && hours>=0 && hours<24 && Enqueue(Kind::Time,hours);
}
extern "C" __declspec(dllexport) int DGMP_SetTimePaused(int value) { return (value==0 || value==1) && Enqueue(Kind::Pause,float(value)); }
extern "C" __declspec(dllexport) int DGMP_SetWeather(uint32_t id) {
    if(id>255) return 0;
    { std::lock_guard lock(mutex); if(!published.weather.empty() && std::none_of(published.weather.begin(),published.weather.end(),[id](const auto& w){return w.id==id;})) return 0; }
    return Enqueue(Kind::Weather,float(id));
}
extern "C" __declspec(dllexport) int DGMP_SetWeatherAutomatic(int value) { return (value==0 || value==1) && Enqueue(Kind::Automatic,float(value)); }
extern "C" __declspec(dllexport) int DGMP_GetEnvironmentState(DGMP_EnvironmentState* result) {
    if(!result || result->structSize<sizeof(*result)) return 0;
    std::lock_guard lock(mutex); *result=published.state; result->structSize=sizeof(*result); return 1;
}
extern "C" __declspec(dllexport) uint32_t DGMP_GetWeatherCount() { std::lock_guard lock(mutex); return uint32_t(published.weather.size()); }
extern "C" __declspec(dllexport) int DGMP_GetWeatherInfo(uint32_t index,DGMP_WeatherInfo* result) {
    if(!result || result->structSize<sizeof(*result)) return 0;
    std::lock_guard lock(mutex); if(index>=published.weather.size()) return 0; *result=published.weather[index]; return 1;
}
