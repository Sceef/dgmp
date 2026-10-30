local L=require("i18n")
local R=require("core")
local S=require("services")
local J=require("jobs")
require("director")
local chat=require("rpchat")
chat.install({radius=20})
protectItems({"kWeaponID_","kAmmoID_"})
setPvp(true)
-- Cover every camp service even when the editor layout spans several elevations.
local minX,minY,minZ,maxX,maxY,maxZ=math.huge,math.huge,math.huge,-math.huge,-math.huge,-math.huge
for _,name in ipairs({"spawn","office","shop","clinic","garage","warehouse","cargo","wardrobe"}) do
    local p=R.points[name]
    if p then
        minX,minY,minZ=math.min(minX,p.x),math.min(minY,p.y),math.min(minZ,p.z)
        maxX,maxY,maxZ=math.max(maxX,p.x),math.max(maxY,p.y),math.max(maxZ,p.z)
    end
end
if not getZone("outpost_camp") then createZone({name="outpost_camp",type="pve",shape="box",x=(minX+maxX)/2,y=(minY+maxY)/2,z=(minZ+maxZ)/2,
    length=(maxX-minX)/100+30,width=(maxY-minY)/100+30,height=(maxZ-minZ)/200+20,yaw=0,
    world=0}) end
setTime(13,true)
local serviceBlips={
    office={icon="MAP_player_safehouse",title=L("Комендатура","Command office"),description=L("Регистрация, профессии и задания лагеря.","Registration, professions and camp jobs.")},
    shop={icon="MAP_objective_encampment_merchant",title=L("Магазин снабжения","Supply store"),description=L("Покупка припасов и оружейная.","Supplies and armory.")},
    clinic={icon="MAP_objective_NERO_checkpoint",title=L("Медпункт","Clinic"),description=L("Лечение и вызов медика.","Treatment and medic calls.")},
    garage={icon="MAP_objective_encampment_mechanic",title=L("Гараж","Garage"),description=L("Покупка, выдача и обслуживание мотоцикла.","Buy, collect and service your motorcycle.")},
    warehouse={icon="MAP_objective_encampment_locker",title=L("Склад","Warehouse"),description=L("Личный тайник и работа по доставке ящиков.","Personal storage and crate delivery work.")},
}
local function station(name,label,look,fn)
    local pt=R.points[name]
    createActor({x=pt.x,y=pt.y,z=pt.z,yaw=pt.yaw or 0,world=0,look=look,anim="idle",name=label,solid=true})
    createInteraction({x=pt.x,y=pt.y,z=pt.z,world=0,radius=3,text=label,onUse=function(p)
        if R.at(p,name,5) then fn(p) end
    end})
    local b=assert(serviceBlips[name])
    createBlip({x=pt.x,y=pt.y,z=pt.z,world=0,icon=b.icon,title=b.title,description=b.description,color="#D9C898"})
end
station("office",L("Комендант · регистрация","Commander · registration"), "m_sur_copeland_guard_08",S.office)
station("shop",L("Торговец","Trader"), "m_mil_diamondlake_worker_003",S.shop)
station("clinic",L("Медпункт","Clinic"), "f_mil_diamondlake_medic_001",S.clinic)
station("garage",L("Механик · гараж","Mechanic · garage"), "m_mil_diamondlake_worker_001",S.garage)
station("warehouse",L("Кладовщик · склад и работа","Quartermaster · storage and work"), "m_mil_diamondlake_worker_003",S.warehouse)
R.command("gps",function(p,_,name)
    if name=="off" then return R.mark(p,nil) end
    if not R.points[name] then return R.msg(p,"/gps office|shop|clinic|garage|warehouse|cargo|salvage|patrol1|patrol2|expedition|helipad|off",false) end
    R.mark(p,name,L("Рубеж: ","Rubezh: ")..name)
end)
R.command("stats",function(p)
    local a=R.acc(p)
    R.msg(p,(L("%s [%d] · %s · $%d / банк $%d · репутация %d · активных минут %d","%s [%d] · %s · $%d / bank $%d · reputation %d · active minutes %d")):format(p.name,p.id,R.cfg.roles[a.role].name,a.money,a.bank,a.rep,math.floor(a.played/60)))
end)
R.command("help",function(p)
    R.dialog(p,{title=R.cfg.title,text=L("F — NPC / удерживать F — ящик · E — транспорт · I — рюкзак\nN — голос рядом · K — рация\n\nНачало: /gps office → пропуск → /jobs\n/camp /stats /gps <точка> /jobs /cancel /mission join\n/shop /armory /garage /lock /stash /bag /use <предмет>\n/bank deposit|withdraw <сумма> /pay <id> <сумма>\n/duty /call medic|mechanic|guard /calls /respond <id> /aid <id>\n/radio 101 /radio off /donate <предмет> /upgrade\n/me /do /try /b /s /w /pm /o\n/sit /hands /wave /kneel /talk /guard /stop /cinema\n\nПравила: не атакуйте без RP-причины. В лагере PvP выключен.\nНезавершённая работа отменяется при смерти, выходе или reload.\nАдминистраторам: /director","F — interact / hold F — carry a crate · E — vehicle · I — backpack\nN — proximity voice · K — radio\n\nGetting started: visit the commander, register and deliver your first crate.\n/camp /stats /gps <point> /jobs /cancel /mission join\n/shop /armory /garage /lock /stash /bag /use <item>\n/bank deposit|withdraw <amount> /pay <id> <amount>\n/duty /call medic|mechanic|guard /calls /respond <id> /aid <id>\n/radio 101 /radio off /donate <item> /upgrade\n/me /do /try /b /s /w /pm /o\n/sit /hands /wave /kneel /talk /guard /stop /cinema\n\nRules: no attacks without an RP reason. PvP is disabled in camp.\nUnfinished jobs are cancelled on death, disconnect or reload.\nAdmins: /director"),button1=L("Понятно","Understood")})
end)
for cmd,anim in pairs({sit="sit",hands="surrender",wave="wave",kneel="kneel",talk="talk",guard="guard"}) do
    R.command(cmd,function(p) p:playAnim(anim,{loop=cmd~="wave"}) end)
end
R.command("stop",function(p) p:stopAnim() end)
on("inventoryViolation",function(p,v)
    if v.pickedUp and v.item:find("^kAmmoID_") then return true end
end)
on("cheat",function(p,kind,info)
    for _,q in ipairs(players()) do if q.admin then R.message(q,"[AC] "..p.name..": "..kind.." — "..info.detail,"#E07070") end end
end)
log("[outpost] Rubezh 1.0 loaded. /help; admin: /director.")
