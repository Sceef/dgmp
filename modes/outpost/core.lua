local L=require("i18n")
local C, D = require("config"), require("domain")
local R = { cfg = C, rules = D, sessions = {}, points = {}, hooks = {} }
function R.message(p,text,color) p:message(L.resolve(text,p),color) end
function R.dialog(p,spec) if not p.carrying then p:dialog(L.deep(spec,p)) end end
function R.hint(p,title,text,...) p:hint(L.resolve(title,p),L.resolve(text,p),...) end
function R.items(p,spec,fn) if not p.carrying then p:itemWindow(L.deep(spec,p),fn) end end
function R.meter(p,text,value) p:objectiveMeter(L.resolve(text,p),value) end
function R.customize(p,spec) p:customize(L.deep(spec,p)) end
function R.kick(p,reason) p:kick(L.resolve(reason,p)) end
function R.ban(p,reason,minutes) p:ban(L.resolve(reason,p),minutes) end
function R.broadcast(text,color)
    for _,p in ipairs(players()) do R.message(p,text,color) end
end
R.state = storage.get("outpost:v1") or D.newState()
assert(R.state.version == C.version, "Unsupported outpost save version: back up storage before migration")
local logic = require("logic")
-- Literal names allow the DGMP editor to discover the mode's points.
local placed = { spawn = logic.point("spawn"), office = logic.point("office"), shop = logic.point("shop"),
    clinic = logic.point("clinic"), garage = logic.point("garage"), warehouse = logic.point("warehouse"),
    cargo = logic.point("cargo"), salvage = logic.point("salvage"), patrol1 = logic.point("patrol1"),
    patrol2 = logic.point("patrol2"), expedition = logic.point("expedition"), helipad = logic.point("helipad") }
for name, fallback in pairs(C.points) do
    R.points[name] = R.state.points[name] or (placed[name].placed and placed[name]) or fallback
end
local wardrobe=logic.point("wardrobe")
if wardrobe.placed then R.points.wardrobe=wardrobe end
-- Where the dead come back: the editor's "clinic_spawn", else 2 m in front of the medic (the clinic
-- point is where the medic NPC stands: a body put there is inside it).
local revive=logic.point("clinic_spawn")
if revive.placed then R.points.revive=revive
else
    local c=R.points.clinic; local a=math.rad(c.yaw or 0)
    R.points.revive={x=c.x+math.cos(a)*200,y=c.y+math.sin(a)*200,z=c.z,yaw=((c.yaw or 0)+180)%360}
end
-- Independent parking bays, placed on the garage floor; their yaw is the bike's heading.
R.garageSpawns=logic.points("garage_spawn")
if #R.garageSpawns==0 then log("[outpost] Garage issuance disabled: place garage_spawn in the mode editor.") end
function R.msg(p, text, good) R.message(p,text, good == false and "#E07070" or "#D9C898") end
function R.session(p) return R.sessions[p.id] end
function R.acc(p) local s = R.session(p); return s and s.account end
function R.ready(p)
    return p and p.valid and not p.bot and R.session(p) and not R.session(p).dead and p.world == 0
end
function R.distance(p, point)
    local x, y, z = p:position()
    return math.sqrt((x - point.x)^2 + (y - point.y)^2 + (z - point.z)^2) / 100
end
function R.near(p, point, radius) return R.ready(p) and R.distance(p, point) <= (radius or 4) end
function R.at(p, name, radius)
    if R.near(p, R.points[name], radius) then return true end
    R.mark(p,name); R.msg(p, L("Подойдите к отмеченной точке.","Go to the marked location."), false)
    return false
end
function R.peer(p, who, radius)
    local t = who and getPlayer(tonumber(who) or who)
    if not R.ready(t) or t == p or t.world ~= p.world then R.msg(p, L("Нет подходящего игрока рядом.","No suitable player nearby."), false); return end
    local x, y, z = t:position()
    if not R.near(p, { x=x, y=y, z=z }, radius or 5) then R.msg(p, L("Подойдите ближе к игроку.","Move closer to the player."), false); return end
    return t
