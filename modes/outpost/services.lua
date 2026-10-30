local L=require("i18n")
local R = require("core")
local C,D = R.cfg,R.rules
local S = { fleet = {}, calls = {} }
local function idle(p)
    if R.session(p).job then R.msg(p,L("Сначала завершите текущее задание.","Finish your current job first."),false); return false end
    return true
end
function S.roles(p)
    local rows={}
    for _,id in ipairs(C.roleOrder) do rows[#rows+1]=C.roles[id].name end
    R.menu(p,L("Профессия · форма и рация на смене","Profession · uniform and duties"),rows,function(q,i)
        if not idle(q) then return end
        local a,s=R.acc(q),R.session(q)
        a.role=C.roleOrder[i]; s.duty=false; q:setUniform(nil)
        R.commit("role",q,a.role); R.msg(q,L("Профессия: ","Profession: ")..C.roles[a.role].name..L(". Начните смену у коменданта.",". Start your shift with the commander."))
    end,"office")
end
function S.office(p)
    R.menu(p,L("Комендатура Рубежа","Rubezh command office"),{L("Пропуск и первая работа","Registration and first job"),L("Задания лагеря","Camp jobs"),L("Выбрать профессию","Choose profession"),L("Надеть / снять форму","Put on / remove uniform"),L("Состояние лагеря","Camp status")},function(q,i)
        if i==1 then
            if not idle(q) then return end
            local a=R.acc(q)
            if not a.intro then
                a.intro=true; a.rep=a.rep+1; R.commit("registration",q)
            end
            local jobs=require("jobs")
            if a.starterDone then jobs.board(q) else jobs.referCargo(q) end
        elseif i==2 then require("jobs").board(q)
        elseif i==3 then S.roles(q)
        elseif i==4 then S.duty(q)
        else S.camp(q) end
    end,"office")
end
function S.warehouse(p)
    R.menu(p,L("Кладовщик","Quartermaster"),{L("Открыть склад","Open storage"),L("Начать работу · доставка ящика","Start work · crate delivery")},function(q,i)
        if i==1 then S.bag(q,true) else require("jobs").start(q,"cargo") end
    end,"warehouse")
end
function S.duty(p)
    if not idle(p) or not R.at(p,"office",6) then return end
    local a,s=R.acc(p),R.session(p)
    if a.role=="resident" then return R.msg(p,L("Выберите профессию у коменданта.","Choose a profession with the commander."),false) end
    s.duty=not s.duty
    p:setUniform(s.duty and C.roles[a.role].uniform or nil)
    R.sync(p); R.msg(p,s.duty and L("Смена началась.","Your shift has started.") or L("Смена завершена.","Your shift has ended."))
end
function S.camp(p)
    local c=R.state.camp
    R.dialog(p,{title=C.title,text=(L("Уровень %d/3 · Казна $%d\nПровизия %d · Медикаменты %d · Детали %d · Топливо %d\n\nВыполняйте задания, приносите припасы на склад и участвуйте в экспедициях.\nУлучшения лагеря доступны у коменданта с репутацией 10.\nУлучшение увеличивает выплату за активность.","Level %d/3 · Treasury $%d\nFood %d · Medicine %d · Parts %d · Fuel %d\n\nHelp the camp by completing jobs, bringing supplies to the warehouse and joining expeditions.\nUpgrade the camp at the command office (reputation 10 required).\nUpgrades increase activity payments.")):format(c.level,c.treasury,c.food,c.medicine,c.parts,c.fuel),button1=L("Понятно","Understood")})
end
function S.shop(p)
    local rows={}
    for _,id in ipairs(C.itemOrder) do local it=C.items[id]; rows[#rows+1]={it.name,"$"..it.price,L("Запас: ","Stock: ")..R.state.camp[it.stock]} end
    rows[#rows+1]=L("Оружейная","Armory")
    R.menu(p,L("Снабжение · покупка по 1 шт.","Supplies · purchase one item at a time"),rows,function(q,i)
        if i==#C.itemOrder+1 then return S.armory(q) end
        local id=C.itemOrder[i]
        local ok,err=D.buy(C,R.acc(q),R.state.camp,id)
        if R.result(q,ok,err,"buy",id) then
            if id=="radio" then q:setVoiceChannel("Rubezh 101") end
            R.msg(q,L("Куплено: ","Purchased: ")..C.items[id].name)
        end
    end,"shop")
end
function S.armory(p)
    local rows={}
    for _,it in ipairs(C.weapons) do rows[#rows+1]={it.name,"$"..it.price,L("Репутация ","Reputation ")..it.rep} end
    R.menu(p,L("Оружейная","Armory"),rows,function(q,i)
        local a,it,c=R.acc(q),C.weapons[i],R.state.camp
        if a.money<it.price or a.rep<it.rep or c.parts<3 then return R.msg(q,L("Не хватает денег, репутации или деталей лагеря (3).","Not enough money, reputation or camp parts (3)."),false) end
        a.money=a.money-it.price; c.treasury=c.treasury+it.price; c.parts=c.parts-3
        R.commit("weapon",q,it.key)
        q:giveWeapon(it.key,it.ammo,{replace=true})
        R.msg(q,L("Выдано: ","Received: ")..it.name)
    end,"shop")
end
local function nearbyBike(p)
    local best,dist
    for _,v in ipairs(vehicles()) do
        if v.model=="bike" and v.world==p.world then
            local d=R.distance(p,v)
            if d<=5 and (not dist or d<dist) then best,dist=v,d end
        end
    end
    return best
end
function S.use(p,id)
    local a,s=R.acc(p),R.session(p)
    if now()<(s.useAt or 0) then return R.msg(p,L("Подождите перед следующим действием.","Wait before your next action."),false) end
    if (a.bag[id] or 0)<1 then return R.msg(p,L("Нет этого предмета.","You do not have this item."),false) end
    if id=="food" or id=="water" then
        local field=id=="food" and "hunger" or "thirst"
        if a[field]>=100 then return R.msg(p,L("Пока не требуется.","Not needed yet.")) end
        D.take(a.bag,id,1); a[field]=math.min(100,a[field]+40)
    elseif id=="medkit" then
        if p.health>=95 then return R.msg(p,L("Лечение не требуется.","No treatment needed.")) end
        D.take(a.bag,id,1); p:heal(math.min(100,p.health+40))
    elseif id=="fuel" or id=="scrap" then
        local v=nearbyBike(p)
        if not v or v.driver or v.passenger then return R.msg(p,L("Нужен свободный мотоцикл не дальше 5 м.","An unoccupied motorcycle must be within 5 m."),false) end
        if id=="scrap" and (a.role~="mechanic" or not s.duty) then return R.msg(p,L("Ремонт доступен механику на смене.","Only mechanics on duty can repair motorcycles."),false) end
        local field=id=="fuel" and "fuel" or "health"
        if v[field]>=.99 then return R.msg(p,L("Обслуживание не требуется.","No maintenance needed.")) end
        D.take(a.bag,id,1); setVehicle(v.id,{[field]=math.min(1,v[field]+.5)})
        p:playAnim("kneel",{loop=false})
    elseif id=="radio" then
        p:setVoiceChannel("Rubezh 101"); return R.msg(p,L("Рация: канал 101. Говорить — K.","Radio tuned to channel 101. Hold K to speak."))
    else return end
    s.useAt=now()+3
    R.commit("use",p,id); R.msg(p,L("Использовано: ","Used: ")..C.items[id].name)
end
function S.bag(p,stash)
    local a=R.acc(p)
    local function pane(bag,id,title)
        local items={}
        for _,key in ipairs(C.itemOrder) do
            if (bag[key] or 0)>0 then
                items[#items+1]={id=key,name=C.items[key].name,count=bag[key],text=C.items[key].weight..L(" кг/шт."," kg each"),
                    actions=id=="bag" and {{id="use",label=L("Использовать","Use")}} or {}}
            end
        end
        return {id=id,title=title,items=items,info=L.format(L("%.1f кг","%.1f kg"),D.weight(C,bag)),empty=L("Пусто","Empty")}
    end
    local panes={pane(a.bag,"bag",L("Рюкзак · до 30 кг","Backpack · up to 30 kg"))}
    if stash then panes[2]=pane(a.stash,"stash",L("Личный тайник · до 100 кг","Personal storage · up to 100 kg")) end
    R.items(p,{title=stash and L("Склад · личный тайник","Warehouse · personal storage") or L("Рюкзак","Backpack"),panes=panes,hint=L("Перенос по 1 шт.; использование — кнопкой предмета.","Transfer one item at a time; use the item's action button.")},function(q,action)
        if not R.ready(q) or q.carrying or R.acc(q)~=a then return end
        if stash and not R.at(q,"warehouse",5) then q:closeItemWindow(); return end
        if type(action)~="table" or action.type=="close" then return end
        local pi,ii=action.pane,action.item
        if not D.integer(pi,1,#panes) or not D.integer(ii,1,#panes[pi].items) then return end
        local id=panes[pi].items[ii].id
        if action.type=="action" and pi==1 and action.id=="use" then S.use(q,id)
        elseif action.type=="move" and stash then
            local ok,err=D.move(C,a,id,pi==1); R.result(q,ok,err,"stash",id)
            if (a.bag.radio or 0)==0 then q:setVoiceChannel(nil) end
        end
        S.bag(q,stash)
    end)
end
function S.clinic(p)
    R.menu(p,L("Медпункт","Clinic"),{L("Лечение · $","Treatment · $")..C.treatmentPrice,L("Отправить вызов медика","Call a medic")},function(q,i)
        if i==2 then return S.call(q,"medic") end
        local a,c=R.acc(q),R.state.camp
        if q.health>=95 then return R.msg(q,L("Лечение не требуется.","No treatment needed.")) end
        if a.money<C.treatmentPrice or c.medicine<1 then return R.msg(q,L("Не хватает денег или медикаментов в лагере.","Not enough money or camp medical supplies."),false) end
        a.money=a.money-C.treatmentPrice; c.treasury=c.treasury+C.treatmentPrice; c.medicine=c.medicine-1
        q:heal(); R.commit("clinic",q)
    end,"clinic")
end
function S.call(p,kind)
    if kind~="medic" and kind~="mechanic" and kind~="guard" then return R.msg(p,"/call medic|mechanic|guard",false) end
    local s=R.session(p)
    if now()<(s.callAt or 0) then return R.msg(p,L("Повторный вызов через минуту.","Wait one minute before sending another call."),false) end
    s.callAt=now()+60; S.calls[p.id]={player=p,kind=kind,expires=now()+300}
    R.msg(p,L("Заявка отправлена. Срок действия — 5 минут.","Call sent. It remains active for 5 minutes."))
    for _,q in ipairs(players()) do
        local a=R.acc(q)
        if a and a.role==kind and R.session(q).duty then R.msg(q,L("Вызов от ","Call from ")..p.name.." ["..p.id.."]. /respond "..p.id) end
    end
end
local function nearBay(at,pt,radius)
    return (at.x-pt.x)^2+(at.y-pt.y)^2<=(radius*100)^2 and math.abs(at.z-pt.z)<300
end
local function freeBay(pt)
    for _,v in ipairs(vehicles()) do
        if (v.world==0 or v.world==-1) and nearBay(v,pt,2.5) then return false end
    end
    for _,q in ipairs(players()) do
        if q.valid and q.world==0 then
            local x,y,z=q:position()
            if nearBay({x=x,y=y,z=z},pt,1.5) then return false end
        end
    end
    return true
end
local function issueBike(p,bike)
    if #R.garageSpawns==0 then return R.msg(p,L("Гараж ещё не настроен. Выдача транспорта временно недоступна.","The garage has not been configured yet. Vehicle collection is temporarily unavailable."),false) end
    local pt
    for _,bay in ipairs(R.garageSpawns) do if freeBay(bay) then pt=bay; break end end
    if not pt then return R.msg(p,L("Все места выдачи заняты. Освободите место в гараже и попробуйте снова.","All collection bays are occupied. Clear a garage bay and try again."),false) end
    local id=createVehicle({model="bike",x=pt.x,y=pt.y,z=pt.z,yaw=pt.yaw or 0,world=0,
        owner=p,locked=bike.locked~=false,blip="owner",fuel=bike.fuel,health=bike.health})
    if not id then R.msg(p,L("Не удалось выдать мотоцикл. Попробуйте ещё раз.","Could not issue your motorcycle. Please try again."),false); return end
    S.fleet[R.session(p).key]=id; p:markVehicle(id)
    R.msg(p,L("Мотоцикл выдан на месте выдачи в гараже и отмечен на карте.","Your motorcycle is ready in a garage collection bay and marked on the map."))
    return id
end
local function parkedAtGarage(p,v)
    if not v or v.world~=p.world or v.driver or v.passenger then return false end
    if R.near(p,v,12) then return true end
    for _,bay in ipairs(R.garageSpawns) do if nearBay(v,bay,4) then return true end end
    return false
end
-- One motorcycle per player: bought once, then collected and returned. Returning works from anywhere
-- (a bike stuck in the woods comes home); maintenance needs it here.
function S.garage(p)
    local a=R.acc(p)
    local rows,acts={},{}
    local function row(text,act) rows[#rows+1]=text; acts[#acts+1]=act end
    if not a.bike then row(L("Купить и получить мотоцикл · $","Buy and collect a motorcycle · $")..C.bikePrice,"buy")
    else
        row(L("Получить свой мотоцикл","Collect your motorcycle"),"collect")
        row(L("Вернуть свой мотоцикл в гараж (откуда угодно)","Return your motorcycle to the garage (from anywhere)"),"return")
    end
    row(L("Сервис NPC · $60","NPC maintenance · $60"),"service")
    R.menu(p,L("Гараж","Garage"),rows,function(q,i)
        local a=R.acc(q)
        local id=S.fleet[R.session(q).key]
        local act=acts[i]
        if act=="buy" then
            if a.bike then return R.msg(q,L("У вас уже есть мотоцикл.","You already own a motorcycle.")) end
            if a.money<C.bikePrice then return R.msg(q,L("Не хватает денег.","Not enough money."),false) end
            local bike={fuel=1,health=1}
            if not issueBike(q,bike) then return end
            a.money=a.money-C.bikePrice; R.state.camp.treasury=R.state.camp.treasury+C.bikePrice
            a.bike=bike; R.commit("buyBike",q)
            R.msg(q,L("Мотоцикл куплен и выдан. Повторно получать его не нужно.","Motorcycle purchased and issued. You do not need to collect it again."))
        elseif act=="collect" then
            if id and vehicleInfo(id) then q:markVehicle(id); return R.msg(q,L("Мотоцикл уже выдан и отмечен на карте.","Your motorcycle is already out and marked on the map.")) end
            issueBike(q,a.bike)
        elseif act=="return" then
            local v=id and vehicleInfo(id)
            if not v then
                S.fleet[R.session(q).key]=nil
                return R.msg(q,L("Мотоцикл уже в гараже. Выберите «Получить свой мотоцикл».","Your motorcycle is already in the garage. Select Collect your motorcycle."))
            end
            if v.driver or v.passenger then return R.msg(q,L("На мотоцикле кто-то сидит.","Someone is riding your motorcycle."),false) end
            a.bike={fuel=v.fuel,health=v.health,locked=v.locked}; removeVehicle(id); S.fleet[R.session(q).key]=nil; R.commit("park",q)
            R.msg(q,L("Мотоцикл убран в гараж.","Motorcycle returned to the garage."))
        else
            local v=id and vehicleInfo(id); local c=R.state.camp
            if not parkedAtGarage(q,v) then return R.msg(q,L("Поставьте свободный мотоцикл на место в гараже или рядом с механиком.","Park your unoccupied motorcycle in a garage bay or near the mechanic."),false) end
            if v.fuel>=.99 and v.health>=.99 then return R.msg(q,L("Сервис не требуется.","No service needed.")) end
            if a.money<60 or c.parts<1 or c.fuel<1 then return R.msg(q,L("Не хватает денег или запасов лагеря.","Not enough money or camp supplies."),false) end
            a.money=a.money-60; c.treasury=c.treasury+60; c.parts=c.parts-1; c.fuel=c.fuel-1
            setVehicle(id,{fuel=1,health=1}); a.bike={fuel=1,health=1,locked=v.locked}; R.commit("service",q)
        end
    end,"garage")
end
on("vehicleEnter",function(p,id,allowed,passenger)
    for key,vid in pairs(S.fleet) do
        -- Behind the rider: anyone; at the handlebars: its owner, or anyone while it is unlocked (/lock).
        if vid==id then local v=vehicleInfo(id); return R.ready(p) and (passenger or R.session(p).key==key or (v and not v.locked)) end
    end
end)
-- The game's inventory (weapons, ammo, materials) is the character's: kept from every snapshot the
-- client sends, given back at the next join. Not in the first seconds after a join: the server takes
-- the weapons as the player comes in (StartWeapons=none), and that snapshot is not what it owns.
local LOADOUT_SETTLE=5
local function keepLoadout(p)
    local s,a=R.session(p),R.acc(p); local inv=p:inventory()
    if not a or not inv or not next(inv) or now()-(s.joined or 0)<LOADOUT_SETTLE or p.world~=0 then return false end  -- (a duel's weapons are not its own)
    local copy={}; for key,n in pairs(inv) do if n>0 then copy[key]=n end end
    a.loadout=copy
    return true
end
on("inventoryChange",function(p) if R.ready(p) and keepLoadout(p) then storage.set("outpost:v1",R.state) end end)
R.hooks[#R.hooks+1]=function(p,why)
    S.calls[p.id]=nil
    if why=="death" then return end
    keepLoadout(p)
    -- Out in the world: it goes with its owner and comes back where it stood at the next join.
    local s=R.session(p); local id=s and S.fleet[s.key]; local v=id and vehicleInfo(id)
    if v then
        local out=v.world==0  -- (ridden as its owner quits: where it is now)
        s.account.bike={fuel=v.fuel,health=v.health,locked=v.locked,x=out and v.x or nil,y=out and v.y or nil,z=out and v.z or nil,yaw=out and v.yaw or nil}
        removeVehicle(id)
    end
    if s then S.fleet[s.key]=nil end
end
-- The bike left out at the last quit (or server stop) stands there again; bought weapons come back.
on("playerJoin",function(p)
    local s,a=R.session(p),R.acc(p)
    if s then s.joined=now() end
    if a and a.loadout then p:restoreInventory(a.loadout)
    elseif a then
        -- Bought before the inventory was kept: the mode's ledger remembers the weapons, with the ammo
        -- that came with them.
        local bought={}; for _,w in ipairs(C.weapons) do bought[w.key]=w.ammo end
        for _,e in ipairs(R.state.ledger or {}) do
            if e.kind=="weapon" and e.account==s.key and bought[e.detail] then p:giveWeapon(e.detail,bought[e.detail],{replace=true}) end
        end
    end
    local bike=a and a.bike
    if not bike or not bike.x or S.fleet[s.key] then return end
    local id=createVehicle({model="bike",x=bike.x,y=bike.y,z=bike.z,yaw=bike.yaw or 0,world=0,
        owner=p,locked=bike.locked~=false,blip="owner",fuel=bike.fuel,health=bike.health})
    if id then S.fleet[s.key]=id; p:markVehicle(id) end
end)
R.command("bag",function(p) S.bag(p) end)
bindKey("I",function(p) if R.ready(p) then S.bag(p) end end)
R.command("use",function(p,_,id) S.use(p,id) end)
R.command("camp",S.camp)
R.command("duty",S.duty)
R.command("shop",function(p) if R.at(p,"shop") then S.shop(p) end end)
R.command("armory",function(p) if R.at(p,"shop") then S.armory(p) end end)
-- /lock: its owner locks or unlocks the motorcycle from within 30 m; unlocked, anyone may ride it.
R.command("lock",function(p)
    local a=R.acc(p); local id=S.fleet[R.session(p).key]; local v=id and vehicleInfo(id)
    if not v then return R.msg(p,L("Ваш мотоцикл не выдан. Получите его в гараже.","Your motorcycle is not out. Collect it at the garage."),false) end
    if v.world~=p.world or R.distance(p,v)>30 then return R.msg(p,L("Подойдите к своему мотоциклу ближе (30 м).","Get within 30 m of your motorcycle."),false) end
    local locked=not v.locked
    setVehicle(id,{locked=locked}); a.bike.locked=locked; R.commit("bikeLock",p,tostring(locked))
    R.msg(p,locked and L("Мотоцикл заперт: за руль садитесь только вы.","Motorcycle locked: only you can ride it.")
        or L("Мотоцикл открыт: за руль может сесть любой. /lock — запереть.","Motorcycle unlocked: anyone can ride it. /lock to lock it again."))
end)
R.command("garage",function(p) if R.at(p,"garage",6) then S.garage(p) end end)
R.command("stash",function(p) if R.at(p,"warehouse") then S.bag(p,true) end end)
R.command("call",function(p,_,kind) S.call(p,kind) end)
R.command("calls",function(p)
    local count=0
    for id,c in pairs(S.calls) do
        if c.expires>now() and R.ready(c.player) then R.msg(p,"["..id.."] "..c.player.name.." → "..C.roles[c.kind].name); count=count+1 end
    end
    if count==0 then R.msg(p,L("Активных вызовов нет.","No active calls.")) end
end)
R.command("respond",function(p,_,who)
    local c=S.calls[tonumber(who)]
    if not c or c.expires<=now() or not R.ready(c.player) then return R.msg(p,L("Вызов неактуален.","This call is no longer active."),false) end
    if R.acc(p).role~=c.kind or not R.session(p).duty then return R.msg(p,L("Нужен специалист на смене.","A specialist on duty is required."),false) end
    local x,y,z=c.player:position(); local s=R.session(p)
    R.mark(p,nil)
    s.marker=createMarker({x=x,y=y,z=z,world=0,player=p,title=L("Вызов: ","Call: ")..c.player.name,track=false,route=true})
    R.msg(c.player,p.name..L(" принял вызов."," accepted your call.")); R.msg(p,L("На карте отмечено место вызова.","The caller's location is marked on your map."))
end)
R.command("aid",function(p,_,who)
    local a,s=R.acc(p),R.session(p)
    if a.role~="medic" or not s.duty then return R.msg(p,L("Нужен медик на смене.","A medic on duty is required."),false) end
    local t=R.peer(p,who,4); if not t then return end
    if now()<(s.aidAt or 0) or t.health>=95 then return R.msg(p,L("Лечение сейчас не требуется или ещё действует задержка.","No treatment needed, or treatment is still on cooldown."),false) end
    local ok,err=D.take(a.bag,"medkit",1); if not ok then return R.msg(p,err,false) end
    s.aidAt=now()+10; t:heal(); p:playAnim("kneel",{loop=false}); S.calls[t.id]=nil
    R.commit("aid",p,t.name); R.msg(t,L("Медик ","Medic ")..p.name..L(" оказал помощь. Оплата по договорённости."," treated you. Arrange payment with the medic."))
end)
R.command("radio",function(p,_,channel)
    if channel=="off" then p:setVoiceChannel(nil); return end
    if (R.acc(p).bag.radio or 0)<1 then return R.msg(p,L("Сначала купите рацию.","Buy a radio first."),false) end
    local n=tonumber(channel)
    if not D.integer(n,100,199) then return R.msg(p,L("/radio 100..199 | off; общий канал — 101.","/radio 100..199 | off; shared channel: 101."),false) end
    p:setVoiceChannel("Rubezh "..n)
end)
R.command("pay",function(p,args)
    local who,raw=args:match("^(%S+)%s+(%d+)$")
    local t=R.peer(p,who); if not t then return end
    local ok,err=D.transfer(C,R.acc(p),R.acc(t),tonumber(raw))
    if R.result(p,ok,err,"pay",t.name.." "..(raw or "")) then R.msg(p,L("Передано $","Paid $")..raw); R.msg(t,p.name..L(" передал вам $"," paid you $")..raw) end
end)
R.command("bank",function(p,args)
    if not R.at(p,"office") then return end
    local op,raw=args:match("^(%a+)%s+(%d+)$")
    if op~="deposit" and op~="withdraw" then return R.msg(p,L("/bank deposit <сумма> | withdraw <сумма>","/bank deposit <amount> | withdraw <amount>"),false) end
    local ok,err=D.bank(C,R.acc(p),tonumber(raw),op=="withdraw")
    R.result(p,ok,err,"bank",args)
end)
R.command("donate",function(p,_,id)
    if not R.at(p,"warehouse") then return end
    local it=C.items[id]
    if not it or id=="radio" then return R.msg(p,L("/donate food|water|medkit|scrap|fuel — 1 шт.","/donate food|water|medkit|scrap|fuel — 1 item"),false) end
    local a=R.acc(p); local ok,err=D.take(a.bag,id,1)
    if not ok then return R.msg(p,err,false) end
    R.state.camp[it.stock]=R.state.camp[it.stock]+1
    R.commit("donate",p,id); R.msg(p,L("Припасы переданы лагерю.","Supplies donated to the camp."))
end)
R.command("upgrade",function(p)
    if not R.at(p,"office") then return end
    if R.acc(p).rep<10 then return R.msg(p,L("Нужна репутация 10.","Reputation 10 is required."),false) end
    local ok,err=D.upgrade(R.state.camp)
    if R.result(p,ok,err,"upgrade") then R.broadcast(L("Rubezh улучшен до уровня ","Rubezh upgraded to level ")..R.state.camp.level,"#D9C898") end
end)
return S
