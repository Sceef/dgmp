local L=require("i18n")
local R=require("core")
local C=R.cfg
local logic=require("logic")
local squad=require("squad")
local J={ crates={}, runs={} }
-- Salvage and the NERO expedition are runs: one per player, or per squad (its members share one). A run has
-- a virtual world of its own; whoever of its members comes to the site goes into it (with the bike), so the
-- caches, the freakers and the medical supplies there are the run's only — other players and squads at the
-- same place have their own and nobody spoils anyone's job. Leaving the site brings one back to the shared world.
local ENTER, LEAVE = 120, 180   -- metres from the site: into the run's world / back
-- Salvage: caches to search around the site (editor points "salvage_cache", else a ring around "salvage").
local CACHES, SEARCH_SECONDS = 3, 4
local cacheSpots=logic.points("salvage_cache")
local cacheModels={"v1_ToolChest_b","gear_backpack_01_heavy","ammo_box_rifle_01"}
-- Expedition: freakers guard the NERO site (editor points "expedition_zombie", else around "expedition"); once they
-- are dead a crate of medical supplies is there (editor point "expedition_meds", else the site itself).
local RAIDERS = 4
local raidSpots=logic.points("expedition_zombie")
local medsSpot=logic.point("expedition_meds")
local MEDS = "nero_meds"

