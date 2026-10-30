local L=require("i18n")
-- Optional staging tools. All commands are admin-only; none run on normal joins.
local R=require("core")
local M={horde=nil,heli=nil,trap=nil,flight=false}
local function clear()
    if M.horde then removeHorde(M.horde); M.horde=nil end
    if M.trap then removeTrap(M.trap); M.trap=nil end
    if M.heli then removeVehicle(M.heli); M.heli=nil end
    M.flight=false
end
local actions={
    dawn=function() setTime(6.5,true); setWeather(1) end,
    day=function() setTime(13,true); setWeather(1) end,
    night=function() setTime(22,true) end,
    alarm=function() R.broadcast(L("[РАЦИЯ] Тревога! Группа снабжения, проверьте район NERO. Общий канал 101.","[RADIO] Alarm! Supply team, check the NERO area. Shared channel 101."),"#E07070") end,
    horde=function(p)
        if M.horde and getHorde(M.horde) then return R.msg(p,L("Стая уже существует. /director clear для сброса.","A pack already exists. Use /director clear to reset."),false) end
        local pt=R.points.expedition
        M.horde=createHorde(pt.x+2000,pt.y+1000,pt.z,6,{radius=8,world=0,types={"Swarmer"}})
        R.msg(p,M.horde and L("Создана стая у района NERO (экспериментальная синхронизация).","A pack has spawned at NERO (experimental synchronisation).") or L("Стая не создана. Проверьте Zombies=1.","Could not create the pack. Check Zombies=1."))
    end,
    trap=function(p)
        if M.trap then return R.msg(p,L("Ловушка уже поставлена. /director clear для сброса.","A trap is already set. Use /director clear to reset."),false) end
        local pt=R.points.expedition
        M.trap=createTrap({type="beartrap",x=pt.x+1500,y=pt.y,z=pt.z,world=0,lethal=false,players=true,zombies=true})
        R.msg(p,L("Капкан установлен в 15 м к востоку от точки NERO.","A bear trap is set 15 m east of the NERO point."))
    end,
    evac=function(p)
        if M.heli then return R.msg(p,L("Вертолёт уже вызван.","The helicopter has already been called."),false) end
        local pt=R.points.expedition
        M.heli=createVehicle({model="helicopter",x=pt.x,y=pt.y-1500,z=pt.z,yaw=pt.yaw,world=0,
            blip="all",npc={look="m_sur_copeland_guard_08",name="NERO"}})
        if not M.heli then return R.msg(p,L("Вертолёт не создан.","Could not create the helicopter."),false) end
        R.broadcast(L("[РАЦИЯ] Эвакуация у района NERO. Четыре места. Посадка на E; вылет по команде режиссёра.","[RADIO] Evacuation at NERO. Four seats. Press E to board; the director controls departure."),"#D9C898")
    end,
    fly=function(p)
        if not M.heli or M.flight then return R.msg(p,L("Сначала /director evac. Повторный вылет во время полёта недоступен.","Call /director evac first. Another departure is unavailable during a flight."),false) end
        local pt=R.points.helipad; local id=M.heli
        M.flight=true
        driveVehicle(id,{points={{x=pt.x,y=pt.y,z=pt.z}},speed=25,altitude=45,
            onArrive=function(vid)
                if M.heli~=vid then return end
                M.flight=false
                R.broadcast(L("[РАЦИЯ] Эвакуация завершена. Выход на E, сдайте медикаменты на склад.","[RADIO] Evacuation complete. Press E to exit and deliver medical supplies to the warehouse."),"#7CD67C")
            end})
    end,
    clear=function(p) clear(); setTime(13,true); setWeather("auto"); setNametags("visible"); R.msg(p,L("Съёмочные объекты убраны.","Staged objects removed.")) end,
    kit=function(p)
        local a=R.acc(p)
        a.money=math.max(a.money,1500); a.rep=math.max(a.rep,30)
        a.hunger=100; a.thirst=100
        for id,n in pairs({medkit=2,fuel=1,scrap=3,radio=1,food=2,water=2}) do
            local want=math.max(0,n-(a.bag[id] or 0))
            if want>0 then R.rules.add(R.cfg,a.bag,id,want,R.cfg.bagWeight) end
        end
        p:giveWeapon("kWeaponID_M9",30,{replace=true}); p:setVoiceChannel("Rubezh 101"); p:heal()
        R.commit("directorKit",p); R.msg(p,L("Съёмочный комплект выдан только вам. Экономика персонажа изменена.","Filming supplies issued to you. Your character's economy has changed."))
    end,
}
R.command("director",function(p,_,action)
    local fn=actions[action]
    if not fn then return R.dialog(p,{title=L("Пульт режиссёра","Director controls"),text=L("/director dawn|day|night — свет\n/director alarm — тревога по рации\n/director kit — личный комплект для съёмки\n/director horde — 6 фриков у NERO\n/director trap — капкан у NERO\n/director evac — вертолёт у NERO\n/director fly — вылет в лагерь\n/director clear — убрать стаю, капкан и вертолёт\n/scene <точка> — переместиться\n/bring <id> — вызвать участника\n/setpoint <точка> — сохранить текущую позицию\n/cinema — скрыть свой HUD режима","/director dawn|day|night — lighting\n/director alarm — radio alarm\n/director kit — personal filming supplies\n/director horde — 6 freakers at NERO\n/director trap — trap at NERO\n/director evac — helicopter at NERO\n/director fly — fly to camp\n/director clear — remove staged objects\n/scene <point> — teleport\n/bring <id> — bring a player\n/setpoint <point> — save position\n/cinema — hide your mode HUD"),button1=L("Закрыть","Close")}) end
    fn(p); log("[outpost director] "..p.name.." "..action)
end,true)
R.command("scene",function(p,_,name)
    local pt=R.points[name]
    if not pt then return R.msg(p,L("Точки: office shop clinic garage warehouse cargo salvage patrol1 patrol2 expedition helipad spawn","Points: office shop clinic garage warehouse cargo salvage patrol1 patrol2 expedition helipad spawn"),false) end
    p:setWorld(0); p:setPosition(pt.x,pt.y,pt.z+100,pt.yaw or 0)
end,true)
R.command("bring",function(p,_,who)
    local q=who and getPlayer(tonumber(who) or who)
    if not R.ready(q) then return R.msg(p,L("Нет такого активного игрока.","No such active player."),false) end
    local x,y,z,yaw=p:position(); q:setWorld(p.world); q:setPosition(x+150,y,z,yaw)
end,true)
R.command("setpoint",function(p,_,name)
    if not R.points[name] then return R.msg(p,L("Неизвестная точка. См. /scene.","Unknown point. See /scene."),false) end
    local x,y,z,yaw=p:position()
    -- Player positions are capsule centres; world actors and logic points use ground Z.
    R.state.points[name]={x=x,y=y,z=z-90,yaw=yaw}
    R.commit("point",p,name); storage.save()
    R.msg(p,L("Точка сохранена. Выполните reload в консоли сервера, чтобы перестроить NPC, зоны и метки.","Point saved. Run reload in the server console to rebuild NPCs, zones and markers."))
end,true)
R.command("cinema",function(p)
    local s=R.session(p); s.cinema=not s.cinema; R.hud(p)
    R.msg(p,s.cinema and L("HUD режима скрыт. /cinema — вернуть.","Mode HUD hidden. Use /cinema to show it.") or L("HUD режима включён.","Mode HUD enabled."))
end)
R.command("kick",function(p,args,who)
    local q=who and getPlayer(tonumber(who) or who)
    if q then R.kick(q,args:match("^%S+%s+(.+)$") or L("Нарушение правил RP","RP rule violation")) end
end,true)
R.command("ban",function(p,args,who)
    local q=who and getPlayer(tonumber(who) or who)
    local minutes,reason=args:match("^%S+%s+(%d+)%s+(.+)$")
    if not q or not minutes then return R.msg(p,L("/ban <id> <минуты; 0 навсегда> <причина>","/ban <id> <minutes; 0 forever> <reason>"),false) end
    R.ban(q,reason,tonumber(minutes))
end,true)
R.command("unban",function(p,args) R.msg(p,L("Снято банов: ","Bans removed: ")..unban(args)) end,true)
on("stop",clear)
return M
