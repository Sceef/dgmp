-- Own inventory demo (modes/_lib/inventory.lua, docs/API.md "Свой инвентарь"): gas cans lie by the spawn,
-- F picks one up into your backpack, I opens it (next to the bike: backpack | trunk side by side), "Refuel"
-- pours a can into the bike's tank. Scrap is a stackable item; the backpack holds 30 kg and is kept between visits.

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

-- ---------------------------------------------------------------- the world of the demo
local s = server.spawn
local bike = createVehicle({ x = s.x + 600, y = s.y, z = s.z, yaw = 90, fuel = 0.1 })
for i = 0, 3 do placeCan(s.x + 300, s.y - 200 + i * 130, s.z - 90) end

on("playerJoin", function(player)
    local bag = inventory.of(player)
    if bag:count("scrap") == 0 then bag:add("scrap", 25) end
    player:message(T(player, "Inventory demo: F takes a gas can, I opens the backpack (next to the bike: and its trunk).",
                             "Инвентарь: F — взять канистру, I — рюкзак (у мотоцикла — ещё и багажник)."), GREEN)
end)

bindKey("I", function(player)
    if inventory.isOpen(player) then return inventory.close(player) end
    local v = nearVehicle(player, 4)
    if v then
        inventory.open(player, inventory.of(player), inventory.trunk(v.id), { title = T(player, "Backpack and trunk", "Рюкзак и багажник") })
    else
        inventory.open(player, inventory.of(player), nil, { title = T(player, "Backpack", "Рюкзак") })
    end
end)
command("bike", function(player)  -- the demo bike back next to you, empty
    local x, y, z, yaw = player:position()
    setVehicle(bike, { x = x + 300, y = y, z = z, fuel = 0.05 })
end)