local function alive(p) local s=p and p.valid and R.session(p); return s and not s.dead end
local function dist(p,pt) local x,y,z=p:position(); return math.sqrt((x-pt.x)^2+(y-pt.y)^2+(z-pt.z)^2)/100 end
local function nearRun(p,pt,radius) return alive(p) and dist(p,pt)<=radius end
local function partyKey(p) local s=squad.of(p); return s and ("s"..s.id) or ("p"..p.id) end
local function members(run) local list={} for id in pairs(run.members) do local q=getPlayer(id); if q and R.session(q) then list[#list+1]=q end end return list end

-- ---------------------------------------------------------------- the tutorial after the first delivery
-- Bike -> pistol -> expedition -> medical supplies to the clinic: the demo of the camp, step by step.
local TUTORIAL={
    bike={L("Обучение: купите мотоцикл в гараже","Tutorial: buy a motorcycle at the garage"),"garage",
        L("МОТОЦИКЛ","MOTORCYCLE"),L("Для вылазок нужен транспорт. Комендант выделил подъёмные — идите в гараж к механику и купите мотоцикл. E — сесть, /lock — запереть.","Trips outside need wheels. The commander gave you a starting allowance — go to the mechanic at the garage and buy a motorcycle. E: get on, /lock: lock it.")},
    pistol={L("Обучение: купите пистолет у торговца (Оружейная)","Tutorial: buy a pistol from the trader (Armory)"),"shop",
        L("ОРУЖИЕ","WEAPON"),L("За воротами фрики. У торговца откройте «Оружейная» и купите M9 с патронами.","Freakers roam outside the walls. Open the Armory at the trader and buy an M9 with ammo.")},
    expedition={L("Обучение: возьмите экспедицию NERO у коменданта","Tutorial: take the NERO expedition from the commander"),"office",
        L("ЭКСПЕДИЦИЯ","EXPEDITION"),L("У коменданта: «Задания лагеря» → «Экспедиция NERO». Можно одному или отрядом (F на игроке → «Пригласить в отряд»). На месте у вас будут свои фрики и свои медикаменты.","At the commander: Camp jobs → NERO expedition. Go alone or with a squad (F at a player → Invite to the squad). At the site the freakers and the supplies are yours alone.")},
}
function J.tutorial(p,stage)
    local a=R.acc(p); if not a then return end
    a.tutorial=stage; R.commit("tutorial",p,stage)
    local t=TUTORIAL[stage]
    if t then R.task(p,t[1],t[2]); R.hint(p,t[3],t[4])
    elseif stage=="done" then R.hint(p,L("ОБУЧЕНИЕ ПРОЙДЕНО","TUTORIAL COMPLETE"),L("Вы освоились в Рубеже. Дальше — задания лагеря у коменданта, профессии и смены, отряды и обмен (F на игроке). /help — все команды.","You know your way around Rubezh now. Next: camp jobs at the commander, professions and shifts, squads and trading (F at a player). /help lists every command.")) end
end
function J.tutorialAdvance(p,from,to)
    local a=R.acc(p)
    if a and a.tutorial==from then J.tutorial(p,to) end
end

-- ---------------------------------------------------------------- runs (salvage, expedition)
local function salvageTask(p,run)
    R.task(p,L.format(L("Обыщите тайники у точки: %d/%d","Search the caches at the site: %d/%d"),run.found,#run.caches),"salvage")
end
local function raidTask(p,run)
    if run.stage=="raid" then R.task(p,L.format(L("NERO: уничтожьте фриков у точки: %d/%d","NERO: kill the freakers at the site: %d/%d"),run.killed,run.raiders),"expedition")
    elseif (R.acc(p).bag[MEDS] or 0)>0 then R.task(p,L("Отнесите медикаменты NERO лекарю в медпункт","Bring the NERO medical supplies to the clinic"),"clinic")
    else R.task(p,L("NERO: район зачищен — заберите медикаменты на точке","NERO: the area is clear — take the medical supplies at the site"),"expedition") end
end
local function task(p,run) if run.kind=="salvage" then salvageTask(p,run) else raidTask(p,run) end end

local function destroyRun(run)
    for _,c in ipairs(run.caches or {}) do
        if c.object then removeObject(c.object) end
        if c.use then removeInteraction(c.use) end
    end
    for zid in pairs(run.zombies or {}) do removeZombie(zid) end
    if run.crate then removeObject(run.crate) end
    if run.crateUse then removeInteraction(run.crateUse) end
    for _,q in ipairs(players()) do if q.world==run.world then q:setWorld(0) end end
    freeWorld(run.world)
    J.runs[run.kind..":"..run.key]=nil
end

local function spawnCaches(run)
    local spots={}
    if #cacheSpots>0 then
        local pool={}; for i,pt in ipairs(cacheSpots) do pool[i]=pt end
        for _=1,math.min(CACHES,#pool) do spots[#spots+1]=table.remove(pool,math.random(#pool)) end
    else
        local base,turn=R.points.salvage,math.random()*2*math.pi
        for i=1,CACHES do
            local a=turn+i*2*math.pi/CACHES
            spots[i]={x=base.x+math.cos(a)*400,y=base.y+math.sin(a)*400,z=base.z,yaw=math.deg(a)}
        end
    end
    run.caches,run.found={},0
    for i,pt in ipairs(spots) do
        local c={x=pt.x,y=pt.y,z=pt.z}
        c.object=createObject({model=cacheModels[(i-1)%#cacheModels+1],x=pt.x,y=pt.y,z=pt.z,yaw=pt.yaw or 0,world=run.world})
        c.use=createInteraction({x=pt.x,y=pt.y,z=pt.z,radius=2,world=run.world,aimHeight=0.4,text={ru="Обыскать тайник",en="Search the cache"},onUse=function(q)
            local s=R.session(q)
            if not s or not s.job or s.job.run~=run then return end
            if c.done or s.searching or q.vehicle then return end
            if c.by and c.by~=q.id then return R.msg(q,L("Этот тайник уже обыскивает ваш напарник.","Your teammate is already searching this cache."),false) end
            c.by=q.id; s.searching={cache=c,since=now()}
            q:playAnim("dcn_crate_search",{loop=true,freeze=false})
        end})
        run.caches[#run.caches+1]=c
    end
end

local function raidCleared(run)
    -- The crate of medical supplies (one pack for each member).
    run.stage="meds"
    local pt=medsSpot.placed and medsSpot or R.points.expedition
    run.crate=createObject({model="v1_ToolChest_b",x=pt.x,y=pt.y,z=pt.z,yaw=pt.yaw or 0,world=run.world})
    run.taken={}
    run.crateUse=createInteraction({x=pt.x,y=pt.y,z=pt.z,radius=2.5,world=run.world,aimHeight=0.4,text={ru="Забрать медикаменты NERO",en="Take the NERO medical supplies"},onUse=function(q)
        local s=R.session(q)
        if not s or not s.job or s.job.run~=run then return end
        if run.taken[q.id] then return R.msg(q,L("Свою упаковку вы уже взяли.","You have taken your pack already."),false) end
        local a=R.acc(q)
        local ok,err=R.rules.add(C,a.bag,MEDS,1,C.bagWeight)
        if not ok then return R.msg(q,err,false) end
        run.taken[q.id]=true; q:playAnim("dcn_crate_search",{loop=false,freeze=false})
        R.commit("meds",q); raidTask(q,run)
        R.msg(q,L("Медикаменты NERO в рюкзаке. Отнесите их лекарю в медпункт лагеря.","The NERO medical supplies are in your backpack. Bring them to the camp clinic."))
    end})
    for _,q in ipairs(members(run)) do
        raidTask(q,run)
        R.msg(q,L("[РАЦИЯ] NERO: район зачищен. Заберите медикаменты у точки.","[RADIO] NERO: the area is clear. Take the medical supplies at the site."),"#7CD67C")
    end
end
local function raiderDown(run,zid)
    if not run.zombies[zid] then return end
    run.zombies[zid]=nil; run.killed=run.killed+1
    if run.killed<run.raiders then for _,q in ipairs(members(run)) do raidTask(q,run) end return end
    raidCleared(run)
end

local function spawnRaid(run)
    local base=R.points.expedition
    run.zombies,run.killed,run.raiders,run.stage={},0,0,"raid"
    for i=1,RAIDERS do
        local pt=raidSpots[(i-1)%math.max(1,#raidSpots)+1]
        if not pt then
            local a=i*2*math.pi/RAIDERS
            pt={x=base.x+math.cos(a)*900,y=base.y+math.sin(a)*900,z=base.z}
        end
        local zid=createZombie(pt.x,pt.y,pt.z+50,"Swarmer",math.random(0,359),run.world)
        if zid then run.zombies[zid]=true; run.raiders=run.raiders+1 end
    end
    if run.raiders==0 then raidCleared(run) end  -- Zombies=0 on this server: nothing to fight
end
on("zombieDeath",function(z)
    for _,run in pairs(J.runs) do if run.zombies and run.zombies[z.id] then raiderDown(run,z.id) return end end
end)

-- The player's run of this kind: its squad's one when there is, else a new one.
local function joinRun(p,kind)
    local key=partyKey(p)
    local run=J.runs[kind..":"..key]
    if not run then
        run={kind=kind,key=key,world=newWorld(),members={},expires=now()+C.missionSeconds,site=R.points[kind]}
        J.runs[kind..":"..key]=run
        if kind=="salvage" then spawnCaches(run) else spawnRaid(run) end
    end
    run.members[p.id]=true
    R.session(p).job={kind=kind,run=run,expires=run.expires}
    return run
end
local function leaveRun(p)
    local s=R.session(p); local run=s and s.job and s.job.run
    if not run then return end
    run.members[p.id]=nil
    if p.valid and p.world==run.world then p:setWorld(0) end
    if not next(run.members) then destroyRun(run) end
end

-- ---------------------------------------------------------------- jobs
local function cooldown(p,id)
    local left=(R.acc(p).cooldowns[id] or 0)-os.time()
    if left>0 then R.msg(p,L("Следующее задание через ","Next job available in ")..left..L(" с."," s."),false); return false end
    return true
end
local function available(p,id)
    local a,s=R.acc(p),R.session(p)
    if not a.intro then R.msg(p,L("Сначала получите пропуск у коменданта.","Register with the commander first."),false); return false end
    if s.job then R.msg(p,L("У вас уже есть активное задание.","You already have an active job."),false); return false end
    if a.hunger==0 or a.thirst==0 then R.msg(p,L("Перед работой поешьте и выпейте воду.","Eat and drink before starting work."),false); return false end
    return cooldown(p,id)
end
function J.cancel(p,quiet)
    local s=R.session(p); if not s or not s.job then return end
    local job=s.job
    if job.crate then J.crates[job.crate]=nil; removeCarry(job.crate) end
    if s.searching then s.searching.cache.by=nil; s.searching=nil end
    leaveRun(p)
    s.job=nil
    R.meter(p); p:stopAnim()
    R.task(p,nil,nil)
    if not quiet then R.msg(p,L("Задание отменено. Награда не начислена.","Job cancelled. No reward paid.")) end
end
local function finish(p,money,rep,label)
    local job=R.session(p).job; if not job then return end
    R.acc(p).cooldowns[job.kind]=os.time()+C.jobCooldown
    local first=job.kind=="cargo" and not R.acc(p).starterDone
    if job.kind=="cargo" then R.acc(p).starterDone=true end
    J.cancel(p,true)
    R.reward(p,money,rep,label)
    if first and not R.acc(p).tutorial then
        local a=R.acc(p)
        a.money=math.min(C.maxMoney,a.money+C.tutorialAllowance); R.commit("allowance",p,C.tutorialAllowance)
        R.msg(p,L("Подъёмные от коменданта: +$","Starting allowance from the commander: +$")..C.tutorialAllowance)
        after(3000,function() if p.valid then J.tutorial(p,"bike") end end)
    end
end
function J.start(p,kind)
    if kind~="cargo" and kind~="salvage" and kind~="patrol" then return end
    if not available(p,kind) or not R.at(p,kind=="cargo" and "warehouse" or "office",6) then return end
    local s=R.session(p)
    if kind=="cargo" then
        s.job={kind=kind,expires=now()+C.missionSeconds}
        local pt=R.points.cargo
        local id=createCarry({model="crate",x=pt.x+(p.id%4)*100,y=pt.y,z=pt.z,yaw=0,world=0,tag="outpost",roll="drop"})
        if not id then s.job=nil; return R.msg(p,L("Не удалось выдать груз.","Could not create the cargo."),false) end
        J.crates[id]=p.id; s.job.crate=id
        R.task(p,L("Поднимите свой ящик (держать F), отнесите на склад","Pick up your crate (hold F) and take it to the warehouse"),"cargo")
    elseif kind=="salvage" then
        local run=joinRun(p,"salvage")
        R.task(p,L("Доберитесь до точки поиска деталей и обыщите тайники","Reach the salvage site and search its caches"),"salvage")
        if next(run.members,next(run.members)) then R.msg(p,L("Вы присоединились к поиску своего отряда.","You joined your squad's salvage run.")) end
    elseif kind=="patrol" then
        if R.acc(p).role~="guard" or not s.duty then return R.msg(p,L("Патруль доступен охране на смене.","Only guards on duty can patrol."),false) end
        s.job={kind=kind,expires=now()+C.missionSeconds,stage=1}
        R.task(p,L("Патруль: проверьте первый пост","Patrol: inspect the first checkpoint"),"patrol1")
    end
end
-- Every second while the player searches a cache: done after SEARCH_SECONDS next to it.
local function searchTick(p,run)
    local s=R.session(p)
    local c=s.searching.cache
    if not nearRun(p,c,2.5) or c.done then c.by=nil; s.searching=nil; R.meter(p); p:stopAnim(); return end
    local progress=math.min(1,(now()-s.searching.since)/SEARCH_SECONDS)
    R.meter(p,L("Обыск тайника","Searching the cache"),progress)
    if progress<1 then return end
    local a=R.acc(p)
    s.searching=nil; c.by=nil; R.meter(p); p:stopAnim()
    if R.rules.weight(C,a.bag)+1>C.bagWeight then return R.msg(p,L("Рюкзак полон: освободите 1 кг.","Your backpack is full: free up 1 kg."),false) end
    R.rules.add(C,a.bag,"scrap",1,C.bagWeight)
    c.done=true; run.found=run.found+1
    if c.use then removeInteraction(c.use); c.use=nil end
    if c.object then removeObject(c.object); c.object=nil end
    if run.found<#run.caches then
        R.msg(p,L("Детали найдены: +1. Осталось тайников: ","Parts found: +1. Caches left: ")..(#run.caches-run.found))
        for _,q in ipairs(members(run)) do salvageTask(q,run) end
    else
        R.state.camp.parts=R.state.camp.parts+2
        for _,q in ipairs(members(run)) do finish(q,95,4,L("Детали найдены","Parts found")) end
    end
end
function J.referCargo(p)
    if not available(p,"cargo") then return end
    R.task(p,L("Получите работу у кладовщика","Ask the quartermaster for work"),"warehouse")
    R.hint(p,L("ПЕРВАЯ РАБОТА","FIRST JOB"),L("Обратитесь к кладовщику на складе и выберите «Начать работу». Он выдаст задание на доставку ящика.","Speak to the quartermaster at the warehouse and select Start work. He will give you a crate delivery job."))
end
function J.board(p)
    R.menu(p,L("Задания лагеря","Camp jobs"),{L("Снабжение · физический ящик · $65 / 3 реп.","Supply delivery · physical crate · $65 / 3 rep."),L("Поиск деталей · выезд и обыск · $95 / 4 реп.","Salvage parts · travel and search · $95 / 4 rep."),L("Патруль охраны · два поста · $110 / 5 реп.","Guard patrol · two checkpoints · $110 / 5 rep."),L("Экспедиция NERO · фрики и медикаменты · $180 / 8 реп.","NERO expedition · freakers and medical supplies · $180 / 8 rep.")},function(q,i)
        if i==1 then J.referCargo(q) elseif i==4 then J.join(q) else J.start(q,({"cargo","salvage","patrol"})[i]) end
    end,"office")
end
function J.join(p)
    if not available(p,"expedition") or not R.at(p,"office",6) then return end
    local run=joinRun(p,"expedition")
    raidTask(p,run)
    if run.stage=="raid" and run.killed==0 then R.task(p,L("NERO: доберитесь до района, его охраняют фрики","NERO: reach the area, freakers guard it"),"expedition") end
    R.msg(p,L("Возьмите оружие. У точки NERO фрики: перебейте их, заберите медикаменты и отнесите лекарю в медпункт. С отрядом — одна вылазка на всех, у каждого своя упаковка.","Take a weapon. Freakers guard the NERO site: kill them, take the medical supplies and bring them to the clinic. With a squad it is one run for all, a pack for each."))
    J.tutorialAdvance(p,"expedition","expedition_run")
end
-- The clinic: medical supplies from an expedition (the clinic NPC's menu).
function J.hasMeds(p) local a=R.acc(p); return a and (a.bag[MEDS] or 0)>0 end
function J.deliverMeds(p)
    local a=R.acc(p)
    if (a.bag[MEDS] or 0)<1 then return R.msg(p,L("У вас нет медикаментов NERO.","You have no NERO medical supplies."),false) end
    R.rules.take(a.bag,MEDS,1)
    R.state.camp.medicine=R.state.camp.medicine+8
    R.state.camp.treasury=R.state.camp.treasury+50
    local s=R.session(p)
    if s.job and s.job.kind=="expedition" then finish(p,180,8,L("Экспедиция: медикаменты доставлены","Expedition: medical supplies delivered"))
    else R.reward(p,120,5,L("Медикаменты NERO сданы","NERO medical supplies handed in")) end
    R.broadcast(L("Медпункт: ","Clinic: ")..p.name..L(" доставил медикаменты NERO."," delivered NERO medical supplies."),"#7CD67C")
    J.tutorialAdvance(p,"expedition_run","done")
end
on("carryPick",function(p,id)
    if J.crates[id] then
        local s=R.session(p)
        -- Only its job's player: no one else picks it up, shelved or knocked out of the owner's hands alike.
        if J.crates[id]~=p.id then R.msg(p,L("Это чужой груз: его доставляет другой игрок.","This is someone else's cargo: another player is delivering it."),false); return false end
        return R.ready(p) and s.job and s.job.crate==id
    end
end)
on("carryDrop",function(p,id,info)
    if not J.crates[id] then return end
    local s=R.session(p); local job=s and s.job
    if not job or job.crate~=id or not R.ready(p) then return end
    if info.reason=="put" and R.near(p,R.points.warehouse,7) then
        local pt=R.points.warehouse
        if (info.x-pt.x)^2+(info.y-pt.y)^2+(info.z-pt.z)^2>700^2 then return end
        if not job.picked or now()-job.picked<3 then return false end
        R.state.camp.food=R.state.camp.food+4; R.state.camp.fuel=R.state.camp.fuel+1
        finish(p,65,3,L("Груз доставлен","Cargo delivered"))
    end
end)
-- A squad member who leaves the squad keeps its run alone (a new key would split the run's world).
every(1000,function()
    for _,run in pairs(J.runs) do
        if now()>=run.expires then
            for _,q in ipairs(members(run)) do J.cancel(q,true); R.msg(q,L("Время задания вышло.","The job has run out of time."),false) end
            if J.runs[run.kind..":"..run.key] then destroyRun(run) end
        elseif run.zombies and run.stage=="raid" then
            -- A raider gone without a zombieDeath (removed, forgotten) counts as killed: the raid cannot stall.
            for zid in pairs(run.zombies) do local z=getZombie(zid); if not z or z.dead then raiderDown(run,zid) end end
        end
    end
    for _,p in ipairs(players()) do
        local s=R.session(p); local job=s and s.job
        if job and alive(p) then
            if now()>job.expires then J.cancel(p); goto continue end
            local run=job.run
            if run then
                -- Into the run's world at the site, back to the shared one away from it.
                local d=dist(p,run.site)
                if p.world==0 and d<=ENTER then
                    p:setWorld(run.world)
                    p:tip(L.resolve(L("Место задания: здесь только вы и ваш отряд","The job site: only you and your squad are here"),p),4)
                    task(p,run)
                    if s.siteHint==run then -- (once per run)
                    elseif run.kind=="salvage" then s.siteHint=run; R.hint(p,L("ПОИСК ДЕТАЛЕЙ","SALVAGE"),L("Рядом с точкой спрятаны тайники. Найдите их и обыщите (F).","Caches are hidden around the site. Find them and search them (F)."))
                    elseif run.stage=="raid" then s.siteHint=run; R.hint(p,L("ЭКСПЕДИЦИЯ NERO","NERO EXPEDITION"),L("Фрики охраняют точку. Убейте их, затем заберите медикаменты (F у ящика).","Freakers guard the site. Kill them, then take the medical supplies (F at the crate).")) end
                elseif p.world==run.world and d>LEAVE then p:setWorld(0) end
                if s.searching then searchTick(p,run) end
            elseif job.kind=="cargo" then
                if p.carrying==job.crate and not job.picked then
                    job.picked=now(); R.task(p,L("Поставьте ящик на площадку снабжения (держать F)","Put the crate down at the supply warehouse (hold F)"),"warehouse")
                end
            elseif job.kind=="patrol" and R.ready(p) then
                local point=job.stage==1 and "patrol1" or "patrol2"
                if R.near(p,R.points[point],12) then
                    job.since=job.since or now()
                    R.meter(p,L("Осмотр поста","Inspecting the checkpoint"),math.min(1,(now()-job.since)/5))
                    if now()-job.since>=5 then
                        R.meter(p); job.since=nil
                        if job.stage==1 then job.stage=2; R.task(p,L("Патруль: проверьте второй пост","Patrol: inspect the second checkpoint"),"patrol2")
                        else finish(p,110,5,L("Патруль завершён","Patrol completed")) end
                    end
                elseif job.since then job.since=nil; R.meter(p) end
            end
        end
        ::continue::
    end
end)
-- A death ends a crate delivery or a patrol; a run (salvage, expedition) goes on after the respawn.
R.hooks[#R.hooks+1]=function(p,why)
    local s=R.session(p)
    if why~="death" or not (s and s.job and s.job.run) then J.cancel(p,true) end
end
-- The tutorial goes on for those who left in the middle; the voice chat is explained once.
on("playerJoin",function(p)
    after(9000,function()
        if not p.valid then return end
        local a=R.acc(p); if not a then return end
        if not a.voiceHint then
            a.voiceHint=true; R.commit()
            R.hint(p,L("ГОЛОСОВОЙ ЧАТ","VOICE CHAT"),L("N — говорить с теми, кто рядом. K — в рацию (купите рацию у торговца, /radio 101). Tab — список игроков: там можно заглушить любого и открыть настройки микрофона (/voice).","N: talk to those nearby. K: talk on the radio (buy a radio from the trader, /radio 101). Tab: the player list, where you can mute anyone and open the microphone settings (/voice)."))
        elseif a.tutorial and TUTORIAL[a.tutorial] then J.tutorial(p,a.tutorial)
        elseif a.starterDone and a.tutorial==nil and not a.bike then J.tutorial(p,"bike") end
    end)
end)
R.command("jobs",function(p) if R.at(p,"office",6) then J.board(p) end end)
R.command("cancel",function(p) J.cancel(p) end)
R.command("mission",function(p,_,action)
    if action=="join" then return J.join(p) end
    local s=R.session(p); local run=s.job and s.job.run
    if not run then return R.msg(p,L("Нет активной вылазки. Экспедиция NERO — у коменданта («Задания лагеря»), можно одному или отрядом.","No active run. The NERO expedition is at the command office (Camp jobs), alone or with a squad.")) end
    local names={}; for _,q in ipairs(members(run)) do names[#names+1]=q.name end
    R.msg(p,L("Вылазка: осталось ","Run: time remaining ")..math.floor(run.expires-now())..L(" с · участники: "," s · members: ")..table.concat(names,", "))
end)
return J