end
function R.hud(p)
    local s, a = R.session(p), R.acc(p)
    if not a or s.cinema then p:hud("outpost"); return end
    p:hud("outpost", L.deep({ x=.025, y=.15, size=1.0, bg="#101818B0", text=(
        L("{D9C898}РУБЕЖ{FFFFFF}  |  %s%s\n$%d  ·  Банк $%d  ·  Репутация %d\nСытость %d%%  ·  Вода %d%%\n{A4B5AD}%s","{D9C898}RUBEZH{FFFFFF}  |  %s%s\n$%d  ·  Bank $%d  ·  Reputation %d\nHunger %d%%  ·  Water %d%%\n{A4B5AD}%s"))
        :format(C.roles[a.role].name, s.duty and L(" · на смене"," · on duty") or "", a.money, a.bank, a.rep,
        a.hunger, a.thirst, s.taskText or (a.intro and L("Лагерь ждёт снабжения","The camp needs supplies") or L("Первый шаг: зарегистрируйтесь у коменданта","First step: register with the commander"))) },p))
end
function R.sync(p) local a = R.acc(p); if a then p:setMoney(a.money); R.hud(p) end end
function R.commit(kind, p, detail)
    if detail then detail=tostring(detail) end
    if kind then
        D.record(R.state, kind, p and D.identity(p) or "server", detail or "", os.time())
        log("[outpost] " .. kind .. " " .. (p and p.name or "server") .. " " .. (detail or ""))
    end
    storage.set("outpost:v1", R.state)
    for _, q in ipairs(players()) do if R.acc(q) then R.sync(q) end end
end
function R.result(p, ok, err, kind, detail)
    if not ok then R.msg(p, err or L("Действие недоступно.","This action is unavailable."), false); return false end
    R.commit(kind, p, detail)
    return true
end
function R.reward(p, money, rep, reason)
    local a = R.acc(p)
    a.money, a.rep = math.min(C.maxMoney, a.money + money), a.rep + rep
    R.commit("reward", p, reason .. ": " .. money)
    R.msg(p, reason .. ": +$" .. money .. L(", репутация +",", reputation +") .. rep)
end
function R.command(name, fn, admin)
    command(name, function(p, args, who)
        if not R.ready(p) then return end
        if admin and not p.admin then return R.msg(p, L("Доступно только администратору.","Administrators only."), false) end
        fn(p, args or "", who)
    end)
