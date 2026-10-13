#include "ue.h"
#include "log.h"
#include "profile.h"
#include "panel.h"
#include "policy.h"
#include "environment.h"
#include "devmenu.h"
#include "../api/dgmp.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ue;
namespace fs = std::filesystem;
namespace {
UObject* gameMode=nullptr;
UObject* pawn=nullptr;
UObject* controller=nullptr;
UObject* movement=nullptr;
FWeakObjectPtr pawnIdentity{-1,0};
bool ready=false, flying=false, disabled=false, requested=false;
bool autoLoad=true, autoFly=true, suppressStory=true, suppressAI=true;
bool revealMap=true;
unsigned int editorLeft=0,editorRight=0;
bool spawnApplied=false, wantedFly=true;
bool pendingArrival=false;
bool bootstrapComplete=false;
UObject* devInputController=nullptr;
bool devInputLocked=false;
bool devPreviousCursor=false;
FVector arrivalPosition{};
float arrivalYaw=0;
double arrivalAfter=0;
float speed=2000, lastDelta=0;
double startTime=0, requestTime=0, stableSince=0, nextScan=0, nextSweep=0, nextStatus=0;
unsigned int triggers=0, removedAI=0, errors=0;
int commandSeq=0;
uint8_t previousMovement=1;
bool previousCollision=true, previousIgnoreMove=false;
std::string stage="waiting-menu";
std::wstring note=L"";
std::map<int,bool> keyDown;
std::map<int,int> processed;
fs::path Ini() { return fs::path(profile::Root())/L"DGMP.ini"; }
double Now() { return GetTickCount64()/1000.0; }

UObject* Instance(const char* className) { return FindRuntimeInstance(className); }
bool BoolArg(UObject* o,const char* function,const char* name,bool value) {
    auto fn=o?Fn(o,function):nullptr; if(!fn) return false;
    Params p(fn); if(!p.SetBool(name,value)) return false; return Call(o,function,&p);
}
void DumpFunction(UObject* o,const char* function) {
    auto f=o?Fn(o,function):nullptr;
    dg::Log("Function %s on %s: %s",function,GetName(o).c_str(),f?"available":"missing");
    if(!f) return;
    for(auto p=Field<UObject*>(f,off::UStruct_Children);p;p=Field<UObject*>(p,off::UField_Next))
        dg::Log("  %s %s offset=%d size=%d",GetName(p->Class).c_str(),GetName(p).c_str(),Field<int>(p,off::UProperty_Offset),Field<int>(p,off::UProperty_ElementSize));
}
FVector Position() {
    auto root=IsValid(pawn)?Field<UObject*>(pawn,off::Actor_RootComponent):nullptr;
    return IsValid(root)?Field<FVector>(root,off::Scene_C2W_Trans):FVector{};
}
FRotator View() {
    auto fn=IsValid(controller)?Fn(controller,"GetControlRotation"):nullptr;
    if(fn) { Params p(fn); Call(controller,"GetControlRotation",&p); return p.Get<FRotator>("ReturnValue"); }
    return {};
}
bool SetPosition(const FVector& pos) {
    auto fn=IsValid(pawn)?Fn(pawn,"K2_SetActorLocation"):nullptr; if(!fn) return false;
    Params p(fn); if(!p.Set("NewLocation",pos) || !p.SetBool("bTeleport",true)) return false;
    p.SetBool("bSweep",false); Call(pawn,"K2_SetActorLocation",&p); return p.GetBool("ReturnValue");
}
bool Foreground() { DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid); return pid==GetCurrentProcessId(); }
bool Down(int key) { return (GetAsyncKeyState(key)&0x8000)!=0; }
bool Pressed(int key) { const bool d=Down(key); bool old=keyDown[key]; keyDown[key]=d; return d && !old; }
void DevInputMode(UObject* target,bool ui) {
    auto library=FindObject("/Script/UMG.Default__WidgetBlueprintLibrary");
    const char* name=ui?"SetInputMode_UIOnly":"SetInputMode_GameOnly";
    auto function=library?Fn(library,name):nullptr;
    auto first=FirstParam(function);
    if(first && GetName(first->Class)=="ObjectProperty") {
        Params p(function); if(p.Set(GetName(first).c_str(),target)) Call(library,name,&p);
    }
}
void DevInputPolicy() {
    if(devInputLocked && (!devmenu::IsOpen() || devInputController!=controller)) {
        if(IsValid(devInputController)) {
            BoolArg(devInputController,"SetIgnoreMoveInput","bNewMoveInput",false);
            BoolArg(devInputController,"SetIgnoreLookInput","bNewLookInput",false);
            SetBoolProp(devInputController,"bShowMouseCursor",devPreviousCursor);
            DevInputMode(devInputController,false);
        }
        devInputLocked=false; devInputController=nullptr;
    }
    if(devmenu::IsOpen() && !devInputLocked && IsValid(controller)) {
        devInputController=controller;
        devPreviousCursor=GetBoolProp(controller,"bShowMouseCursor");
        SetBoolProp(controller,"bShowMouseCursor",true);
        DevInputMode(controller,true);
        BoolArg(controller,"SetIgnoreMoveInput","bNewMoveInput",true);
        BoolArg(controller,"SetIgnoreLookInput","bNewLookInput",true);
        if(IsValid(movement)) Call(movement,"StopMovementImmediately");
        for(const char* stop:{"StopPrimaryFire","StopAutoWeaponFireInput","StopManualAim","StopManualAimWeapon","StopManualAimThrowable"}) Call(controller,stop);
        devInputLocked=true;
    }
    if(devInputLocked && Foreground()) { ClipCursor(nullptr); ReleaseCapture(); }
}
bool Paused() {
    auto gs=FindObject("/Script/Engine.Default__GameplayStatics"); auto f=gs?Fn(gs,"IsGamePaused"):nullptr;
    if(!f || !IsValid(gameMode)) return false;
    Params p(f); p.Set("WorldContextObject",gameMode); Call(gs,"IsGamePaused",&p); return p.GetBool("ReturnValue");
}
void SetFlying(bool on) {
    if(!IsValid(pawn) || !IsValid(controller) || !IsValid(movement) || flying==on) return;
    if(on) {
        previousMovement=Field<uint8_t>(movement,0x198);
        bool collisionFound=false;
        previousCollision=GetBoolProp(pawn,"bActorEnableCollision",&collisionFound);
        if(!collisionFound) previousCollision=true;
        previousIgnoreMove=false;
        auto fn=Fn(controller,"IsMoveInputIgnored"); if(fn) { Params p(fn); Call(controller,"IsMoveInputIgnored",&p); previousIgnoreMove=p.GetBool("ReturnValue"); }
        BoolArg(controller,"SetIgnoreMoveInput","bNewMoveInput",true);
        BoolArg(pawn,"SetActorEnableCollision","bNewActorEnableCollision",false);
        SetBoolProp(pawn,"bCanBeDamaged",false);
        Call(movement,"StopMovementImmediately");
        for(const char* stop:{"StopPrimaryFire","StopAutoWeaponFireInput","StopManualAim","StopManualAimWeapon","StopManualAimThrowable"}) Call(controller,stop);
    } else {
        BoolArg(controller,"SetIgnoreMoveInput","bNewMoveInput",false);
        BoolArg(pawn,"SetActorEnableCollision","bNewActorEnableCollision",previousCollision);
    }
    if(auto fn=Fn(movement,"SetMovementMode")) { Params p(fn); p.Set<uint8_t>("NewMovementMode",on?0:previousMovement); p.Set<uint8_t>("NewCustomMode",0); Call(movement,"SetMovementMode",&p); }
    flying=on; wantedFly=on;
    dg::Log("Fly %s; prior movement=%u collision=%d ignored=%d",on?"ON":"OFF",previousMovement,previousCollision,previousIgnoreMove);
}
void Refresh(double now) {
    const bool valid=Resolve(pawnIdentity)==pawn && IsValid(pawn) && IsValid(controller) && IsValid(movement);
    if(valid && now<nextScan) return;
    nextScan=now+1;
    if(!IsValid(gameMode)) gameMode=Instance("BendGameMode");
    auto nextPawn=Instance("BendDefaultPlayerPawn_C");
    if(nextPawn!=pawn || !valid) {
        pawn=nextPawn; controller=nullptr; movement=nullptr;
        pawnIdentity=pawn?FWeakObjectPtr{pawn->InternalIndex,SerialAt(pawn->InternalIndex)}:FWeakObjectPtr{-1,0};
        flying=false; stableSince=0;
        if(IsValid(pawn)) {
            controller=Field<UObject*>(pawn,off::Pawn_Controller);
            movement=Field<UObject*>(pawn,0x3B8);
            dg::Log("Player acquired: %s controller=%s movement=%s",GetName(pawn).c_str(),GetName(controller).c_str(),GetName(movement).c_str());
            DumpFunction(pawn,"K2_SetActorLocation");
        }
    }
}
bool Loading() {
    if(!IsValid(gameMode)) return true;
    if(GetBoolProp(gameMode,"bIsInLoadGame")) return true;
    auto fn=Fn(gameMode,"GetIsShowingLoadingScreenFromSaveGameLoad");
    if(fn) { Params p(fn); Call(gameMode,"GetIsShowingLoadingScreenFromSaveGameLoad",&p); return p.GetBool("ReturnValue"); } return false;
}
void RequestLoad(double now) {
    auto menu=Instance("UI_MainMenuWidget_C");
    if(!menu) return;
    static double menuSince=0;
    if(!menuSince) { menuSince=now; return; }
    if(now-menuSince<3) return;
    if(!GetBoolProp(menu,"bPressedKey")) {
        dg::Log("Bypassing the press-to-start gate in the private DGMP session");
        if(!SetBoolProp(menu,"bPressedKey",true)) { stage="start-gate-missing"; requested=true; return; }
    }
    auto st=FindObject("/Script/BendGame.Default__BendGameplayStatics");
    auto fn=st?Fn(st,"LoadGame"):nullptr;
    if(!fn || !IsValid(gameMode)) return;
    DumpFunction(st,"LoadGame");
    // SlotNumber is an ESaveGameType value, not the number printed in the UI.
    // Resolve the explicit manual slot instead of relying on MostRecent.
    int bootstrapSlot=-1;
    for(int index=0,count=NumObjects();index<count;++index) {
        auto enumeration=ObjectAt(index);
        if(!enumeration || !IsValid(enumeration) || GetName(enumeration)!="ESaveGameType" || GetName(enumeration->Class)!="Enum") continue;
        struct Item { FName name; uint8_t value; uint8_t padding[7]; };
        struct Items { Item* data; int32_t num,max; };
        auto items=Field<Items>(enumeration,0x40);
        if(items.data && items.num>0 && items.num<128) for(int i=0;i<items.num;++i) {
            const auto name=NameOf(items.data[i].name);
            dg::Log("Save slot enum: %d %s",items.data[i].value,name.c_str());
            if(name=="ESaveGameType_ManualSave") bootstrapSlot=items.data[i].value;
        }
        break;
    }
    if(bootstrapSlot<0 || bootstrapSlot>255) {
        requested=true; stage="load-slot-missing"; bootstrapComplete=true;
        panel::Loading(false,0,L"Не найден стартовый слот");
        note=L"Manual save enum unavailable; see logs/DGMP.log";
        return;
    }
    Params p(fn);
    if(!p.Set("WorldContextObject",gameMode) || !p.Set<uint8_t>("SlotNumber",uint8_t(bootstrapSlot)) || !p.SetBool("UseUIOnFailure",true)) { stage="load-parameters-missing"; requested=true; return; }
    requested=true; requestTime=now; stage="load-requested";
    Call(st,"LoadGame",&p);
    const auto loaded=p.Get<UObject*>("ReturnValue");
    dg::Log("Bootstrap LoadGame(type=%d manual-slot, context=%s), return=%p",bootstrapSlot,GetName(gameMode).c_str(),loaded);
    if(loaded && IsValid(menu)) {
        DumpFunction(menu,"OnLoadSuccess");
        dg::Log("Completing the menu's successful-load transition");
        Call(menu,"OnLoadSuccess");
    } else if(!loaded) {
        stage="load-failed"; bootstrapComplete=true;
        panel::Loading(false,0,L"Ошибка загрузки шаблона");
        note=L"Template load failed; see logs/DGMP.log";
    }
}
float ReadFloat(const wchar_t* section,const wchar_t* key,float fallback,const fs::path& file=Ini()) {
    wchar_t buf[80],def[80]; swprintf_s(def,L"%.3f",fallback);
    GetPrivateProfileStringW(section,key,def,buf,80,file.c_str());
    float value=float(wcstod(buf,nullptr)); return std::isfinite(value)?value:fallback;
}
void SavePoint(bool spawn) {
    if(!ready || !IsValid(pawn)) return;
    const auto pos=Position(); const auto view=View();
    if(spawn) {
        const wchar_t* keys[]={L"X",L"Y",L"Z",L"Yaw"}; const float values[]={pos.X,pos.Y,pos.Z,view.Yaw};
        for(int i=0;i<4;++i) { wchar_t buf[80]; swprintf_s(buf,L"%.6f",values[i]); WritePrivateProfileStringW(L"Spawn",keys[i],buf,Ini().c_str()); }
        WritePrivateProfileStringW(L"Spawn",L"Enabled",L"1",Ini().c_str());
        note=L"Spawn saved (F8 to return)";
    } else {
        std::ofstream file(fs::path(profile::Root())/L"maps"/L"markers.jsonl",std::ios::app);
        file.precision(9);
        file << "{\"type\":\"marker\",\"x\":" << pos.X << ",\"y\":" << pos.Y << ",\"z\":" << pos.Z << ",\"yaw\":" << view.Yaw << "}\n";
        note=L"Map marker saved";
    }
    dg::Log("%s saved: %.3f %.3f %.3f yaw=%.3f",spawn?"Spawn":"Marker",pos.X,pos.Y,pos.Z,view.Yaw);
}
void ReturnSpawn() {
    if(!GetPrivateProfileIntW(L"Spawn",L"Enabled",0,Ini().c_str())) return;
    FVector pos{ReadFloat(L"Spawn",L"X",0),ReadFloat(L"Spawn",L"Y",0),ReadFloat(L"Spawn",L"Z",0)};
    const float yaw=ReadFloat(L"Spawn",L"Yaw",0);
    arrivalPosition=pos; arrivalYaw=yaw; arrivalAfter=Now()+3; pendingArrival=true;
    panel::Loading(true,0.85f,L"Перемещение на точку спавна");
    auto fn=Fn(gameMode,"RequestFastTravel");
    if(fn) {
        FTransform player,bike; player.Translation=pos; player.Rotation=RotatorToQuat({0,yaw,0}); bike=player; bike.Translation.Y+=250;
        Params p(fn); p.Set("PlayerTransform",player); p.Set("BikeTransform",bike); p.Set("TravelDestination",FName{0,0}); Call(gameMode,"RequestFastTravel",&p);
    } else SetPosition(pos);
    if(auto f=IsValid(controller)?Fn(controller,"SetControlRotation"):nullptr) { Params p(f); p.Set("NewRotation",FRotator{0,yaw,0}); Call(controller,"SetControlRotation",&p); }
    dg::Log("Return to spawn requested: %.3f %.3f %.3f yaw=%.3f",pos.X,pos.Y,pos.Z,yaw);
    note=L"Returning to saved spawn";
}
void AlignArrival(double now) {
    if(!pendingArrival || now<arrivalAfter || Loading() || !IsValid(pawn)) return;
    // Bend fast travel treats Z as a feet position. Our editor stores the actor's root position.
    const auto before=Position();
    if(!SetPosition(arrivalPosition)) { dg::Log("Could not align arrival; retrying"); arrivalAfter=now+1; return; }
    if(auto f=IsValid(controller)?Fn(controller,"SetControlRotation"):nullptr) { Params p(f); p.Set("NewRotation",FRotator{0,arrivalYaw,0}); Call(controller,"SetControlRotation",&p); }
    pendingArrival=false; note=L"Saved spawn restored";
    dg::Log("Spawn aligned exactly: %.3f %.3f %.3f (travel offset %.3f)",arrivalPosition.X,arrivalPosition.Y,arrivalPosition.Z,before.Z-arrivalPosition.Z);
}
void BlockTriggers() {
    if(!suppressStory) return;
    auto triggerCls=FindClass("BendGameMissionTrigger"),restrictCls=FindClass("XpecRestrictArea");
    std::vector<UObject*> list;
    for(int i=0,n=NumObjects();i<n;++i) {
        auto o=ObjectAt(i); if(!o || (o->ObjectFlags&0x10) || !IsValid(o)) continue;
        if((triggerCls && IsA(o,triggerCls)) || (restrictCls && IsA(o,restrictCls))) list.push_back(o);
    }
    for(auto o:list) {
        BoolArg(o,"SetActorEnableCollision","bNewActorEnableCollision",false);
        BoolArg(o,"SetActorHiddenInGame","bNewHidden",true);
        BoolArg(o,"SetActorTickEnabled","bEnabled",false);
    }
}
bool Once(UObject* o) { if(!o) return false; const int serial=SerialAt(o->InternalIndex); if(processed[o->InternalIndex]==serial) return false; processed[o->InternalIndex]=serial; return true; }
void Sweep(double now) {
    if(now<nextSweep) return; nextSweep=now+0.008;
    static double nextManagers=0;
    if(now>=nextManagers) { nextManagers=now+1;
    if(suppressStory) {
        if(auto mission=Instance("BendGameMission")) {
            SetBoolProp(mission,"MissionAndJobProgressionBlocked",true);
            if(Once(mission)) { dg::Log("Mission progression blocked; active=%d",GetBoolProp(mission,"IsMissionActive")); }
        }
        for(const char* key:{"CascadeLocked","BelknapLocked","LostLakeLocked","CraterLakeLocked"}) SetBoolProp(gameMode,key,false);
    }
    if(suppressAI) {
        if(auto ai=Instance("BendAIManager")) BoolArg(ai,"SetCrowdSimulationEnabled","Enabled",false);
        if(auto events=Instance("BendAmbientEventManager")) if(Once(events)) { BoolArg(events,"Pause","ForceUnload",true); Call(events,"RemoveAllEventsQueuedToLoad"); }
        if(auto gen=Instance("BendAmbientGenerator")) {
            if(Once(gen)) Call(gen,"ForceDespawnAll");
            if(auto f=Fn(gen,"SetImportanceToBlockBelow")) { Params p(f); p.Set<uint8_t>("importance",255); Call(gen,"SetImportanceToBlockBelow",&p); }
        }
    }
    }
    auto triggerCls=FindClass("BendGameMissionTrigger"), restrictCls=FindClass("XpecRestrictArea"), aiCls=FindClass("BendAIPawn");
    std::vector<UObject*> hide;
    static int cursor=0; const int n=NumObjects(); if(cursor>=n) cursor=0;
    const int end=std::min(n,cursor+2048);
    for(int i=cursor;i<end;++i) {
        auto o=ObjectAt(i); if(!o || o==pawn || (o->ObjectFlags&0x10) || !IsValid(o)) continue;
        if(suppressStory && ((triggerCls && IsA(o,triggerCls)) || (restrictCls && IsA(o,restrictCls)))) {
            if(Once(o)) { hide.push_back(o); ++triggers; }
        } else if(suppressAI && aiCls && IsA(o,aiCls) && !GetBoolProp(o,"bHidden")) {
            if(Once(o)) { hide.push_back(o); ++removedAI; }
        }
    }
    cursor=end;
    for(auto o:hide) {
        BoolArg(o,"SetActorHiddenInGame","bNewHidden",true);
        BoolArg(o,"SetActorEnableCollision","bNewActorEnableCollision",false);
        BoolArg(o,"SetActorTickEnabled","bEnabled",false);
        if(IsA(o,aiCls)) { auto ctrl=Field<UObject*>(o,off::Pawn_Controller); if(IsValid(ctrl)) BoolArg(ctrl,"SetActorTickEnabled","bEnabled",false); }
    }
    if(processed.size()>20000) processed.clear();
}
void Fly(float dt) {
    if(!flying) return;
    if(Field<uint8_t>(movement,0x198)!=0) { auto fn=Fn(movement,"SetMovementMode"); if(fn) { Params p(fn); p.Set<uint8_t>("NewMovementMode",0); Call(movement,"SetMovementMode",&p); } }
    if(!Foreground() || Paused() || policy::GetState().menuActive || panel::IsLoading() || devmenu::IsOpen()) return;
    const auto r=View(); const float rad=0.01745329252f;
    const float cy=cosf(r.Yaw*rad),sy=sinf(r.Yaw*rad),cp=cosf(r.Pitch*rad),sp=sinf(r.Pitch*rad);
    const float forward=float(Down('W'))-float(Down('S')),right=float(Down('D'))-float(Down('A')),up=float(Down(VK_SPACE))-float(Down(VK_CONTROL));
    FVector delta{forward*cp*cy-right*sy,forward*cp*sy+right*cy,forward*sp+up};
    const float len=sqrtf(delta.X*delta.X+delta.Y*delta.Y+delta.Z*delta.Z); if(len<0.001f) return;
    auto pos=Position(); const float distance=speed*std::clamp(dt,0.f,0.1f)*(Down(VK_SHIFT)?4.f:1.f)/std::max(1.f,len);
    pos.X+=delta.X*distance; pos.Y+=delta.Y*distance; pos.Z+=delta.Z*distance; SetPosition(pos);
}
void Commands(double now) {
    bool focus=Foreground();
    const bool devKey=Pressed(VK_F3);
    const int keys[]{VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN,VK_ESCAPE};
    bool menuKeys[6]{}; for(int i=0;i<6;++i) menuKeys[i]=Pressed(keys[i]);
    const bool flyKey=Pressed(VK_F2),saveKey=Pressed(VK_F5),markKey=Pressed(VK_F6),returnKey=Pressed(VK_F8),slower=Pressed(VK_OEM_4),faster=Pressed(VK_OEM_6);
    const bool left=Pressed(VK_LBUTTON),right=Pressed(VK_RBUTTON);
    if(focus && ready && !panel::IsLoading()) {
        if(devKey && !policy::GetState().menuActive && !Paused()) DGMP_SetDevMenuVisible(!devmenu::IsOpen());
        devmenu::Update(true);
        const bool wasDev=devmenu::IsOpen();
        if(wasDev) {
            for(int i=0;i<6;++i) if(menuKeys[i]) devmenu::Key(keys[i]);
        } else {
            if(flyKey) SetFlying(!flying); if(saveKey) SavePoint(true); if(markKey) SavePoint(false); if(returnKey) ReturnSpawn();
            if(slower) speed=std::max(100.f,speed/2); if(faster) speed=std::min(40000.f,speed*2);
            if(flying && !policy::GetState().menuActive && !Paused()) {
                if(left) { ++editorLeft; note=L"Editor LMB reserved"; dg::Log("Editor LMB: %.3f %.3f %.3f",Position().X,Position().Y,Position().Z); }
                if(right) { ++editorRight; note=L"Editor RMB reserved"; dg::Log("Editor RMB: %.3f %.3f %.3f",Position().X,Position().Y,Position().Z); }
            }
        }
    }
    devmenu::Update(ready && !panel::IsLoading()); DevInputPolicy();
    static double next=0; if(now<next) return; next=now+0.25;
    const auto path=fs::path(profile::Root())/L"control.ini";
    const int seq=GetPrivateProfileIntW(L"Control",L"Sequence",0,path.c_str());
    if(seq<=commandSeq) return;
    commandSeq=seq; wchar_t command[80]; GetPrivateProfileStringW(L"Control",L"Command",L"",command,80,path.c_str());
    dg::Log("Control command seq=%d: %ls",seq,command);
    if(!wcscmp(command,L"save_spawn")) SavePoint(true);
    else if(!wcscmp(command,L"mark")) SavePoint(false);
    else if(!wcscmp(command,L"fly_toggle") && ready) SetFlying(!flying);
    else if(!wcscmp(command,L"return_spawn") && ready) ReturnSpawn();
    else if(!wcscmp(command,L"open_map") && ready) policy::OpenMap();
    else if(!wcscmp(command,L"close_map")) policy::CloseMap();
    else if(!wcscmp(command,L"map_category")) {
        const int category=GetPrivateProfileIntW(L"Control",L"Category",-1,path.c_str());
        const int visible=GetPrivateProfileIntW(L"Control",L"Visible",0,path.c_str());
        dg::Log("Map category request %d=%d accepted=%d",category,visible,DGMP_SetMapCategoryVisible(uint32_t(category),visible));
    }
    else if(!wcscmp(command,L"dev_menu")) DGMP_SetDevMenuVisible(GetPrivateProfileIntW(L"Control",L"Visible",1,path.c_str()));
    else if(!wcscmp(command,L"set_time")) DGMP_SetTimeOfDay(ReadFloat(L"Control",L"Hours",12,path));
    else if(!wcscmp(command,L"time_paused")) DGMP_SetTimePaused(GetPrivateProfileIntW(L"Control",L"Paused",1,path.c_str()));
    else if(!wcscmp(command,L"set_weather")) DGMP_SetWeather(uint32_t(GetPrivateProfileIntW(L"Control",L"WeatherId",-1,path.c_str())));
    else if(!wcscmp(command,L"weather_automatic")) DGMP_SetWeatherAutomatic(GetPrivateProfileIntW(L"Control",L"Enabled",1,path.c_str()));
    else if(!wcscmp(command,L"test_photo") && ready) policy::TestEvent("PM_Hotkey");
    else if(!wcscmp(command,L"test_fire") && ready && flying) policy::TestEvent("WeaponFire");
    else if(!wcscmp(command,L"test_aim") && ready && flying) policy::TestEvent("Player_ManualAim");
    else if(!wcscmp(command,L"retry_load") && !ready) { requested=false; requestTime=0; }
    else if(!wcscmp(command,L"test_fly_step") && ready && flying) { auto pos=Position(); pos.X+=1000; dg::Log("Test fly step accepted=%d",SetPosition(pos)); }
    else if(!wcscmp(command,L"quit")) {
        if(auto f=IsValid(controller)?Fn(controller,"ConsoleCommand"):nullptr) {
            std::wstring cmd=L"quit"; FString str{cmd.c_str(),int32_t(cmd.size()+1),int32_t(cmd.size()+1)};
            Params p(f); p.Set("Command",str); p.SetBool("bWriteToLog",true); Call(controller,"ConsoleCommand",&p);
        }
    }
}
void Status(double now) {
    if(now<nextStatus) return; nextStatus=now+1;
    const auto pos=IsValid(pawn)?Position():FVector{}; const auto rotation=IsValid(controller)?View():FRotator{};
    const auto tmp=fs::path(profile::Root())/L"telemetry.json.tmp",dest=fs::path(profile::Root())/L"telemetry.json";
    const auto ui=policy::GetState();
    {
        std::ofstream file(tmp); file.precision(9); file << std::boolalpha;
        file << "{\"stage\":\"" << stage << "\",\"ready\":" << ready << ",\"fly\":" << flying
             << ",\"loadingCover\":" << panel::IsLoading()
             << ",\"menu\":" << (IsValid(gameMode) && GetBoolProp(gameMode,"bIsMainMenuActive"))
             << ",\"loading\":" << Loading() << ",\"x\":" << pos.X << ",\"y\":" << pos.Y << ",\"z\":" << pos.Z << ",\"yaw\":" << rotation.Yaw
             << ",\"triggersDisabled\":" << triggers << ",\"aiHidden\":" << removedAI << ",\"errors\":" << errors << ",\"sequence\":" << commandSeq
             << ",\"photoDisabled\":" << ui.photoDisabled << ",\"photoActive\":" << (IsValid(controller) && GetBoolProp(controller,"IsPhotoModeActive"))
             << ",\"mapRevealed\":" << ui.mapRevealed << ",\"fogMaterialOverridden\":" << ui.fogMaterialOverridden
             << ",\"allMapRegions\":" << ui.allRegions << ",\"mapOnly\":" << ui.mapOnly << ",\"mapMenuActive\":" << ui.menuActive << ",\"mapEnum\":" << ui.mapEnum
             << ",\"blockedPhoto\":" << ui.blockedPhoto << ",\"blockedCombat\":" << ui.blockedCombat << ",\"blockedMenus\":" << ui.blockedMenus
             << ",\"hiddenMapControls\":" << ui.hiddenControls << ",\"editorLeftClicks\":" << editorLeft << ",\"editorRightClicks\":" << editorRight
             << ",\"blockedGameplay\":" << ui.blockedGameplay << ",\"hiddenNativeMarkers\":" << ui.hiddenMarkers << ",\"visibleFuelMarkers\":" << ui.visibleFuelMarkers
             << ",\"disabledNestZones\":" << ui.disabledZones << ",\"hiddenHudControls\":" << ui.hiddenHudControls << ",\"mapMarkerMask\":" << ui.markerMask
             << ",\"playerVODisabled\":" << ui.playerVODisabled << ",\"openWorldVOPaused\":" << ui.openWorldVOPaused << ",\"zoneOverlayHidden\":" << ui.zoneOverlayHidden
             << ",\"savingBlocked\":" << ui.savingBlocked << ",\"blockedSaveRequests\":" << ui.blockedSaveRequests << ",\"blockedSaveFileWrites\":" << profile::BlockedSaveWrites() << ",\"hiddenSaveControls\":" << ui.hiddenSaveControls << ",\"activeNativeJobs\":" << ui.activeNativeJobs << ",\"pausedNativeJobs\":" << ui.pausedNativeJobs << ",\"nativeMissionActive\":" << ui.nativeMissionActive << ",\"trackingCleared\":" << ui.trackingCleared << ",\"nestObjectsHidden\":" << ui.nestObjectsHidden << ",\"storyBlockersDisabled\":" << ui.storyBlockersDisabled
             << ",\"doorsUnlocked\":" << ui.doorsUnlocked << ",\"zoneParametersCleared\":" << ui.zoneParametersCleared;
        const auto env=environment::GetSnapshot();
        file << ",\"devMenuOpen\":" << devmenu::IsOpen() << ",\"environmentAvailable\":" << bool(env.state.available)
             << ",\"timeOfDayHours\":" << env.state.hours << ",\"timePaused\":" << bool(env.state.timePaused)
             << ",\"weatherId\":" << env.state.weatherId << ",\"weatherAutomatic\":" << bool(env.state.weatherAutomatic)
             << ",\"environmentRequested\":" << env.state.requestedRevision << ",\"environmentProcessed\":" << env.state.processedRevision
             << ",\"environmentLastResult\":" << env.state.lastRequestResult << ",\"weatherCatalog\":[";
        for(size_t i=0;i<env.weather.size();++i) { if(i) file << ','; file << "{\"id\":" << env.weather[i].id << ",\"name\":\"" << env.weather[i].name << "\"}"; }
        file << "]}\n";
    }
    MoveFileExW(tmp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING);
    wchar_t text[1024]; swprintf_s(text,L"DGMP v0.5.1 | %hs | Fly: %ls | %.0f m/s\nX %.2f  Y %.2f  Z %.2f  Yaw %.1f\nF2 fly | WASD move | Space/Ctrl up/down | Shift boost\n[ / ] speed | F5 save spawn | F6 marker | F8 return\n%ls",stage.c_str(),flying?L"ON":L"OFF",speed/100,pos.X,pos.Y,pos.Z,rotation.Yaw,note.c_str()); panel::Text(text);
    static double nextLog=0; if(now>=nextLog) { nextLog=now+10; dg::Log("Status %s ready=%d fly=%d pos=(%.1f %.1f %.1f) triggers=%u ai=%u",stage.c_str(),ready,flying,pos.X,pos.Y,pos.Z,triggers,removedAI); }
}
void FinishBootstrapFade() {
    if(!suppressStory) return;
    auto mission=Instance("BendGameMission"); if(!IsValid(mission)) return;
    if(GetBoolProp(mission,"FadingFromBlack")) {
        Call(mission,"StopFading");
        if(auto f=Fn(mission,"FadeFromBlackDone")) { Params p(f); Call(mission,"FadeFromBlackDone",&p); }
        // The native fade component can retain its last near-black blend while story jobs are paused.
        if(auto f=Fn(mission,"GetFadingPostProcess")) {
            Params p(f); Call(mission,"GetFadingPostProcess",&p);
            if(auto component=p.Get<UObject*>("ReturnValue");IsValid(component)) {
                for(auto cls=component->Class;cls;cls=SuperOf(cls)) if(auto property=FindProperty(cls,"BlendWeight")) {
                    Field<float>(component,Field<int>(property,off::UProperty_Offset))=0; break;
                }
            }
        }
        dg::Log("Bootstrap native fade completed: fading=%d",GetBoolProp(mission,"FadingFromBlack"));
    }
}
void Update(float dt) {
    const double now=Now(); lastDelta=dt; Refresh(now);
    if(IsValid(gameMode)) {
        const bool menu=GetBoolProp(gameMode,"bIsMainMenuActive");
        static double nextEarlySweep=0;
        if(!ready && now>=nextEarlySweep) { nextEarlySweep=now+0.25; BlockTriggers(); }
        if(!ready && !requested && autoLoad && menu && now-startTime>6 && !Loading()) RequestLoad(now);
        if(!menu && IsValid(pawn) && IsValid(controller) && IsValid(movement) && !Loading()) {
            if(!stableSince) stableSince=now;
            if(now-stableSince>4) {
                if(!ready) { ready=true; stage="world-ready"; dg::Log("WORLD READY: independent editor profile, player spawned"); }
                if(wantedFly && !flying) SetFlying(true);
                if(!spawnApplied) { spawnApplied=true; if(GetPrivateProfileIntW(L"Spawn",L"Enabled",0,Ini().c_str())) ReturnSpawn(); }
                AlignArrival(now); Sweep(now); Fly(dt);
            }
        } else {
            stableSince=0;
            if(ready && menu) { ready=false; stage="main-menu"; }
        }
        if(requested && !ready && now-requestTime>90 && stage=="load-requested") { stage="load-timeout"; bootstrapComplete=true; panel::Loading(false,0,L"Ошибка загрузки"); note=L"Auto load timed out; see logs/DGMP.log"; dg::Log("Bootstrap load timed out"); }
    }
    policy::Update(pawn,controller,gameMode,flying || devmenu::IsOpen() || !ready || pendingArrival,revealMap,true,suppressStory);
    if(!bootstrapComplete || panel::IsLoading()) {
        if(ready && spawnApplied && !pendingArrival && !Loading()) {
            FinishBootstrapFade();
            bootstrapComplete=true; panel::Loading(false,1.f,L"Мир готов");
            dg::Log("Loading cover released: world ready and spawn fully aligned");
        } else if(pendingArrival) panel::Loading(true,0.88f,L"Подготовка точки спавна");
        else if(!ready && !requested) panel::Loading(true,0.25f,L"Инициализация мира");
        else if(!ready) panel::Loading(true,Loading()?0.55f:0.72f,L"Загрузка мира");
    }
    Commands(now); environment::Update(ready && !panel::IsLoading() && !Loading()); Status(now);
}
void ReportException(unsigned long code) {
    ++errors; dg::Log("DGMP frame exception 0x%08X (%u)",code,errors);
    if(errors>=5) { disabled=true; panel::Text(L"DGMP disabled after repeated errors. See logs/DGMP.log"); }
}
int CaptureException(EXCEPTION_POINTERS* info) {
    dg::Log("Exception site=%p gameRVA=%llX",info->ExceptionRecord->ExceptionAddress,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)-Base()));
    return EXCEPTION_EXECUTE_HANDLER;
}
void SafeUpdate(float dt) {
    __try { if(!disabled) Update(dt); }
    __except(CaptureException(GetExceptionInformation())) { ReportException(GetExceptionCode()); }
}
void SafeInputPolicy() {
    __try { if(!disabled) { DevInputPolicy(); policy::Update(pawn,controller,gameMode,flying || devmenu::IsOpen() || !ready || pendingArrival,revealMap,true,suppressStory); } }
    __except(CaptureException(GetExceptionInformation())) { ReportException(GetExceptionCode()); }
}
using TickFn=void(__fastcall*)(UObject*,float,bool);
TickFn originalTick=nullptr;
void* copiedVtable[512];
void __fastcall Tick(UObject* engine,float dt,bool idle) { SafeInputPolicy(); originalTick(engine,dt,idle); SafeUpdate(dt); }
UObject* FindEngine() {
    for(int i=0,n=NumObjects();i<n;++i) { auto o=ObjectAt(i); if(o && !(o->ObjectFlags&0x10) && IsValid(o) && o->Class && GetName(o->Class)=="GameEngine") return o; } return nullptr;
}
}
void DGMPInitThread() {
    const auto logPath=fs::path(profile::Root())/L"logs"/L"DGMP.log";
    dg::LogOpen(logPath.string().c_str()); panel::Start();
    dg::Log("DGMP v0.5.1 loading; built %s %s; profile imports patched=%d",__DATE__,__TIME__,profile::PatchedCount());
    dg::Log("Isolated profile: %ls",profile::Saved().c_str());
    panel::Loading(true,0.1f,L"Подготовка DGMP");
    if(!ue::Init()) { panel::Text(L"DGMP: unsupported game build; see log"); panel::Loading(true,0,L"Эта сборка игры не поддерживается"); return; }
    autoLoad=GetPrivateProfileIntW(L"DGMP",L"AutoLoad",1,Ini().c_str())!=0;
    autoFly=GetPrivateProfileIntW(L"DGMP",L"AutoFly",1,Ini().c_str())!=0; wantedFly=autoFly;
    suppressStory=GetPrivateProfileIntW(L"DGMP",L"SuppressStory",1,Ini().c_str())!=0;
    suppressAI=GetPrivateProfileIntW(L"DGMP",L"SuppressAI",1,Ini().c_str())!=0;
    revealMap=GetPrivateProfileIntW(L"World",L"RevealMap",1,Ini().c_str())!=0;
    uint32_t mapMask=0;
    if(GetPrivateProfileIntW(L"Map",L"Player",1,Ini().c_str())) mapMask|=1u<<0;
    if(GetPrivateProfileIntW(L"Map",L"Motorcycle",1,Ini().c_str())) mapMask|=1u<<1;
    if(GetPrivateProfileIntW(L"Map",L"Fuel",1,Ini().c_str())) mapMask|=1u<<7;
    if(GetPrivateProfileIntW(L"Map",L"UserWaypoint",1,Ini().c_str())) mapMask|=1u<<9;
    if(GetPrivateProfileIntW(L"Map",L"OtherNativeMarkers",0,Ini().c_str())) mapMask|=0x3FFFFFFFu&~((1u<<0)|(1u<<1)|(1u<<7)|(1u<<9));
    policy::ConfigureMarkers(mapMask);
    speed=std::clamp(ReadFloat(L"DGMP",L"FlySpeed",2000),100.f,40000.f); startTime=Now();
    UObject* engine=nullptr;
    for(int i=0;i<240 && !engine;++i) { Sleep(250); if(NumObjects()>1000) engine=FindEngine(); }
    if(!engine) { dg::Log("GameEngine not found"); panel::Text(L"DGMP: GameEngine not found"); return; }
    auto vt=engine->VTable;
    if(reinterpret_cast<uintptr_t>(vt[60])-Base()!=0x1AC2A20 || reinterpret_cast<uintptr_t>(vt[73])-Base()!=0x288F730) { dg::Log("Engine vtable mismatch; no hook installed"); return; }
    memcpy(copiedVtable,vt,sizeof(copiedVtable)); originalTick=reinterpret_cast<TickFn>(vt[73]); copiedVtable[73]=reinterpret_cast<void*>(&Tick); engine->VTable=copiedVtable;
    dg::Log("DGMP engine tick installed; automatic bootstrap enabled=%d",autoLoad);
}
