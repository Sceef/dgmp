#include "policy.h"
#include "overlay.h"
#include "lang.h"
#include "log.h"
#include "devmenu.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

using namespace ue;
namespace policy {
namespace {
State state;
bool editorActive=false,revealMap=true,onlyMap=true;
bool editorSession=false;  // single-player editor (SetEditorSession)
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
UObject* weaponHud=nullptr;
UObject* dlcUi=nullptr;
double rewardWatchUntil=0,rewardStuckSince=0;  // WatchRewardScreens
bool rewardLastPaused=false,rewardLastMenu=false;
UObject* rewardsPanel=nullptr;
double rewardAutoConfirmUntil=0;
bool playerDied=false;  // TakeDeath
double deathLogUntil=0;
bool blockDeathReload=false,deathReloadBlocked=false;
bool carryInputBlock=false, carryRollBlock=false;  // SetCarryInputBlock, SetCarryRollBlock
// Input actions of the controller that holster the item in the hands (InpActEvt_<prefix>...).
bool HolstersCarriedItem(const std::string& name) {
    if(name.rfind("InpActEvt_",0)!=0) return false;
    static const char* kPrefixes[]={"Player_ManualAim","Player_SelectWeapon","Player_WeaponSwap","WeaponWheel","UseBinoculars","BinocularTagEnemies",
                                    "QuickUseMedkit","Reload","Player_Detonator","Weapon_Zoom","Gamepad_LeftTrigger"};
    for(const char* p:kPrefixes) if(name.compare(10,std::strlen(p),p)==0) return true;
    return false;
}
bool interceptLethal=false,lethalHit=false;  // SetInterceptLethalHits / TakeLethalHit  // diagnostics: what the game does after a death
std::set<UObject*> takeable;  // SetTakeable
void(*observer)(UObject*,const std::string&)=nullptr; std::set<UObject*> observed;  // SetObserver / Observe
std::set<UObject*> damageWatched; DamageWatcher damageWatcher=nullptr;
std::set<UObject*> traceObjects; std::set<std::string> traceSeen; double traceUntil=0;  // TraceEvents  // _Objectives.RewardsPanel (UI_HudJob_C), hooked while a quest reward is shown
bool questHud=false;  // a server quest owns the objective HUD (SetQuestHud)
UObject* pauseControls=nullptr;
// Every OptionsTopMenuWidget_C is tracked: the game builds one per pause from several owners
// (Pause_Controls, Menu3D/Map3D, mission fades, native controller), not only Pause_Controls.
UObject* optionsClass=nullptr;
UObject* optionsDefaults=nullptr;
struct PauseMenu { UObject* object; int serial; ULONGLONG seen; };
std::vector<PauseMenu> pauseMenus;
UObject* closeMenu=nullptr;
int closeSerial=0;
std::array<bool,256> blockedPauseOptions{};
uint8_t returnOption=0; // Menu_QuitToMainMenu, relabelled "return to game"
bool pauseOptionsResolved=false;
std::string activateEvent; // the menu's handler bound to OptionsCategoryWidget.OnCategoryActivated
struct TextValue { uint8_t bytes[0x18]; };
std::map<UObject*,int> ownTutorials;  // tutorial windows DGMP shows on purpose (server hints)
struct Tip { int serial; ULONGLONG closeAt; };
std::map<UObject*,Tip> ownTips;       // survival-tip lines: no input, closed by timer
bool AnyOwnTutorial();
bool OwnTutorial(UObject* w) {
    if(!IsValid(w)) return false;
    if(auto it=ownTutorials.find(w);it!=ownTutorials.end() && it->second==SerialAt(w->InternalIndex)) return true;
    auto tip=ownTips.find(w); return tip!=ownTips.end() && tip->second.serial==SerialAt(w->InternalIndex);
}
TextValue returnText{};
bool returnTextReady=false;
std::vector<const void*> returnLabels; // FTextData of every "return to game" text we made
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
struct Hook { UObject* object; int32_t index; int serial; EventFn original; std::array<void*,512> table; };
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
UObject* Instance(const char* name) { return FindRuntimeInstance(name); }
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
int ProfileAt(UObject* optionsMenu,int index) {
    struct Array { uint8_t* data; int32_t num,max; };
    const auto profiles=Read<Array>(optionsMenu,"ActiveMenuProfile");
    return profiles.data && index>=0 && index<profiles.num && profiles.num<=128?profiles.data[index*0x40+1]:-1;
}
void __fastcall Event(UObject* object,UObject* function,void* parameters) {
    auto it=activeHooks.find(object);
    const EventFn original=it!=activeHooks.end()?it->second->original:reinterpret_cast<EventFn>(Base()+0x1AC2A20);
    const std::string name=GetName(function);
    if(devmenu::IsOpen() && (object==localPawn || object==abilityController) && (name.rfind("InpActEvt_",0)==0 || name.rfind("InpAxisEvt_",0)==0)) return;
    if(carryInputBlock && object==abilityController && HolstersCarriedItem(name)) return;
    if(carryRollBlock && object==abilityController && name.rfind("InpActEvt_Player_Roll",0)==0) return;
    // Server quest rewards: the storylines shortcut on the reward panel does nothing (see UpdateRewardPanel).
    if(object==rewardsPanel && GetTickCount64()/1000.0<rewardWatchUntil) {
        if(name=="OnSelect1Pressed" || name.rfind("BndEvt__MenuInteract_Triangle",0)==0) return;
        // "Continue": the next page is an empty "storyline updated"; keep continuing until it closes.
        if(name=="OnConfirmPressed" || name.rfind("BndEvt__MenuInteract_Confirm",0)==0) rewardAutoConfirmUntil=GetTickCount64()/1000.0+2;
    }
    if(observer && observed.count(object)) observer(object,name);
    if(damageWatcher && parameters && damageWatched.count(object) && (name=="ReceivePointDamage" || name=="ReceiveRadialDamage")) {
        auto at=[&](const char* field) -> uint8_t* { auto p=FindProperty(function,field); return p?static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset):nullptr; };
        auto obj=[&](const char* field) -> UObject* { auto p=at(field); return p?*reinterpret_cast<UObject**>(p):nullptr; };
        const float damage=at("Damage")?*reinterpret_cast<float*>(at("Damage")):at("DamageReceived")?*reinterpret_cast<float*>(at("DamageReceived")):0.f;
        UObject* instigator=name=="ReceivePointDamage"?obj("InstigatedBy"):obj("InstigatedBy");
        damageWatcher(object,name=="ReceivePointDamage"?obj("HitComponent"):nullptr,damage,instigator,obj("DamageCauser"));
    }
    // The editor keeps running when its window loses focus (alt-tab to notes, the launcher...).
    if(editorSession && name=="PauseOnFocusLost") { static int n=0; if(n++<3) dg::Log("Editor: pause on focus loss skipped"); return; }
    // Multiplayer death: no checkpoint reload (it rolls the world and the backpack back to the save);
    // the client brings the player back itself (mp RespawnInPlace).
    if(blockDeathReload && name=="PlayerDeathStartFadeToBlack" && IsAByName(object,"BendGameMode")) { dg::Log("Death reload blocked"); deathReloadBlocked=true; return; }
    // Every hit on the player goes through ModifyDamage (its return value is the damage dealt).
    if(interceptLethal && object==localPawn && name=="ModifyDamage" && parameters) {
        original(object,function,parameters);
        if(auto ret=FindProperty(function,"ReturnValue")) {
            float& damage=*reinterpret_cast<float*>(static_cast<uint8_t*>(parameters)+Field<int>(ret,off::UProperty_Offset));
            const float health=Field<float>(object,0x1FD4);  // BendPawn health
            if(damage>0 && damage>=health-0.5f) { dg::Log("Lethal hit cancelled: %.1f (health %.1f)",damage,health); damage=0; lethalHit=true; }
        }
        return;
    }
    if(object==localPawn && name=="OnDied_Event") { playerDied=true; deathLogUntil=GetTickCount64()/1000.0+30; dg::Log("Player died"); }
    if(deathLogUntil && GetTickCount64()/1000.0<deathLogUntil && name.find("Tick")==std::string::npos && name.rfind("InpAxisEvt_",0)!=0 && name.rfind("EvaluateGraph",0)!=0) {
        static std::set<std::string> seen;
        const std::string key=GetName(object->Class)+"::"+name;
        if(seen.insert(key).second) dg::Log("DEATH %s",key.c_str());
    }
    // Quest pickups: the "backpack full" checks answer "room left", so the game offers "take".
    if(parameters && takeable.count(object) && (name=="IsInventoryFull" || name=="LastCheckWasFullOnObject" || name=="IsPlayerFullOnAmmo" || name=="CanPickupItem")) {
        original(object,function,parameters);
        if(auto p=FindProperty(function,"ReturnValue")) {
            auto byte=static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset)+Field<uint8_t>(p,off::BoolProperty_ByteOffset);
            const uint8_t mask=Field<uint8_t>(p,off::BoolProperty_FieldMask);
            if(name=="CanPickupItem") *byte|=mask; else *byte&=uint8_t(~mask);
        }
        return;
    }
    if(traceUntil && traceObjects.count(object)) {
        if(GetTickCount64()/1000.0>traceUntil) { traceUntil=0; dg::Log("TRACE end (%zu distinct)",traceSeen.size()); }
        else if(name.rfind("InpAxisEvt_",0)!=0 && name.find("Tick")==std::string::npos && name.rfind("ExecuteUbergraph",0)!=0) {
            const std::string key=GetName(object->Class)+"::"+name;
            if(traceSeen.insert(key).second) dg::Log("TRACE %s",key.c_str());
        }
    }
    const bool quickSave=Has(name,"QuickSave") || Has(name,"Quicksave");
    const bool pauseMenu=optionsClass && object->Class==optionsClass;
    if(pauseMenu) {
        const bool forbidden=name=="OnLoadGameSelected" || name=="OnFileSelectedForLoad" || name=="OnFileSelectedForCriticalLoad" || name=="OnPhotoModeSelected";
        // The quit-to-main-menu entry is shown as "return to game": every path into it only closes the menu.
        bool back=name=="OnQuitSelected" || name=="QuitToMainMenu" || name=="QuitToMainMenu_ForChallengeMenu" || name=="QuitToMainMenu_ForArcadeMenu";
        int option=-1;
        if(name=="HandleSelectedProfileOption" && parameters) if(auto p=FindProperty(function,"ProfileOption"))
            option=Field<uint8_t>(parameters,Field<int>(p,off::UProperty_Offset));
        if(!activateEvent.empty() && name==activateEvent && parameters) if(auto p=FindProperty(function,"Index"))
            option=ProfileAt(object,Field<int>(parameters,Field<int>(p,off::UProperty_Offset)));
        const bool forbiddenSelection=option>=0 && blockedPauseOptions[option];
        back=back || (option>=0 && pauseOptionsResolved && option==returnOption);
        if(back) {
            closeMenu=object; closeSerial=SerialAt(object->InternalIndex); // closed from Update, outside the menu's own handler
            static unsigned int logged=0; if(++logged<=8) dg::Log("Pause menu return-to-game requested: %s",name.c_str());
            return;
        }
        if(forbidden || forbiddenSelection) {
            ++state.blockedPauseRequests;
            if(state.blockedPauseRequests<=8) dg::Log("Native pause option blocked: %s",name.c_str());
            return;
        }
    }
    const bool saveMenu=pauseMenu && (name=="OnSaveGameSelected" || name=="OnFileSelectedForSave" || name=="PrepareGameStateForSave" || name=="SpawnOverwriteSavePopup" || name=="OnSaveConfirm" || name=="OnSaveConfirmPS5");
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
    if((editorActive || overlay::CursorMode()) && (CombatInput(name) || name=="StartPrimaryFire" || name=="StartManualAimWeapon" || name=="StartManualAimThrowable" || name=="StartMeleeAttack")) {
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
    // While a server hint (our own panel) is open the game must see tutorials enabled: that is what
    // gives the panel its pause and close-button input.
    if(storyOff && object==hudWidget && name=="AreTutorialsDisabled" && parameters && !AnyOwnTutorial()) {
        if(auto p=FindProperty(function,"ReturnValue")) {
            auto byte=static_cast<uint8_t*>(parameters)+Field<int>(p,off::UProperty_Offset)+Field<uint8_t>(p,off::BoolProperty_ByteOffset);
            *byte|=Field<uint8_t>(p,off::BoolProperty_FieldMask);
        }
        return;
    }
    // Image popups (infestation zones etc.) are "multi" tutorials registered on the HUD; the
    // DLC UI manager exposes the same interface. Dropping the request also skips their pause.
    if(storyOff && (object==hudWidget || object==dlcUi) && (name=="DisplaySimpleTutorial" || name=="ProcessSimpleTutorialQueue"
        || name=="SpawnSimpleTutorial" || name=="I_SpawnSimpleTutorial" || name=="RegisterTutorialMulti" || name=="I_RegisterTutorialMulti")
        && !(parameters && (name=="RegisterTutorialMulti" || name=="I_RegisterTutorialMulti") && OwnTutorial(*static_cast<UObject**>(parameters)))) {
        ++state.blockedTutorials;
        if(state.blockedTutorials<=8) dg::Log("Native tutorial blocked: %s",name.c_str());
        return;
    }
    // The mission popup (giver, cash reward) reads mission data a server quest does not have.
    if(storyOff && object==hudWidget && name=="DisplayMissionInfoPopup") return;
    if(storyOff && !questHud && object==hudWidget && (name=="DisplayObjective" || name=="AddObjectiveUnlocked" || name=="EnableMissionFailMessage")) return;
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
void Install(UObject* o,bool quiet=false) {
    if(!IsValid(o) || o->VTable[60]==reinterpret_cast<void*>(&Event)) return;
    const uintptr_t rva=reinterpret_cast<uintptr_t>(o->VTable[60])-Base();
    // AActor overrides UObject::ProcessEvent and forwards to it after its world checks.
    if(rva!=0x1AC2A20 && rva!=0x261E620) {
        static std::map<UObject*,int> rejected;
        const int serial=SerialAt(o->InternalIndex);
        if(rejected[o]!=serial) { rejected[o]=serial; dg::Log("Policy: unexpected ProcessEvent RVA=%llX on %s",static_cast<unsigned long long>(rva),GetName(o).c_str()); }
        return;
    }
    auto hook=std::make_unique<Hook>(); hook->object=o; hook->index=o->InternalIndex; hook->serial=SerialAt(o->InternalIndex);
    hook->original=reinterpret_cast<EventFn>(o->VTable[60]);
    memcpy(hook->table.data(),o->VTable,sizeof(void*)*hook->table.size());
    hook->table[60]=reinterpret_cast<void*>(&Event); o->VTable=hook->table.data();
    activeHooks[o]=hook.get(); hooks.push_back(std::move(hook));
    if(!quiet) dg::Log("Policy ProcessEvent installed: %s",GetName(o).c_str());
}
int ResolveMapEnum() {
    auto fn=Fn(menu,"GetMenu"); if(!fn) return -1;
    // Ask the game's own menu lookup instead of hard-coding its cooked enum ordering.
    for(uint8_t value=0;value<16;++value) {
        Params p(fn); if(!p.Set("Menu",value)) return -1; Call(menu,"GetMenu",&p);
        auto widget=p.Get<UObject*>("UserWidget");
        if(IsValid(widget) && IsAByName(widget,"UI_MapMenuWidget3D_C")) {
            dg::Log("Resolved map menu enum=%u from GetMenu",value); return value;
        }
    } return -1;
}
void Hide(UObject* widget) {
    if(!IsValid(widget)) return;
    if(Read<uint8_t>(widget,"Visibility",255)==1 && !GetBoolProp(widget,"bIsEnabled")) return;
    auto fn=Fn(widget,"SetVisibility"); if(!fn) return;
    Params p(fn); p.Set<uint8_t>("InVisibility",1); Call(widget,"SetVisibility",&p); // Collapsed
    if(auto f=Fn(widget,"SetIsEnabled")) { Params q(f); q.SetBool("bInIsEnabled",false); Call(widget,"SetIsEnabled",&q); }
}
void ResolvePauseOptions() {
    if(pauseOptionsResolved) return;
    auto structure=FindObject("/Script/BendGame.PlayerProfileSubMenuClass");
    auto property=structure?FindProperty(structure,"ProfileOption"):nullptr;
    auto enumObject=property?Field<UObject*>(property,0x78):nullptr;
    if(!IsValid(enumObject) || GetName(enumObject->Class)!="Enum") return;
    struct EnumItem { FName name; uint8_t value; uint8_t padding[7]; };
    struct EnumArray { EnumItem* data; int32_t num,max; };
    const auto names=Field<EnumArray>(enumObject,0x40);
    if(!names.data || names.num<1 || names.num>256) return;
    unsigned int found=0;
    for(int i=0;i<names.num;++i) {
        const auto name=NameOf(names.data[i].name);
        if(name=="EPlayerSaveProfileType::Menu_SaveGame" || name=="EPlayerSaveProfileType::Menu_LoadGame" || name=="EPlayerSaveProfileType::Menu_PhotoMode") {
            blockedPauseOptions[names.data[i].value]=true; ++found;
        } else if(name=="EPlayerSaveProfileType::Menu_QuitToMainMenu") { returnOption=names.data[i].value; ++found; }
    }
    pauseOptionsResolved=found==4;
    if(pauseOptionsResolved) dg::Log("Resolved three forbidden native pause options and the return-to-game option");
}
bool MakeReturnText(TextValue& out) {
    static unsigned int attempts=0;
    if(attempts>=8) return false; // FindObject rescans the whole object array on a miss
    auto library=FindObject("/Script/Engine.Default__KismetTextLibrary");
    auto f=library?Fn(library,"Conv_StringToText"):nullptr;
    if(!f) { if(++attempts==8) dg::Log("Pause menu: Conv_StringToText unavailable; return entry keeps its native label"); return false; }
    // In the game's own menu: the game's interface language (its culture), not the launcher's.
    bool russian=!lang::English();
    if(auto intl=FindObject("/Script/Engine.Default__KismetInternationalizationLibrary")) if(auto c=Fn(intl,"GetCurrentCulture")) {
        Params q(c); Call(intl,"GetCurrentCulture",&q);
        const FString culture=q.Get<FString>("ReturnValue");  // the engine's string; left to it (read once)
        if(culture.Data && culture.Num>=3) russian=(culture.Data[0]==L'r' || culture.Data[0]==L'R') && (culture.Data[1]==L'u' || culture.Data[1]==L'U');
    }
    const wchar_t* label=russian?L"Вернуться в игру":L"Return to game"; const int32_t len=int32_t(wcslen(label)+1);
    Params p(f); if(!p.Set("InString",FString{label,len,len})) { attempts=8; return false; }
    Call(library,"Conv_StringToText",&p);
    // The parameter frame is never destructed, so the returned reference now belongs to `out`.
    out=p.Get<TextValue>("ReturnValue");
    const auto data=Field<const void*>(&out,0);
    if(data) returnLabels.push_back(data);
    return data!=nullptr;
}
bool ReturnLabel(const void* data) { return data && std::find(returnLabels.begin(),returnLabels.end(),data)!=returnLabels.end(); }
void PatchPauseDefaults() {
    // New menus copy their entry tables from the class defaults: relabel quit-to-main-menu there once.
    if(!IsValid(optionsDefaults)) return;
    ResolvePauseOptions(); if(!pauseOptionsResolved) return;
    if(!returnTextReady) returnTextReady=MakeReturnText(returnText);
    if(!returnTextReady) return;
    struct Array { uint8_t* data; int32_t num,max; };
    for(const char* key:{"MenuProfiles","MenuProfilesMain_savedOff","MenuProfiles_ResettingJobs"}) {
        const auto list=Read<Array>(optionsDefaults,key);
        if(!list.data || list.num<1 || list.num>128) continue;
        for(int i=0;i<list.num;++i) if(list.data[i*0x40+1]==returnOption) {
            auto& text=*reinterpret_cast<TextValue*>(list.data+i*0x40+8);
            if(!ReturnLabel(Field<const void*>(&text,0))) {
                // A separate text per slot: the defaults own its only reference, the old label leaks.
                TextValue fresh{}; if(!MakeReturnText(fresh)) return;
                text=fresh; dg::Log("Pause menu defaults: %s[%d] relabelled to return to game",key,i);
            }
            if(i>0) {
                // "Return to game" goes first. Entries only move inside the array, so ownership is unchanged.
                uint8_t entry[0x40]; std::memcpy(entry,list.data+i*0x40,0x40);
                std::memmove(list.data+0x40,list.data,size_t(i)*0x40);
                std::memcpy(list.data,entry,0x40);
                dg::Log("Pause menu defaults: %s return entry moved from %d to the top",key,i);
            }
            break;
        }
    }
}
void FilterPauseMenu(UObject* optionsMenu) {
    ResolvePauseOptions();
    if(!pauseOptionsResolved || !IsValid(optionsMenu)) return;
    struct Array { uint8_t* data; int32_t num,max; };
    struct Widgets { UObject** data; int32_t num,max; };
    auto category=Read<UObject*>(optionsMenu,"OptionsCategoryWidget");
    auto profiles=Read<Array>(optionsMenu,"ActiveMenuProfile");
    auto widgets=Read<Widgets>(category,"CategoryWidgets");
    // This build's native layout is PlayerProfileSubMenuClass (64 bytes), with ProfileOption at +1.
    // Remove matching UI entries and profile entries together so category indices stay aligned.
    const bool aligned=profiles.data && profiles.num>0 && profiles.num<=128 && widgets.data
        && widgets.num>=profiles.num && widgets.num<=128 && Read<int>(category,"MaxCategoryIndex")==profiles.num;
    bool changed=false;
    if(aligned) for(int i=profiles.num-1;i>=0;--i) if(blockedPauseOptions[profiles.data[i*0x40+1]]) {
        auto widget=widgets.data[i];
        profiles.data[i*0x40+0x38]=1;
        if(IsValid(widget)) { Hide(widget); SetBoolProp(widget,"bDisabled",true); }
        if(RemoveArrayEntry(optionsMenu,"ActiveMenuProfile",i)) {
            if(IsValid(widget)) Call(widget,"RemoveFromParent");
            if(!RemoveArrayEntry(category,"CategoryWidgets",i)) {
                dg::Log("Pause category removal failed at index %d",i); break;
            }
            widgets=Read<Widgets>(category,"CategoryWidgets");
            profiles=Read<Array>(optionsMenu,"ActiveMenuProfile");
            ++state.removedPauseControls; changed=true;
        }
    }
    if(changed) {
        Write<int>(category,"MaxCategoryIndex",profiles.num);
        for(int i=0;i<profiles.num;++i) if(IsValid(widgets.data[i])) Write<int>(widgets.data[i],"CursorPosition",i);
        if(auto f=Fn(category,"SetCategory")) { Params p(f); p.Set<int>("Index",0); Call(category,"SetCategory",&p); }
        dg::Log("Pause menu filtered: remaining=%d; removed=%u",profiles.num,state.removedPauseControls);
    }
    for(const char* key:{"MenuProfiles","MenuProfilesMain_savedOff","MenuProfiles_ResettingJobs"}) {
        auto source=Read<Array>(optionsMenu,key);
        if(!source.data || source.num<1 || source.num>128) continue;
        for(int i=source.num-1;i>=0;--i) if(blockedPauseOptions[source.data[i*0x40+1]]) {
            source.data[i*0x40+0x38]=1;
            RemoveArrayEntry(optionsMenu,key,i);
            source=Read<Array>(optionsMenu,key);
        }
    }
    // If a future menu layout cannot be matched safely, keep the forbidden entries disabled and collapsed.
    profiles=Read<Array>(optionsMenu,"ActiveMenuProfile"); widgets=Read<Widgets>(category,"CategoryWidgets");
    if(profiles.data && profiles.num>=0 && profiles.num<=128) for(int i=0;i<profiles.num;++i)
        if(blockedPauseOptions[profiles.data[i*0x40+1]]) {
            profiles.data[i*0x40+0x38]=1;
            if(widgets.data && i<widgets.num && IsValid(widgets.data[i])) { Hide(widgets.data[i]); SetBoolProp(widgets.data[i],"bDisabled",true); }
        }
    // Entry widgets carry their own copy of the label (menus built before the defaults were patched).
    const bool labelsAligned=profiles.data && profiles.num>0 && profiles.num<=128 && widgets.data && widgets.num>=profiles.num
        && widgets.num<=128 && Read<int>(category,"MaxCategoryIndex")==profiles.num;
    if(returnTextReady && labelsAligned) for(int i=0;i<profiles.num;++i)
        if(profiles.data[i*0x40+1]==returnOption && IsValid(widgets.data[i]) && !ReturnLabel(Read<const void*>(widgets.data[i],"DisplayText")))
            if(auto f=Fn(widgets.data[i],"SetText")) { Params p(f); if(p.Set("Text",returnText)) Call(widgets.data[i],"SetText",&p); }
    // Find which bound handler means "entry activated" (the other one is cursor movement).
    if(activateEvent.empty() && IsValid(category)) {
        struct Bound { FWeakObjectPtr object; FName function; };
        struct Delegates { Bound* data; int32_t num,max; };
        const auto bound=Read<Delegates>(category,"OnCategoryActivated");
        if(bound.data && bound.num>0 && bound.num<=16) for(int i=0;i<bound.num;++i) if(Resolve(bound.data[i].object)==optionsMenu) {
            const auto name=NameOf(bound.data[i].function);
            if(name.rfind("BndEvt__",0)==0 && Fn(optionsMenu,name.c_str())) { activateEvent=name; dg::Log("Pause menu activation handler: %s",name.c_str()); break; }
        }
    }
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
    if(!IsValid(weaponHud)) weaponHud=Instance("UI_HudWeapon_C");
    state.hiddenSaveControls=0;
    if(IsValid(weaponHud)) {
        Install(weaponHud);
        for(const char* key:{"QuicksaveIcon","SaveLabel","SavePrompt"}) if(auto w=Read<UObject*>(weaponHud,key);IsValid(w)) { Hide(w); ++state.hiddenSaveControls; }
    }
}
void TrackPauseMenu(UObject* o,ULONGLONG now) {
    if(!IsValid(o) || o->Class!=optionsClass) return;
    if(o->ObjectFlags&0x10) { optionsDefaults=o; return; }
    if(o->ObjectFlags&0x20) return;
    const int serial=SerialAt(o->InternalIndex);
    for(const auto& m:pauseMenus) if(m.object==o && m.serial==serial) return;
    pauseMenus.push_back({o,serial,now}); Install(o);
}
void ScanPauseMenus(bool full,ULONGLONG now) {
    // Exact class compare only: cheap enough to sweep while paused, slowly otherwise.
    static int cursor=0;
    const int n=NumObjects();
    if(cursor>=n) cursor=0;
    const int begin=full?0:cursor,end=full?n:std::min(n,cursor+(pauseMenus.empty() && !IsValid(optionsDefaults)?16384:4096));
    for(int i=begin;i<end;++i) if(auto o=ObjectAt(i);o && o->Class==optionsClass) TrackPauseMenu(o,now);
    if(!full) cursor=end;
}
bool GamePaused(UObject* context) {
    auto statics=IsValid(context)?FindObject("/Script/Engine.Default__GameplayStatics"):nullptr;
    auto f=statics?Fn(statics,"IsGamePaused"):nullptr; if(!f) return false;
    Params p(f); p.Set("WorldContextObject",context); Call(statics,"IsGamePaused",&p); return p.GetBool("ReturnValue");
}
void UpdatePauseMenu(UObject* controller,UObject* gameMode) {
    // The game builds a new OptionsTopMenuWidget_C on every pause, and only some of them are
    // published in Pause_Controls_C.PauseMenuWidget (cleared again on close). Menus opened from
    // the 3D menu/map, mission fades or the native controller were never found while an older,
    // not yet collected instance was cached, so they stayed unfiltered and unhooked.
    const ULONGLONG now=GetTickCount64();
    static ULONGLONG nextClass=0,nextFilter=0,nextFull=0;
    static unsigned int fullScans=0;
    static bool wasPaused=false;
    if(!IsValid(optionsClass)) {
        if(now<nextClass) return;
        nextClass=now+1000; optionsClass=FindClass("OptionsTopMenuWidget_C");
        if(!IsValid(optionsClass)) { optionsClass=nullptr; return; }
    }
    if(auto m=closeMenu) {
        closeMenu=nullptr;
        if(IsValid(m) && SerialAt(m->InternalIndex)==closeSerial) {
            if(auto f=Fn(m,"RequestClose")) { Params p(f); p.SetBool("ClosedByPhotoMode",false); Call(m,"RequestClose",&p); }
            else Call(m,"OnCancelPressed");
            dg::Log("Pause menu closed by the return-to-game entry: %s",GetName(m).c_str());
        }
    }
    if(!IsValid(pauseControls)) pauseControls=Instance("Pause_Controls_C");
    TrackPauseMenu(Read<UObject*>(pauseControls,"PauseMenuWidget"),now);
    const bool paused=GamePaused(gameMode) || BoolCall(controller,"IsPauseMenuActive");
    if(paused && !wasPaused) { fullScans=2; nextFull=now; } // the menu may be built a frame after the pause starts
    wasPaused=paused;
    if(fullScans && now>=nextFull) { --fullScans; nextFull=now+150; ScanPauseMenus(true,now); }
    else ScanPauseMenus(false,now);
    const bool slow=now>=nextFilter;
    if(slow) { nextFilter=now+250; PatchPauseDefaults(); }
    for(size_t i=0;i<pauseMenus.size();) {
        const auto m=pauseMenus[i];
        if(!IsValid(m.object) || SerialAt(m.object->InternalIndex)!=m.serial) { pauseMenus.erase(pauseMenus.begin()+i); continue; }
        // Entries appear over the first frames of the open animation: filter every frame at first.
        if(slow || now<m.seen+2000) { Install(m.object); FilterPauseMenu(m.object); }
        ++i;
    }
}
void HideMapControls() {
    if(!onlyMap || !IsValid(mapWidget)) return;
    unsigned int count=0;
    for(const char* name:{"MenuInteract_Skills","MenuInteract_Inventory","MenuInteract_Modifiers","MenuInteract_Storylines","MenuInteract_Storylines2","MenuInteract_Storylines3","MenuInteract_Storylines4","MenuInteract_Deacon","Mission_Box","Mission_Overlay","MissionInfoBox","SelectedStorylinePanel","Arcade_Mission_Box","ArcadeInteract_Modifiers","RegionEntryBorder","RegionStatusBorder","Region_Info","Region_Percent","Storyline_Overlay","StorylineAccent","StorylineBackground","StorylinePrompt_Panel","StorylinePrompt_TitleBorder","StorylinePrompt_Ttile","Storyline_Banner","ObjectivePromptLayout"}) {
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
std::map<UObject*,int> ownMarkers;  // component -> serial
std::map<UObject*,int> deniedMarkers;  // component -> serial
std::map<UObject*,int> radarOnlyMarkers;  // component -> serial
bool MarkerAllowed(UObject* component) {
    if(auto it=ownMarkers.find(component);it!=ownMarkers.end() && it->second==SerialAt(component->InternalIndex)) return true;
    if(auto it=deniedMarkers.find(component);it!=deniedMarkers.end() && it->second==SerialAt(component->InternalIndex)) return false;
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
    static int cursor=0;
    const int n=NumObjects();
    if(cursor>=n) cursor=0;
    if(cursor==0) state.hiddenMarkers=state.visibleFuelMarkers=state.disabledZones=0;
    std::vector<UObject*> objects;
    const int end=std::min(n,cursor+2048);
    for(int i=cursor;i<end;++i) {
        auto o=ObjectAt(i); if(!o || !IsValid(o)) continue;
        if((miniClass && IsA(o,miniClass)) || (iconClass && IsA(o,iconClass)) || (storyOff && ((objectiveClass && IsA(o,objectiveClass)) || (triggerClass && IsA(o,triggerClass)) || (jobMarkerClass && IsA(o,jobMarkerClass)) || (jobScriptClass && IsA(o,jobScriptClass)) || (zoneClass && IsA(o,zoneClass)))))
            if(RuntimeObject(o)) objects.push_back(o);
    }
    cursor=end;
    for(auto o:objects) {
        if(miniClass && IsA(o,miniClass)) {
            const bool visible=MarkerAllowed(o);
            const int serial=SerialAt(o->InternalIndex);
            auto baseline=markerBaselines.find(o->InternalIndex);
            if(baseline==markerBaselines.end() || baseline->second.serial!=serial)
                baseline=markerBaselines.insert_or_assign(o->InternalIndex,MarkerBaseline{serial,GetBoolProp(o,"bRenderWaypointMarker")}).first;
            BoolArg(o,"SetHudAndMapVisibility","Visible",visible);
            auto radar=radarOnlyMarkers.find(o);
            const bool onMap=visible && !(radar!=radarOnlyMarkers.end() && radar->second==serial);
            if(auto f=Fn(o,"SetVisibility")) { Params p(f); p.SetBool("VisibleOnMiniMap",visible); p.SetBool("VisibleOnMap",onMap); Call(o,"SetVisibility",&p); }
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
    static ULONGLONG nextHud=0;
    if(GetTickCount64()<nextHud) return;
    nextHud=GetTickCount64()+500;
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
            if(First(hudWidget)) { Call(hudWidget,"ClearTutorials"); Call(hudWidget,"ClearTutorialMultis"); }
            state.hiddenHudControls=0;
            for(const char* key:{"_Objectives","MissionPopup","NotifyBanner","NotifyCollectible","NotifyLevelReward","NotifyStandardWrapper","NotifyStoryline","SimpleTutorialOverlay","ObjCallout","XP"}) {
                const bool questWidget=!strcmp(key,"_Objectives");
                if(questHud && questWidget) { if(auto w=Read<UObject*>(hudWidget,key);IsValid(w) && Read<uint8_t>(w,"Visibility",255)==1) { BoolArg(w,"SetIsEnabled","bInIsEnabled",true); if(auto f=Fn(w,"SetVisibility")) { Params p(f); p.Set<uint8_t>("InVisibility",4); Call(w,"SetVisibility",&p); } } continue; }
                if(auto w=Read<UObject*>(hudWidget,key);IsValid(w)) { Hide(w); ++state.hiddenHudControls; }
            }
        }
    }
}
bool AnyOwnTutorial() {
    for(auto it=ownTutorials.begin();it!=ownTutorials.end();) {
        if(IsValid(it->first) && SerialAt(it->first->InternalIndex)==it->second) {
            // Closed panels stay in memory until GC: only an open one counts.
            const auto visibility=Read<uint8_t>(it->first,"Visibility",1);
            if(visibility!=1 && visibility!=2 && BoolCall(it->first,"IsInViewport")) return true;
            ++it;
        } else it=ownTutorials.erase(it);
    }
    return false;
}
void DismissTutorials(UObject* controller) {
    if(!storyOff || !IsValid(hudWidget)) return;
    struct Widgets { UObject** data; int32_t num,max; };
    std::vector<UObject*> open;
    auto multi=Read<Widgets>(hudWidget,"ActiveTutorialMultiWidgets");
    if(multi.data && multi.num>0 && multi.num<=64) open.assign(multi.data,multi.data+multi.num);
    if(auto w=Read<UObject*>(hudWidget,"ActiveSimpleTutorialWidget");IsValid(w)) open.push_back(w);
    std::erase_if(open,[](UObject* w) { return OwnTutorial(w); });
    // Tips close themselves after their time (the panel's own close animation).
    for(auto it=ownTips.begin();it!=ownTips.end();) {
        if(!IsValid(it->first) || SerialAt(it->first->InternalIndex)!=it->second.serial) { it=ownTips.erase(it); continue; }
        if(GetTickCount64()>=it->second.closeAt) {
            if(auto f=Fn(it->first,"RequestClose")) { Params p(f); p.SetBool("ClosedByPhotoMode",false); Call(it->first,"RequestClose",&p); }
            it=ownTips.erase(it); continue;
        }
        ++it;
    }
    if(AnyOwnTutorial()) {
        // A server hint is open: dismiss only foreign ones, leave its pause alone, and keep the
        // player's own input blocked so Interact reaches the panel's Continue prompt.
        for(auto w:open) if(IsValid(w)) { if(!Call(w,"Dismiss")) Call(w,"RemoveFromParent"); }
        if(IsValid(controller)) SetBoolProp(controller,"bTutorialBlockingInputActive",true);
        // Panels made outside the HUD get no input binding: feed Interact (E) to their own hold-to-close.
        static bool held=false;
        DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        const bool down=pid==GetCurrentProcessId() && (GetAsyncKeyState('E')&0x8000);
        if(down!=held) {
            held=down;
            for(auto& [w,serial]:ownTutorials) if(IsValid(w) && SerialAt(w->InternalIndex)==serial) Call(w,down?"OnTutorialClosedPressed":"OnTutorialClosedReleased");
        }
        return;
    }
    if(open.empty()) {
        // Without an open panel nothing may keep the player's input locked.
        if(IsValid(controller)) SetBoolProp(controller,"bTutorialBlockingInputActive",false);
        return;
    }
    // A request that slipped past the hook (other call path): close it the native way so its pause ends too.
    for(auto w:open) if(IsValid(w)) { if(!Call(w,"Dismiss")) Call(w,"RemoveFromParent"); }
    Call(hudWidget,"ClearTutorialMultis"); Call(hudWidget,"ClearTutorials");
    state.dismissedTutorials+=unsigned(open.size());
    if(state.dismissedTutorials<=8) dg::Log("Native tutorial panels dismissed: %zu",open.size());
}
void ActorOff(UObject* actor) {
    if(!IsValid(actor)) return;
    BoolArg(actor,"SetActorHiddenInGame","bNewHidden",true);
    BoolArg(actor,"SetActorEnableCollision","bNewActorEnableCollision",false);
    BoolArg(actor,"SetActorTickEnabled","bEnabled",false);
}
bool routeOwned=false;
void ClearTrackedRoute() {
    if(!storyOff) return;
    auto mission=Instance("BendGameMission"),jobs=Instance("BendJobManager");
    if(IsValid(mission)) Write<UObject*>(mission,"CurrentTrackedMission",nullptr);
    if(IsValid(jobs)) Write<UObject*>(jobs,"CurrentTrackedJob",nullptr);
    state.trackingCleared=IsValid(mission) && !Read<UObject*>(mission,"CurrentTrackedMission") && (!IsValid(jobs) || !Read<UObject*>(jobs,"CurrentTrackedJob"));
    if(IsValid(mapWidget)) Write<UObject*>(mapWidget,"TrackedMissionJobData",nullptr);
    if(IsValid(map) && !routeOwned) {
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
    const auto tutorialSimple=FindClass("BendTutorialSimpleData"),tutorialMulti=FindClass("BendTutorialMultiData");
    const auto nestClass=FindClass("BendNest"),pieceClass=FindClass("FreakerNestPieceBase_C"),spawnClass=FindClass("BendNestSpawnLocation");
    const auto meshClass=FindClass("StaticMeshComponent"),blockerClass=FindClass("BendSplineBlocker"),restrictedClass=FindClass("XpecRestrictArea");
    const auto barricadeClass=FindClass("BP_AmbushZoneBarricade_MASTER_C"),doorClass=FindClass("BendDoor"),zoneClass=FindClass("BendNestingZone");
    auto material=Read<UObject*>(map,"MapMID");
    auto scalarFn=IsValid(material)?Fn(material,"SetScalarParameterValue"):nullptr;
    static int cursor=0;
    const int n=NumObjects();
    if(cursor>=n) cursor=0;
    if(cursor==0) state.nestObjectsHidden=state.storyBlockersDisabled=state.doorsUnlocked=state.zoneParametersCleared=0;
    std::vector<UObject*> objects;
    const int end=std::min(n,cursor+2048);
    for(int i=cursor;i<end;++i) {
        auto o=ObjectAt(i); if(!o || !IsValid(o)) continue;
        if((tutorialSimple && IsA(o,tutorialSimple)) || (tutorialMulti && IsA(o,tutorialMulti))) { SetBoolProp(o,"bTutorialDisplayed",true); continue; }
        const bool zone=zoneClass && IsA(o,zoneClass) && !(o->ObjectFlags&0x10);
        if(zone || ((nestClass && IsA(o,nestClass)) || (pieceClass && IsA(o,pieceClass)) || (spawnClass && IsA(o,spawnClass)) || (meshClass && IsA(o,meshClass)) || (blockerClass && IsA(o,blockerClass)) || (restrictedClass && IsA(o,restrictedClass)) || (barricadeClass && IsA(o,barricadeClass)) || (doorClass && IsA(o,doorClass))))
            if(zone || RuntimeObject(o)) objects.push_back(o);
    }
    cursor=end;
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
    static ULONGLONG nextRoute=0;
    if(GetTickCount64()>=nextRoute) { nextRoute=GetTickCount64()+500; ClearTrackedRoute(); }
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
bool ShowNativeHint(UObject* controller,const std::wstring& title,const std::wstring& text,const std::string& image,bool pause,float seconds) {
    if(!IsValid(controller) || !IsValid(hudWidget)) return false;
    auto widgets=FindObject("/Script/UMG.Default__WidgetBlueprintLibrary"),texts=FindObject("/Script/Engine.Default__KismetTextLibrary");
    auto panelClass=FindClass("UI_TutorialPanel_C");
    auto create=widgets?Fn(widgets,"Create"):nullptr; auto conv=texts?Fn(texts,"Conv_StringToText"):nullptr;
    if(!create || !conv || !panelClass) { dg::Log("Native hint unavailable (create=%p text=%p class=%p)",create,conv,panelClass); return false; }
    auto makeText=[&](const std::wstring& value) {
        Params p(conv); p.Set("InString",FString{value.c_str(),int32_t(value.size()+1),int32_t(value.size()+1)}); Call(texts,"Conv_StringToText",&p);
        return p.Get<TextValue>("ReturnValue");  // the frame is never destructed: the reference moves to the caller
    };
    Params c(create); c.Set("WorldContextObject",controller); c.Set("WidgetType",panelClass); c.Set("OwningPlayer",controller);
    Call(widgets,"Create",&c);
    UObject* panel=c.Get<UObject*>("ReturnValue");
    if(!IsValid(panel)) { dg::Log("Native hint: panel creation failed"); return false; }
    UObject* texture=nullptr;
    if(!image.empty()) { texture=FindObject(image.c_str()); if(!texture) texture=LoadAsset(image.c_str()); }
    const bool tip=title.empty();
    if(tip) {
        // "Survival tip" style is applied after the panel is on screen (below).
    } else if(auto f=Fn(panel,"SetDetailsStandard")) {
        // Window with title, text and picture (verified signature: Title, Description, Image).
        Params d(f); d.Set("Title",makeText(title)); d.Set("Description",makeText(text)); d.Set("Image",texture);
        Call(panel,"SetDetailsStandard",&d);
        if(!texture) Hide(Read<UObject*>(panel,"StandardTutorialImage"));  // no grey placeholder without a picture
    }
    SetBoolProp(panel,"bPauseOnOpen",pause); SetBoolProp(panel,"bNeedsToPause",pause);
    // The [E] Continue prompt closes through this input action (empty on panels made outside the HUD).
    for(const char* action:{"Interact","UI_Confirm","UI_Accept","Accept"}) if(const auto n=FindName(action);n.Index>=0) { Write<FName>(panel,"CloseActionName",n); dg::Log("Native hint close action: %s",action); break; }
    if(title.empty()) ownTips[panel]={SerialAt(panel->InternalIndex),GetTickCount64()+ULONGLONG((seconds>0?seconds:5.f)*1000)};
    else ownTutorials[panel]=SerialAt(panel->InternalIndex);
    auto reg=Fn(hudWidget,"I_RegisterTutorialMulti");
    if(!reg) { dg::Log("Native hint: HUD has no I_RegisterTutorialMulti"); return false; }
    Params r(reg); r.Set("Tutorial",panel); Call(hudWidget,"I_RegisterTutorialMulti",&r);
    // Registration only tracks the panel: it still has to be put on screen and opened (verified in game).
    if(auto f=Fn(panel,"AddToViewport")) { Params v(f); v.Set<int32_t>("ZOrder",50); Call(panel,"AddToViewport",&v); }
    if(tip) {
        // A line of text at the bottom of the screen, closed with Interact (verified in game).
        if(auto f=Fn(panel,"SetDetailsSimple")) {
            Params d(f); d.Set("Content",makeText(text)); d.SetBool("PauseGame",false); d.Set<uint8_t>("CloseButton",28); Call(panel,"SetDetailsSimple",&d);  // no close button: it fades on its own
        }
    } else {
        Call(panel,"SetVisible");
        if(pause) Call(panel,"AttemptPause");
    }
    dg::Log("Native hint shown: %s (image %s)",GetName(panel).c_str(),texture?GetName(texture).c_str():"none");
    return true;
}
void SetRouteOwned(bool owned) { routeOwned=owned; }
void SetQuestHud(bool active) { questHud=active; }
void SetObserver(void(*fn)(UObject*,const std::string&)) { observer=fn; }
void SetCarryInputBlock(bool on) { carryInputBlock=on; }
void SetCarryRollBlock(bool on) { carryRollBlock=on; }
// Hooks of objects that are gone (their slot was freed or reused): the copied vtable is no longer used.
void PruneHooks() {
    size_t freed=0;
    for(auto it=hooks.begin();it!=hooks.end();) {
        Hook* h=it->get();
        // Live while its slot still holds it and it still uses our table (serials are assigned lazily,
        // so they cannot tell objects apart). A reused slot or a new object at the address is not ours.
        if(ObjectAt(h->index)==h->object && h->object->VTable==h->table.data()) { ++it; continue; }
        if(auto a=activeHooks.find(h->object);a!=activeHooks.end() && a->second==h) activeHooks.erase(a);
        observed.erase(h->object); traceObjects.erase(h->object); takeable.erase(h->object); damageWatched.erase(h->object);
        it=hooks.erase(it); ++freed;
    }
    if(freed>100) dg::Log("Policy hooks pruned: %zu (live %zu)",freed,hooks.size());
}
void Observe(UObject* o) {
    if(!IsValid(o)) return;
    // Hooked already (the local pawn for its damage, or an address reused by a new object): only observed too.
    if(o->VTable[60]==reinterpret_cast<void*>(&Event)) { observed.insert(o); return; }
    static ULONGLONG nextPrune=0;
    if(GetTickCount64()>=nextPrune) { nextPrune=GetTickCount64()+10000; PruneHooks(); }
    Install(o,true); observed.insert(o);
}
// Story vignettes that still play in the open world (Deacon looking at Sarah's photo when walking into
// some places): their notifies (photo prop, animation camera) are removed and they play near-instantly.
void NeutralizeVignettes() {
    static ULONGLONG next=0;  // a lookup of a not yet loaded asset walks the object table
    if(GetTickCount64()<next) return;
    next=GetTickCount64()+3000;
    for(const char* path:{"/Game/Animation/vignettes/ambient/anim_deacon_photo.anim_deacon_photo"}) {
        UObject* anim=FindObject(path);
        if(!IsValid(anim)) continue;
        if(auto p=Property(anim,"Notifies")) {
            auto& num=Field<int32_t>(anim,Field<int>(p,off::UProperty_Offset)+8);  // TArray<FAnimNotifyEvent>::Num
            if(num) { num=0; dg::Log("Vignette neutralized: %s",path); }
        }
        Write<float>(anim,"RateScale",50.f);
    }
}
int TraceEvents(double seconds, const std::vector<UObject*>& extra) {
    UObject* controller=abilityController;
    traceObjects.clear(); traceSeen.clear();
    // World objects the player acts on: searchable cars, doors, pickups, explosives.
    std::vector<UObject*> worldClasses;
    for(const char* c:{"BendBasicInteract","BendInventoryPickup","BendDoor","Explosive_C","BendSearchBody"}) if(auto cls=FindClass(c)) worldClasses.push_back(cls);
    for(int i=0,n=NumObjects();i<n;++i) {
        UObject* o=ObjectAt(i);
        if(!o || !IsValid(o)) continue;
        if(!(o->ObjectFlags & 0x10)) {  // not a class default object
            bool world=false; for(auto cls:worldClasses) world|=IsA(o,cls);
            if(world) { Install(o); traceObjects.insert(o); continue; }
        }
        for(UObject* outer=o;outer;outer=outer->Outer) if(outer==localPawn || (controller && outer==controller) || std::find(extra.begin(),extra.end(),outer)!=extra.end()) { Install(o); traceObjects.insert(o); break; }
    }
    traceUntil=GetTickCount64()/1000.0+seconds;
    dg::Log("TRACE start: %zu objects for %.0f s",traceObjects.size(),seconds);
    return int(traceObjects.size());
}
void SetDamageWatcher(DamageWatcher fn) { damageWatcher=fn; }
void WatchDamage(UObject* o) { if(!IsValid(o)) return; Install(o,true); damageWatched.insert(o); }
void SetBlockDeathReload(bool on) { blockDeathReload=on; }
bool TakeDeathReloadBlocked() { const bool b=deathReloadBlocked; deathReloadBlocked=false; return b; }
void SetInterceptLethalHits(bool on) { interceptLethal=on; }
bool TakeLethalHit() { const bool h=lethalHit; lethalHit=false; return h; }
void SetEditorSession(bool on) { editorSession=on; }
// ABendPlayerController::PauseOnFocusLost (virtual, called from native code: no event to block)
// opens the pause menu. Its body becomes a plain return; the bytes are checked against this build.
// UBaseMenuWidget::SetGamePaused (native, called by every menu of the game) pauses the world
// through the player controller. Its body becomes a plain return; the bytes are checked first.
void DisableMenuPause() {
    static bool done=false; if(done) return; done=true;
    uint8_t* code=reinterpret_cast<uint8_t*>(Base()+0x8021F0);
    static const uint8_t expected[]{0x48,0x89,0x5C,0x24,0x08};  // mov [rsp+8], rbx
    if(std::memcmp(code,expected,sizeof(expected))!=0) { dg::Log("Menu pause: unexpected code at BaseMenuWidget::SetGamePaused, left alone"); return; }
    DWORD old=0;
    if(VirtualProtect(code,1,PAGE_EXECUTE_READWRITE,&old)) { code[0]=0xC3; VirtualProtect(code,1,old,&old); FlushInstructionCache(GetCurrentProcess(),code,1); }
    dg::Log("Menu pause disabled (menus do not pause the world)");
    // UBendAudioManager::PauseMenuOpened pauses the game's sounds for the menu; the running bike's
    // engine does not come back when it closes. The world goes on, so its sound does too.
    uint8_t* audio=reinterpret_cast<uint8_t*>(Base()+0x3FA1B0);
    static const uint8_t audioExpected[]{0x48,0x8B,0x81,0xE0,0x0A,0x00,0x00};  // mov rax, [rcx+0AE0h]
    if(std::memcmp(audio,audioExpected,sizeof(audioExpected))!=0) { dg::Log("Menu sound pause: unexpected code at PauseMenuOpened, left alone"); return; }
    if(VirtualProtect(audio,1,PAGE_EXECUTE_READWRITE,&old)) { audio[0]=0xC3; VirtualProtect(audio,1,old,&old); FlushInstructionCache(GetCurrentProcess(),audio,1); }
    dg::Log("Menu sound pause disabled");
}
void DisableFocusPause(UObject* engine) {
    if(IsValid(engine)) SetBoolProp(engine,"bPauseOnLossOfFocus",false);
    static bool done=false; if(done) return; done=true;
    uint8_t* code=reinterpret_cast<uint8_t*>(Base()+0x7986A0);
    static const uint8_t expected[]{0x40,0x53,0x48,0x83,0xEC,0x20};  // push rbx; sub rsp, 20h
    if(std::memcmp(code,expected,sizeof(expected))!=0 && !(code[0]==0x53 && code[1]==0x48 && code[2]==0x83 && code[3]==0xEC && code[4]==0x20)) {
        dg::Log("Focus pause: unexpected code at PauseOnFocusLost, left alone");
        return;
    }
    DWORD old=0;
    if(VirtualProtect(code,1,PAGE_EXECUTE_READWRITE,&old)) { code[0]=0xC3; VirtualProtect(code,1,old,&old); FlushInstructionCache(GetCurrentProcess(),code,1); }
    dg::Log("Focus pause disabled (PauseOnFocusLost returns at once)");
}
bool TakeDeath() { const bool d=playerDied; playerDied=false; return d; }
bool RewardPanelOpen() { return IsValid(rewardsPanel) && Read<bool>(rewardsPanel,"Active"); }
void SetTakeable(UObject* o,bool on) { if(on) takeable.insert(o); else takeable.erase(o); }
void WatchRewardScreens() { rewardWatchUntil=GetTickCount64()/1000.0+180; rewardStuckSince=0; }
void AllowMarker(UObject* component,bool allowed) { if(allowed) ownMarkers[component]=SerialAt(component->InternalIndex); else ownMarkers.erase(component); }
void RadarOnly(UObject* component,bool radarOnly) { if(radarOnly) radarOnlyMarkers[component]=SerialAt(component->InternalIndex); else radarOnlyMarkers.erase(component); }
void DenyMarker(UObject* component,bool denied) { if(denied) deniedMarkers[component]=SerialAt(component->InternalIndex); else deniedMarkers.erase(component); }
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
    if(now<rewardWatchUntil && IsValid(hudWidget)) {
        UObject* panel=Read<UObject*>(Read<UObject*>(hudWidget,"_Objectives"),"RewardsPanel");
        if(IsValid(panel) && panel!=rewardsPanel) { rewardsPanel=panel; Install(panel); }
        static double nextConfirm=0;
        if(IsValid(rewardsPanel) && Read<bool>(rewardsPanel,"Active") && now<rewardAutoConfirmUntil && now>=nextConfirm) {
            nextConfirm=now+0.3; Call(rewardsPanel,"OnConfirmPressed"); dg::Log("Quest reward: continued past the next page");
        }
        // The panel disables the pause menu while it is up; its mission would enable it again.
        static bool wasOpen=false;
        const bool open=IsValid(rewardsPanel) && Read<bool>(rewardsPanel,"Active");
        if(wasOpen && !open) {
            if(auto ui=Instance("UIManagerBP_C");IsValid(ui) && BoolCall(ui,"IsPauseMenuDisabled")) {
                BoolArg(ui,"SetPauseMenuDisabled","IsDisabled",false); dg::Log("Quest reward: pause menu enabled again");
            }
            if(GamePaused(controller) && !BoolCall(controller,"IsPauseMenuActive")) if(auto statics=FindObject("/Script/Engine.Default__GameplayStatics")) if(auto f=Fn(statics,"SetGamePaused")) {
                Params p(f); p.Set("WorldContextObject",controller); p.SetBool("bPaused",false); Call(statics,"SetGamePaused",&p);
                dg::Log("Quest reward panel closed: game unpaused");
            }
        }
        wasOpen=open;
    }
    if(now<rewardWatchUntil && IsValid(controller)) {
        // The reward panel pauses on its own and re-pauses (replaying its sound) when unpaused under it.
        const bool panelOpen=IsValid(rewardsPanel) && Read<bool>(rewardsPanel,"Active");
        const bool paused=GamePaused(controller),menuOpen=panelOpen || state.menuActive || BoolCall(controller,"IsPauseMenuActive") || AnyOwnTutorial();
        if(paused!=rewardLastPaused || menuOpen!=rewardLastMenu) dg::Log("Quest reward watch: paused=%d menu=%d",paused,menuOpen);
        rewardLastPaused=paused; rewardLastMenu=menuOpen;
        if(!paused || menuOpen) rewardStuckSince=0;
        else if(!rewardStuckSince) rewardStuckSince=now;
        else if(now-rewardStuckSince>=1.0) {
            rewardStuckSince=0;
            auto statics=FindObject("/Script/Engine.Default__GameplayStatics");
            if(auto f=statics?Fn(statics,"SetGamePaused"):nullptr) {
                Params p(f); p.Set("WorldContextObject",controller); p.SetBool("bPaused",false); Call(statics,"SetGamePaused",&p);
                dg::Log("Quest reward screens closed: game unpaused");
            }
        }
    }
    if(now>=nextRefresh) {
        if(storyOff) NeutralizeVignettes();
        nextRefresh=now+0.5;
        ApplySavingPolicy(gameMode);
        if(!IsValid(menu)) { menu=Instance("BP_Menu3D_C"); state.mapEnum=-1; }
        if(!IsValid(map)) map=Instance("BP_Map3D_C");
        if(!IsValid(mapWidget)) mapWidget=Instance("UI_MapMenuWidget3D_C");
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
        if(storyOff) {
            if(!IsValid(dlcUi)) dlcUi=Instance("DLC_UIManagerBP_C");
            if(IsValid(dlcUi)) Install(dlcUi);
        }

    }
    static ULONGLONG nextBatch=0;
    if(GetTickCount64()>=nextBatch) {
        nextBatch=GetTickCount64()+8;
        SuppressPresentation(); CleanWorld();
    }
    DismissTutorials(controller);
    UpdatePauseMenu(controller,gameMode);
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
