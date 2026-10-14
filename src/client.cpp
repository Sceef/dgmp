#include "ue.h"
#include "log.h"
#include "profile.h"
#include "panel.h"
#include "policy.h"
#include "environment.h"
#include "devmenu.h"
#include "mp.h"
#include "devcmd.h"
#include "audio.h"
#include "hud.h"
#include "overlay.h"
#include "editorui.h"
#include "lang.h"
#include "../api/dgmp.h"
#include <windows.h>
#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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
bool multiplayer=false;          // launched by DGMP.exe to join a server
bool mpSpawned=false;            // the server's spawn teleport has been applied
double mpJoinedAt=0, mpFailedAt=0;
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
    if(devInputLocked && (!(devmenu::IsOpen() || overlay::CursorMode()) || devInputController!=controller)) {
        if(IsValid(devInputController)) {
            BoolArg(devInputController,"SetIgnoreMoveInput","bNewMoveInput",false);
            BoolArg(devInputController,"SetIgnoreLookInput","bNewLookInput",false);
            SetBoolProp(devInputController,"bShowMouseCursor",devPreviousCursor);
            DevInputMode(devInputController,false);
        }
        devInputLocked=false; devInputController=nullptr;
    }
    if((devmenu::IsOpen() || overlay::CursorMode()) && !devInputLocked && IsValid(controller)) {
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
// The player's body (and what hangs on it: weapons, backpack) is hidden while flying in the editor.
void HideBody(bool hidden) {
    if(!IsValid(pawn)) return;
    BoolArg(pawn,"SetActorHiddenInGame","bNewHidden",hidden);
    if(auto f=Fn(pawn,"GetAttachedActors")) {
        Params p(f); Call(pawn,"GetAttachedActors",&p);
        struct Arr { UObject** data; int32_t num,max; };
        const auto arr=p.Get<Arr>("OutActors");
        for(int i=0;i<arr.num && arr.data;++i) if(IsValid(arr.data[i])) BoolArg(arr.data[i],"SetActorHiddenInGame","bNewHidden",hidden);
    }
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
    if(!multiplayer) HideBody(on);
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
        panel::Loading(false,0,lang::Pick(L"Не найден стартовый слот",L"Start slot not found"));
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
        panel::Loading(false,0,lang::Pick(L"Ошибка загрузки шаблона",L"Template load error"));
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
// Fast travel (moves the bike along and streams the destination), then AlignArrival snaps exactly.
void TravelTo(const FVector& pos,float yaw) {
    arrivalPosition=pos; arrivalYaw=yaw; arrivalAfter=Now()+3; pendingArrival=true;
    panel::Loading(true,0.85f,lang::Pick(L"Перемещение",L"Travelling"));
    auto fn=Fn(gameMode,"RequestFastTravel");
    if(fn) {
        FTransform player,bike; player.Translation=pos; player.Rotation=RotatorToQuat({0,yaw,0}); bike=player; bike.Translation.Y+=250;
        Params p(fn); p.Set("PlayerTransform",player); p.Set("BikeTransform",bike); p.Set("TravelDestination",FName{0,0}); Call(gameMode,"RequestFastTravel",&p);
    } else SetPosition(pos);
    if(auto f=IsValid(controller)?Fn(controller,"SetControlRotation"):nullptr) { Params p(f); p.Set("NewRotation",FRotator{0,yaw,0}); Call(controller,"SetControlRotation",&p); }
    dg::Log("Travel requested: %.3f %.3f %.3f yaw=%.3f",pos.X,pos.Y,pos.Z,yaw);
}
void ReturnSpawn() {
    if(!GetPrivateProfileIntW(L"Spawn",L"Enabled",0,Ini().c_str())) return;
    TravelTo({ReadFloat(L"Spawn",L"X",0),ReadFloat(L"Spawn",L"Y",0),ReadFloat(L"Spawn",L"Z",0)},ReadFloat(L"Spawn",L"Yaw",0));
    note=L"Returning to saved spawn";
}
// Server teleports: the first one (spawn) and long jumps travel with the bike; short hops on foot move directly.
void ApplyServerTeleport() {
    FVector pos; float yaw; bool initial;
    if(!mp::TakeTeleport(pos,yaw,initial)) return;
    const auto here=Position(); const float dx=pos.X-here.X,dy=pos.Y-here.Y,dz=pos.Z-here.Z;
    const bool onBike=IsValid(pawn) && Fn(pawn,"IsInVehicle") && [&]{ Params p(Fn(pawn,"IsInVehicle")); Call(pawn,"IsInVehicle",&p); return p.GetBool("ReturnValue"); }();
    if(initial || onBike || dx*dx+dy*dy+dz*dz>20000.f*20000.f) TravelTo(pos,yaw);
    else {
        SetPosition(pos);
        if(auto f=IsValid(controller)?Fn(controller,"SetControlRotation"):nullptr) { Params p(f); p.Set("NewRotation",FRotator{0,yaw,0}); Call(controller,"SetControlRotation",&p); }
    }
    if(initial) mpSpawned=true;
    dg::Log("Server teleport%s: %.0f %.0f %.0f yaw=%.0f",initial?" (spawn)":"",pos.X,pos.Y,pos.Z,yaw);
}
// The game's fast-travel fade sometimes never fades back in (black screen with sound, map and pause
// dead because input stays blocked). If the screen stays fully black outside any travel, fade it in.
void RecoverStuckFade(double now) {
    static double blackSince=0,next=0;
    if(now<next) return; next=now+0.5;
    auto mission=Instance("BendGameMission");
    if(pendingArrival || Loading() || !IsValid(mission)) { blackSince=0; return; }
    float value=0;
    for(auto cls=mission->Class;cls;cls=SuperOf(cls)) if(auto p=FindProperty(cls,"FadingTimeline_FadingValue_4D9177FC4E790DD0D8692285798AB37D")) { value=Field<float>(mission,Field<int>(p,off::UProperty_Offset)); break; }
    const bool black=value>=0.99f && !GetBoolProp(mission,"FadingFromBlack");
    if(!black) { blackSince=0; return; }
    if(!blackSince) { blackSince=now; return; }
    if(now-blackSince<4) return;
    blackSince=0;
    if(auto f=Fn(mission,"CallFadeOutOfBlack")) { Params p(f); p.SetBool("UnhideHudAtEnd",true); Call(mission,"CallFadeOutOfBlack",&p); }
    dg::Log("Stuck black screen recovered (fade value %.2f)",value);
}
// Fast travel puts the bike too low (half in the ground). Once the player stands on the ground after
// the arrival, a bike parked next to it is lifted to the player's feet.
double bikeFixAt=0;
void FixBikeAfterArrival(double now) {
    if(!bikeFixAt || now<bikeFixAt) return;
    bikeFixAt=0;
    auto f=IsValid(controller)?Fn(controller,"GetAssignedVehicle"):nullptr; if(!f) return;
    Params p(f); Call(controller,"GetAssignedVehicle",&p);
    UObject* bike=p.Get<UObject*>("ReturnValue"); if(!IsValid(bike)) return;
    auto get=Fn(bike,"K2_GetActorLocation"); if(!get) return;
    Params g(get); Call(bike,"K2_GetActorLocation",&g); FVector at=g.Get<FVector>("ReturnValue");
    const auto me=Position(); const float dx=at.X-me.X,dy=at.Y-me.Y;
    if(dx*dx+dy*dy>1500.f*1500.f) return;
    const float feet=me.Z-88.f;  // capsule half height
    if(at.Z>feet-10.f) return;
    at.Z=feet+5.f;
    if(auto set=Fn(bike,"K2_SetActorLocation")) { Params s(set); s.Set("NewLocation",at); s.SetBool("bTeleport",true); Call(bike,"K2_SetActorLocation",&s); }
    dg::Log("Bike lifted to the ground after arrival: z=%.0f",at.Z);
}
void AlignArrival(double now) {
    FixBikeAfterArrival(now);
    if(!pendingArrival || now<arrivalAfter || Loading() || !IsValid(pawn)) return;
    // Bend fast travel treats Z as a feet position. Our editor stores the actor's root position.
    const auto before=Position();
    if(!SetPosition(arrivalPosition)) { dg::Log("Could not align arrival; retrying"); arrivalAfter=now+1; return; }
    if(auto f=IsValid(controller)?Fn(controller,"SetControlRotation"):nullptr) { Params p(f); p.Set("NewRotation",FRotator{0,arrivalYaw,0}); Call(controller,"SetControlRotation",&p); }
    pendingArrival=false; note=L"Saved spawn restored"; bikeFixAt=now+1.5;
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
// Story and AI managers. After a death reload they exist before they are ready, and calling into them
// crashes inside the game: such a pass is dropped (SweepManagersSafe) and tried again later.
void SweepManagers(double now) {
    if(suppressStory) {
        if(auto mission=Instance("BendGameMission")) {
            SetBoolProp(mission,"MissionAndJobProgressionBlocked",true);
            if(Once(mission)) { dg::Log("Mission progression blocked; active=%d",GetBoolProp(mission,"IsMissionActive")); }
        }
        for(const char* key:{"CascadeLocked","BelknapLocked","LostLakeLocked","CraterLakeLocked"}) SetBoolProp(gameMode,key,false);
    }
    if(suppressAI) {
        if(auto ai=Instance("BendAIManager")) BoolArg(ai,"SetCrowdSimulationEnabled","Enabled",false);
        // Ambient events (ambushes, strangers, the "?" map markers) come back after fast travel and
        // world reloads, so the pause is re-asserted every few seconds instead of once.
        static double nextEvents=0;
        if(auto events=Instance("BendAmbientEventManager"); events && (Once(events) || now>=nextEvents)) {
            nextEvents=now+5; BoolArg(events,"Pause","ForceUnload",true); Call(events,"RemoveAllEventsQueuedToLoad");
        }
        if(auto gen=Instance("BendAmbientGenerator")) {
            if(Once(gen)) Call(gen,"ForceDespawnAll");
            if(auto f=Fn(gen,"SetImportanceToBlockBelow")) { Params p(f); p.Set<uint8_t>("importance",255); Call(gen,"SetImportanceToBlockBelow",&p); }
        }
    }
}
bool SweepManagersSafe(double now) {
    __try { SweepManagers(now); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Sweep(double now) {
    if(now<nextSweep) return; nextSweep=now+0.008;
    static double nextManagers=0;
    if(now>=nextManagers) {
        nextManagers=now+1;
        if(!SweepManagersSafe(now)) { nextManagers=now+3; dg::Log("World managers not ready; retrying"); }
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
    if(!Foreground() || Paused() || policy::GetState().menuActive || panel::IsLoading() || devmenu::IsOpen() || overlay::CursorMode()) return;
    const auto r=View(); const float rad=0.01745329252f;
    const float cy=cosf(r.Yaw*rad),sy=sinf(r.Yaw*rad),cp=cosf(r.Pitch*rad),sp=sinf(r.Pitch*rad);
    const float forward=float(Down('W'))-float(Down('S')),right=float(Down('D'))-float(Down('A')),up=float(Down(VK_SPACE))-float(Down(VK_CONTROL));
    FVector delta{forward*cp*cy-right*sy,forward*cp*sy+right*cy,forward*sp+up};
    const float len=sqrtf(delta.X*delta.X+delta.Y*delta.Y+delta.Z*delta.Z); if(len<0.001f) return;
    auto pos=Position(); const float distance=speed*std::clamp(dt,0.f,0.1f)*(Down(VK_SHIFT)?4.f:1.f)/std::max(1.f,len);
    pos.X+=delta.X*distance; pos.Y+=delta.Y*distance; pos.Z+=delta.Z*distance; SetPosition(pos);
}
struct Aim;
Aim AimTarget();
void Pick(const char* kind);
// The quest window can arm a pick ("Назначить точку" ...): the next left click in the editor takes it.
// It writes editor_pick_mode.json {"kind":"point"|"object"|"item"}; the game deletes it when used.
bool ArmedPick(bool fire) {
    const auto file=fs::path(profile::Root())/L"editor_pick_mode.json";
    std::ifstream in(file); std::stringstream ss; ss << in.rdbuf(); in.close();
    const std::string text=ss.str();
    const char* kind=text.find("\"object\"")!=std::string::npos?"object":text.find("\"item\"")!=std::string::npos?"item":text.find("\"point\"")!=std::string::npos?"point":nullptr;
    if(!kind) return false;
    if(fire) { std::error_code ec; fs::remove(file,ec); Pick(kind); }
    return true;
}
void Commands(double now) {
    bool focus=Foreground();
    const bool devKey=Pressed(VK_F3);
    const int keys[]{VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN,VK_ESCAPE};
    bool menuKeys[6]{}; for(int i=0;i<6;++i) menuKeys[i]=Pressed(keys[i]);
    // While a text field of the editor UI has the keyboard, editor hotkeys stay quiet; in cursor mode
    // clicks belong to the UI (editorui handles clicks into the world).
    if(overlay::Typing()) focus=false;
    const bool flyKey=Pressed(VK_F2),saveKey=Pressed(VK_F5),markKey=Pressed(VK_F6),objectKey=Pressed(VK_F7),itemKey=Pressed(VK_F9),returnKey=Pressed(VK_F8),slower=Pressed(VK_OEM_4),faster=Pressed(VK_OEM_6);
    const bool left=Pressed(VK_LBUTTON),right=Pressed(VK_RBUTTON);
    // Joined players have no editor tools: the server owns time, weather and positions.
    if(focus && ready && !panel::IsLoading() && !multiplayer) {
        if(devKey && !policy::GetState().menuActive && !Paused()) DGMP_SetDevMenuVisible(!devmenu::IsOpen());
        devmenu::Update(true);
        const bool wasDev=devmenu::IsOpen();
        if(wasDev) {
            for(int i=0;i<6;++i) if(menuKeys[i]) devmenu::Key(keys[i]);
        } else {
            if(flyKey) SetFlying(!flying); if(saveKey) SavePoint(true); if(markKey) { SavePoint(false); Pick("point"); } if(returnKey) ReturnSpawn();
            if(objectKey) Pick("object"); if(itemKey) Pick("item");
            if(slower) speed=std::max(100.f,speed/2); if(faster) speed=std::min(40000.f,speed*2);
            if(flying && !overlay::CursorMode() && !policy::GetState().menuActive && !Paused()) {
                if(left) { ++editorLeft; if(!ArmedPick(true)) { note=L"Editor LMB reserved"; dg::Log("Editor LMB: %.3f %.3f %.3f",Position().X,Position().Y,Position().Z); } }
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
// What the camera points at (up to 50 m), for the launcher's quest editor: the world object a quest
// can name (a searchable car, door, pickup, explosive near the hit point) or else the hit actor, and the
// hit point. Written into telemetry.json as "aim".
struct Aim { bool valid=false,object=false; std::string id,cls; FVector at{}; };  // object: a quest-usable world object
Aim AimTarget() {
    Aim aim;
    UObject* mgr=IsValid(controller)?Field<UObject*>(controller,off::PC_CameraManager):nullptr;
    static UObject* kismet=nullptr; static UObject* statics=nullptr;
    if(!IsValid(kismet)) kismet=FindObject("/Script/Engine.Default__KismetSystemLibrary");
    if(!IsValid(statics)) statics=FindObject("/Script/Engine.Default__GameplayStatics");
    auto trace=kismet?Fn(kismet,"LineTraceSingle_NEW"):nullptr; auto brk=statics?Fn(statics,"BreakHitResult"):nullptr;
    if(!IsValid(mgr) || !IsValid(pawn) || !trace || !brk) return aim;
    const FVector from=Field<FVector>(mgr,off::CamMgr_POV_Location);
    const FRotator rot=Field<FRotator>(mgr,off::CamMgr_POV_Location+0xC);  // FMinimalViewInfo: Location, Rotation
    const float rad=0.01745329252f, cp=cosf(rot.Pitch*rad);
    const FVector to{from.X+cosf(rot.Yaw*rad)*cp*5000.f,from.Y+sinf(rot.Yaw*rad)*cp*5000.f,from.Z+sinf(rot.Pitch*rad)*5000.f};
    Params t(trace); t.Set("WorldContextObject",pawn); t.Set("Start",from); t.Set("End",to); t.Set<uint8_t>("TraceChannel",0); t.SetBool("bTraceComplex",false); t.SetBool("bIgnoreSelf",true);
    Call(kismet,"LineTraceSingle_NEW",&t);
    if(!t.GetBool("ReturnValue")) return aim;
    Params b(brk);
    auto hitProp=FindProperty(trace,"OutHit"),inProp=FindProperty(brk,"Hit");
    if(!hitProp || !inProp) return aim;
    std::memcpy(static_cast<uint8_t*>(b.Data())+Field<int>(inProp,off::UProperty_Offset),static_cast<uint8_t*>(t.Data())+Field<int>(hitProp,off::UProperty_Offset),136);
    Call(statics,"BreakHitResult",&b);
    aim.valid=true; aim.at=b.Get<FVector>("ImpactPoint");
    UObject* hit=b.Get<UObject*>("HitActor");
    // A quest names the interactable, which usually sits next to (not on) the mesh the trace hits.
    static std::vector<UObject*> classes;
    if(classes.empty()) for(const char* c:{"BendBasicInteract","BendInventoryPickup","BendDoor","Explosive_C"}) if(auto cls=FindClass(c)) classes.push_back(cls);
    UObject* best=nullptr; float bestDist=250.f*250.f;
    for(int i=0,n=NumObjects();i<n;++i) {
        UObject* o=ObjectAt(i);
        if(!o || (o->ObjectFlags&0x10) || !IsValid(o)) continue;
        bool wanted=false; for(auto cls:classes) if(IsA(o,cls)) { wanted=true; break; }
        if(!wanted) continue;
        auto f=Fn(o,"K2_GetActorLocation"); if(!f) continue;
        Params l(f); Call(o,"K2_GetActorLocation",&l); const FVector at=l.Get<FVector>("ReturnValue");
        const float dx=at.X-aim.at.X,dy=at.Y-aim.at.Y,dz=at.Z-aim.at.Z,d=dx*dx+dy*dy+dz*dz;
        if(d<bestDist) { bestDist=d; best=o; }
    }
    aim.object=best!=nullptr;
    if(!best) best=hit;
    if(IsValid(best)) { aim.id=GetName(best); aim.cls=GetName(best->Class); }
    return aim;
}
// Quest editor picks (F6 point, F7 object, F9 item): what the camera points at, appended to
// editor_picks.jsonl for the launcher's quest window, which puts it into the selected step.
void Pick(const char* kind) {
    static bool fresh=false;
    const auto path=fs::path(profile::Root())/L"editor_picks.jsonl";
    if(!fresh) { fresh=true; std::ofstream(path,std::ios::trunc); }  // a new editor session
    const Aim aim=AimTarget();
    FVector at=aim.valid?aim.at:Position();
    if(!strcmp(kind,"object") && !aim.object) {
        note=L"F7: наведитесь на багажник, дверь, предмет или бочку";
        policy::ShowNativeHint(controller,L"",note,"",false,2);
        return;
    }
    std::ofstream file(path,std::ios::app); file.precision(9);
    file << "{\"seq\":" << GetTickCount64() << ",\"kind\":\"" << kind << "\",\"x\":" << at.X << ",\"y\":" << at.Y << ",\"z\":" << at.Z
         << ",\"id\":\"" << (strcmp(kind,"object")?"":aim.id) << "\",\"class\":\"" << (strcmp(kind,"object")?"":aim.cls) << "\"}\n";
    const std::wstring what=!strcmp(kind,"point")?L"точка":!strcmp(kind,"object")?L"объект":L"предмет";
    note=L"Квест: "+what+L" добавлен"+(strcmp(kind,"object")?L"":L" ("+std::wstring(aim.id.begin(),aim.id.end())+L")");
    policy::ShowNativeHint(controller,L"",L"Квест: "+what+L" отправлен в окно квестов","",false,2);
    dg::Log("Quest editor pick %s at %.0f %.0f %.0f %s",kind,at.X,at.Y,at.Z,aim.id.c_str());
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
             << ",\"savingBlocked\":" << ui.savingBlocked << ",\"blockedSaveRequests\":" << ui.blockedSaveRequests << ",\"blockedSaveFileWrites\":" << profile::BlockedSaveWrites() << ",\"hiddenSaveControls\":" << ui.hiddenSaveControls << ",\"removedPauseControls\":" << ui.removedPauseControls << ",\"blockedPauseRequests\":" << ui.blockedPauseRequests << ",\"activeNativeJobs\":" << ui.activeNativeJobs << ",\"pausedNativeJobs\":" << ui.pausedNativeJobs << ",\"nativeMissionActive\":" << ui.nativeMissionActive << ",\"trackingCleared\":" << ui.trackingCleared << ",\"nestObjectsHidden\":" << ui.nestObjectsHidden << ",\"storyBlockersDisabled\":" << ui.storyBlockersDisabled
             << ",\"doorsUnlocked\":" << ui.doorsUnlocked << ",\"zoneParametersCleared\":" << ui.zoneParametersCleared;
        const auto env=environment::GetSnapshot();
        file << ",\"devMenuOpen\":" << devmenu::IsOpen() << ",\"environmentAvailable\":" << bool(env.state.available)
             << ",\"timeOfDayHours\":" << env.state.hours << ",\"timePaused\":" << bool(env.state.timePaused)
             << ",\"weatherId\":" << env.state.weatherId << ",\"weatherAutomatic\":" << bool(env.state.weatherAutomatic)
             << ",\"environmentRequested\":" << env.state.requestedRevision << ",\"environmentProcessed\":" << env.state.processedRevision
             << ",\"environmentLastResult\":" << env.state.lastRequestResult << ",\"weatherCatalog\":[";
        for(size_t i=0;i<env.weather.size();++i) { if(i) file << ','; file << "{\"id\":" << env.weather[i].id << ",\"name\":\"" << env.weather[i].name << "\"}"; }
        file << "]";
        const Aim aim=AimTarget();
        if(aim.valid) file << ",\"aim\":{\"id\":\"" << aim.id << "\",\"class\":\"" << aim.cls << "\",\"object\":" << aim.object << ",\"x\":" << aim.at.X << ",\"y\":" << aim.at.Y << ",\"z\":" << aim.at.Z << "}";
        else file << ",\"aim\":null";
        file << "}\n";
    }
    MoveFileExW(tmp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING);
    wchar_t text[1024]; swprintf_s(text,L"DGMP v0.5.2 | %hs | Fly: %ls | %.0f m/s\nX %.2f  Y %.2f  Z %.2f  Yaw %.1f\nF2 fly | WASD move | Space/Ctrl up/down | Shift boost\n[ / ] speed | F5 save spawn | F6 marker | F8 return\n%ls",stage.c_str(),flying?L"ON":L"OFF",speed/100,pos.X,pos.Y,pos.Z,rotation.Yaw,note.c_str());
    // In the editor the status lives in the in-game UI (bottom left, folded); the old panel only if that is not up.
    if(!multiplayer && overlay::Visible()) { editorui::SetStatus(text); panel::Text(L""); }
    else panel::Text(multiplayer?L"":text);
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
                if(!ready) { ready=true; stage="world-ready"; dg::Log("WORLD READY: isolated DGMP profile, player spawned"); if(!multiplayer) editorui::Start(); }
                if(wantedFly && !flying) SetFlying(true);
                if(!spawnApplied) { spawnApplied=true; if(!multiplayer && GetPrivateProfileIntW(L"Spawn",L"Enabled",0,Ini().c_str())) ReturnSpawn(); }
                if(multiplayer) ApplyServerTeleport();
                AlignArrival(now); RecoverStuckFade(now); Sweep(now); Fly(dt);
            }
        } else {
            stableSince=0;
            if(ready && menu) { ready=false; stage="main-menu"; }
        }
        if(requested && !ready && now-requestTime>90 && stage=="load-requested") { stage="load-timeout"; bootstrapComplete=true; panel::Loading(false,0,lang::Pick(L"Ошибка загрузки",L"Loading failed")); note=L"Auto load timed out; see logs/DGMP.log"; dg::Log("Bootstrap load timed out"); }
    }
    policy::Update(pawn,controller,gameMode,flying || devmenu::IsOpen() || !ready || pendingArrival,revealMap,true,suppressStory);
    if(multiplayer) {
        const bool inWorld=ready && IsValid(pawn) && !Loading();
        const bool gameplay=inWorld && !panel::IsLoading() && !policy::GetState().menuActive && !Paused();
        mp::Update(now,dt,pawn,controller,inWorld,gameplay);
        const auto phase=mp::GetPhase();
        if(phase==mp::Phase::Joined && !mpJoinedAt) mpJoinedAt=now;
        // A server that never sends a spawn still lets the player in after a few seconds.
        if(mpJoinedAt && !mpSpawned && now-mpJoinedAt>5) mpSpawned=true;
        if(phase==mp::Phase::Failed && !bootstrapComplete) {
            if(!mpFailedAt) { mpFailedAt=now; dg::Log("Multiplayer join failed: %ls",mp::FailReason().c_str()); }
            panel::Loading(true,1.f,std::wstring(lang::Pick(L"Не удалось подключиться: ",L"Could not connect: "))+mp::FailReason());
            if(now-mpFailedAt>8) {
                // Back to the launcher: nothing to play without the server.
                if(auto f=IsValid(controller)?Fn(controller,"ConsoleCommand"):nullptr) {
                    std::wstring cmd=L"quit"; FString str{cmd.c_str(),int32_t(cmd.size()+1),int32_t(cmd.size()+1)};
                    Params p(f); p.Set("Command",str); p.SetBool("bWriteToLog",true); Call(controller,"ConsoleCommand",&p);
                } else ExitProcess(0);
                mpFailedAt=now+1e9;
            }
            Commands(now); environment::Update(ready && !Loading()); Status(now);
            return;
        }
    }
    if(multiplayer && mp::Respawning() && !pendingArrival) panel::Loading(true,0.9f,lang::Pick(L"Возрождение",L"Respawning"));
    if(!bootstrapComplete || panel::IsLoading()) {
        if(multiplayer && mp::Respawning() && !pendingArrival) {
            // death: wait for the server's respawn teleport
        } else if(multiplayer && ready && !Loading() && !pendingArrival && !mpSpawned) {
            panel::Loading(true,0.92f,mp::GetPhase()==mp::Phase::Joined?lang::Pick(L"Вход на сервер",L"Joining the server"):lang::Pick(L"Подключение к серверу",L"Connecting to the server"));
        } else if(ready && spawnApplied && !pendingArrival && !Loading()) {
            FinishBootstrapFade();
            bootstrapComplete=true; panel::Loading(false,1.f,lang::Pick(L"Мир готов",L"World ready"));
            dg::Log("Loading cover released: world ready and spawn fully aligned");
        } else if(pendingArrival) panel::Loading(true,0.88f,lang::Pick(L"Перемещение",L"Travelling"));
        else if(!ready && !requested) panel::Loading(true,0.25f,lang::Pick(L"Инициализация мира",L"Initializing the world"));
        else if(!ready) panel::Loading(true,Loading()?0.55f:0.72f,lang::Pick(L"Загрузка мира",L"Loading the world"));
    }
    Commands(now); environment::Update(ready && !panel::IsLoading() && !Loading()); Status(now);
    if(!multiplayer && ready && !Loading()) { editorui::Update(now,pawn,controller); mp::UpdateEditorPreview(now,pawn); }
    // Silent while the loading cover is up: no menu music or chatter from the template save.
    audio::Update(!bootstrapComplete || panel::IsLoading());
    devcmd::Update(pawn,controller,gameMode);
}
void ReportException(unsigned long code) {
    ++errors; dg::Log("DGMP frame exception 0x%08X (%u)",code,errors);
    if(errors>=5) { disabled=true; panel::Text(L"DGMP disabled after repeated errors. See logs/DGMP.log"); }
}
// "module+rva" for an address: version.dll offsets resolve with build/version.map.
void Where(const void* address,char* out,size_t size) {
    HMODULE module=nullptr; char path[MAX_PATH]="?";
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<const char*>(address),&module) && module) {
        GetModuleFileNameA(module,path,MAX_PATH);
        const char* name=strrchr(path,'\\'); name=name?name+1:path;
        snprintf(out,size,"%s+%llX",name,static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module)));
    } else snprintf(out,size,"%p",address);
}
int CaptureException(EXCEPTION_POINTERS* info) {
    char where[160]; Where(info->ExceptionRecord->ExceptionAddress,where,sizeof(where));
    dg::Log("Exception 0x%08X at %s",info->ExceptionRecord->ExceptionCode,where);
    // The filter runs on the faulting stack, so the captured frames include the code that failed.
    void* frames[40]; const USHORT count=CaptureStackBackTrace(0,40,frames,nullptr);
    for(USHORT i=0;i<count;++i) { Where(frames[i],where,sizeof(where)); dg::Log("  #%u %s",i,where); }
    return EXCEPTION_EXECUTE_HANDLER;
}
void Recover(unsigned long code) {
    // A caught stack overflow leaves the guard page consumed: restore it or the next one kills the game.
    if(code==EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
    ReportException(code);
}
void SafeUpdate(float dt) {
    __try { if(!disabled) Update(dt); }
    __except(CaptureException(GetExceptionInformation())) { Recover(GetExceptionCode()); }
}
void SafeInputPolicy() {
    __try { if(!disabled) { DevInputPolicy(); policy::Update(pawn,controller,gameMode,flying || devmenu::IsOpen() || !ready || pendingArrival,revealMap,true,suppressStory); } }
    __except(CaptureException(GetExceptionInformation())) { Recover(GetExceptionCode()); }
}
using TickFn=void(__fastcall*)(UObject*,float,bool);
TickFn originalTick=nullptr;
void* copiedVtable[512];
void __fastcall Tick(UObject* engine,float dt,bool idle) {
    static bool guarded=false; if(!guarded) { ULONG reserve=64*1024; SetThreadStackGuarantee(&reserve); guarded=true; }  // room to log a stack overflow
    SafeInputPolicy(); originalTick(engine,dt,idle); SafeUpdate(dt);
}
UObject* FindEngine() {
    for(int i=0,n=NumObjects();i<n;++i) { auto o=ObjectAt(i); if(o && !(o->ObjectFlags&0x10) && IsValid(o) && o->Class && GetName(o->Class)=="GameEngine") return o; } return nullptr;
}
}
void DGMPInitThread() {
    const auto logPath=fs::path(profile::Root())/L"logs"/L"DGMP.log";
    dg::LogOpen(logPath.string().c_str()); panel::Start();
    if(overlay::Install()) overlay::SetCover(&panel::DrawCover);  // the loading cover lives in the game's frames
    dg::Log("DGMP v0.5.2 loading; built %s %s; profile imports patched=%d",__DATE__,__TIME__,profile::PatchedCount());
    dg::Log("Isolated profile: %ls",profile::Saved().c_str());
    panel::Loading(true,0.1f,lang::Pick(L"Подготовка DGMP",L"Preparing DGMP"));
    if(!ue::Init()) { panel::Text(L"DGMP: unsupported game build; see log"); panel::Loading(true,0,lang::Pick(L"Эта сборка игры не поддерживается",L"This game build is not supported")); return; }
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
    multiplayer=mp::Init();
    policy::SetEditorSession(!multiplayer);
    if(multiplayer) { autoFly=wantedFly=false; dg::Log("Multiplayer client session: editor tools disabled"); }
    UObject* engine=nullptr;
    for(int i=0;i<240 && !engine;++i) { Sleep(250); if(NumObjects()>1000) engine=FindEngine(); }
    if(!engine) { dg::Log("GameEngine not found"); panel::Text(L"DGMP: GameEngine not found"); return; }
    auto vt=engine->VTable;
    if(reinterpret_cast<uintptr_t>(vt[60])-Base()!=0x1AC2A20 || reinterpret_cast<uintptr_t>(vt[73])-Base()!=0x288F730) { dg::Log("Engine vtable mismatch; no hook installed"); return; }
    memcpy(copiedVtable,vt,sizeof(copiedVtable)); originalTick=reinterpret_cast<TickFn>(vt[73]); copiedVtable[73]=reinterpret_cast<void*>(&Tick); engine->VTable=copiedVtable;
    dg::Log("DGMP engine tick installed; automatic bootstrap enabled=%d",autoLoad);
}
