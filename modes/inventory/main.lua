-- Own inventory and traps demo (modes/_lib/inventory.lua, docs/API.md "Свой инвентарь", "Ловушки"): gas cans lie
-- by the spawn, F picks one up into your backpack, I (or /inv) opens it (next to the bike: backpack | trunk side
-- by side), "Refuel" pours a can into the bike's tank. A mine and a tripwire in the backpack are set from it.
-- Scrap is a stackable item; the backpack holds 30 kg and is kept between visits.
-- Test commands: /place mine|beartrap|wire (aimed, without the item), /mine, /wire, /clothesline, /beartrap, /boom
-- (in front of you), /traps, /bike.

local inventory = require("inventory")
local GREEN, RED = "#7CD67C", "#E07070"
local function T(player, en, ru) return player.lang == "ru" and ru or en end

-- The server vehicle within `metres` of the player (the nearest), or nil.
local function nearVehicle(player, metres)
    local x, y, z = player:position()
    local best, bestD = nil, (metres * 100) ^ 2
    for _, v in ipairs(vehicles()) do
        local d = (v.x - x) ^ 2 + (v.y - y) ^ 2 + (v.z - z) ^ 2
        if v.world == player.world and d < bestD then best, bestD = v, d end
    end
    return best
end

-- ---------------------------------------------------------------- cans lying in the world
local cans = {}  -- interaction id -> object id
local function placeCan(x, y, z)
    local object = createObject({ model = "jerry_can", x = x, y = y, z = z })
    local id
    id = createInteraction({ x = x, y = y, z = z + 50, radius = 1.5, key = "F", text = "Gas can / Канистра",
        onUse = function(player)
            if inventory.of(player):add("gas", 1) == 0 then return player:message(T(player, "The backpack is full.", "Рюкзак полон."), RED) end
            removeInteraction(id); removeObject(cans[id]); cans[id] = nil
            player:playSound("ui_tutorial_x_press")
            player:tip(T(player, "+ Gas can (I: backpack)", "+ Канистра (I — рюкзак)"))
        end })
    cans[id] = object
end

-- ---------------------------------------------------------------- items
inventory.define("gas", {
    name = { en = "Gas can", ru = "Канистра бензина" }, icon = "gas.png", weight = 5, stack = 1,
    text = { en = "Fills a tank by 40 %", ru = "Заправляет бак на 40 %" },
    actions = {
        { id = "refuel", label = { en = "Refuel", ru = "Заправить" },
          when = function(player) return nearVehicle(player, 5) ~= nil end,   -- the button shows only next to a vehicle
          fn = function(player)
              local v = nearVehicle(player, 5)
              if not v then return false end
              if v.fuel >= 0.99 then player:message(T(player, "The tank is full.", "Бак полон."), RED) return false end
              setVehicle(v.id, { fuel = math.min(1, v.fuel + 0.4) })
              player:message(T(player, "Tank: ", "Бак: ") .. math.floor(math.min(1, v.fuel + 0.4) * 100) .. " %", GREEN)
              return true   -- one can is used up
          end },
        { id = "drop", label = { en = "Drop", ru = "Выбросить" },
          fn = function(player)
              local x, y, z, yaw = player:position()
              local a = math.rad(yaw)
              placeCan(x + math.cos(a) * 80, y + math.sin(a) * 80, z - 90)
              return true
          end },
    },
})
inventory.define("scrap", {
    name = { en = "Scrap", ru = "Металлолом" }, icon = "scrap.png", weight = 0.5, stack = 20,
    text = { en = "Stacks by 20", ru = "Стопки по 20" },
})

-- ---------------------------------------------------------------- traps
local place  -- place(player, kind): the player aims where a trap goes (defined below)
-- A point `metres` in front of the player on the ground, and its heading.
local function ahead(player, metres)
    local x, y, z, yaw = player:position()
    local a = math.rad(yaw)
    return x + math.cos(a) * metres * 100, y + math.sin(a) * metres * 100, z - 90, yaw
end
local function setMine(player) local x, y, z = ahead(player, 3); return createTrap({ type = "mine", x = x, y = y, z = z, world = player.world }) end
-- A wire across the way 4 m ahead, 4 m wide, at shin height (explosive) or at a rider's neck (clothesline).
local function setWire(player, explosive)
    local x, y, z, yaw = ahead(player, 4)
    local side = math.rad(yaw + 90)
    local h = explosive and 30 or 150
    return createTrap({ type = "wire", explosive = explosive, world = player.world,
        x = x - math.cos(side) * 200, y = y - math.sin(side) * 200, z = z + h,
        x2 = x + math.cos(side) * 200, y2 = y + math.sin(side) * 200, z2 = z + h,
        onTrigger = not explosive and function(trap, victim)
            if not victim then return false end            -- zombies walk under it
            victim:damage(25)
            if victim:isOnBike() then victim:playAnim("dcn_trap_tripwire_clothsline_bike_long") end
            victim:message(T(victim, "A wire across the road!", "Растяжка поперёк дороги!"), RED)
        end or nil })
