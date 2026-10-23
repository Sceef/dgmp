-- Zombies (docs/zombies.md, server.ini Zombies=1). Zombie zones keep themselves filled while a player is
-- near: placed in the editor ("World" > "Zombies", world.json "zombieZones"), or the one below at the
-- spawn when the map has none. A kill pays its killer; /horde brings a pack in front of you.
-- Admin commands: /z [count] [type] [metres], /zown <id> [player id|off], /zclear, /zlist, /zones, /zone <name> on|off.

local GREEN, GOLD, RED = "#7CD67C", "#F2D16B", "#FF7070"
local BOUNTY = 10

if #zombieZones() == 0 then
    local s = server.spawn
    createZombieZone({ name = "spawn", x = s.x + 3000, y = s.y, z = s.z, radius = 25, types = { "Swarmer" }, max = 5, respawn = 45 })
end

on("playerJoin", function(player)
    player:message("Zombies: " .. #zombieZones() .. " zone(s) fill up when you come near. /horde [count] brings a pack.", GREEN)
end)

-- A point `metres` in front of the player.
local function ahead(player, metres)
    local x, y, z, yaw = player:position()
    local a = math.rad(yaw)
    return x + math.cos(a) * metres * 100, y + math.sin(a) * metres * 100, z
end

command("horde", function(player, args, count)
    local x, y, z = ahead(player, 25)
    local id = createHorde(x, y, z, math.min(tonumber(count) or 8, 30), { radius = 8, types = { "Swarmer", "Newt" }, world = player.world })
    if not id then return player:message("No horde: zombies are off (server.ini Zombies=1) or the server has too many.", RED) end
    player:message("Horde " .. id .. " is coming.", GOLD)
end)

on("zombieDeath", function(z, killer)
    if killer then
        killer:giveMoney(BOUNTY)
        killer:tip("+" .. BOUNTY .. " $  (" .. z.type .. ")")
    end
end)
on("hordeDeath", function(id) broadcast("Horde " .. id .. " is dead.", GOLD) end)

-- ---------------------------------------------------------------- admin tools
local function admin(player) if player.admin then return true end player:message("Admins only.", RED) end

command("z", function(player, args, count, kind, metres)
    if not admin(player) then return end
    count = tonumber(count) or 1; kind = kind or "Swarmer"
    local x, y, z = ahead(player, tonumber(metres) or 15)
    for _ = 1, count do
        if not createZombie(x, y, z, kind, 0, player.world) then return player:message("createZombie failed (Zombies=1?)", RED) end
    end
end)
command("zown", function(player, args, id, who)  -- that zombie is run by your (that player's) game; off: the nearest again
    if not admin(player) then return end
    local owner = who == "off" and nil or (who and getPlayer(tonumber(who) or who) or player)
    player:message(tostring(setZombieOwner(tonumber(id) or -1, owner)))
end)
command("zclear", function(player) if admin(player) then for _, z in ipairs(zombies()) do removeZombie(z.id) end end end)
command("zlist", function(player)
    if not admin(player) then return end
    for _, z in ipairs(zombies()) do
        player:message(("%d %s hp %s owner %s %s%s"):format(z.id, z.type, tostring(z.health), z.owner and z.owner.name or "-",
            z.zone and ("zone " .. z.zone) or z.horde and ("horde " .. z.horde) or "", z.dead and " dead" or ""))
    end
end)
command("zones", function(player)
    for _, zone in ipairs(zombieZones()) do
        player:message(("%s: %d/%d alive, %s, %s"):format(zone.name, zone.alive, zone.max, table.concat(zone.types, ", "), zone.enabled and "on" or "off"))
    end
end)
command("zone", function(player, args, name, state)
    if not admin(player) then return end
    if not getZombieZone(name or "") then return player:message("No such zone: /zones lists them.", RED) end
    setZombieZone(name, { enabled = state ~= "off" })
end)
