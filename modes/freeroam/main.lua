-- Freeroam: the reference DGMP mode. Everything here uses only the public Lua API.
-- Coordinates are Unreal units (100 = 1 metre), the same as the editor overlay.

local GREEN, YELLOW, GREY = "#7CD67C", "#F2D16B", "#A0A0A0"
local adminUsage  -- set with the admin tools below

on("start", function()
    log("Freeroam started on " .. server.name)
end)

on("playerJoin", function(player)
    broadcast(player.name .. " joined the server", GREY)
    player:message("Welcome to " .. server.name .. "! Type /help for commands.", GREEN)
    player.data.joinedAt = now()
    -- First time on this server: choose a character among the mode's skins (skins.json, made in
    -- the editor's Characters section); kept by nick, worn on every visit.
    if not player.skin then
        player:customize({ title = "Your character", cancel = false,
                           onDone = function(p, look, id) p:message("Character saved. /skin changes it.", GREEN) end })
    end
end)

-- Skins: /skin chooses again (a clothes shop in an RP mode would do the same); /uniform picks clothes
-- over the skin (a job) or puts one on by id, /nouniform takes them off.
command("skin", function(player) player:customize({ title = "Character" }) end)
command("uniform", function(player, args)
    if args == "" then
        if #skinList("uniforms") == 0 then player:message("This mode has no uniforms.", GREY); return end
        player:customize({ title = "Uniform", list = "uniforms" })
        return
    end
    local ok, err = pcall(function() player:setUniform(args) end)
    player:message(ok and ("Uniform: " .. args) or err, ok and GREEN or YELLOW)
end)
command("nouniform", function(player) player:setUniform(nil); player:message("Uniform off", GREEN) end)

on("playerQuit", function(player, reason)
    broadcast(player.name .. " left (" .. reason .. ")", GREY)
end)

-- Vehicles: every player gets a bike of its own next to it (locked, on its own map only). /bike brings
-- it, /lock and /unlock; an unlocked bike can be ridden by anyone. It goes when its player leaves.
local bikes = {}  -- player id -> vehicle id
local function besidePlayer(player)
    local x, y, z, yaw = player:position()
    local r = math.rad(yaw or 0)
    return x - math.sin(r) * 300, y + math.cos(r) * 300, z - 90, yaw or 0
end
on("playerJoin", function(player)
    after(3000, function()
        if not player.valid or bikes[player.id] then return end
        local x, y, z, yaw = besidePlayer(player)
        bikes[player.id] = createVehicle({ x = x, y = y, z = z, yaw = yaw, owner = player, locked = true })
    end)
end)
on("playerQuit", function(player)
    if bikes[player.id] then removeVehicle(bikes[player.id]); bikes[player.id] = nil end
end)
command("bike", function(player)
    local id = bikes[player.id]
    local v = id and vehicleInfo(id)
    if not v then
        local x, y, z, yaw = besidePlayer(player)
        bikes[player.id] = createVehicle({ x = x, y = y, z = z, yaw = yaw, owner = player, locked = true })
    elseif v.driver then player:message("Someone is riding your bike.", YELLOW); return
    else
        local x, y, z, yaw = besidePlayer(player)
        setVehicle(id, { x = x, y = y, z = z, yaw = yaw })
    end
    player:message("Your bike is here.", GREEN)
end)
local function setLock(player, locked)
    local id = bikes[player.id]
    if not id or not vehicleInfo(id) then player:message("You have no bike: /bike", GREY); return end
    setVehicle(id, { locked = locked })
    player:message(locked and "Bike locked" or "Bike unlocked: anyone can ride it", GREEN)
end
command("lock", function(player) setLock(player, true) end)
command("unlock", function(player) setLock(player, false) end)

on("chat", function(player, text)
    -- Return false to block a message; nothing to filter in freeroam.
end)

local function findPlayer(query)
    if not query then return nil end
    local byId = tonumber(query)
    if byId then return getPlayer(byId) end
    local found = getPlayer(query)
    if found then return found end
    for _, p in ipairs(players()) do
        if p.name:lower():find(query:lower(), 1, true) then return p end
    end
