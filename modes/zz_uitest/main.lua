-- Temporary test of dialogs, HUD, interactions and keys (Claude); delete after.
local logic = require("logic")
on("playerJoin", function(player)
    player:hud("money", { text = "{7CD67C}$ 500", x = 0.97, y = 0.08, align = "right", size = 1.6 })
    player:hud("logo", { image = "logo.png", x = 0.5, y = 0.02, align = "center", h = 0.05 })
    player:hud("hunger", { bar = 0.6, x = 0.02, y = 0.9, w = 0.12, h = 0.012, barColor = "#E07070", text = "Hunger" })
    player:dialog({ title = "Welcome", text = "{FFD84A}Hello{FFFFFF}, " .. player.name, onResponse = function(p, ok, item, text)
        log(("MSGBOX ok=%s item=%s text=[%s]"):format(tostring(ok), tostring(item), text))
    end })
end)
bindKey("Y", function(player, key) log("KEY " .. player.name .. " " .. key) end)
on("key", function(p, k) log("EVENT key " .. k) end)
on("use", function(p, id) log("EVENT use " .. id) end)
on("dialogResponse", function(p, id, ok, item, text) log("EVENT dialog " .. id .. " " .. tostring(ok)) end)
-- Day, stopped (the test is easier to see); the HUD again for those already in after a reload.
on("start", function()
    setTime(9, true)
    for _, p in ipairs(players()) do
        p:hud("money", { text = "{7CD67C}$ 500", x = 0.97, y = 0.08, align = "right", size = 1.6 })
    end
end)
-- NPCs: all placed in the editor (World > NPC, world.json "actors"), none made here.
local shopkeeper = logic.actor("shopkeeper")
local guards = logic.actors("guard")
log(("shopkeeper placed=%s id=%s at %.0f %.0f; guards %d"):format(tostring(shopkeeper.placed), tostring(shopkeeper.id), shopkeeper.x, shopkeeper.y, #guards))
-- The shop: talking to the shopkeeper (nothing until she is placed).
if shopkeeper.placed then
    createIcon({ x = shopkeeper.x, y = shopkeeper.y, z = shopkeeper.z + 230, image = "shop.png", size = 40, distance = 25, text = "Shop" })
    createInteraction({ x = shopkeeper.x, y = shopkeeper.y, z = shopkeeper.z, radius = 2, text = "Shop", onUse = function(p, id)
        log("USE " .. p.name .. " " .. id)
        p:dialog({ title = "Shop", text = "{img:shop.png} Welcome to Ava's shop", items = { "{img:box.gif} Bike\t{7CD67C}$100", "{img:box.gif} Food\t$5" }, headers = { "Item", "Price" },
            onResponse = function(p2, ok, item, text)
                log(("LIST ok=%s item=%s text=[%s]"):format(tostring(ok), tostring(item), text))
                p2:dialog({ style = "password", title = "Login", text = "Password?", onResponse = function(p3, ok3, item3, text3)
                    log(("PASSWORD ok=%s item=%s text=[%s]"):format(tostring(ok3), tostring(item3), text3))
                end })
            end })
    end })
end
-- Virtual worlds: NPCs with a world set in the editor are seen only there.
command("world", function(player, args)
    player:setWorld(tonumber(args) or 0)
    player:message("World " .. player.world, "#7CD67C")
end)

-- Server vehicles: one free bike for anyone (on everyone's map), one locked to "tester", /bike buys one.
local X, Y, Z = -85462.8, -301759.8, -5279.7
local free = createVehicle({ x = X - 300, y = Y + 200, z = Z, yaw = 90, blip = "all" })
local owned = createVehicle({ x = X - 300, y = Y - 100, z = Z, yaw = 90, owner = "tester", locked = true })
local gang = createVehicle({ x = X - 300, y = Y - 400, z = Z, yaw = 90, blip = "all", blipColor = "#E05050" })  -- a gang's, red
local stranger = createVehicle({ x = X - 300, y = Y - 700, z = Z, yaw = 90, owner = "stranger", locked = true })  -- someone else's: no marker for you
local nobody = createVehicle({ x = X - 300, y = Y - 1000, z = Z, yaw = 90, blip = "none" })                      -- nobody's, unmarked
local function nearestOwn(player)
    local px, py = player:position()
    local best, bestD = nil, 600 * 600
    for _, v in ipairs(vehicles()) do
        local d = (v.x - px) ^ 2 + (v.y - py) ^ 2
        if v.owner == player.name:lower() and d < bestD then best, bestD = v, d end
    end
    return best
end
command("bike", function(player)
    local x, y, z, yaw = player:position()
    local id = createVehicle({ x = x + 250, y = y, z = z - 90, yaw = yaw or 0, owner = player, locked = true })
    player:message("Bike " .. id .. " is yours (locked). /lock /unlock", "#7CD67C")
end)
-- /npcbike [model]: an NPC drives one round a square in front of you (10 m/s), /npcstop stops it.
local npcCar
command("npcbike", function(player, args)
    local x, y, z, yaw = player:position()
    local model = (args ~= "" and args) or "bike"
    if npcCar then removeVehicle(npcCar) end
    npcCar = createVehicle({ x = x + 500, y = y, z = z - 90, yaw = 0, model = model, npc = { name = "Driver" } })
    local s = 2500
    driveVehicle(npcCar, { points = { { x + 500 + s, y, z - 90 }, { x + 500 + s, y + s, z - 90 }, { x + 500, y + s, z - 90 }, { x + 500, y, z - 90 } }, speed = 10, loop = true })
    player:message("NPC " .. model .. " #" .. npcCar .. " drives round a 25 m square", "#7CD67C")
end)
command("npcstop", function(player) if npcCar then stopVehicle(npcCar); player:message("Stopped", "#7CD67C") end end)
command("npcgo", function(player)
    if not npcCar then return end
    local x, y, z = player:position()
    driveVehicle(npcCar, { points = { { x, y, z - 90 } }, speed = 8, onArrive = function(id) player:message("NPC arrived (#" .. id .. ")", "#7CD67C") end })
end)
on("vehicleArrive", function(id) log("vehicleArrive " .. id) end)
-- /heli: a helicopter with a pilot 20 m in front of you; [E] Board; /heligo [m]: flies that far ahead
-- (100 by default) at 40 m and lands; /heliback: back to where it was made.
local heli, heliHome
command("heli", function(player)
    local x, y, z, yaw = player:position()
    local r = math.rad(yaw or 0)
    if heli then removeVehicle(heli) end
    heliHome = { x = x + math.cos(r) * 2000, y = y + math.sin(r) * 2000, z = z - 90 }
    heli = createVehicle({ x = heliHome.x, y = heliHome.y, z = heliHome.z, yaw = yaw or 0, model = "helicopter", npc = { name = "Pilot" } })
    player:message("Helicopter #" .. heli .. ": [E] Board, /heligo [m], /heliback", "#7CD67C")
end)
command("heligo", function(player, args)
    if not heli then return end
    local v = vehicleInfo(heli)
    local x, y, z, yaw = player:position()
    local d = (tonumber(args) or 100) * 100
    local r = math.rad(yaw or 0)
    driveVehicle(heli, { points = { { v.x + math.cos(r) * d, v.y + math.sin(r) * d, v.z } }, speed = 20, altitude = 40,
        onArrive = function(id) for _, p in ipairs(vehicleInfo(id).passengers) do p:message("Landed. [E] Get out", "#7CD67C") end end })
end)
command("heliback", function(player)
    if heli and heliHome then driveVehicle(heli, { points = { { heliHome.x, heliHome.y, heliHome.z } }, speed = 20, altitude = 40 }) end
end)
-- /car golfcart|dumptruck|militarytruck|bike: one in front of you, yours, unlocked.
command("car", function(player, args)
    local model = (args ~= "" and args) or "golfcart"
    local x, y, z, yaw = player:position()
    local r = math.rad(yaw or 0)
    local ok, id = pcall(createVehicle, { x = x + math.cos(r) * 700, y = y + math.sin(r) * 700, z = z - 90, yaw = (yaw or 0) + 90, model = model, owner = player })
    if ok then player:message(model .. " #" .. id .. " is yours. [E] to get on", "#7CD67C") else player:message(tostring(id), "#E05050") end
end)
command("lock", function(player) local v = nearestOwn(player); if v then setVehicle(v.id, { locked = true }); player:message("Locked", "#7CD67C") end end)
command("unlock", function(player) local v = nearestOwn(player); if v then setVehicle(v.id, { locked = false }); player:message("Unlocked", "#7CD67C") end end)
command("vinfo", function(player)
    for _, v in ipairs(vehicles()) do
        player:message(("#%d %s owner=%s locked=%s fuel=%.2f hp=%.2f driver=%s"):format(v.id, v.blip, tostring(v.owner), tostring(v.locked), v.fuel, v.health, v.driver and v.driver.name or "-"), "#F2D16B")
    end
end)
on("vehicleEnter", function(player, id, allowed) log(("vehicleEnter %s #%d lock says %s"):format(player.name, id, tostring(allowed))) end)
on("vehicleExit", function(player, id) local v = vehicleInfo(id); log(("vehicleExit %s #%d at %.0f %.0f fuel %.2f"):format(player.name, id, v.x, v.y, v.fuel)) end)
