#include "policy.h"
#include "log.h"
#include "devmenu.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ue;
namespace policy {
namespace {
State state;
bool editorActive=false,revealMap=true,onlyMap=true;
bool storyOff=true;
std::atomic<uint32_t> markerMask{(1u<<0)|(1u<<1)|(1u<<7)|(1u<<9)};
UObject* localPawn=nullptr;
UObject* menu=nullptr;
UObject* map=nullptr;
UObject* mapWidget=nullptr;
UObject* clearFogTexture=nullptr;
UObject* hudWidget=nullptr;
UObject* worldVO=nullptr;
UObject* saveGameMode=nullptr;
UObject* optionsMenu=nullptr;
UObject* weaponHud=nullptr;
std::map<int,int> configuredObjects;
struct MarkerBaseline { int serial; bool waypoint; };
std::map<int,MarkerBaseline> markerBaselines;
UObject* abilityController=nullptr;
bool abilitiesBlocked=false;
std::array<bool,7> previousAbilities{};
const std::array<const char*,7> abilityNames{{"FocusModeAbilityDisabled","BinocularsAbilityDisabled","DodgeAbilityDisabled","ItemPickupAbilityDisabled","CanUseVehicle","WeaponWheelDisabled","BikeAmmoInteractionDisabled"}};
double nextRefresh=0;
using EventFn=void(__fastcall*)(UObject*,UObject*,void*);
struct ByteArray { const uint8_t* data; int32_t num,max; };
struct Hook { UObject* object; int serial; EventFn original; std::array<void*,512> table; };
std::vector<std::unique_ptr<Hook>> hooks;
std::map<UObject*,Hook*> activeHooks;

UObject* Property(UObject* o,const char* name) {
    if(!IsValid(o)) return nullptr;
    for(auto cls=o->Class;cls;cls=SuperOf(cls)) if(auto p=FindProperty(cls,name)) return p;
    return nullptr;
}
template<typename T> T Read(UObject* o,const char* name,T fallback={}) {
    auto p=Property(o,name); return p?Field<T>(o,Field<int>(p,off::UProperty_Offset)):fallback;
}
template<typename T> bool Write(UObject* o,const char* name,T value) {
    auto p=Property(o,name); if(!p) return false;
    Field<T>(o,Field<int>(p,off::UProperty_Offset))=value; return true;
}
UObject* Instance(const char* name) {
    auto cls=FindClass(name); if(!cls) return nullptr;
    for(int i=0,n=NumObjects();i<n;++i) { auto o=ObjectAt(i); if(o && !(o->ObjectFlags&0x10) && IsValid(o) && IsA(o,cls)) return o; }
    return nullptr;
}
bool BoolCall(UObject* o,const char* function) {
    auto f=IsValid(o)?Fn(o,function):nullptr; if(!f) return false;
    Params p(f); Call(o,function,&p); return p.GetBool("ReturnValue");
}
bool Has(const std::string& value,const char* fragment) { return value.find(fragment)!=std::string::npos; }
bool CombatInput(const std::string& name) {
    if(name.rfind("InpActEvt_",0)!=0) return false;
    return Has(name,"WeaponFire") || Has(name,"Player_ManualAim") || Has(name,"Gamepad_RightTrigger") || Has(name,"Gamepad_LeftTrigger");
}
bool EditorInput(const std::string& name) {
    if(name.rfind("InpActEvt_",0)==0) {
        // Menu input belongs to BP_Menu3D; keep its navigation and pause/settings available.
        return !Has(name,"TogglePauseMenu") && !Has(name,"PauseMenu") && !Has(name,"UI_");
    }
    return name.rfind("InpAxisEvt_",0)==0 && (Has(name,"Weapon") || Has(name,"Binocular"));
}
bool NonMapInput(const std::string& name) {
    return Has(name,"InpActEvt_TogglePauseMenu_") && (Has(name,"Skills") || Has(name,"Inventory") || Has(name,"Storylines") || Has(name,"Injectors"));
}
void __fastcall Event(UObject* object,UObject* function,void* parameters) {
    auto it=activeHooks.find(object);
    const EventFn original=it!=activeHooks.end()?it->second->original:reinterpret_cast<EventFn>(Base()+0x1AC2A20);
    const std::string name=GetName(function);
    if(devmenu::IsOpen() && (object==localPawn || object==abilityController) && (name.rfind("InpActEvt_",0)==0 || name.rfind("InpAxisEvt_",0)==0)) return;
    const bool quickSave=Has(name,"QuickSave") || Has(name,"Quicksave");
    const bool saveMenu=object==optionsMenu && (name=="OnSaveGameSelected" || name=="OnFileSelectedForSave" || name=="PrepareGameStateForSave" || name=="SpawnOverwriteSavePopup" || name=="OnSaveConfirm" || name=="OnSaveConfirmPS5");
    const bool savePrompt=(object==localPawn || object==weaponHud) && (name=="DisplaySaveGameUI" || name=="DisplaySaveGameUIOnCheckpoint" || name=="DisplaySaveGameNotification" || quickSave);
    if(saveMenu || savePrompt || ((object==localPawn || object==abilityController) && quickSave)) {
        ++state.blockedSaveRequests;
        if(state.blockedSaveRequests<=8) dg::Log("Native save request blocked: %s",name.c_str());
        return;
    }
    if(object==saveGameMode && name=="SetSavingBlockedByBlueprint" && parameters) {
        if(auto p=FirstParam(function);p && GetName(p->Class)=="BoolProperty") {
            auto byte=static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset)+Field<uint8_t>(p,off::BoolProperty_ByteOffset);
            *byte|=Field<uint8_t>(p,off::BoolProperty_FieldMask);
        }
    }
    if(storyOff && IsAByName(object,"BendGameMissionTrigger")) {
        if(name=="DoAutoTracking" || name=="TimerCheckForTrackedJob" || name=="OnNewMissionOrJobTracked" || name=="UpdateTriggersWorldDisplay" || name=="MarkCloseToMissionTrigger") return;
        if(name=="MarkMissionAsActiveTracked" && parameters) {
            auto p=FindProperty(function,"Tracked");
            if(p && *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset))) return;
        }
    }
    if(storyOff && IsAByName(object,"BendJobManager") && (name=="UnPauseJob" || name=="ActivateJobAtIndex" || name=="ActivateJobTemp" || name=="CheckForAnyNewActiveJobs")) return;
    if(Has(name,"InpActEvt_PM_Hotkey") || name=="TogglePhotoMode") {
        ++state.blockedPhoto;
        if(state.blockedPhoto<=4) dg::Log("Photo mode event blocked: %s",name.c_str());
        return;
    }
    if(editorActive && (CombatInput(name) || name=="StartPrimaryFire" || name=="StartManualAimWeapon" || name=="StartManualAimThrowable" || name=="StartMeleeAttack")) {
        ++state.blockedCombat;
        if(state.blockedCombat<=8) dg::Log("Editor combat event blocked: %s",name.c_str());
        return;
    }
    if(editorActive && (object==localPawn || object==abilityController) && EditorInput(name)) {
        const bool action=name.rfind("InpActEvt_",0)==0;
        if(action) {
            ++state.blockedGameplay;
            static std::map<std::string,bool> logged;
            if(!logged[name]) { logged[name]=true; dg::Log("Editor gameplay input blocked: %s",name.c_str()); }
        }
        return;
    }
    if(storyOff && object==localPawn && (name=="EnteredInfestation" || name=="LeftInfestation" || name=="OnEnteredInfestation_Event_0" || name=="PlayInventoryVO" || name=="CheckAmmoCounts")) return;
    if(storyOff && object==hudWidget && (name=="DisplayObjective" || name=="AddObjectiveUnlocked" || name=="DisplayMissionInfoPopup" || name=="EnableMissionFailMessage")) return;
    if(onlyMap && object==menu && NonMapInput(name)) {
        ++state.blockedMenus; dg::Log("Non-map shortcut blocked: %s",name.c_str()); return;
    }
    if(onlyMap && object==menu && state.mapEnum>=0 && (name=="DisplayMenu" || name=="DisplayMenu_For3D")) {
        if(parameters) {
            auto p=FindProperty(function,name=="DisplayMenu"?"Menu":"MenuTypes");
            if(p) *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset))=uint8_t(state.mapEnum);
        }
    }
    original(object,function,parameters);
}
void Install(UObject* o) {
    if(!IsValid(o) || o->VTable[60]==reinterpret_cast<void*>(&Event)) return;
    const uintptr_t rva=reinterpret_cast<uintptr_t>(o->VTable[60])-Base();
    // AActor overrides UObject::ProcessEvent and forwards to it after its world checks.
    if(rva!=0x1AC2A20 && rva!=0x261E620) {
        static std::map<UObject*,int> rejected;
        const int serial=SerialAt(o->InternalIndex);
        if(rejected[o]!=serial) { rejected[o]=serial; dg::Log("Policy: unexpected ProcessEvent RVA=%llX on %s",static_cast<unsigned long long>(rva),GetName(o).c_str()); }
        return;
    }
    auto hook=std::make_unique<Hook>(); hook->object=o; hook->serial=SerialAt(o->InternalIndex);
    hook->original=reinterpret_cast<EventFn>(o->VTable[60]);
    memcpy(hook->table.data(),o->VTable,sizeof(void*)*hook->table.size());
    hook->table[60]=reinterpret_cast<void*>(&Event); o->VTable=hook->table.data();
    activeHooks[o]=hook.get(); hooks.push_back(std::move(hook));
    dg::Log("Policy ProcessEvent installed: %s",GetName(o).c_str());
}
int ResolveMapEnum() {
    auto fn=Fn(menu,"GetMenu"); if(!fn) return -1;
    // Ask the game's own menu lookup instead of hard-coding its cooked enum ordering.
    for(uint8_t value=0;value<16;++value) {
        Params p(fn); if(!p.Set("Menu",value)) return -1; Call(menu,"GetMenu",&p);
        auto widget=p.Get<UObject*>("UserWidget");
        if(IsValid(widget) && IsAByName(widget,"UI_MapMenuWidget3D_C")) {
            mapWidget=widget; dg::Log("Resolved map menu enum=%u from GetMenu",value); return value;
        }
    } return -1;
}
void Hide(UObject* widget) {
    if(!IsValid(widget)) return;
    auto fn=Fn(widget,"SetVisibility"); if(!fn) return;
    Params p(fn); p.Set<uint8_t>("InVisibility",1); Call(widget,"SetVisibility",&p); // Collapsed
    if(auto f=Fn(widget,"SetIsEnabled")) { Params q(f); q.SetBool("bInIsEnabled",false); Call(widget,"SetIsEnabled",&q); }
}
bool SaveOption(UObject* enumObject,uint8_t value) {
    if(!IsValid(enumObject) || GetName(enumObject->Class)!="Enum") return false;
    struct EnumItem { FName name; uint8_t value; uint8_t padding[7]; };
    struct EnumArray { EnumItem* data; int32_t num,max; };
    const auto names=Field<EnumArray>(enumObject,0x40);
    if(!names.data || names.num<1 || names.num>256) return false;
    for(int i=0;i<names.num;++i) if(names.data[i].value==value) {
        const auto name=NameOf(names.data[i].name);
        return Has(name,"::Save") || Has(name,"::QuickSave") || Has(name,"::Quicksave");
    }
    return false;
}
void ApplySavingPolicy(UObject* gameMode) {
    saveGameMode=gameMode;
    if(IsValid(gameMode)) {
        Install(gameMode);
        if(auto f=Fn(gameMode,"SetSavingBlockedByBlueprint")) {
            if(auto first=FirstParam(f);first && GetName(first->Class)=="BoolProperty") {
                Params p(f); p.SetBool(GetName(first).c_str(),true); Call(gameMode,"SetSavingBlockedByBlueprint",&p);
            }
        }
        SetBoolProp(gameMode,"bWantsOWSave",false);
        state.savingBlocked=BoolCall(gameMode,"IsSavingBlockedByBlueprint");
    }
    if(!IsValid(optionsMenu)) optionsMenu=Instance("OptionsTopMenuWidget_C");
    if(!IsValid(weaponHud)) weaponHud=Instance("UI_HudWeapon_C");
    state.hiddenSaveControls=0;
    if(IsValid(weaponHud)) {
        Install(weaponHud);
        for(const char* key:{"QuicksaveIcon","SaveLabel","SavePrompt"}) if(auto w=Read<UObject*>(weaponHud,key);IsValid(w)) { Hide(w); ++state.hiddenSaveControls; }
    }
    if(IsValid(optionsMenu)) {
        Install(optionsMenu);
        // Decode the native option enum rather than guessing a localized label
        // or a category index. The profile structure is from this build's SDK.
        auto structure=FindObject("/Script/BendGame.PlayerProfileSubMenuClass");
        auto type=structure?FindProperty(structure,"Type"):nullptr;
        auto enumeration=type?Field<UObject*>(type,0x70):nullptr;
        struct Array { uint8_t* data; int32_t num,max; };
        struct Widgets { UObject** data; int32_t num,max; };
        auto category=Read<UObject*>(optionsMenu,"OptionsCategoryWidget");
        const auto widgets=Read<Widgets>(category,"CategoryWidgets");
        for(const char* key:{"MenuProfiles","ActiveMenuProfile","MenuProfilesMain_savedOff"}) {
            const auto profiles=Read<Array>(optionsMenu,key);
            if(!profiles.data || profiles.num<0 || profiles.num>128) continue;
            for(int i=0;i<profiles.num;++i) if(SaveOption(enumeration,profiles.data[i*0x40])) {
                profiles.data[i*0x40+0x38]=1; // SelectionDetails.bDisabled
                if(std::string(key)=="ActiveMenuProfile" && widgets.data && widgets.num==profiles.num && IsValid(widgets.data[i])) {
                    Hide(widgets.data[i]); SetBoolProp(widgets.data[i],"bDisabled",true); ++state.hiddenSaveControls;
                }
            }
        }
    }
}
void HideMapControls() {
    if(!onlyMap || !IsValid(mapWidget)) return;
    unsigned int count=0;
    for(const char* name:{"MenuInteract_Skills","MenuInteract_Inventory","MenuInteract_Modifiers","MenuInteract_Storylines","MenuInteract_Storylines2","MenuInteract_Storylines3","MenuInteract_Storylines4","MenuInteract_Deacon","Mission_Box","Mission_Overlay","MissionInfoBox","SelectedStorylinePanel","Arcade_Mission_Box","ArcadeInteract_Modifiers","RegionEntryBorder","RegionStatusBorder","Region_Info","Region_Percent","Storyline_Overlay","StorylineAccent","StorylineBackground"}) {
        auto widget=Read<UObject*>(mapWidget,name); if(IsValid(widget)) { Hide(widget); ++count; }
    }
    state.hiddenControls=count;
}
void ApplyMenuPolicy() {
    if(!IsValid(menu)) return;
    if(state.mapEnum<0) state.mapEnum=ResolveMapEnum();
    if(!onlyMap || state.mapEnum<0) return;
    const uint8_t value=uint8_t(state.mapEnum);
    auto fn=Fn(menu,"SetAllowedMenus");
    if(fn) {
        ByteArray values{&value,1,1};
        Params p(fn); p.Set("Allowed Menus",values); Call(menu,"SetAllowedMenus",&p);
    }
    if(auto f=Fn(menu,"SetOverrideMenu")) { Params p(f); p.Set("Menu",value); Call(menu,"SetOverrideMenu",&p); }
    // All requests through selector, mouse navigation or remembered tab lead to the map.
    Write<uint8_t>(menu,"LastMenuEnum",value);
    auto allowed=Read<ByteArray>(menu,"AllowedMenus");
    state.mapOnly=allowed.num==1 && allowed.data && allowed.data[0]==value;
}
void ApplyFogMaterial() {
    state.fogMaterialOverridden=false;
    if(!revealMap || !IsValid(map)) return;
    auto material=Read<UObject*>(map,"MapMID");
    if(!IsValid(material)) return;
    if(!IsValid(clearFogTexture)) clearFogTexture=LoadAsset("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
    auto set=Fn(material,"SetTextureParameterValue"),get=Fn(material,"K2_GetTextureParameterValue");
    const FName name=FindName("FogOfWarSource");
    if(!IsValid(clearFogTexture) || !set || !get || name.Index<0) return;
    Params p(get); p.Set("ParameterName",name); Call(material,"K2_GetTextureParameterValue",&p);
    if(p.Get<UObject*>("ReturnValue")!=clearFogTexture) {
        Params q(set); q.Set("ParameterName",name); q.Set("Value",clearFogTexture); Call(material,"SetTextureParameterValue",&q);
        dg::Log("Map material FogOfWarSource overridden with %s",GetName(clearFogTexture).c_str());
    }
    Params verify(get); verify.Set("ParameterName",name); Call(material,"K2_GetTextureParameterValue",&verify);
    state.fogMaterialOverridden=verify.Get<UObject*>("ReturnValue")==clearFogTexture;
}
bool First(UObject* object) {
    const int serial=SerialAt(object->InternalIndex);
    if(auto it=configuredObjects.find(object->InternalIndex);it!=configuredObjects.end() && it->second==serial) return false;
    configuredObjects[object->InternalIndex]=serial; return true;
}
bool RuntimeObject(UObject* object) {
    if(!IsValid(object)) return false;
    for(auto o=object;o;o=o->Outer) {
        if(o->ObjectFlags&0x30) return false;
        const auto type=GetName(o->Class);
        if(type=="Class" || type=="BlueprintGeneratedClass" || type=="WidgetBlueprintGeneratedClass") return false;
    }
    return true;
}
void BoolArg(UObject* object,const char* fn,const char* parameter,bool value) {
    if(auto f=Fn(object,fn)) { Params p(f); p.SetBool(parameter,value); Call(object,fn,&p); }
}
bool MarkerAllowed(UObject* component) {
    const uint8_t type=Read<uint8_t>(component,"LocationType",255);
    if(type>=30 || !(markerMask.load()&(1u<<type))) return false;
    if(type==9 && storyOff && IsValid(map)) {
        auto owner=component->Outer;
        auto playerWaypoint=Read<UObject*>(map,"UserWaypointMarker");
        // Mission, fast travel and player waypoints share the same native category.
        return IsValid(playerWaypoint) && (owner==playerWaypoint || component==Read<UObject*>(playerWaypoint,"MiniMapLocationComponent"));
    }
    return true;
}
void SceneVisible(UObject* component,bool visible) {
    if(auto f=Fn(component,"SetVisibility")) { Params p(f); p.SetBool("bNewVisibility",visible); p.SetBool("bPropagateToChildren",true); Call(component,"SetVisibility",&p); }
    BoolArg(component,"SetHiddenInGame","NewHidden",!visible);
}
void SuppressPresentation() {
    auto miniClass=FindClass("MiniMapLocationComponent"),iconClass=FindClass("MapWidgetComponent");
    auto objectiveClass=FindClass("BendObjective"),triggerClass=FindClass("BendGameMissionTrigger"),jobMarkerClass=FindClass("BendJobMiniMapMarker"),jobScriptClass=FindClass("BendJobScript"),zoneClass=FindClass("BendNestingZoneActor");
    state.hiddenMarkers=state.visibleFuelMarkers=state.disabledZones=0;
    std::vector<UObject*> objects;
    for(int i=0,n=NumObjects();i<n;++i) {
        auto o=ObjectAt(i); if(!o || !IsValid(o)) continue;
        if((miniClass && IsA(o,miniClass)) || (iconClass && IsA(o,iconClass)) || (storyOff && ((objectiveClass && IsA(o,objectiveClass)) || (triggerClass && IsA(o,triggerClass)) || (jobMarkerClass && IsA(o,jobMarkerClass)) || (jobScriptClass && IsA(o,jobScriptClass)) || (zoneClass && IsA(o,zoneClass)))))
            if(RuntimeObject(o)) objects.push_back(o);
    }
    for(auto o:objects) {
        if(miniClass && IsA(o,miniClass)) {
            const bool visible=MarkerAllowed(o);
            const int serial=SerialAt(o->InternalIndex);
            auto baseline=markerBaselines.find(o->InternalIndex);
            if(baseline==markerBaselines.end() || baseline->second.serial!=serial)
                baseline=markerBaselines.insert_or_assign(o->InternalIndex,MarkerBaseline{serial,GetBoolProp(o,"bRenderWaypointMarker")}).first;
            BoolArg(o,"SetHudAndMapVisibility","Visible",visible);
            if(auto f=Fn(o,"SetVisibility")) { Params p(f); p.SetBool("VisibleOnMiniMap",visible); p.SetBool("VisibleOnMap",visible); Call(o,"SetVisibility",&p); }
            SetBoolProp(o,"bHidden",!visible);
            SetBoolProp(o,"bRenderWaypointMarker",visible && baseline->second.waypoint);
            if(!visible) { ++state.hiddenMarkers; }
            else if(Read<uint8_t>(o,"LocationType")==7) ++state.visibleFuelMarkers;
        } else if(iconClass && IsA(o,iconClass)) {
            auto location=Resolve(Read<FWeakObjectPtr>(o,"Location",{-1,0}));
            auto component=Read<UObject*>(location,"Component");
            if(!IsValid(component) && IsValid(location)) component=Resolve(Read<FWeakObjectPtr>(location,"mLocationComponent",{-1,0}));
            if(IsValid(component)) SceneVisible(o,MarkerAllowed(component));
        } else if((objectiveClass && IsA(o,objectiveClass)) || (triggerClass && IsA(o,triggerClass)) || (jobMarkerClass && IsA(o,jobMarkerClass))) {
            if(triggerClass && IsA(o,triggerClass)) {
                if(GetBoolProp(o,"CurrentlyTracked")) {
                    BoolArg(o,"MarkMissionAsActiveTracked","Tracked",false);
                    dg::Log("Native mission trigger untracked: %s",GetName(o).c_str());
                }
                SetBoolProp(o,"CurrentlyTracked",false);
                SetBoolProp(o,"IconShouldBeVisible",false);
                Install(o);
            }
            BoolArg(o,"SetActorHiddenInGame","bNewHidden",true);
            BoolArg(o,"SetActorEnableCollision","bNewActorEnableCollision",false);
            BoolArg(o,"SetActorTickEnabled","bEnabled",false);
            for(const char* key:{"InWorldIconComponent","MissionIcon","ObjectiveTriggerSphereComponent"}) if(auto c=Read<UObject*>(o,key);IsValid(c)) SceneVisible(c,false);
            if(auto w=Read<UObject*>(o,"InWorldIcon_CODE");IsValid(w)) Hide(w);
            if(auto w=Read<UObject*>(o,"InWorldIcon");IsValid(w)) Hide(w);
            if(auto w=Read<UObject*>(o,"InWorldJobIcon_CODE");IsValid(w)) Hide(w);
            if(jobMarkerClass && IsA(o,jobMarkerClass)) { SetBoolProp(o,"HasInWorldMarker",false); SetBoolProp(o,"IsCurrentlyActive",false); SetBoolProp(o,"JobHasFocus",false); }
        } else if(jobScriptClass && IsA(o,jobScriptClass)) {
            BoolArg(o,"SetActorTickEnabled","bEnabled",false);
            SetBoolProp(o,"FailDistanceCheckActive",false);
        } else if(zoneClass && IsA(o,zoneClass)) {
            ++state.disabledZones;
            BoolArg(o,"SetActorEnableCollision","bNewActorEnableCollision",false);
            BoolArg(o,"SetActorTickEnabled","bEnabled",false);
            if(auto c=Read<UObject*>(o,"AudioComponent");IsValid(c)) Call(c,"Stop");
        }
    }
    if(storyOff && IsValid(localPawn)) {
        BoolArg(localPawn,"SetCanPlayVO","CanPlayVO",false);
        BoolArg(localPawn,"SetCanPlayResponseVO","CanPlayResponseVO",false);
        state.playerVODisabled=true;
        if(auto f=Fn(localPawn,"GetVocalAudioComponent")) { Params p(f); Call(localPawn,"GetVocalAudioComponent",&p); if(auto a=p.Get<UObject*>("ReturnValue");IsValid(a)) Call(a,"Stop"); }
    }
    if(storyOff) {
        if(!IsValid(worldVO)) worldVO=Instance("BendOpenWorldVOManager");
        if(IsValid(worldVO)) {
            if(First(worldVO)) {
                if(auto f=Fn(worldVO,"ToggleOpenWorldVOManager")) { Params p(f); p.Set<int32_t>("Token",0x44474D50); p.SetBool("Pause",true); p.SetBool("InterruptVO",true); Call(worldVO,"ToggleOpenWorldVOManager",&p); }
                Call(worldVO,"DisableRFO");
            }
            SetBoolProp(worldVO,"ManagerPaused",true); SetBoolProp(worldVO,"SkipOWVOConvos",true);
            BoolArg(worldVO,"SetActorTickEnabled","bEnabled",false);
            state.openWorldVOPaused=GetBoolProp(worldVO,"ManagerPaused");
        }
        if(!IsValid(hudWidget)) {
            if(auto hud=Instance("BendHUD");IsValid(hud)) {
                auto slate=Read<UObject*>(hud,"SlateHUD"); hudWidget=Read<UObject*>(slate,"HudWidget");
            }
        }
        if(IsValid(hudWidget)) {
            Install(hudWidget);
            state.hiddenHudControls=0;
            for(const char* key:{"_Objectives","MissionPopup","NotifyBanner","NotifyCollectible","NotifyLevelReward","NotifyStandardWrapper","NotifyStoryline","SimpleTutorialOverlay","ObjCallout","XP"})
                if(auto w=Read<UObject*>(hudWidget,key);IsValid(w)) { Hide(w); ++state.hiddenHudControls; }
        }
    }
}
void ActorOff(UObject* actor) {
    if(!IsValid(actor)) return;
    BoolArg(actor,"SetActorHiddenInGame","bNewHidden",true);
    BoolArg(actor,"SetActorEnableCollision","bNewActorEnableCollision",false);
    BoolArg(actor,"SetActorTickEnabled","bEnabled",false);
}
void ClearTrackedRoute() {
    if(!storyOff) return;
    auto mission=Instance("BendGameMission"),jobs=Instance("BendJobManager");
    if(IsValid(mission)) Write<UObject*>(mission,"CurrentTrackedMission",nullptr);
    if(IsValid(jobs)) Write<UObject*>(jobs,"CurrentTrackedJob",nullptr);
    state.trackingCleared=IsValid(mission) && !Read<UObject*>(mission,"CurrentTrackedMission") && (!IsValid(jobs) || !Read<UObject*>(jobs,"CurrentTrackedJob"));
    if(IsValid(mapWidget)) Write<UObject*>(mapWidget,"TrackedMissionJobData",nullptr);
    if(IsValid(map)) {
        auto marker=Read<UObject*>(map,"MissionWaypointMarker");
        if(IsValid(marker)) {
            Call(marker,"ClearPath");
            BoolArg(marker,"SetPathVisible","Visible",false);
            ActorOff(marker);
            if(auto child=Read<UObject*>(marker,"DesignWaypointWidget");IsValid(child)) SceneVisible(child,false);
        }
        auto path=Read<UObject*>(map,"PathObjectiveWaypoint_BP");
        if(IsValid(path)) {
            // MapRoadProceduralPath stores its mesh independently of the waypoint marker.
            if(Fn(path,"ResetPath")) Call(path,"ResetPath");
            else if(Fn(path,"ClearPath")) Call(path,"ClearPath");
            BoolArg(path,"SetPathVisible","Visible",false); ActorOff(path);
        }
        if(auto spline=Read<UObject*>(map,"SplineObjectiveWaypoint_BP");IsValid(spline)) SceneVisible(spline,false);
    }
}
void PauseNativeJobs(UObject* gameMode) {
    if(!storyOff || !IsValid(gameMode) || GetBoolProp(gameMode,"bIsMainMenuActive")) return;
    auto mission=Instance("BendGameMission"),jobs=Instance("BendJobManager");
    if(!IsValid(mission) || !IsValid(jobs) || GetBoolProp(mission,"InPostSaveGameLoad")) return;
    if(GetBoolProp(gameMode,"bIsInLoadGame")) return;
    Install(jobs);
    struct ObjectArray { UObject** data; int32_t num,max; };
    auto active=Read<ObjectArray>(jobs,"ActiveJobRefsInOrder");
    if(active.num>0 && active.num<=1024) {
        // Use the manager's pause transition so scripts and their marker ownership
        // are updated together. Never mark jobs complete or grant story rewards.
        if(auto f=Fn(jobs,"PauseAllActiveJobsForMission")) {
            Params p(f); p.SetBool("Pause",true); Call(jobs,"PauseAllActiveJobsForMission",&p);
            dg::Log("Native jobs pause requested: before=%d after=%d success=%d",active.num,Read<ObjectArray>(jobs,"ActiveJobRefsInOrder").num,p.GetBool("ReturnValue"));
        }
    }
    active=Read<ObjectArray>(jobs,"ActiveJobRefsInOrder");
    const auto paused=Read<ObjectArray>(jobs,"PausedJobRefsInOrder");
    state.activeNativeJobs=active.num>=0 && active.num<=1024?unsigned(active.num):0;
    state.pausedNativeJobs=paused.num>=0 && paused.num<=1024?unsigned(paused.num):0;
    state.nativeMissionActive=GetBoolProp(mission,"IsMissionActive");
    BoolArg(jobs,"SetActorTickEnabled","bEnabled",false);
}
void CleanWorld() {
    if(!storyOff) return;
    const auto nestClass=FindClass("BendNest"),pieceClass=FindClass("FreakerNestPieceBase_C"),spawnClass=FindClass("BendNestSpawnLocation");
    const auto meshClass=FindClass("StaticMeshComponent"),blockerClass=FindClass("BendSplineBlocker"),restrictedClass=FindClass("XpecRestrictArea");
    const auto barricadeClass=FindClass("BP_AmbushZoneBarricade_MASTER_C"),doorClass=FindClass("BendDoor"),zoneClass=FindClass("BendNestingZone");
    auto material=Read<UObject*>(map,"MapMID");
    auto scalarFn=IsValid(material)?Fn(material,"SetScalarParameterValue"):nullptr;
    state.nestObjectsHidden=state.storyBlockersDisabled=state.doorsUnlocked=state.zoneParametersCleared=0;
    std::vector<UObject*> objects;
    for(int i=0,n=NumObjects();i<n;++i) {
        auto o=ObjectAt(i); if(!o || !IsValid(o)) continue;
        const bool zone=zoneClass && IsA(o,zoneClass) && !(o->ObjectFlags&0x10);
        if(zone || ((nestClass && IsA(o,nestClass)) || (pieceClass && IsA(o,pieceClass)) || (spawnClass && IsA(o,spawnClass)) || (meshClass && IsA(o,meshClass)) || (blockerClass && IsA(o,blockerClass)) || (restrictedClass && IsA(o,restrictedClass)) || (barricadeClass && IsA(o,barricadeClass)) || (doorClass && IsA(o,doorClass))))
            if(zone || RuntimeObject(o)) objects.push_back(o);
    }
    for(auto o:objects) {
        if(zoneClass && IsA(o,zoneClass)) {
            if(scalarFn) { Params p(scalarFn); p.Set("ParameterName",o->Name); p.Set<float>("Value",0); Call(material,"SetScalarParameterValue",&p); ++state.zoneParametersCleared; }
        } else if((nestClass && IsA(o,nestClass)) || (pieceClass && IsA(o,pieceClass)) || (spawnClass && IsA(o,spawnClass))) {
            ActorOff(o); ++state.nestObjectsHidden;
            for(const char* key:{"AmbientAudioComponent","FireAudioComponent"}) if(auto a=Read<UObject*>(o,key);IsValid(a)) Call(a,"Stop");
        } else if(meshClass && IsA(o,meshClass)) {
            auto mesh=Read<UObject*>(o,"StaticMesh");
            // Remove only nest meshes. A merged building containing a nest is still a building.
            if(IsValid(mesh) && Has(FullName(mesh),"/Game/Environments/Global/FreakerNests/")) {
                SceneVisible(o,false); ++state.nestObjectsHidden;
                if(auto f=Fn(o,"SetCollisionEnabled")) { Params p(f); p.Set<uint8_t>("NewType",0); Call(o,"SetCollisionEnabled",&p); }
            }
        } else if(blockerClass && IsA(o,blockerClass)) {
            if(GetBoolProp(o,"mBlocked")) BoolArg(o,"UnblockSegments","bAutoUpdate",true);
            SetBoolProp(o,"mBlocked",false); ++state.storyBlockersDisabled;
            if(First(o)) dg::Log("Story spline blocker opened: %s",GetName(o).c_str());
        } else if((restrictedClass && IsA(o,restrictedClass)) || (barricadeClass && IsA(o,barricadeClass))) {
            ActorOff(o); ++state.storyBlockersDisabled;
            if(First(o)) dg::Log("Story restriction removed: %s",GetName(o).c_str());
        } else if(doorClass && IsA(o,doorClass)) {
            // Keep ordinary doors in the world while removing their story lock flags.
            const bool locked=GetBoolProp(o,"LockedCW") || GetBoolProp(o,"LockedCCW");
            SetBoolProp(o,"LockedCW",false); SetBoolProp(o,"LockedCCW",false);
            if(locked && First(o)) dg::Log("Door lock removed: %s",GetName(o).c_str());
            ++state.doorsUnlocked;
        }
    }
    state.zoneOverlayHidden=state.zoneParametersCleared>0;
    ClearTrackedRoute();
}
void EditorAbilities(UObject* controller) {
    if(abilityController!=controller) { abilityController=controller; abilitiesBlocked=false; }
    if(!IsValid(controller)) return;
    if(editorActive && !abilitiesBlocked) {
        for(size_t i=0;i<abilityNames.size();++i) previousAbilities[i]=GetBoolProp(controller,abilityNames[i]);
        if(IsValid(localPawn)) { Call(localPawn,"DeactivateFocusMode_NEW"); Call(localPawn,"CloseWeaponWheel_Blueprint"); }
        abilitiesBlocked=true;
    }
    if(abilitiesBlocked) {
        for(size_t i=0;i<abilityNames.size();++i) SetBoolProp(controller,abilityNames[i],editorActive?(i==4?false:true):previousAbilities[i]);
        if(!editorActive) abilitiesBlocked=false;
    }
}
}
State GetState() { return state; }
void ConfigureMarkers(uint32_t mask) { markerMask.store(mask&0x3FFFFFFFu); }
void Update(UObject* pawn,UObject* controller,UObject* gameMode,bool editor,bool reveal,bool mapOnly,bool suppressStory) {
    editorActive=editor; revealMap=reveal; onlyMap=mapOnly; localPawn=pawn; storyOff=suppressStory;
    if(IsValid(gameMode)) SetBoolProp(gameMode,"bWantsOWSave",false);
    state.markerMask=markerMask.load();
    if(IsValid(controller)) {
        state.photoDisabled=SetBoolProp(controller,"AllowPhotoMode",false);
        Install(controller); Install(pawn);
        EditorAbilities(controller);
    }
    const double now=GetTickCount64()/1000.0;
    if(now>=nextRefresh) {
        nextRefresh=now+0.5;
        ApplySavingPolicy(gameMode);
        if(!IsValid(menu)) { menu=Instance("BP_Menu3D_C"); state.mapEnum=-1; }
        if(!IsValid(map)) map=Instance("BP_Map3D_C");
        if(!IsValid(mapWidget) && IsValid(menu)) mapWidget=Read<UObject*>(menu,"Menu_Base_Map");
        if(IsValid(menu)) { Install(menu); ApplyMenuPolicy(); }
        if(IsValid(map)) {
            const bool wasFog=GetBoolProp(map,"bDisableFog");
            SetBoolProp(map,"bDisableFog",reveal);
            SetBoolProp(map,"DisableFogOfWarBP_BP",reveal);
            if(reveal) {
                if(auto fn=Fn(map,"SetActiveRegions")) { Params p(fn); p.SetBool("Top",true); p.SetBool("Middle",true); p.SetBool("Bottom",true); Call(map,"SetActiveRegions",&p); }
            }
            if(wasFog!=reveal || state.mapRevealed!=reveal) { Call(map,"UpdateFogOfWar"); dg::Log("Map fog disabled=%d; all three map regions enabled=%d",reveal,reveal); }
            state.mapRevealed=GetBoolProp(map,"bDisableFog") && GetBoolProp(map,"DisableFogOfWarBP_BP");
            state.allRegions=GetBoolProp(map,"TopRegionActive_BP") && GetBoolProp(map,"MiddleRegionActive_BP") && GetBoolProp(map,"BottomRegionActive_BP");
            ApplyFogMaterial();
            if(storyOff) {
                auto material=Read<UObject*>(map,"MapMID");
                if(auto f=IsValid(material)?Fn(material,"SetScalarParameterValue"):nullptr) {
                    const auto name=FindName("HatchOpacity");
                    if(name.Index>=0) { Params p(f); p.Set("ParameterName",name); p.Set<float>("Value",0); Call(material,"SetScalarParameterValue",&p); state.zoneOverlayHidden=true; }
                }
            }
        }
        PauseNativeJobs(gameMode);
        SuppressPresentation();
        CleanWorld();
    }
    if(IsValid(menu)) state.menuActive=BoolCall(menu,"IsMenuActive");
    if(!onlyMap) state.mapOnly=false;
    if(state.menuActive) HideMapControls();
    if(IsValid(gameMode) && reveal) for(const char* key:{"CascadeLocked","BelknapLocked","LostLakeLocked","CraterLakeLocked"}) SetBoolProp(gameMode,key,false);
}
void OpenMap() {
    if(!IsValid(menu) || state.mapEnum<0 || state.menuActive) return;
    if(auto f=Fn(menu,"DisplayMenu")) { Params p(f); p.Set<uint8_t>("Menu",uint8_t(state.mapEnum)); p.SetBool("NoAnimation",true); Call(menu,"DisplayMenu",&p); }
}
void CloseMap() { if(IsValid(menu) && state.menuActive) Call(menu,"RequestClose"); }
void TestEvent(const char* event) {
    if(!IsValid(localPawn)) return;
    UObject* f=nullptr;
    for(auto cls=localPawn->Class;cls && !f;cls=SuperOf(cls)) for(auto child=Field<UObject*>(cls,off::UStruct_Children);child;child=Field<UObject*>(child,off::UField_Next)) {
        const std::string name=GetName(child);
        if(GetName(child->Class)=="Function" && name.rfind("InpActEvt_",0)==0 && Has(name,event)) { f=child; break; }
    }
    if(f) { Params p(f); reinterpret_cast<EventFn>(localPawn->VTable[60])(localPawn,f,p.Data()); }
}
extern "C" __declspec(dllexport) int DGMP_SetMapCategoryVisible(uint32_t category,int visible) {
    if(category>=30 || (visible!=0 && visible!=1)) return 0;
    if(visible) markerMask.fetch_or(1u<<category); else markerMask.fetch_and(~(1u<<category));
    return 1;
}
extern "C" __declspec(dllexport) uint32_t DGMP_GetMapMarkerMask() { return markerMask.load(); }
}
