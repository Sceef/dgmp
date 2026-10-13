#include "devmenu.h"
#include "environment.h"
#include "../api/dgmp.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
namespace devmenu {
namespace {
std::atomic<bool> open{false};
std::atomic<int> requested{-1};
struct Input { int row,direction; bool midday; };
std::mutex mutex;
std::deque<Input> inputs;
Snapshot published;
int selected=0;
bool rejected=false;
std::wstring Wide(const char* value) { std::wstring result; for(;*value;++value) result+=wchar_t(static_cast<unsigned char>(*value)); return result; }
std::wstring WeatherName(const DGMP_WeatherInfo& info) {
    const auto name=Wide(info.name);
    if(name.find(L"HeavyRain")!=std::wstring::npos) return L"Сильный дождь";
    if(name.find(L"LightRain")!=std::wstring::npos) return L"Лёгкий дождь";
    if(name.find(L"HeavySnow")!=std::wstring::npos) return L"Сильный снег";
    if(name.find(L"LightSnow")!=std::wstring::npos) return L"Лёгкий снег";
    if(name.find(L"Clear")!=std::wstring::npos) return L"Ясно";
    if(name.find(L"Cloud")!=std::wstring::npos) return L"Облачно";
    if(name.find(L"Rain")!=std::wstring::npos) return L"Дождь";
    if(name.find(L"Snow")!=std::wstring::npos) return L"Снег";
    return name;
}
void Action(const Input& input) {
    selected=std::clamp(input.row,0,7);
    if(!input.direction && !input.midday) return;
    auto env=environment::GetSnapshot(); const auto& state=env.state;
    bool accepted=false;
    if(selected==0) {
        float hours=input.midday?12.f:state.hours+float(input.direction)*0.25f;
        hours=std::fmod(hours+24.f,24.f); accepted=DGMP_SetTimeOfDay(hours)!=0;
    } else if(selected==1) accepted=DGMP_SetTimePaused(!state.timePaused)!=0;
    else if(selected==2 && !env.weather.empty()) {
        auto it=std::find_if(env.weather.begin(),env.weather.end(),[&](const auto& w){return w.id==state.weatherId;});
        const int current=it==env.weather.end()?0:int(it-env.weather.begin());
        const int count=int(env.weather.size());
        const int next=(current+(input.direction<0?-1:1)+count)%count;
        accepted=DGMP_SetWeather(env.weather[next].id)!=0;
    } else if(selected==3) accepted=DGMP_SetWeatherAutomatic(!state.weatherAutomatic)!=0;
    else if(selected>=4) {
        constexpr uint32_t categories[]{DGMP_MAP_PLAYER,DGMP_MAP_MOTORCYCLE,DGMP_MAP_FUEL,DGMP_MAP_WAYPOINT};
        const auto category=categories[selected-4];
        accepted=DGMP_SetMapCategoryVisible(category,!(DGMP_GetMapMarkerMask()&(1u<<category)))!=0;
    }
    rejected=!accepted;
}
}
bool IsOpen() { return open.load(); }
Snapshot GetSnapshot() { std::lock_guard lock(mutex); return published; }
void Click(int row,int direction) {
    std::lock_guard lock(mutex);
    if(inputs.size()<64) inputs.push_back({row,direction,false});
}
void Key(int key) {
    if(!IsOpen()) return;
    if(key==VK_ESCAPE) { requested=0; return; }
    if(key==VK_UP) selected=(selected+7)%8;
    else if(key==VK_DOWN) selected=(selected+1)%8;
    else if(key==VK_LEFT || key==VK_RIGHT || key==VK_RETURN) {
        Action({selected,key==VK_LEFT?-1:1,key==VK_RETURN && selected==0});
    }
}
void Update(bool ready) {
    if(!ready) { open=false; return; }
    if(const int value=requested.exchange(-1);value>=0) {
        const bool was=open.exchange(value!=0);
        if(!was || !value) { std::lock_guard lock(mutex); inputs.clear(); }
    }
    std::deque<Input> pending;
    { std::lock_guard lock(mutex); pending.swap(inputs); }
    if(IsOpen()) for(const auto& input:pending) Action(input);
    auto env=environment::GetSnapshot(); const auto& state=env.state;
    Snapshot view; view.selected=selected;
    wchar_t time[64]; const int minutes=std::isfinite(state.hours)?int(state.hours*60)%1440:0;
    swprintf_s(time,L"Время суток     %02d:%02d",minutes/60,minutes%60);
    std::wstring weather=L"Ожидание списка";
    for(const auto& item:env.weather) if(item.id==state.weatherId) { weather=WeatherName(item); break; }
    auto toggle=[](bool value){return value?L"ВКЛ":L"ВЫКЛ";};
    view.rows={time,std::wstring(L"Остановить часы     ")+toggle(state.timePaused),L"Погода     "+weather,std::wstring(L"Автосмена погоды     ")+toggle(state.weatherAutomatic)};
    constexpr uint32_t categories[]{DGMP_MAP_PLAYER,DGMP_MAP_MOTORCYCLE,DGMP_MAP_FUEL,DGMP_MAP_WAYPOINT};
    const wchar_t* labels[]{L"Метка игрока",L"Метка мотоцикла",L"Метки топлива",L"Пользовательская метка"};
    const auto mask=DGMP_GetMapMarkerMask();
    for(int i=0;i<4;++i) view.rows.push_back(std::wstring(labels[i])+L"     "+toggle((mask&(1u<<categories[i]))!=0));
    view.error=rejected || state.lastRequestResult<0;
    view.status=!state.available?L"Ожидание готовности мира":view.error?L"Команда отклонена. Подробности в логе.":state.requestedRevision>state.processedRevision?L"Применение настроек…":L"Изменения применяются к текущему миру";
    std::lock_guard lock(mutex); published=std::move(view);
}
}
using namespace devmenu;
extern "C" __declspec(dllexport) int DGMP_SetDevMenuVisible(int value) {
    if(value!=0 && value!=1) return 0;
    requested=value; return 1;
}
extern "C" __declspec(dllexport) int DGMP_IsDevMenuVisible() { return IsOpen(); }
