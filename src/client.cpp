#include "ue.h"
#include "log.h"
#include "profile.h"
#include "panel.h"
#include "skins.h"
#include "policy.h"
#include "environment.h"
#include "devmenu.h"
#include "mp.h"
#include "shared/version.h"
#include "devcmd.h"
#include "audio.h"
#include "hud.h"
#include "overlay.h"
#include "editorui.h"
#include "lang.h"
#include "zombies.h"
#include "diagnostics.h"
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
unsigned int triggers=0, removedAI=0, removedAIGear=0, errors=0;
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
    const bool editorInput=(devmenu::IsOpen() || overlay::CursorMode()) && !overlay::NativeInput();
    if(devInputLocked && (!editorInput || devInputController!=controller)) {
        if(IsValid(devInputController)) {
            BoolArg(devInputController,"SetIgnoreMoveInput","bNewMoveInput",false);
            BoolArg(devInputController,"SetIgnoreLookInput","bNewLookInput",false);
            SetBoolProp(devInputController,"bShowMouseCursor",devPreviousCursor);
            DevInputMode(devInputController,false);
        }
        devInputLocked=false; devInputController=nullptr;
    }
    if(editorInput && !devInputLocked && IsValid(controller)) {
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
// ---------------------------------------------------------------- editor camera
// Flying in the editor looks through a camera of its own: no spring arm, lag or wall collisions of
// the game's third-person camera. The hidden player follows it so the world streams in around it.
// In cursor mode, holding the right button looks around and lets WASD move (like editors do).
UObject* editorCam=nullptr;
FVector camPos{}, camVel{};
FRotator camRot{}, lastControl{};
bool camLook=false; POINT lookAnchor{};
float Wrap180(float a) { a=fmodf(a+180.f,360.f); if(a<0) a+=360.f; return a-180.f; }
UObject* GameStatics() { static UObject* s=nullptr; if(!IsValid(s)) s=FindObject("/Script/Engine.Default__GameplayStatics"); return s; }
void ViewThrough(UObject* target) {
    if(auto fn=IsValid(controller) && IsValid(target)?Fn(controller,"SetViewTargetWithBlend"):nullptr) {
        Params p(fn); p.Set("NewViewTarget",target); p.Set<float>("BlendTime",0.f); Call(controller,"SetViewTargetWithBlend",&p);
    }
}
void PlaceCamera() {
    if(!IsValid(editorCam)) return;
    if(auto fn=Fn(editorCam,"K2_SetActorLocationAndRotation")) { Params p(fn); p.Set("NewLocation",camPos); p.Set("NewRotation",camRot); p.SetBool("bTeleport",true); Call(editorCam,"K2_SetActorLocationAndRotation",&p); }
}
void StartEditorCamera() {
    if(multiplayer || IsValid(editorCam) || !GameStatics()) return;
    UObject* mgr=IsValid(controller)?Field<UObject*>(controller,off::PC_CameraManager):nullptr;
    camPos=IsValid(mgr)?Field<FVector>(mgr,off::CamMgr_POV_Location):Position();
    camRot=View(); camRot.Roll=0; camRot.Pitch=Wrap180(camRot.Pitch); camVel={}; lastControl=View();
    FTransform t; t.Translation=camPos;
    Params begin(Fn(GameStatics(),"BeginSpawningActorFromClass"));
    begin.Set("WorldContextObject",pawn); begin.Set("ActorClass",FindClass("CameraActor")); begin.Set("SpawnTransform",t); begin.SetBool("bNoCollisionFail",true);
    Call(GameStatics(),"BeginSpawningActorFromClass",&begin);
    editorCam=begin.Get<UObject*>("ReturnValue");
    if(!editorCam) { dg::Log("Editor camera: spawn failed"); return; }
    Params finish(Fn(GameStatics(),"FinishSpawningActor")); finish.Set("Actor",editorCam); finish.Set("SpawnTransform",t);
    Call(GameStatics(),"FinishSpawningActor",&finish);
    // Fill the whole window (no letterbox).
    for(auto c=editorCam->Class;c;c=SuperOf(c)) if(auto prop=FindProperty(c,"CameraComponent")) {
        UObject* comp=Field<UObject*>(editorCam,Field<int>(prop,off::UProperty_Offset));
        if(IsValid(comp)) if(auto fn=Fn(comp,"SetConstraintAspectRatio")) { Params a(fn); a.SetBool("bInConstrainAspectRatio",false); Call(comp,"SetConstraintAspectRatio",&a); }
        break;
    }
    PlaceCamera(); ViewThrough(editorCam);
    dg::Log("Editor camera on at %.0f %.0f %.0f",camPos.X,camPos.Y,camPos.Z);
}
void StopEditorCamera() {
    if(!IsValid(editorCam)) { editorCam=nullptr; return; }
    ViewThrough(pawn);
    Call(editorCam,"K2_DestroyActor"); editorCam=nullptr;
    if(camLook) { camLook=false; ShowCursor(TRUE); }
}
// One frame of the editor camera: look (game mouse look, or right button in cursor mode) and move.
void EditorCamera(float dt) {
    if(overlay::NativeInput() || policy::GetState().menuActive) {
        camVel={};
        if(camLook) { camLook=false; ShowCursor(TRUE); }
        lastControl=View();
        return;
    }
    // Something else moved the player (F8, a teleport, a load): the camera goes along.
    const FVector at=Position();
    const float dx=at.X-camPos.X, dy=at.Y-camPos.Y, dz=at.Z-(camPos.Z-200.f);
    if(dx*dx+dy*dy+dz*dz>500.f*500.f) { camPos={at.X,at.Y,at.Z+200.f}; camVel={}; }
    const bool cursor=overlay::CursorMode();
    const bool rmb=Down(VK_RBUTTON) && Foreground();
    if(cursor) {
        if(rmb && !camLook && !overlay::MouseOverUi()) { camLook=true; GetCursorPos(&lookAnchor); ShowCursor(FALSE); }
        if(!rmb && camLook) { camLook=false; ShowCursor(TRUE); }
        if(camLook) {
            POINT now; GetCursorPos(&now);
            const float lookY=float(now.y-lookAnchor.y)*0.15f;
            camRot.Yaw+=float(now.x-lookAnchor.x)*0.15f;
            camRot.Pitch=std::clamp(camRot.Pitch+(editorui::InvertLook()?-lookY:lookY),-89.f,89.f);
            SetCursorPos(lookAnchor.x,lookAnchor.y);
            if(auto fn=Fn(controller,"SetControlRotation")) { Params p(fn); p.Set("NewRotation",camRot); Call(controller,"SetControlRotation",&p); }
            lastControl=camRot;
        }
    } else {
        if(camLook) { camLook=false; ShowCursor(TRUE); }
        // The game's mouse look turns the control rotation; the camera follows its changes.
        const FRotator c=View();
        const float dYaw=Wrap180(c.Yaw-lastControl.Yaw), dPitch=Wrap180(c.Pitch-lastControl.Pitch);
        lastControl=c;
        camRot.Yaw=Wrap180(camRot.Yaw+dYaw);
        camRot.Pitch=std::clamp(camRot.Pitch+(editorui::InvertLook()?-dPitch:dPitch),-89.f,89.f);
    }
    const bool canMove=Foreground() && !Paused() && !policy::GetState().menuActive && !devmenu::IsOpen() && !overlay::Typing() && (!cursor || camLook);
    FVector want{};
    if(canMove) {
        const float rad=0.01745329252f, cy=cosf(camRot.Yaw*rad), sy=sinf(camRot.Yaw*rad), cp=cosf(camRot.Pitch*rad), sp=sinf(camRot.Pitch*rad);
        const float forward=float(Down('W'))-float(Down('S')), right=float(Down('D'))-float(Down('A'));
        const float up=float(Down(VK_SPACE)||Down('E'))-float(Down(VK_CONTROL)||Down('Q'));
        want={forward*cp*cy-right*sy, forward*cp*sy+right*cy, forward*sp+up};
        const float len=sqrtf(want.X*want.X+want.Y*want.Y+want.Z*want.Z);
        if(len>0.001f) { const float v=speed*(Down(VK_SHIFT)?4.f:1.f)/len; want={want.X*v,want.Y*v,want.Z*v}; }
    }
    // Smooth start and stop.
    const float k=1.f-expf(-std::clamp(dt,0.f,0.1f)*10.f);
    camVel={camVel.X+(want.X-camVel.X)*k, camVel.Y+(want.Y-camVel.Y)*k, camVel.Z+(want.Z-camVel.Z)*k};
    const float step=std::clamp(dt,0.f,0.1f);
    camPos={camPos.X+camVel.X*step, camPos.Y+camVel.Y*step, camPos.Z+camVel.Z*step};
    PlaceCamera();
    SetPosition({camPos.X,camPos.Y,camPos.Z-200.f});  // the hidden player keeps the world streamed in
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
    if(!multiplayer) { if(on) StartEditorCamera(); else { StopEditorCamera(); SetPosition(camPos); } }
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
double travelSince=0;  // when the pending travel was asked for
void TravelTo(const FVector& pos,float yaw) {
    arrivalPosition=pos; arrivalYaw=yaw; arrivalAfter=Now()+3; pendingArrival=true; travelSince=Now();
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
double editorMapOpeningUntil=0;
bool editorMapWasOpen=false, mapGroundPending=false;
FVector mapGroundTarget{};
double mapGroundDeadline=0, mapGroundCheck=0;
void OpenEditorMap() {
    if(multiplayer || !ready || pendingArrival || mapGroundPending) return;
    overlay::SetNativeInput(true);
    DevInputPolicy(); // release the editor's UIOnly input before the native menu takes focus
    camVel={};
    if(camLook) { camLook=false; ShowCursor(TRUE); }
    // A CameraActor view target bypasses BendCamMenu. Retain the flight state and
    // position, but let the game's pawn camera render its native map until close.
    if(flying && IsValid(editorCam)) ViewThrough(pawn);
    if(policy::OpenMap()) { editorMapOpeningUntil=Now()+2; editorMapWasOpen=true; }
    else {
        overlay::SetNativeInput(false);
        if(flying && IsValid(editorCam)) ViewThrough(editorCam);
    }
}
bool MapGround(const FVector& at,FVector& ground) {
    auto kismet=FindObject("/Script/Engine.Default__KismetSystemLibrary"), statics=GameStatics();
    auto trace=kismet?Fn(kismet,"LineTraceSingle_NEW"):nullptr, brk=statics?Fn(statics,"BreakHitResult"):nullptr;
    if(!trace || !brk) return false;
    auto hit=FindProperty(trace,"OutHit"), input=FindProperty(brk,"Hit");
    if(!hit || !input || Field<int>(hit,off::UProperty_ElementSize)!=136 || Field<int>(input,off::UProperty_ElementSize)!=136) return false;
    Params ray(trace); ray.Set("WorldContextObject",pawn);
    ray.Set("Start",FVector{at.X,at.Y,100000.f}); ray.Set("End",FVector{at.X,at.Y,-100000.f});
    ray.Set<uint8_t>("TraceChannel",0); ray.SetBool("bTraceComplex",true); ray.SetBool("bIgnoreSelf",true);
    if(!Call(kismet,"LineTraceSingle_NEW",&ray) || !ray.GetBool("ReturnValue")) return false;
    Params result(brk); memcpy(static_cast<uint8_t*>(result.Data())+Field<int>(input,off::UProperty_Offset),static_cast<uint8_t*>(ray.Data())+Field<int>(hit,off::UProperty_Offset),136);
    if(!Call(statics,"BreakHitResult",&result)) return false;
    ground=result.Get<FVector>("ImpactPoint");
    return std::isfinite(ground.Z) && result.Get<FVector>("ImpactNormal").Z>0.5f;
}
void EditorMap(double now) {
    if(multiplayer) return;
    const bool active=policy::GetState().menuActive;
    if(active) editorMapOpeningUntil=0;
    const bool usingMap=active || now<editorMapOpeningUntil;
    overlay::SetNativeInput(usingMap);
    if(!usingMap && editorMapWasOpen) {
        if(flying && IsValid(editorCam)) ViewThrough(editorCam);
        lastControl=View(); camVel={};
        diagnostics::Event("map closed; editor camera restored");
    }
    editorMapWasOpen=usingMap;
    FVector destination;
    if(ready && policy::TakeEditorMapTeleport(destination)) {
        policy::CloseMap();
        if(!flying) SetFlying(true);
        // The native map supplies XY, not reliable terrain height. Stream the destination
        // above terrain first, then trace its loaded geometry; never guess a walking Z.
        mapGroundTarget={destination.X,destination.Y,60000.f};
        mapGroundPending=true; mapGroundDeadline=now+30; mapGroundCheck=now+3;
        TravelTo(mapGroundTarget,camRot.Yaw);
        diagnostics::Event("map teleport streaming x=%.1f y=%.1f",destination.X,destination.Y);
    }
    if(mapGroundPending && !pendingArrival && !Loading() && now>=mapGroundCheck) {
        mapGroundCheck=now+0.5;
        FVector ground;
        if(MapGround(mapGroundTarget,ground)) {
            SetFlying(true);
            camPos={ground.X,ground.Y,ground.Z+1000.f}; camVel={};
            const bool moved=SetPosition({camPos.X,camPos.Y,camPos.Z-200.f});
            PlaceCamera(); ViewThrough(editorCam); lastControl=View();
            if(moved) { mapGroundPending=false; note=lang::Pick(L"Телепорт по карте выполнен",L"Map teleport complete"); }
            diagnostics::Event("map teleport ground=%.1f camera=%.1f moved=%d",ground.Z,camPos.Z,moved);
        }
        if(mapGroundPending && now>=mapGroundDeadline) {
            mapGroundPending=false;
            note=lang::Pick(L"Поверхность не найдена: оставлен режим полёта",L"No ground found: remaining in flight mode");
            dg::Log("Map teleport: terrain trace timed out; flight retained at safe altitude");
        }
    }
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
    // A fast travel that never finishes (the game's loading never shows or never ends): the player is
    // put there directly, so the cover comes down instead of staying up for good.
    if(pendingArrival && travelSince && now-travelSince>30 && Loading()) {
        dg::Log("Travel stuck for %.0f s (game loading); placing the player directly",now-travelSince);
        travelSince=0; arrivalAfter=now;
    }
    if(!pendingArrival || now<arrivalAfter || (Loading() && travelSince) || !IsValid(pawn)) return;
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
// Native weapons and ambient hand props are separate actors: hiding their NPC does not hide them.
// Follow ownership AND scene attachment, including nested props and actors created after the NPC
// was suppressed. Never classify by proximity or mesh name: server/player gear uses the same assets.
UObject* ComponentActor(UObject* component,UObject* actorClass) {
    for(int depth=0;IsValid(component) && depth<8;++depth,component=component->Outer)
        if(IsA(component,actorClass)) return component;
    return nullptr;
}
UObject* AttachmentActor(UObject* component,UObject* actorClass) {
    return IsValid(component)?ComponentActor(Field<UObject*>(component,off::Scene_AttachParent),actorClass):nullptr;
}
UObject* NativeNpcFor(UObject* object,UObject* actorClass,UObject* aiClass,UObject* dummyClass,int ownerOffset) {
    UObject* queue[32]{};
    size_t count=1; queue[0]=object;
    auto append=[&](UObject* o) {
        if(count<32 && IsValid(o) && IsA(o,actorClass) && std::find(queue,queue+count,o)==queue+count) queue[count++]=o;
    };
    for(size_t i=0;i<count;++i) {
        auto o=queue[i];
        if(o==pawn || (o->ObjectFlags&0x30)) continue;
        if(((aiClass && IsA(o,aiClass)) || (dummyClass && IsA(o,dummyClass))) && !zombies::IsOurs(o)) return o;
        if(ownerOffset>=0) append(Field<UObject*>(o,ownerOffset));
        auto root=Field<UObject*>(o,off::Actor_RootComponent);
        append(AttachmentActor(root,actorClass));
    }
    return nullptr;
}
// A hidden native NPC still animates (its meshes tick apart from the actor), and its animations carry
// sounds: snoring, a freaker's moans, heard from NPCs no one can see. Its meshes stop, its sounds too.
void Silence(UObject* actor) {
    static UObject *skeletal=FindClass("SkeletalMeshComponent"), *audio=FindClass("AudioComponent");
    auto f=Fn(actor,"GetComponentsByClass"); if(!f) return;
    struct Raw { UObject** data; int num, max; };
    for(UObject* cls:{skeletal,audio}) {
        if(!cls) continue;
        Params p(f); p.Set("ComponentClass",cls); Call(actor,"GetComponentsByClass",&p);
        const Raw list=p.Get<Raw>("ReturnValue");
        for(int i=0;i<list.num && i<64;++i) {
            UObject* c=list.data[i]; if(!IsValid(c)) continue;
            BoolArg(c,"SetComponentTickEnabled","bEnabled",false);
            if(cls==audio) Call(c,"Stop");
        }
    }
}
void Sweep(double now) {
    if(now<nextSweep) return; nextSweep=now+0.008;
    static double nextManagers=0;
    if(now>=nextManagers) {
        nextManagers=now+1;
        if(!SweepManagersSafe(now)) { nextManagers=now+3; dg::Log("World managers not ready; retrying"); }
    }
    auto triggerCls=FindClass("BendGameMissionTrigger"), restrictCls=FindClass("XpecRestrictArea"), aiCls=FindClass("BendAIPawn");
    // Camp civilians/guards use a separate LOD actor, not a BendAIPawn.
    // DGMP NPCs use plain SkeletalMeshActor and are deliberately excluded.
    auto dummyCls=FindClass("XpecDummySkeletalMeshActor");
    auto actorCls=FindClass("Actor");
    auto sceneCls=FindClass("SceneComponent");
    int ownerOffset=-1;
    if(auto property=actorCls?FindProperty(actorCls,"Owner"):nullptr) ownerOffset=Field<int>(property,off::UProperty_Offset);
    std::vector<UObject*> hide;
    std::vector<UObject*> hideComponents;
    static int cursor=0; const int n=NumObjects(); if(cursor>=n) cursor=0;
    const int end=std::min(n,cursor+2048);
    for(int i=cursor;i<end;++i) {
        auto o=ObjectAt(i); if(!o || o==pawn || (o->ObjectFlags&0x30) || !IsValid(o)) continue;
        // Ambient activity actors can remain in the scenery while only ONE component is on an
        // NPC's hand (e.g. a spice bottle); hiding the whole activity would also remove its pot.
        if(suppressAI && sceneCls && actorCls && IsA(o,sceneCls) &&
           (!GetBoolProp(o,"bHiddenInGame") || GetBoolProp(o,"bVisible"))) {
            if(auto parent=AttachmentActor(o,actorCls)) {
                if(auto npc=NativeNpcFor(parent,actorCls,aiCls,dummyCls,ownerOffset);
                   npc && ComponentActor(o,actorCls)!=npc) {
                    hideComponents.push_back(o);
                    if(Once(o)) {
                        ++removedAIGear;
                        if(removedAIGear<=12) dg::Log("Native NPC prop hidden: %s (NPC %s)",FullName(o).c_str(),FullName(npc).c_str());
                    }
                }
            }
        }
        if(suppressStory && ((triggerCls && IsA(o,triggerCls)) || (restrictCls && IsA(o,restrictCls)))) {
            if(Once(o)) { hide.push_back(o); ++triggers; }
        } else if(suppressAI && ((aiCls && IsA(o,aiCls)) || (dummyCls && IsA(o,dummyCls))) && !zombies::IsOurs(o)) {
            // LOD transitions can show an already-processed actor again.
            if(Once(o)) ++removedAI;
            if(!GetBoolProp(o,"bHidden") || GetBoolProp(o,"bActorEnableCollision")) hide.push_back(o);
        } else if(suppressAI && actorCls && IsA(o,actorCls) &&
                  (!GetBoolProp(o,"bHidden") || GetBoolProp(o,"bActorEnableCollision"))) {
            if(auto npc=NativeNpcFor(o,actorCls,aiCls,dummyCls,ownerOffset)) {
                hide.push_back(o);
                if(Once(o)) {
                    ++removedAIGear;
                    if(removedAIGear<=12) dg::Log("Native NPC gear hidden: %s (NPC %s)",FullName(o).c_str(),FullName(npc).c_str());
                }
            }
        }
    }
    cursor=end;
    for(auto o:hide) {
        BoolArg(o,"SetActorHiddenInGame","bNewHidden",true);
        BoolArg(o,"SetActorEnableCollision","bNewActorEnableCollision",false);
        BoolArg(o,"SetActorTickEnabled","bEnabled",false);
        if(aiCls && IsA(o,aiCls)) { auto ctrl=Field<UObject*>(o,off::Pawn_Controller); if(IsValid(ctrl)) BoolArg(ctrl,"SetActorTickEnabled","bEnabled",false); }
        if((aiCls && IsA(o,aiCls)) || (dummyCls && IsA(o,dummyCls))) Silence(o);
    }
    for(auto o:hideComponents) {
        if(auto f=Fn(o,"SetHiddenInGame")) { Params p(f); p.SetBool("NewHidden",true); p.SetBool("bPropagateToChildren",true); Call(o,"SetHiddenInGame",&p); }
        if(auto f=Fn(o,"SetVisibility")) { Params p(f); p.SetBool("bNewVisibility",false); p.SetBool("bPropagateToChildren",true); Call(o,"SetVisibility",&p); }
        if(auto f=Fn(o,"SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType",0); Call(o,"SetCollisionEnabled",&p); }
        BoolArg(o,"SetComponentTickEnabled","bEnabled",false);
    }
    if(processed.size()>20000) processed.clear();
}
void Fly(float dt) {
    if(!flying) return;
    if(overlay::NativeInput() || policy::GetState().menuActive || mapGroundPending) return;
    if(Field<uint8_t>(movement,0x198)!=0) { auto fn=Fn(movement,"SetMovementMode"); if(fn) { Params p(fn); p.Set<uint8_t>("NewMovementMode",0); Call(movement,"SetMovementMode",&p); } }
    if(!multiplayer) {
        if(!IsValid(editorCam)) StartEditorCamera();
        if(IsValid(editorCam)) { if(!panel::IsLoading() && !skins::CreatorOpen()) EditorCamera(dt); return; }
    }
    if(!Foreground() || Paused() || policy::GetState().menuActive || panel::IsLoading() || devmenu::IsOpen() || overlay::CursorMode()) return;
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
    // While a text field of the editor UI has the keyboard, editor hotkeys stay quiet; in cursor mode
    // clicks belong to the UI (editorui handles clicks into the world).
    if(overlay::Typing()) focus=false;
    const bool flyKey=Pressed(VK_F2),saveKey=Pressed(VK_F5),markKey=Pressed(VK_F6),returnKey=Pressed(VK_F8),slower=Pressed(VK_OEM_4),faster=Pressed(VK_OEM_6);
    const bool left=Pressed(VK_LBUTTON),right=Pressed(VK_RBUTTON);
    const bool mapKey=overlay::TakeEditorMapKey();
    // Joined players have no editor tools: the server owns time, weather and positions.
    if(focus && ready && !panel::IsLoading() && !multiplayer) {
        if(mapKey) {
            if(overlay::NativeInput()) policy::CloseMap(); else OpenEditorMap();
        }
        if(overlay::NativeInput() && menuKeys[5]) policy::CloseMap();
        if(devKey && !policy::GetState().menuActive && !Paused()) DGMP_SetDevMenuVisible(!devmenu::IsOpen());
        devmenu::Update(true);
        const bool wasDev=devmenu::IsOpen();
        if(wasDev) {
            for(int i=0;i<6;++i) if(menuKeys[i]) devmenu::Key(keys[i]);
        } else if(!overlay::NativeInput() && !mapGroundPending) {
            if(flyKey) SetFlying(!flying); if(saveKey) SavePoint(true); if(markKey) SavePoint(false); if(returnKey) ReturnSpawn();
            if(slower) speed=std::max(100.f,speed/2); if(faster) speed=std::min(40000.f,speed*2);
            if(flying && !overlay::CursorMode() && !policy::GetState().menuActive && !Paused()) {
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
    else if(!wcscmp(command,L"open_map") && ready) { if(multiplayer) policy::OpenMap(); else OpenEditorMap(); }
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
              << ",\"triggersDisabled\":" << triggers << ",\"aiHidden\":" << removedAI << ",\"aiGearHidden\":" << removedAIGear << ",\"errors\":" << errors << ",\"sequence\":" << commandSeq
             << ",\"nativeServicesDisabled\":" << ui.nativeServicesDisabled << ",\"blockedNativeInteractions\":" << ui.blockedNativeInteractions
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
        file << "]}\n";
    }
    MoveFileExW(tmp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING);
    wchar_t text[1024]; swprintf_s(text,L"DGMP v%hs | %hs | Fly: %ls | %.0f m/s\nX %.2f  Y %.2f  Z %.2f  Yaw %.1f\nF2 fly | WASD move | Space/Ctrl up/down | Shift boost\n[ / ] speed | F5 save spawn | F6 marker | F8 return\n%ls",dgmp::kVersion,stage.c_str(),flying?L"ON":L"OFF",speed/100,pos.X,pos.Y,pos.Z,rotation.Yaw,note.c_str());
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
    diagnostics::Phase("client-refresh");
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
            if(pendingArrival && !menu && IsValid(pawn) && travelSince && now-travelSince>30) AlignArrival(now);  // (stuck in the game's loading)
            stableSince=0;
            if(ready && menu) { ready=false; stage="main-menu"; }
        }
        if(requested && !ready && now-requestTime>90 && stage=="load-requested") { stage="load-timeout"; bootstrapComplete=true; panel::Loading(false,0,lang::Pick(L"Ошибка загрузки",L"Loading failed")); note=L"Auto load timed out; see logs/DGMP.log"; dg::Log("Bootstrap load timed out"); }
    }
    diagnostics::Phase("client-policy");
    policy::Update(pawn,controller,gameMode,flying || devmenu::IsOpen() || !ready || pendingArrival,revealMap,true,suppressStory);
    EditorMap(now);
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
    diagnostics::Phase("client-commands"); Commands(now);
    diagnostics::Phase("client-environment"); environment::Update(ready && !panel::IsLoading() && !Loading()); Status(now);
    const auto diagnosticPosition=Position(); diagnostics::Heartbeat(now,NumObjects(),diagnosticPosition.X,diagnosticPosition.Y,diagnosticPosition.Z);
    if(!multiplayer && ready && !Loading()) { editorui::Update(now,pawn,controller); mp::UpdateEditorPreview(now,pawn); }
    // Silent while the loading cover is up: no menu music or chatter from the template save.
    audio::Update(!bootstrapComplete || panel::IsLoading());
    devcmd::Update(pawn,controller,gameMode);
    if(ready && !Loading()) zombies::Update(now,dt,pawn);
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
    diagnostics::Phase("pre-tick-policy"); SafeInputPolicy();
    diagnostics::Phase("native-engine-tick"); originalTick(engine,dt,idle);
    diagnostics::Phase("client-update"); SafeUpdate(dt);
    diagnostics::Phase("native/background");
}
UObject* FindEngine() {
    for(int i=0,n=NumObjects();i<n;++i) { auto o=ObjectAt(i); if(o && !(o->ObjectFlags&0x10) && IsValid(o) && o->Class && GetName(o->Class)=="GameEngine") return o; } return nullptr;
}
}
void DGMPInitThread() {
    const auto logPath=fs::path(profile::Root())/L"logs"/L"DGMP.log";
    dg::LogOpen(logPath.string().c_str()); panel::Start();
    if(overlay::Install()) overlay::SetCover(&panel::DrawCover);  // the loading cover lives in the game's frames
    dg::Log("DGMP v%s loading; built %s %s; profile imports patched=%d",dgmp::kVersion,__DATE__,__TIME__,profile::PatchedCount());
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
    if(!multiplayer) diagnostics::Start(profile::Root(),GetPrivateProfileIntW(L"Editor",L"Diagnostics",1,(fs::path(profile::Root())/L"editor.ini").c_str())!=0);
    else if(GetPrivateProfileIntW(L"Development",L"Diagnostics",0,Ini().c_str())!=0) diagnostics::Start(profile::Root(),true);
    policy::SetEditorSession(!multiplayer);
    if(multiplayer) { autoFly=wantedFly=false; dg::Log("Multiplayer client session: editor tools disabled"); }
    UObject* engine=nullptr;
    for(int i=0;i<240 && !engine;++i) { Sleep(250); if(NumObjects()>1000) engine=FindEngine(); }
    if(!engine) { dg::Log("GameEngine not found"); panel::Text(L"DGMP: GameEngine not found"); return; }
    policy::DisableFocusPause(engine);
    auto vt=engine->VTable;
    if(reinterpret_cast<uintptr_t>(vt[60])-Base()!=0x1AC2A20 || reinterpret_cast<uintptr_t>(vt[73])-Base()!=0x288F730) { dg::Log("Engine vtable mismatch; no hook installed"); return; }
    memcpy(copiedVtable,vt,sizeof(copiedVtable)); originalTick=reinterpret_cast<TickFn>(vt[73]); copiedVtable[73]=reinterpret_cast<void*>(&Tick); engine->VTable=copiedVtable;
    dg::Log("DGMP engine tick installed; automatic bootstrap enabled=%d",autoLoad);
}
