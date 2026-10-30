local L=require("i18n")
local R=require("core")
local C=R.cfg
local logic=require("logic")
local J={ crates={}, mission=nil }
-- Salvage: caches to search around the site (editor points "salvage_cache", else a ring around "salvage").
local CACHES, SEARCH_SECONDS = 3, 4
local cacheSpots=logic.points("salvage_cache")
local cacheModels={"v1_ToolChest_b","gear_backpack_01_heavy","ammo_box_rifle_01"}
-- Expedition: freakers guard the NERO site (editor points "expedition_zombie", else around "expedition").
local RAIDERS, RAID_RANGE = 4, 6000
local raidSpots=logic.points("expedition_zombie")
local function clearCaches(job)
    for _,c in ipairs(job.caches or {}) do
        if c.object then removeObject(c.object) end
        if c.use then removeInteraction(c.use) end
    end
    job.caches=nil; job.searching=nil
end
local function clearRaid(m)
    for zid in pairs(m.zombies or {}) do removeZombie(zid) end
    m.zombies={}
end
local function salvageTask(p,job)
    R.task(p,L.format(L("Обыщите тайники у точки: %d/%d","Search the caches at the site: %d/%d"),job.found,#job.caches),"salvage")
end
local function raidTask(p,m)
    if m.cleared then R.task(p,L("NERO: район зачищен, найдите медикаменты","NERO: the area is clear, search for medical supplies"),"expedition")
    else R.task(p,L.format(L("NERO: уничтожьте фриков у точки: %d/%d","NERO: kill the freakers at the site: %d/%d"),m.killed,m.raiders),"expedition") end
end
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
    local job=s.job; s.job=nil
    if job.crate then J.crates[job.crate]=nil; removeCarry(job.crate) end
    clearCaches(job)
    if J.mission then J.mission.members[p.id]=nil end
    R.meter(p); p:stopAnim()
    R.task(p,nil,nil)
    if not quiet then R.msg(p,L("Задание отменено. Награда не начислена.","Job cancelled. No reward paid.")) end
end
local function finish(p,money,rep,label)
    local job=R.session(p).job; if not job then return end
    R.acc(p).cooldowns[job.kind]=os.time()+C.jobCooldown
    if job.kind=="cargo" then R.acc(p).starterDone=true end
    J.cancel(p,true)
    R.reward(p,money,rep,label)
end
function J.start(p,kind)
    if kind~="cargo" and kind~="salvage" and kind~="patrol" then return end
    if not available(p,kind) or not R.at(p,kind=="cargo" and "warehouse" or "office",6) then return end
    local s=R.session(p)
    s.job={kind=kind,expires=now()+C.missionSeconds}
    if kind=="cargo" then
        local pt=R.points.cargo
        local id=createCarry({model="crate",x=pt.x+(p.id%4)*100,y=pt.y,z=pt.z,yaw=0,world=0,tag="outpost",roll="drop"})
        if not id then s.job=nil; return R.msg(p,L("Не удалось выдать груз.","Could not create the cargo."),false) end
        J.crates[id]=p.id; s.job.crate=id
        R.task(p,L("Поднимите свой ящик (держать F), отнесите на склад","Pick up your crate (hold F) and take it to the warehouse"),"cargo")
    elseif kind=="salvage" then R.task(p,L("Доберитесь до точки поиска деталей и обыщите тайники","Reach the salvage site and search its caches"),"salvage")
    elseif kind=="patrol" then
        if R.acc(p).role~="guard" or not s.duty then s.job=nil; return R.msg(p,L("Патруль доступен охране на смене.","Only guards on duty can patrol."),false) end
        s.job.stage=1; R.task(p,L("Патруль: проверьте первый пост","Patrol: inspect the first checkpoint"),"patrol1")
    else s.job=nil end
end
local function spawnCaches(p,job)
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
    job.caches,job.found={},0
    for i,pt in ipairs(spots) do
        local c={x=pt.x,y=pt.y,z=pt.z}
        c.object=createObject({model=cacheModels[(i-1)%#cacheModels+1],x=pt.x,y=pt.y,z=pt.z,yaw=pt.yaw or 0,world=0})
        c.use=createInteraction({x=pt.x,y=pt.y,z=pt.z,radius=2,world=0,text={ru="Обыскать",en="Search"},onUse=function(q)
            local s=R.session(q)
            if not s or s.job~=job then return R.msg(q,L("Этот тайник обыскивает другой игрок.","Another player is searching this cache."),false) end
            if c.done or job.searching or q.vehicle or q:isOnBike() then return end
            job.searching={cache=c,since=now()}
            q:playAnim("dcn_crate_search",{loop=true,freeze=false})
        end})
        job.caches[#job.caches+1]=c
    end
    salvageTask(p,job)
    R.hint(p,L("ПОИСК ДЕТАЛЕЙ","SALVAGE"),L("Рядом с точкой спрятаны тайники. Найдите их и обыщите (F).","Caches are hidden around the site. Find them and search them (F)."))
end
-- Every second while the player searches a cache: done after SEARCH_SECONDS next to it.
local function searchTick(p,job)
    local c=job.searching.cache
    if not R.near(p,c,2.5) then job.searching=nil; R.meter(p); p:stopAnim(); return end
    local progress=math.min(1,(now()-job.searching.since)/SEARCH_SECONDS)
    R.meter(p,L("Обыск тайника","Searching the cache"),progress)
    if progress<1 then return end
    local a=R.acc(p)
    job.searching=nil; R.meter(p); p:stopAnim()
    if R.rules.weight(C,a.bag)+1>C.bagWeight then return R.msg(p,L("Рюкзак полон: освободите 1 кг.","Your backpack is full: free up 1 kg."),false) end
    R.rules.add(C,a.bag,"scrap",1,C.bagWeight)
    c.done=true; job.found=job.found+1
    if c.use then removeInteraction(c.use); c.use=nil end
    if c.object then removeObject(c.object); c.object=nil end
    if job.found<#job.caches then
        R.msg(p,L("Детали найдены: +1. Осталось тайников: ","Parts found: +1. Caches left: ")..(#job.caches-job.found)); salvageTask(p,job)
    else
        R.state.camp.parts=R.state.camp.parts+2
        finish(p,95,4,L("Детали найдены","Parts found"))
    end
end
local function spawnRaiders(m)
    local base=R.points.expedition
    m.zombies,m.killed,m.raiders={},0,0
    for i=1,RAIDERS do
        local pt=raidSpots[(i-1)%math.max(1,#raidSpots)+1]
        if not pt then
            local a=i*2*math.pi/RAIDERS
            pt={x=base.x+math.cos(a)*900,y=base.y+math.sin(a)*900,z=base.z}
        end
        local zid=createZombie(pt.x,pt.y,pt.z+50,"Swarmer",math.random(0,359),0)
        if zid then m.zombies[zid]=true; m.raiders=m.raiders+1 end
    end
    m.spawned=true
    m.cleared=m.raiders==0  -- Zombies=0 on this server: nothing to fight, the search goes on as before
    for _,q in pairs(m.members) do if R.session(q) then raidTask(q,m) end end
end
local function raiderDown(zid)
    local m=J.mission
    if not m or not m.zombies or not m.zombies[zid] then return end
    m.zombies[zid]=nil; m.killed=m.killed+1
    if m.killed>=m.raiders then m.cleared=true end
    for _,q in pairs(m.members) do if R.session(q) then raidTask(q,m) end end
    if m.cleared then R.broadcast(L("[РАЦИЯ] NERO: район зачищен. Ищите медикаменты у точки.","[RADIO] NERO: the area is clear. Search for medical supplies at the site."),"#7CD67C") end
end
on("zombieDeath",function(z) raiderDown(z.id) end)
function J.referCargo(p)
    if not available(p,"cargo") then return end
    R.task(p,L("Получите работу у кладовщика","Ask the quartermaster for work"),"warehouse")
    R.hint(p,L("ПЕРВАЯ РАБОТА","FIRST JOB"),L("Обратитесь к кладовщику на складе и выберите «Начать работу». Он выдаст задание на доставку ящика.","Speak to the quartermaster at the warehouse and select Start work. He will give you a crate delivery job."))
end
function J.board(p)
    R.menu(p,L("Задания лагеря","Camp jobs"),{L("Снабжение · физический ящик · $65 / 3 реп.","Supply delivery · physical crate · $65 / 3 rep."),L("Поиск деталей · выезд и обыск · $95 / 4 реп.","Salvage parts · travel and search · $95 / 4 rep."),L("Патруль охраны · два поста · $110 / 5 реп.","Guard patrol · two checkpoints · $110 / 5 rep."),L("Экспедиция NERO · совместная вылазка · $180 / 8 реп.","NERO expedition · cooperative mission · $180 / 8 rep.")},function(q,i)
        if i==1 then J.referCargo(q) elseif i==4 then J.join(q) else J.start(q,({"cargo","salvage","patrol"})[i]) end
    end,"office")
end
function J.join(p)
    if not available(p,"expedition") or not R.at(p,"office",6) then return end
    if not J.mission then
        J.mission={members={},expires=now()+C.missionSeconds,delivered=0}
        R.broadcast(L("Экспедиция NERO собирается у коменданта. Общий канал рации — 101.","A NERO expedition is forming at the command office. Shared radio channel 101."),"#D9C898")
    end
    local m=J.mission
    if m.expires-now()<120 then return R.msg(p,L("Экспедиция завершается. Дождитесь следующей.","The expedition is ending. Wait for the next one."),false) end
    local n=0; for _ in pairs(m.members) do n=n+1 end
    if n>=8 then return R.msg(p,L("Отряд укомплектован (8 участников).","The squad is full (8 members)."),false) end
    m.members[p.id]=p
    R.session(p).job={kind="expedition",stage="search",expires=m.expires,mission=m}
    if m.spawned then raidTask(p,m)
    else R.task(p,L("NERO: доберитесь до района, его охраняют фрики","NERO: reach the area, freakers guard it"),"expedition") end
    R.msg(p,L("Возьмите оружие и рацию. У точки NERO фрики: перебейте их, затем обыщите район ","Take a weapon and a radio. Freakers guard the NERO site: kill them, then search the area for ")..C.jobSeconds..L(" с и верните медикаменты на склад."," s and bring the medical supplies to the warehouse."))
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
every(1000,function()
    if J.mission and now()>=J.mission.expires then
        local old=J.mission; J.mission=nil; clearRaid(old)
        for _,p in pairs(old.members) do if R.session(p) then J.cancel(p,true); R.msg(p,L("Время экспедиции вышло.","The expedition has run out of time."),false) end end
    end
    -- A raider gone without a zombieDeath (removed, forgotten) counts as killed: the raid cannot stall.
    local m=J.mission
    if m and m.spawned and not m.cleared then
        for zid in pairs(m.zombies) do
            local z=getZombie(zid)
            if not z or z.dead then raiderDown(zid) end
        end
    end
    for _,p in ipairs(players()) do
        local s=R.session(p); local job=s and s.job
        if job and R.ready(p) then
            if now()>job.expires then J.cancel(p); goto continue end
            if job.kind=="cargo" then
                if p.carrying==job.crate and not job.picked then
                    job.picked=now(); R.task(p,L("Поставьте ящик на площадку снабжения (держать F)","Put the crate down at the supply warehouse (hold F)"),"warehouse")
                end
            elseif job.kind=="salvage" then
                if job.searching then searchTick(p,job)
                elseif not job.caches and R.near(p,R.points.salvage,40) then spawnCaches(p,job) end
            elseif job.kind=="expedition" and job.stage=="search" then
                local m=job.mission
                if not m.spawned and R.near(p,R.points.expedition,RAID_RANGE/100) then spawnRaiders(m) end
                if m.cleared and R.near(p,R.points.expedition,10) and not p.vehicle and not p:isOnBike() then
                    job.since=job.since or now()
                    local progress=math.min(1,(now()-job.since)/C.jobSeconds)
                    R.meter(p,L("Поиск медикаментов","Searching for medical supplies"),progress)
                    if progress>=1 then
                        R.meter(p)
                        job.stage="return"; R.task(p,L("Медикаменты найдены: доставьте их на склад лагеря","Medical supplies found: deliver them to the camp warehouse"),"warehouse")
                    end
                elseif job.since then job.since=nil; R.meter(p) end
            elseif job.kind=="expedition" and job.stage=="return" then
                if R.near(p,R.points.warehouse,6) then
                    local m=job.mission
                    m.delivered=m.delivered+1
                    R.state.camp.medicine=R.state.camp.medicine+8
                    R.state.camp.treasury=R.state.camp.treasury+50
                    finish(p,180,8,L("Экспедиция: медикаменты доставлены","Expedition: medical supplies delivered"))
                    R.broadcast(L("Экспедиция NERO: ","NERO expedition: ")..p.name..L(" доставил медикаменты. Всего партий: "," delivered medical supplies. Total shipments: ")..m.delivered,"#7CD67C")
                end
            elseif job.kind=="patrol" then
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
    if J.mission and not next(J.mission.members) then clearRaid(J.mission); J.mission=nil end
end)
R.hooks[#R.hooks+1]=function(p) J.cancel(p,true) end
R.command("jobs",function(p) if R.at(p,"office",6) then J.board(p) end end)
R.command("cancel",function(p) J.cancel(p) end)
R.command("mission",function(p,_,action)
    if action=="join" then return J.join(p) end
    local m=J.mission
    if not m then return R.msg(p,L("Нет активной экспедиции. /mission join у коменданта — собрать отряд (можно одному).","No active expedition. Join at the command office to form a team; solo expeditions are allowed.")) end
    R.msg(p,L("Экспедиция: осталось ","Expedition: time remaining ")..math.floor(m.expires-now())..L(" с; доставлено партий "," s; shipments delivered: ")..m.delivered..". /mission join")
    for id,q in pairs(m.members) do R.msg(p,"["..id.."] "..q.name) end
end)
return J