end
local pickups = {}  -- trap id -> interaction id: its owner may take it back while it has not gone off
on("trap", function(trap, player, zombie)
    log(("trap %d (%s) set off by %s"):format(trap.id, trap.type, player and player.name or zombie and ("zombie " .. zombie) or "?"))
    if pickups[trap.id] then removeInteraction(pickups[trap.id]); pickups[trap.id] = nil end
end)
command("mine", setMine)
command("wire", function(player) setWire(player, true) end)
command("clothesline", function(player) setWire(player, false) end)
command("beartrap", function(player) local x, y, z = ahead(player, 2); createTrap({ type = "beartrap", x = x, y = y, z = z, world = player.world }) end)
command("boom", function(player) local x, y, z = ahead(player, 10); explode(x, y, z, player.world) end)
command("place", function(player, args, kind) if kind == "mine" or kind == "beartrap" or kind == "wire" then place(player, kind, true) end end)
command("traps", function(player)
    for _, t in ipairs(traps()) do
        player:message(("%d %s %s at %.0f %.0f %.0f%s"):format(t.id, t.type, t.armed and "armed" or "-", t.x, t.y, t.z,
            t.x2 and (" to %.0f %.0f %.0f"):format(t.x2, t.y2, t.z2) or ""))
    end
end)

-- Setting one from the backpack: the window closes and the player aims where it goes (client/main.lua);
-- the item stays in the backpack until the trap is there ("placeAt").
local freePlace = {}  -- player id -> kind: /place sets one without the item (testing)
function place(player, kind, free)  -- (the local declared above)
    freePlace[player.id] = free and kind or nil
    inventory.close(player)
    player:clientSend("place", { kind = kind, maxLength = 10 })
    return false
end

inventory.define("mine", {
    name = { en = "Proximity mine", ru = "Мина" }, icon = "mine.png", weight = 1.5, stack = 5,
    text = { en = "Goes off when someone steps near", ru = "Взрывается, когда рядом проходят" },
    actions = { { id = "set", label = { en = "Set", ru = "Поставить" }, fn = function(player) return place(player, "mine") end } },
})
inventory.define("beartrap", {
    name = { en = "Bear trap", ru = "Капкан" }, icon = "beartrap.png", weight = 3, stack = 3,
    text = { en = "Catches whoever steps in", ru = "Ловит того, кто наступит" },
    actions = { { id = "set", label = { en = "Set", ru = "Поставить" }, fn = function(player) return place(player, "beartrap") end } },
})
inventory.define("tripwire", {
    name = { en = "Tripwire", ru = "Растяжка" }, icon = "wire.png", weight = 1, stack = 5,
    text = { en = "Across the way, an IED at one end", ru = "Поперёк пути, на конце СВУ" },
    actions = { { id = "set", label = { en = "Stretch", ru = "Натянуть" }, fn = function(player) return place(player, "wire") end } },
})