end
-- Revalidate readiness and location when the response arrives, not only when opening.
function R.menu(p, title, rows, fn, place)
    if not R.ready(p) or p.carrying then return end
    local s = R.session(p)
    s.menu = (s.menu or 0) + 1
    local nonce = s.menu
    R.dialog(p,{ title=title, style="list", items=rows, button1=L("Выбрать","Select"), button2=L("Закрыть","Close"),
        onResponse=function(q, ok, index)
            if not R.ready(q) or q.carrying or R.session(q) ~= s or s.menu ~= nonce then return end
            s.menu = nonce + 1
            if not ok or not D.integer(index, 1, #rows) then return end
            if place and not R.at(q, place, 5) then return end
            fn(q, index)
        end })
end
function R.mark(p, name, title)
    local s, point = R.session(p), R.points[name]
    if s.marker then removeMarker(s.marker); s.marker = nil end
    if point then s.marker = createMarker({ x=point.x, y=point.y, z=point.z, world=0, player=p,
        title=L.resolve(title or name,p), radius=5, track=false, route=true, category="task", style="basic" }) end
end
function R.task(p, text, point)
    R.session(p).taskText = text
    R.mark(p, point, text)
    R.hud(p)
end
function R.cleanup(p, why)
    for _, fn in ipairs(R.hooks) do fn(p, why) end
    local s = R.session(p)
    if s and s.marker then removeMarker(s.marker); s.marker = nil end
end
on("playerJoin", function(p)
    if p.bot then return end
    local key = D.identity(p)
    for _, s in pairs(R.sessions) do
        if s.key == key then R.kick(p,L("Этот персонаж уже в игре.","This character is already in the game.")); return end
    end
    local a = R.state.accounts[key] or D.account(C, os.time())
    R.state.accounts[key] = a
    local s = { key=key, account=a, last=now(), active=now(), needs=0 }
    R.sessions[p.id] = s
    p:setUniform(nil); p:takeBike(); p:setWorld(0); p:setVoiceMuted(false)
    local pt = a.pos or R.points.spawn
    p:setPosition(pt.x, pt.y, pt.z + (a.pos and 0 or 100), pt.yaw or 0)
    if (a.bag.radio or 0) > 0 then p:setVoiceChannel("Rubezh 101") end
    R.sync(p)
    -- The mode account owns the base appearance; work uniforms remain temporary.
    if a.skin then p:setSkin(a.skin)
    else
        p:setSkin("survivor")
        R.customize(p,{title=L("Создание персонажа","Create your character"),cancel=false,list="skins"})
    end
    if not a.intro then R.task(p, L("Зарегистрируйтесь у коменданта","Register with the commander"), "office") end
    if a.intro and not a.starterDone then
        R.task(p,L("Получите работу у кладовщика","Ask the quartermaster for work"),"warehouse")
    end
    R.commit()
end)
on("customize",function(p,look)
    if look and R.acc(p) then R.acc(p).skin=p.skin; R.commit("appearance",p) end
end)
on("playerQuit", function(p)
    local s = R.session(p); if not s then return end
    if not s.dead and p.world == 0 then
        local x,y,z,yaw=p:position(); s.account.pos={x=x,y=y,z=z,yaw=yaw}
    end
    R.cleanup(p, "quit"); R.sessions[p.id] = nil; R.commit()
end)
on("playerDeath", function(p)
    local s = R.session(p); if not s then return end
    s.dead = true; s.account.pos = nil; s.menu = (s.menu or 0) + 1
    s.duty = false; s.taskText = nil
    p:setVoiceMuted(true); p:setUniform(nil)
    R.cleanup(p, "death"); R.commit()
end)
on("playerRespawn", function(p)
    local s = R.session(p); if not s then return end
    local a, pt = s.account, R.points.revive
    local fee = math.min(a.money, C.respawnFee)
    a.money = a.money - fee; R.state.camp.treasury = R.state.camp.treasury + fee
    a.hunger, a.thirst = math.max(a.hunger, 40), math.max(a.thirst, 40)
    s.dead = false; s.last = now()
    p:setWorld(0); p:setPosition(pt.x,pt.y,pt.z+100,pt.yaw); p:setVoiceMuted(false)
    R.commit("respawn",p,tostring(fee)); R.msg(p,L("Вы в медпункте. Лечение: $","You are at the clinic. Treatment: $") .. fee)
    return true
end)
on("stop", function()
    for _, p in ipairs(players()) do
        local s=R.session(p)
        if s then
            if not s.dead and p.world == 0 then local x,y,z,yaw=p:position(); s.account.pos={x=x,y=y,z=z,yaw=yaw} end
            R.cleanup(p,"stop")
        end
    end
    R.commit(); storage.save()
end)
every(10000, function()
    for _, p in ipairs(players()) do
        local s = R.session(p)
        if s then
            local dt=math.max(0,math.min(20,now()-s.last)); s.last=now()
            local x,y,z=p:position()
            if s.previous and (x-s.previous.x)^2+(y-s.previous.y)^2+(z-s.previous.z)^2 > 100^2 then s.active=now() end
            s.previous={x=x,y=y,z=z}
            if R.ready(p) and now()-s.active < 120 then
                local a=s.account
                a.played=a.played+dt; a.payday=a.payday+dt; s.needs=s.needs+dt
                if a.payday >= C.paydaySeconds then
                    a.payday=a.payday-C.paydaySeconds
                    R.reward(p,C.payday*R.state.camp.level,1,L("Выплата за активность","Activity payment"))
                end
                if s.needs >= C.needsSeconds then
                    s.needs=s.needs-C.needsSeconds
                    a.hunger=math.max(0,a.hunger-3); a.thirst=math.max(0,a.thirst-5)
                    if a.hunger == 0 or a.thirst == 0 then R.msg(p,L("Запасы сил иссякли. Поешьте и выпейте воду из рюкзака.","You are exhausted. Eat and drink from your backpack."),false) end
                end
            end
        end
    end
    R.commit()
end)
return R
