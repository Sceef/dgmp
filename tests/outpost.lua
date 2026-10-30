-- Run from dgmp/: build/lua-test.exe tests/outpost.lua (Lua 5.4).
package.path="modes/outpost/?.lua;modes/_lib/?.lua;"..package.path
local events,commands,timers,all,stored={}, {}, {}, {}, {}
local clock,id=0,0
local function noop() end
local function nextId() id=id+1; return id end
function on(e,f) events[e]=events[e] or {}; table.insert(events[e],f) end
local function emit(e,...)
    local result
    for _,f in ipairs(events[e] or {}) do local v=f(...); if v~=nil then result=v end end
    return result
end
function command(n,f) commands[n]=f end
function every(ms,f) timers[#timers+1]={ms=ms,f=f}; return #timers end
function now() return clock end
function players() return all end
function getPlayer(who) for _,p in ipairs(all) do if p.id==who or p.name==who then return p end end end
local placedLogic={}
function worldLogic() return placedLogic end
function log(...) if os.getenv("LUA_LOG") then print("LOG", ...) end end
function broadcast() end
function setPvp() end
function setTime() end
function setWeather() end
function setNametags() end
function protectItems() end
local createdZones={}
function createZone(o) createdZones[o.name]=o; return o.name end
function getZone(name) return createdZones[name] end
function createActor() return nextId() end
local uses,objectsMade,zombiesMade={}, {}, {}
function createInteraction(o) local n=nextId(); uses[n]=o; return n end
function removeInteraction(n) uses[n]=nil end
function createObject(o) local n=nextId(); objectsMade[n]=o; return n end
function removeObject(n) objectsMade[n]=nil end
function createZombie(x,y,z,type) local n=nextId(); zombiesMade[n]={id=n,x=x,y=y,z=z,type=type}; return n end
function getZombie(n) return zombiesMade[n] end
function removeZombie(n) zombiesMade[n]=nil end
local blipsMade={}
function createBlip(o) local n=nextId(); blipsMade[n]=o; return n end
function removeBlip(n) blipsMade[n]=nil end
local playerMenu
function setPlayerMenu(m) playerMenu=m end
function setScoreboard() end
local deathMode
function setDeathMode(m,o) deathMode={mode=m,opts=o} end
local worldsGiven=0
function newWorld() worldsGiven=worldsGiven+1; return 1000+worldsGiven end
local freed={}
function freeWorld(w) freed[w]=true end
function cancelTimer(n) if timers[n] then timers[n].cancelled=true end end
function after(ms,f) timers[#timers+1]={ms=ms,f=f,once=true}; return #timers end
function createMarker() return nextId() end
function removeMarker() end
function bindKey() end
storage={get=function(k) return stored[k] end,set=function(k,v) stored[k]=v end,save=noop}
local cargo,fleet={},{}
local failVehicle=false
function createCarry(o) local n=nextId(); o.id=n; cargo[n]=o; return n end
function getCarry(n) return cargo[n] end
function removeCarry(n) cargo[n]=nil; for _,p in ipairs(all) do if p.carrying==n then p.carrying=nil end end end
function createVehicle(o) if failVehicle then return nil end; local n=nextId(); o.id=n; fleet[n]=o; return n end
function vehicleInfo(n) return fleet[n] end
function vehicles() local t={}; for _,v in pairs(fleet) do t[#t+1]=v end; return t end
function removeVehicle(n) fleet[n]=nil end
function setVehicle(n,o) for k,v in pairs(o) do fleet[n][k]=v end end
function removeHorde() end
function removeTrap() end
function unban() return 0 end
local methods={}
for _,name in ipairs({"setUniform","takeBike","hud","tip","hint","setVoiceMuted","objectiveMeter","playAnim","stopAnim","markVehicle","customize","closeItemWindow","setScoreboard","leaveVoiceChannel","joinVoiceChannel"}) do methods[name]=noop end
function methods:voiceChannels() local t={}; if self.channel then t[self.channel]="radio" end; return t end
function methods:ask(text,opts,fn) if self.pendingAsk then return false end; self.pendingAsk={text=text,fn=fn}; return true end
function methods:takeWeapons() self.given={} end
function methods:giveItem() end
function methods:freeze(on) self.frozen=on end
function methods:setControls(c) self.controls=c end
function methods:setPvp(v) self.pvpSet=v end
function methods:setHealth(v) self.health=v end
function methods:revive(hp) self.downed=false; self.revivedWith=hp end
function methods:setDowned(o) self.downedTimes=o end
function methods:cancelAsk() self.pendingAsk=nil end
function methods:playAnim(a) self.anim=a end
function methods:stopAnim() self.anim=nil end
function methods:answer(yes) local a=self.pendingAsk; self.pendingAsk=nil; a.fn(self,yes,"answer") end
function methods:giveWeapon(key,ammo) self.given=self.given or {}; self.given[#self.given+1]=key end
function methods:setAmmo(key,n) self.ammoSet=self.ammoSet or {}; self.ammoSet[key]=n end
function methods:inventory() return self.inv end
function methods:restoreInventory(inv) self.restored=inv end
function methods:position() return self.x,self.y,self.z,0 end
function methods:setPosition(x,y,z) self.x,self.y,self.z=x,y,z end
function methods:setWorld(w) self.world=w end
function methods:setMoney(n) self.money=n end
function methods:setVoiceChannel(n) self.channel=n end
function methods:heal(n) self.health=n or 100 end
function methods:message(t) self.messages[#self.messages+1]=t end
function methods:hud(key,spec) self.huds=self.huds or {}; self.huds[key]=spec end
function methods:dialog(o) self.dialogSpec=o end
function methods:customize(o) self.creator=o; self.creatorCount=(self.creatorCount or 0)+1 end
function methods:setSkin(look) self.skin=look end
function methods:setUniform(look) self.uniform=look end
function methods:itemWindow(o,fn) self.window=o; self.windowAction=fn end
function methods:isOnBike() return false end
function methods:kick(reason) self.kicked=reason end
local function player(n,name,admin,fields)
    local p=setmetatable({id=n,name=name,admin=admin,valid=true,world=0,health=100,lang="ru",messages={}}, {__index=function(_,k) assert(methods[k],"Unknown Player API: "..k); return methods[k] end})
    -- Optional DGMP fields are nil, so use explicit field defaults below via the mock metatable.
    for k,v in pairs(fields or {}) do p[k]=v end
    setmetatable(p,{__index=methods}); all[#all+1]=p; emit("playerJoin",p); return p
end
local function cmd(p,name,args)
    assert(commands[name],"Missing command "..name)(p,args or "",(args or ""):match("^%S+"))
end
local function tick(n)
    for _=1,n do clock=clock+1; for _,t in ipairs(timers) do if not t.cancelled and (t.ms<=1000 or clock%(t.ms/1000)==0) then if t.once then t.cancelled=true end; t.f() end end end
end
local count=0
local function check(ok,label) assert(ok,label); count=count+1; print("PASS "..label) end
local C,D=require("config"),require("domain")
local a,b=D.account(C,1),D.account(C,1)
for _,n in ipairs({-1,0,1.5,math.huge,0/0,10000001}) do check(not D.transfer(C,a,b,n),"reject transfer "..tostring(n)) end
check(not D.transfer(C,a,a,1),"reject self transfer")
check(D.transfer(C,a,b,50) and a.money==400 and b.money==500,"conserve transfer")
check(D.bank(C,a,100,false) and a.money==300 and a.bank==100,"bank deposit")
check(not D.bank(C,a,101,true) and a.bank==100,"bank overdraft")
local camp=D.newState().camp
camp.food=0; local before=a.money
check(not D.buy(C,a,camp,"food") and a.money==before,"out of stock is atomic")
a.bag={fuel=6}
check(not D.buy(C,a,camp,"scrap") and a.money==before and not a.bag.scrap,"weight limit is atomic")
a.stash={scrap=1}
check(not D.move(C,a,"scrap",false) and a.stash.scrap==1,"stash capacity failure retains item")
camp.treasury=0
check(not D.upgrade(camp) and camp.level==1,"upgrade cannot overdraw")
check(D.identity({name="Joe",steamId="spoof",verified=false})=="guest:joe","ignore unverified Steam claim")
check(D.identity({name="Joe",steamId="123",verified=true})=="steam:123","verified Steam identity")
local garage=C.points.garage
placedLogic.garage_spawn={{x=garage.x+1800,y=garage.y+400,z=garage.z,yaw=73},
    {x=garage.x+2400,y=garage.y+400,z=garage.z,yaw=-142}}
dofile("modes/outpost/main.lua")
local R,S,J=require("core"),require("services"),require("jobs")
local campZone=assert(createdZones.outpost_camp)
for _,name in ipairs({"spawn","office","shop","clinic","garage","warehouse","cargo"}) do
    local pt=R.points[name]
    check(math.abs(pt.x-campZone.x)<=campZone.length*50 and math.abs(pt.y-campZone.y)<=campZone.width*50
        and math.abs(pt.z+100-campZone.z)<=campZone.height*100,"camp protects "..name.." at player height")
end
local remote=R.points.expedition
check(math.abs(remote.x-campZone.x)>campZone.length*50 or math.abs(remote.y-campZone.y)>campZone.width*50,"expedition remains outside camp protection")
local p,q=player(1,"Alice",true),player(2,"Bob",false)
check(#p.messages==0,"join does not print command advertisements")
check(p.skin=="survivor" and p.creator.list=="skins" and p.creator.cancel==false,"fresh character starts with survivor and one-time appearance selection")
p.skin="chosen-alice"; emit("customize",p,p.skin,"biker")
check(R.acc(p).skin=="chosen-alice","chosen base appearance belongs to server mode account")
local function at(p,name) local pt=R.points[name]; p:setPosition(pt.x,pt.y,pt.z) end
local function choose(p,i) local fn=p.dialogSpec.onResponse; assert(fn); fn(p,true,i); return fn end
at(p,"office"); S.office(p)
check(#p.dialogSpec.items==5,"commander has no wardrobe option")
choose(p,1)
check(R.acc(p).intro and R.acc(p).rep==1,"register character once")
S.office(p); choose(p,1); check(R.acc(p).rep==1,"registration replay gives no reward")
check(not R.session(p).job and not next(cargo) and require("i18n").resolve(R.session(p).taskText,p)=="Получите работу у кладовщика","commander refers to quartermaster without starting cargo")
J.start(p,"cargo")
check(not R.session(p).job,"commander cannot issue cargo directly")
at(p,"warehouse"); S.warehouse(p)
check(#p.dialogSpec.items==2,"quartermaster offers storage and work")
choose(p,1); check(p.window.panes[2].id=="stash","quartermaster opens personal storage")
S.warehouse(p); choose(p,2)
check(R.session(p).job and cargo[R.session(p).job.crate],"accepting quartermaster work creates physical crate")
J.cancel(p,true)
at(p,"shop"); S.shop(p); local response=p.dialogSpec.onResponse
at(p,"salvage"); response(p,true,1)
check(R.acc(p).money==450,"shop response rechecks distance")
at(p,"shop"); S.shop(p); response=choose(p,1); response(p,true,1)
check(R.acc(p).money==435 and R.acc(p).bag.food==3,"purchase callback cannot replay")
S.shop(p); p.dialogSpec.onResponse(p,true,999)
check(R.acc(p).money==435,"invalid menu index")
at(q,"shop"); q.world=1; cmd(p,"pay","2 20")
check(R.acc(q).money==450,"pay rejects another world")
q.world=0; cmd(p,"pay","2 20")
check(R.acc(p).money==415 and R.acc(q).money==470,"nearby payment")
local money=R.acc(q).money; cmd(q,"director","kit")
check(R.acc(q).money==money,"non-admin cannot obtain filming kit")
at(p,"warehouse"); J.start(p,"cargo")
local crate=R.session(p).job.crate
local originalJob=R.session(p).job
local originalTask=R.session(p).taskText
local originalRole=R.acc(p).role
S.warehouse(p); response=p.dialogSpec.onResponse
response(p,true,2); response(p,true,2)
at(p,"office"); J.start(p,"salvage"); J.start(p,"patrol"); J.join(p); J.referCargo(p)
S.office(p); choose(p,1); S.roles(p); choose(p,2); S.duty(p)
check(R.session(p).job==originalJob and cargo[crate] and not J.mission,"active cargo blocks duplicate work, other jobs and expedition")
check(R.session(p).taskText==originalTask and R.acc(p).role==originalRole and not R.session(p).duty,"job rejection preserves route and profession")
check(emit("carryPick",q,crate)==false,"another player cannot steal job crate")
S.office(p); response=p.dialogSpec.onResponse
p.carrying=crate; local dialogBefore=p.dialogSpec
response(p,true,1)
S.office(p); S.shop(p); at(p,"warehouse"); S.warehouse(p)
check(p.dialogSpec==dialogBefore and R.session(p).job==originalJob,"carrying blocks NPC dialogs and pending dialog responses")
local windowBefore=p.window; S.bag(p,true)
check(p.window==windowBefore,"carrying also blocks storage dialogs")
tick(1); at(p,"warehouse"); tick(4)
local pt=R.points.warehouse
emit("carryDrop",p,crate,{reason="put",x=pt.x,y=pt.y,z=pt.z})
check(R.acc(p).money==480 and not R.session(p).job and not cargo[crate],"cargo delivered and removed")
emit("carryDrop",p,crate,{reason="put",x=pt.x,y=pt.y,z=pt.z})
check(R.acc(p).money==480,"duplicate cargo drop cannot pay twice")
at(p,"office"); J.start(p,"salvage")
originalJob=R.session(p).job
at(p,"warehouse"); J.start(p,"cargo"); at(p,"office"); J.join(p); J.start(p,"patrol")
check(R.session(p).job==originalJob and not next(cargo),"salvage blocks cargo and other assignments")
at(p,"salvage"); tick(1)
local caches={}
for id,o in pairs(uses) do if o.onUse and objectsMade[id-1] then caches[#caches+1]={use=id,o=o} end end
check(#caches==3 and #R.session(p).job.caches==3,"salvage site spawns three caches to search")
uses[caches[1].use].onUse(q)
check(not R.session(p).job.searching,"another player cannot search someone's cache")
local function searchCache(c,ticks) p:setPosition(c.o.x,c.o.y,c.o.z); c.o.onUse(p); tick(ticks) end
searchCache(caches[1],2); at(p,"office"); tick(1)
check(not R.session(p).job.searching and R.session(p).job.found==0,"search must stay at the cache")
searchCache(caches[1],5)
check(R.session(p).job.found==1 and not uses[caches[1].use],"searched cache gives parts and goes")
searchCache(caches[2],5); searchCache(caches[3],5)
check(R.acc(p).bag.scrap==3 and R.acc(p).money==575 and not R.session(p).job,"salvage completes and adds material")
check(not next(uses,nil) or not uses[caches[2].use] and not uses[caches[3].use],"finished salvage leaves no caches")
at(p,"office"); J.join(p)
originalJob=R.session(p).job
J.start(p,"salvage"); at(p,"warehouse"); J.start(p,"cargo"); at(p,"office"); J.join(p)
check(R.session(p).job==originalJob and J.mission.members[p.id]==p and not next(cargo),"expedition membership survives attempts to take another job")
at(p,"expedition"); tick(14)
local raiders={}; for id in pairs(J.mission.zombies) do raiders[#raiders+1]=id end
check(#raiders==4 and R.session(p).job.stage=="search","expedition site is guarded by four freakers; no search before they die")
for i=1,3 do emit("zombieDeath",zombiesMade[raiders[i]]) end
tick(14); check(R.session(p).job.stage=="search","search waits for the last freaker")
removeZombie(raiders[4]); tick(1)
check(J.mission.cleared,"a freaker gone without a death still counts")
tick(14)
check(R.session(p).job.stage=="return","expedition collects virtual cargo")
local med=R.state.camp.medicine; at(p,"warehouse"); tick(1)
check(not R.session(p).job and R.acc(p).money==755 and R.state.camp.medicine==med+8,"expedition delivery")
tick(3); check(R.acc(p).money==755,"expedition cannot pay twice")
at(p,"garage")
local treasuryBefore=R.state.camp.treasury
local bays=R.garageSpawns
R.garageSpawns={}; S.garage(p); choose(p,1); R.garageSpawns=bays
check(not R.acc(p).bike and R.acc(p).money==755 and R.state.camp.treasury==treasuryBefore,"unconfigured garage cannot sell or spawn a motorcycle on the road")
local blocked=createVehicle({model="bike",x=bays[1].x,y=bays[1].y,z=bays[1].z,world=0})
local qx,qy,qz=q:position(); q:setPosition(bays[2].x,bays[2].y,bays[2].z+90)
S.garage(p); choose(p,1)
check(not R.acc(p).bike and R.acc(p).money==755 and R.state.camp.treasury==treasuryBefore,"occupied vehicles and players block garage bays without taking payment")
q:setPosition(qx,qy,qz)
failVehicle=true; S.garage(p); choose(p,1); failVehicle=false
check(not R.acc(p).bike and not S.fleet[R.session(p).key] and R.acc(p).money==755 and R.state.camp.treasury==treasuryBefore,"failed vehicle creation does not charge or sell a missing motorcycle")
S.garage(p); choose(p,1)
local bike=S.fleet[R.session(p).key]
check(bike and fleet[bike] and R.acc(p).bike and R.acc(p).money==405,"purchase immediately issues the motorcycle")
check(fleet[bike].x==bays[2].x and fleet[bike].y==bays[2].y and fleet[bike].z==bays[2].z and fleet[bike].yaw==bays[2].yaw,"garage uses first free editor bay with exact position and heading")
removeVehicle(blocked)
S.garage(p)
check(#p.dialogSpec.items==3 and not p.dialogSpec.items[1]:find("Купить",1,true),"an owner's garage menu has no second purchase")
choose(p,1); S.garage(p); choose(p,1)
check(S.fleet[R.session(p).key]==bike and R.acc(p).money==405,"repeated collection cannot duplicate the motorcycle or charge again")
check(emit("vehicleEnter",q,bike,false,false)==false,"bike rejects non-owner driver")
check(emit("vehicleEnter",q,bike,false,true)==true,"bike permits passenger")
cmd(p,"lock")
check(fleet[bike].locked==false and R.acc(p).bike.locked==false and emit("vehicleEnter",q,bike,false,false)==true,"/lock unlocks: anyone may ride the owner's bike")
cmd(p,"lock")
check(fleet[bike].locked==true and emit("vehicleEnter",q,bike,false,false)==false,"/lock again locks it")
local bx,by=p:position(); fleet[bike].x,fleet[bike].y=bx+900000,by  -- stuck far away
fleet[bike].fuel=.35; fleet[bike].health=.6; fleet[bike].driver=p.id
S.garage(p); choose(p,2)
check(fleet[bike] and S.fleet[R.session(p).key]==bike,"occupied motorcycle cannot be parked")
fleet[bike].driver=nil
S.garage(p); choose(p,2)
check(not fleet[bike] and not S.fleet[R.session(p).key] and R.acc(p).bike.fuel==.35 and R.acc(p).bike.health==.6,"returning from far away removes the vehicle and preserves fuel and condition")
S.garage(p); choose(p,2)
check(p.messages[#p.messages]:find("уже в гараже",1,true) and R.acc(p).money==405,"returning a stored motorcycle reports its actual garage state")
local otherWorld=createVehicle({model="bike",x=bays[1].x,y=bays[1].y,z=bays[1].z,world=1})
S.garage(p); choose(p,1); bike=S.fleet[R.session(p).key]
check(fleet[bike] and fleet[bike].fuel==.35 and fleet[bike].health==.6 and R.acc(p).money==405,"stored motorcycle is collected without another payment")
check(fleet[bike].x==bays[1].x and fleet[bike].yaw==bays[1].yaw,"vehicles in another world do not occupy garage bays")
removeVehicle(otherWorld)
at(p,"warehouse"); S.bag(p,true)
local countBefore=R.acc(p).bag.food; at(p,"expedition")
p.windowAction(p,{type="move",pane=1,item=1,count=1})
check(R.acc(p).bag.food==countBefore,"stash response rechecks distance")
at(p,"warehouse"); S.bag(p,true); p.windowAction(p,{type="move",pane=99,item=99})
check(R.acc(p).bag.food==countBefore,"invalid item-window indexes")
R.acc(p).role="medic"; R.session(p).duty=true; R.acc(p).bag.medkit=1
at(q,"warehouse"); q.health=25; cmd(p,"aid","2")
check(q.health==100 and not R.acc(p).bag.medkit,"medic heals and consumes exactly one kit")
local rep=R.acc(p).rep; cmd(p,"aid","2")
check(R.acc(p).rep==rep,"medical assistance does not mint farming rewards")
R.acc(p).cooldowns.cargo=0; at(p,"warehouse"); J.start(p,"cargo"); local deathCrate=R.session(p).job.crate
emit("playerDeath",p)
check(not R.session(p).job and not cargo[deathCrate],"death clears active cargo")
emit("playerRespawn",p)
check(R.acc(p).money==355 and not R.session(p).dead,"respawn charges once and clears dead state")
local clinic=R.points.clinic
check(math.sqrt((p.x-clinic.x)^2+(p.y-clinic.y)^2)>=150,"respawn puts the player beside the medic, not inside the NPC")
R.acc(p).money=R.acc(p).money+180; R.state.camp.parts=R.state.camp.parts+3
at(p,"shop"); S.armory(p); choose(p,1)
p.inv={kWeaponID_M9=1,kAmmoID_9mm=17,kCraftComponentID_Rag=3}
emit("inventoryChange",p)
check(R.acc(p).loadout.kWeaponID_M9==1 and R.acc(p).loadout.kCraftComponentID_Rag==3,"the game's inventory is kept from its snapshots")
local key,account=R.session(p).key,R.acc(p); emit("playerQuit",p)
check(account.loadout.kAmmoID_9mm==17,"the quit keeps the inventory left")
p.inv={kWeaponID_BootKnife=1}  -- what the server leaves at a join (StartWeapons=none)
check(not S.fleet[key] and not fleet[bike],"disconnect despawns personal vehicle")
local left=account.bike
emit("playerJoin",p)
local back=S.fleet[R.session(p).key]
check(back and fleet[back].x==left.x and fleet[back].y==left.y and fleet[back].yaw==left.yaw and fleet[back].fuel==left.fuel,"reconnect puts the personal bike back where it was left")
check(R.acc(p).money==355 and R.acc(p).bike and R.acc(p).intro,"reconnect restores character")
check(p.skin=="chosen-alice" and p.creatorCount==1,"reconnect restores account appearance without free re-selection")
check(p.restored and p.restored.kWeaponID_M9==1 and p.restored.kAmmoID_9mm==17,"reconnect restores the kept inventory")
emit("inventoryChange",p)
check(R.acc(p).loadout.kWeaponID_M9==1,"the stripped snapshot right after a join does not overwrite the kept inventory")
local duplicate=player(3,"ALICE",false)
check(duplicate.kicked~=nil and not R.session(duplicate),"duplicate identity rejected")
q.lang="en"; R.hud(q)
check(q.huds.outpost.text:find("Resident",1,true) and q.huds.outpost.text:find("Reputation",1,true),"English HUD resolves nested role names")
check(not q.huds.outpost.text:find("/",1,true),"normal HUD has no command list")
at(q,"office"); S.office(q)
check(q.dialogSpec.title=="Rubezh command office" and q.dialogSpec.items[1]=="Registration and first job","English dialog and rows")
choose(q,1)
check(not R.session(q).job and q.huds.outpost.text:find("Ask the quartermaster",1,true),"English registration refers to quartermaster")
at(q,"warehouse"); S.warehouse(q); choose(q,2)
check(R.session(q).job and cargo[R.session(q).job.crate],"English quartermaster starts physical cargo")
at(q,"warehouse"); S.bag(q,true)
check(q.window.title=="Warehouse · personal storage" and q.window.panes[1].items[1].name=="Ration","English item window and item names")
at(q,"shop"); S.shop(q); choose(q,#C.itemOrder+1)
check(q.dialogSpec.title=="Armory","armory is accessible from the shop interface")
emit("playerQuit",q); emit("playerJoin",q)
check(q.huds.outpost.text:find("Ask the quartermaster",1,true),"unfinished first delivery returns to quartermaster")
at(q,"office"); S.office(q); choose(q,1)
at(q,"warehouse"); S.warehouse(q); choose(q,2)
local resumedCrate=R.session(q).job.crate
S.warehouse(q); choose(q,2)
check(cargo[resumedCrate] and R.session(q).job.crate==resumedCrate and R.acc(q).rep==1,"starter delivery recovery cannot duplicate crates or registration rewards")
R.hud(p)
check(p.huds.outpost.text:find("Репутация",1,true),"English player does not change Russian player's language")
local localized=require("i18n")("Проверка","Test")
check(require("i18n").resolve(localized,{lang="de"})=="Test","unknown language falls back to English")
local guard=player(4,"GuardQA",false)
local ga,gs=R.acc(guard),R.session(guard)
ga.intro=true; ga.role="guard"; gs.duty=true; guard:setUniform("guard")
at(guard,"office"); J.start(guard,"patrol"); originalJob=gs.job
J.start(guard,"salvage"); J.join(guard)
at(guard,"warehouse"); S.warehouse(guard); choose(guard,2)
at(guard,"office"); S.roles(guard); choose(guard,1); S.duty(guard)
check(gs.job==originalJob and gs.job.kind=="patrol" and ga.role=="guard" and gs.duty and guard.uniform=="guard","active patrol blocks other jobs and preserves guard duty and uniform")
check(R.session(q).job and R.session(q).job.kind=="cargo","different players can work independently")
J.cancel(guard,true)
local first=player(5,"SteamTwin",false,{verified=true,steamId="111",skin="old-nickname-skin"})
first.skin="first-account-skin"; emit("customize",first,first.skin,"biker")
emit("playerQuit",first)
local second=player(6,"SteamTwin",false,{verified=true,steamId="222",skin="first-account-skin"})
check(second.skin=="survivor" and not R.acc(second).skin,"new Steam identity does not inherit another account's nickname skin")
first.name="RenamedSteamTwin"; emit("playerJoin",first)
check(first.skin=="first-account-skin" and first.creatorCount==1,"verified character appearance survives a nickname change")
-- The player menu: a squad invitation asks the other player; a trade swaps both offers at once.
local s1,s2=player(7,"SquadA",false),player(8,"SquadB",false)
for _,pp in ipairs({s1,s2}) do R.acc(pp).intro=true; at(pp,"office") end
check(playerMenu and playerMenu.key=="F","interact turns the player menu on")
local function pick(pp,label)
    for i,v in ipairs(pp.dialogSpec.items) do if v:find(label,1,true) then pp.dialogSpec.onResponse(pp,true,i,v); return true end end
end
emit("playerInteract",s1,s2)
check(pick(s1,"Пригласить в отряд"),"menu offers the squad invitation")
check(s2.pendingAsk and s2.pendingAsk.text:find("SquadA",1,true),"the invited player is asked")
s2:answer(true)
local squadLib=require("squad")
check(squadLib.same(s1,s2),"a yes joins the squad")
local marks=0; for _,b in pairs(blipsMade) do if b.follow==s1 or b.follow==s2 then marks=marks+1 end end
check(marks==2,"squad members see each other on the map")
R.acc(s1).bag={food=2}; R.acc(s2).bag={water=1}
emit("playerInteract",s1,s2)
check(pick(s1,"Предложить обмен"),"menu offers a trade")
s2:answer(true)
check(s1.window and s2.window,"both see the trade window")
s1.windowAction(s1,{type="move",pane=1,item=1,count=2})
s2.windowAction(s2,{type="move",pane=1,item=1,count=1})
s1.windowAction(s1,{type="action",pane=2,item=1,id="ready"})
s2.windowAction(s2,{type="move",pane=1,item=1,count=1})  -- nothing left to add: no change
s2.windowAction(s2,{type="action",pane=2,item=1,id="ready"})
check(R.acc(s1).bag.water==1 and not R.acc(s1).bag.food and R.acc(s2).bag.food==2 and not R.acc(s2).bag.water,"trade swaps both offers")
emit("playerQuit",s2)
check(not squadLib.of(s1),"a squad of one is disbanded")
-- Downed: no medic on duty lets the wounded give up at once; a medic on duty takes the call and revives.
check(deathMode and deathMode.mode=="downed","Rubezh lays players down instead of killing them")
local hurt=player(9,"Hurt",false); R.acc(hurt).intro=true; at(hurt,"clinic"); hurt.downed=true
emit("playerHelp",hurt)
check(hurt.downedTimes and hurt.downedTimes.giveUp==0,"no medic online: giving up right away")
local doc=player(10,"Doc",false); local da=R.acc(doc); da.intro=true; da.role="medic"; R.session(doc).duty=true; da.bag.medkit=1; at(doc,"clinic")
emit("playerHelp",hurt)
check(doc.pendingAsk and doc.pendingAsk.text:find("Hurt",1,true),"the medic on duty is asked to take the call")
doc:answer(true)
emit("playerInteract",doc,hurt)
check(pick(doc,"Поднять раненого"),"the medic sees Revive on the wounded")
tick(8)
check(not hurt.downed and hurt.revivedWith==35 and not da.bag.medkit,"reviving takes a medkit and puts the wounded up")
-- A duel: its own world and pistol, best of three, then everything back.
local d1,d2=player(11,"DuelA",false),player(12,"DuelB",false)
for _,pp in ipairs({d1,d2}) do R.acc(pp).intro=true; at(pp,"office"); pp.inv={kWeaponID_1887=1,kAmmoID_Shotgun=12} end
local moneyBefore=R.acc(d2).money
emit("playerInteract",d1,d2)
check(pick(d1,"Вызвать на дуэль"),"menu offers a duel")
d2:answer(true)
check(d1.world>=1000 and d1.world==d2.world and d1.given[1]=="kWeaponID_M9","the duel has its own world and weapons")
tick(4)
check(not d1.frozen and not d2.frozen,"the countdown lets them fight")
for round=1,2 do emit("playerDeath",d2); emit("playerRespawn",d2); tick(5) end
check(d1.world==0 and d2.world==0 and d2.restored and d2.restored.kWeaponID_1887==1,"after the duel both are back with their own things")
check(R.acc(d2).money==moneyBefore and not R.session(d2).dead,"a duel death costs nothing")
check(next(freed)~=nil,"the duel's world is given back")
emit("stop")
check(stored["outpost:v1"].accounts[key].money==355,"stop saves state")
print("Outpost: "..count.." checks passed")