-- Where the player put it (client/main.lua: aimed, with a ghost of it): within 4 m of the player when it is put. A wire
-- as in the game: its first end is set ("wireStart", kept here as it was then), the player walks on stretching it, and
-- sets the other end within 4 m of where it stands; the wire is 1..10 m long, 30 cm over the ground at each end. The
-- item is taken only when the trap is there.
local ITEM = { mine = "mine", beartrap = "beartrap", wire = "tripwire" }
local REACH = 400       -- units: as far from the player as a point may be when it is put (RANGE in client/main.lua)
local wireStart = {}    -- player id -> {x, y, z}: the first end of the wire it is stretching
on("clientMessage", function(player, name, data)
    if name == "wireStart" and type(data) == "table" and type(data.a) == "table" then
        local px, py, pz = player:position()
        local x, y, z = tonumber(data.a.x), tonumber(data.a.y), tonumber(data.a.z)
        if x and y and z and (x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2 <= REACH ^ 2 then
            wireStart[player.id] = { x = x, y = y, z = z }
        else
            wireStart[player.id] = nil
            player:clientSend("wireRefused", {})
            player:message(T(player, "Too far from you.", "Слишком далеко от вас."), RED)
        end
        return
    end
    if name ~= "placeAt" or type(data) ~= "table" or not ITEM[data.kind] or type(data.a) ~= "table" then return end
    local bag = inventory.of(player)
    local free = freePlace[player.id] == data.kind
    if not free and not bag:has(ITEM[data.kind]) then
        local names = { mine = { "mine", "мины" }, beartrap = { "bear trap", "капкана" }, wire = { "tripwire", "растяжки" } }
        return player:message(T(player, "No " .. names[data.kind][1] .. " in the backpack.", "Нет " .. names[data.kind][2] .. " в рюкзаке."), RED)
    end
    local px, py, pz = player:position()
    local function point(p)
        if type(p) ~= "table" then return nil end
        local x, y, z = tonumber(p.x), tonumber(p.y), tonumber(p.z)
        if not (x and y and z) or (x - px) ^ 2 + (y - py) ^ 2 + (z - pz) ^ 2 > REACH ^ 2 then return nil end
        return x, y, z
    end
    local ax, ay, az
    if data.kind == "wire" then  -- its first end as it was set, not as the client says now
        local a = wireStart[player.id]
        if not a then return player:message(T(player, "Set the wire's first end again.", "Поставьте первый конец растяжки заново."), RED) end
        ax, ay, az = a.x, a.y, a.z
    else
        ax, ay, az = point(data.a)
        if not ax then return player:message(T(player, "Too far from you.", "Слишком далеко от вас."), RED) end
    end
    local trap
    if data.kind == "wire" then
        local bx, by, bz = point(data.b)
        if not bx then return player:message(T(player, "Too far from you.", "Слишком далеко от вас."), RED) end
        local len = math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2)
        if len < 100 or len > 1000 then return player:message(T(player, "A wire is 1 to 10 m long.", "Растяжка — от 1 до 10 м."), RED) end
        trap = createTrap({ type = "wire", x = ax, y = ay, z = az + 30, x2 = bx, y2 = by, z2 = bz + 30, world = player.world })
        wireStart[player.id] = nil
    else
        if (ax - px) ^ 2 + (ay - py) ^ 2 < 150 ^ 2 then  -- under your own feet it would catch you at once
            return player:message(T(player, "Not under your feet: at least 1.5 m away.", "Не под ноги: хотя бы в 1,5 м."), RED)
        end
        trap = createTrap({ type = data.kind, x = ax, y = ay, z = az, yaw = tonumber(data.yaw) or 0, world = player.world })
    end
    if trap then
        if free then freePlace[player.id] = nil else bag:remove(ITEM[data.kind], 1) end
        -- Its owner takes it back with F while it has not gone off (into the backpack).
        local owner, kind = player.id, data.kind
        local id
        id = createInteraction({ x = ax, y = ay, z = az + 50, radius = 1.5, key = "F", world = player.world, text = "Take the trap / Забрать ловушку",
            onUse = function(p)
                if p.id ~= owner then return p:message(T(p, "Not your trap.", "Это не ваша ловушка."), RED) end
                if not getTrap(trap) then return end
                if inventory.of(p):add(ITEM[kind], 1) == 0 then return p:message(T(p, "The backpack is full.", "Рюкзак полон."), RED) end
                removeTrap(trap); removeInteraction(id); pickups[trap] = nil
            end })
        pickups[trap] = id
    end
end)

-- ---------------------------------------------------------------- the world of the demo
local s = server.spawn
local bike = createVehicle({ x = s.x + 600, y = s.y, z = s.z, yaw = 90, fuel = 0.1 })
for i = 0, 3 do placeCan(s.x + 300, s.y - 200 + i * 130, s.z - 90) end

on("playerJoin", function(player)
    local bag = inventory.of(player)
    if bag:count("scrap") == 0 then bag:add("scrap", 25) end
    if bag:count("mine") == 0 then bag:add("mine", 2) end
    if bag:count("tripwire") == 0 then bag:add("tripwire", 2) end
    if bag:count("beartrap") == 0 then bag:add("beartrap", 1) end
    player:message(T(player, "Inventory demo: F takes a gas can, I opens the backpack (next to the bike: and its trunk).",
                             "Инвентарь: F — взять канистру, I — рюкзак (у мотоцикла — ещё и багажник)."), GREEN)
end)

local function toggle(player)
    if inventory.isOpen(player) then return inventory.close(player) end
    local v = nearVehicle(player, 4)
    if v then
        inventory.open(player, inventory.of(player), inventory.trunk(v.id), { title = T(player, "Backpack and trunk", "Рюкзак и багажник") })
    else
        inventory.open(player, inventory.of(player), nil, { title = T(player, "Backpack", "Рюкзак") })
    end
end
bindKey("I", toggle)
command("inv", toggle)
command("bike", function(player)  -- the demo bike back next to you, empty
    local x, y, z, yaw = player:position()
    setVehicle(bike, { x = x + 300, y = y, z = z, fuel = 0.05 })
end)