end

command("help", function(player)
    player:message("/players  /tp <player>  /pos  /time <0-24>  /weather <id|auto>  /hint  /skin  /uniform [id]  /nouniform  /bike  /lock  /unlock", YELLOW)
    if player.admin then player:message(adminUsage, YELLOW) end
end)

command("players", function(player)
    local list = {}
    for _, p in ipairs(players()) do list[#list + 1] = p.name .. " (" .. p.id .. ")" end
    player:message(#list .. " online: " .. table.concat(list, ", "), YELLOW)
end)

command("tp", function(player, args, target)
    local other = findPlayer(target)
    if not other then return player:message("Usage: /tp <player name or id>", YELLOW) end
    if other == player then return player:message("You are already here.", YELLOW) end
    local x, y, z, yaw = other:position()
    player:setPosition(x + 150, y, z + 50, yaw)
    player:message("Teleported to " .. other.name, GREEN)
end)

command("pos", function(player)
    local x, y, z, yaw = player:position()
    player:message(string.format("X %.0f  Y %.0f  Z %.0f  Yaw %.0f", x, y, z, yaw), YELLOW)
end)

command("time", function(player, args, hours)
    hours = tonumber(hours)
    if not hours or hours < 0 or hours >= 24 then return player:message("Usage: /time <0-24>", YELLOW) end
    setTime(hours)
    broadcast(player.name .. " set the time to " .. string.format("%02d:%02d", math.floor(hours), math.floor(hours % 1 * 60)), GREY)
end)

command("weather", function(player, args, id)
    if id == "auto" then setWeather("auto") elseif tonumber(id) then setWeather(tonumber(id))
    else return player:message("Usage: /weather <id|auto>", YELLOW) end
    broadcast(player.name .. " changed the weather", GREY)
end)

command("hint", function(player)
    player:hint("FREEROAM", "This popup was sent by the server mode script.")
end)

-- Admin tools: the same actions from chat (admins listed in server.ini) and from the server console.
-- Item keys are the game's names: kWeaponID_AK47, kAmmoID_AK47, kConsumableID_Bandage_Small ...
local admin = {}

admin.give = function(target, key, count) target:giveItem(key, tonumber(count) or 1); return "gave " .. key end
admin.take = function(target, key, count) target:takeItem(key, tonumber(count) or 1); return "took " .. key end
admin.damage = function(target, amount) target:damage(tonumber(amount) or 20); return "damage " .. (tonumber(amount) or 20) end
admin.weapon = function(target, key, ammo) target:giveWeapon(key, tonumber(ammo)); return "weapon " .. key end
-- replace: the weapon already in that slot disappears instead of dropping to the ground
admin.replace = function(target, key, ammo) target:giveWeapon(key, tonumber(ammo), { replace = true }); return "weapon " .. key .. " (replaced)" end
admin.ammo = function(target, key, count) target:setAmmo(key, tonumber(count) or 0); return "ammo " .. key end
admin.disarm = function(target, what)
    -- disarm            guns only (knife and rock stay)
    -- disarm all        guns, knife, rock, taser
    -- disarm rock|knife only that tool, guns stay
    if what == "all" then target:takeWeapons({ knife = true, rock = true, taser = true })
    elseif what == "rock" or what == "knife" or what == "taser" then target:takeTools({ [what] = true })
    else target:takeWeapons() end
    return "disarmed (" .. (what or "guns") .. ")"
end
admin.clear = function(target) target:clearInventory(); return "inventory cleared" end
-- look <player> <name>: any look from docs/looks.md (m_sur_copeland_guard_08, deacon_standard_shirtless ...)
admin.look = function(target, name)
    if not name then return "usage: look <player> <look name>" end
    local ok, err = pcall(target.setLook, target, name)
    return ok and ("look " .. name) or tostring(err)
end
admin.bike = function(target) target:giveBike(); return "bike given" end
-- kick <player> [reason], ban <player> [minutes] [reason] (no minutes or 0: for ever), whois <player>
admin.kick = function(target, ...) target:kick(table.concat({ ... }, " ")); return "kicked" end
admin.ban = function(target, minutes, ...)
    local words = { ... }
    if minutes and not tonumber(minutes) then table.insert(words, 1, minutes); minutes = nil end
    target:ban(table.concat(words, " "), tonumber(minutes) or 0)
    return "banned" .. (tonumber(minutes) and tonumber(minutes) > 0 and (" for " .. minutes .. " min") or "")
end
admin.whois = function(target)
    return "id " .. target.id .. ", Steam " .. (target.steamId or "not confirmed") .. ", IP " .. target.ip .. (target.admin and ", admin" or "")
end
admin.nobike = function(target) target:takeBike(); return "bike taken" end
admin.inv = function(target)
    local list = {}
    for key, count in pairs(target:inventory()) do list[#list + 1] = key .. "=" .. count end
    table.sort(list)
    return #list .. " items: " .. table.concat(list, ", ")
end

adminUsage = "give|take <player> <item> [n], weapon|replace <player> <weapon> [ammo], ammo <player> <item> <n>, " ..
                   "disarm <player> [all|knife|rock], clear <player>, bike|nobike <player>, inv <player>, kick <player> [reason], ban <player> [minutes] [reason], " ..
                   "unban <name|steamid|ip>, bans, whois <player>, nametags visible|walls|hidden, collisions on|off|players|bikes, hint <title> | <text>, look <player> <name>, looks [filter]"

local function runAdmin(words)
    local action = words[1]
    if action == "nametags" then setNametags(words[2] or "visible"); return "nametags: " .. (words[2] or "visible") end
    if action == "collisions" then
        -- collisions on|off|players|bikes: players' bodies / bikes block each other
        local m = words[2] or "on"
        if m == "players" or m == "bikes" then setCollisions({ players = m == "players", bikes = m == "bikes" }) else setCollisions(m == "on") end
        return "collisions: " .. m
    end
    if action == "unban" then return unban(words[2] or "") .. " ban(s) lifted for " .. tostring(words[2]) end
    if action == "bans" then
        local list = {}
        for _, b in ipairs(bans()) do list[#list + 1] = (b.name or b.steamId or b.ip) .. (b.expires > 0 and (" until " .. os.date("%d.%m %H:%M", b.expires)) or "") end
        return #list .. " bans: " .. table.concat(list, ", ")
    end
    if action == "looks" then
        local list = looks(words[2] or "")
        return #list .. " looks: " .. table.concat(list, ", ", 1, math.min(#list, 40)) .. (#list > 40 and " ..." or "")
    end
    if action == "tip" then
        -- tip [seconds] <text>
        local seconds = tonumber(words[2])
        tip(table.concat(words, " ", seconds and 3 or 2), seconds or 5)
        return "tip sent"
    end
    if action == "hint" then
        -- hint <title words> | <text words>   (shown to everyone in the game's own popup)
        local line = table.concat(words, " ", 2)
        local title, text = line:match("^(.-)%s*|%s*(.*)$")
        hint(title or line, text or "")
        return "hint sent"
    end
    local fn = admin[action or ""]
    if not fn then return nil end
    local target = findPlayer(words[2])
    if not target then return "no such player: " .. tostring(words[2]) end
    local name = target.name  -- (kick and ban remove the player)
    return name .. ": " .. fn(target, table.unpack(words, 3))
end

local chatTools = { unban = true, bans = true }
for name in pairs(admin) do chatTools[name] = true end
for name in pairs(chatTools) do
    command(name, function(player, args)
        if not player.admin then return player:message("Admins only.", YELLOW) end
        local words = { name }
        for w in args:gmatch("%S+") do words[#words + 1] = w end
        local answer = runAdmin(words) or adminUsage
        if player.valid then player:message(answer, YELLOW) end  -- (it may have kicked itself)
    end)
end
command("nametags", function(player, args, mode)
    if not player.admin then return player:message("Admins only.", YELLOW) end
    player:message(runAdmin({ "nametags", mode }), YELLOW)
end)

on("console", function(line)
    local words = {}
    for w in line:gmatch("%S+") do words[#words + 1] = w end
    local result = runAdmin(words)
    if result then log(result); return true end
end)

-- Quest markers (test tools): marker <player> [metres ahead] [category] [style] [route]
admin.marker = function(target, dist, category, style, route)
    local x, y, z, yaw = target:position()
    local d = (tonumber(dist) or 30) * 100
    local r = math.rad(yaw)
    local id = createMarker({ x = x + math.cos(r) * d, y = y + math.sin(r) * d, z = z, category = category or "task",
                              style = style or "basic", route = route == "route", radius = 4,
                              title = "Тестовая цель", description = "Маркер, поставленный сервером. Подойдите к нему.",
                              onEnter = function(player, mid) player:message("Вы на месте (маркер " .. mid .. ")", GREEN) end })
    return "marker " .. id
end
admin.unmarker = function(target, id) removeMarker(tonumber(id) or 0); return "marker removed" end


-- Demo quest (console: questdemo <player>; chat for admins: /questdemo <player>)
local Quest = require("quest")
admin.questdemo = function(target)
    local x, y, z, yaw = target:position()
    local r = math.rad(yaw)
    local q = Quest.start(target, { title = "Припасы для лагеря", category = "camp", text = "Староста просит проверить старую заправку." })
    q:step("Доберитесь до заправки", {
        x = x + math.cos(r) * 6000, y = y + math.sin(r) * 6000, z = z, radius = 6,
        onEnter = function(quest, player)
            local px, py, pz = player:position()
            quest:hold("Обыщите место", {
                x = px, y = py, z = pz, radius = 8, seconds = 10, meter = "Поиск",
                onDone = function(q, p)
                    p:giveItem("kConsumableID_Bandage_Small", 2)
                    q:complete(150)
                end,
            })
        end,
    })
    return "quest started"
end

-- Completion screen test (console: objtest <player> [category] [xp])
admin.objtest = function(target, category, xp)
    target:objective({ title = "Тестовое задание", text = "", state = "update", category = category or "task" })
    after(500, function()
        if target.valid then target:objective({ title = "Тестовое задание", text = "", state = "complete", category = category or "task", xp = tonumber(xp) or 50 }) end
    end)
    after(60000, function() if target.valid then target:objective({ state = "remove", category = category or "task" }) end end)
    return "objective test " .. (category or "task")
end

-- Objective demo (console: questdemo2 <player>): search cars, take an item, blow something up or burn it.
admin.questdemo2 = function(target)
    local q = Quest.start(target, { title = "Мародёр", category = "task", text = "Соберите припасы в округе." })
    q:collect("Обыщите машины", {
        event = "search", count = 2,
        onDone = function(quest)
            quest:collect("Подберите любые предметы", {
                event = "pickup", count = 3,
                onDone = function(quest2)
                    quest2:collect("Взорвите баллон или бочку, или подожгите что-нибудь", {
                        event = { "explosion", "fire" },
                        onDone = function(quest3, player) player:giveItem("kConsumableID_Bandage_Small", 2); quest3:complete(200) end,
                    })
                end,
            })
        end,
    })
    return "quest started"
end

-- Teleport (console: tp <player> <x> <y> <z> | tp <player> camp): long jumps travel with loading.
admin.tp = function(target, x, y, z)
    if x == "camp" then x, y, z = -22285, -186580, -9732 end
    target:setPosition(tonumber(x), tonumber(y), tonumber(z), 0)
    return "teleported"
end

-- Quest items demo (console: questdemo3 <player>): three molotovs appear around the player.
admin.questdemo3 = function(target)
    local x, y, z, yaw = target:position()
    local spots = {}
    for i = 1, 3 do
        local a = math.rad(yaw + (i - 2) * 35)
        spots[i] = { item = "kWeaponID_Molotov_Small", x = x + math.cos(a) * 800, y = y + math.sin(a) * 800, z = z }
    end
    local q = Quest.start(target, { title = "Зажигательная смесь", category = "task", text = "Кто-то спрятал коктейли рядом." })
    q:collect("Соберите коктейли Молотова", {
        event = "pickup", spawn = spots,
        onDone = function(quest) quest:complete(100) end,
    })
    return "quest started"
end
