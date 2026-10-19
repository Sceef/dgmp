-- Carry test (temporary): boxes carried in both hands (docs/API.md "Переноска").
--   /crate [keep|drop|break] - a crate on the ground in front of you; what a dodge roll does to it (keep)
--   /depot  - the delivery point where you stand: a crate put down there is delivered
--   /give   - a crate straight into your hands      /drop - out of your hands
-- world.json has one box of every model (placed in the editor). Server console: "boxes" lists them,
-- "near <key>" puts the first player next to one, "take <key>" puts it into the hands.
local depot, depotMarker = nil, nil
local delivered = 0

local function front(player, metres)
    local x, y, z, yaw = player:position()
    local r = math.rad(yaw)
    return x + math.cos(r) * metres * 100, y + math.sin(r) * metres * 100, z - 90, yaw
end

-- Where each crate was made: a crate broken in a roll comes back there (the job starts over).
local origin = {}

local function makeCrate(x, y, z, yaw, roll)
    local id = createCarry({ model = "crate", x = x, y = y, z = z, yaw = yaw, tag = "cargo", roll = roll })
    origin[id] = { x = x, y = y, z = z, yaw = yaw, roll = roll }
    return id
end

command("crate", function(player, args)
    local roll = args ~= "" and args or "keep"
    if roll ~= "keep" and roll ~= "drop" and roll ~= "break" then player:message("/crate keep | drop | break", 0xFF6666FF) return end
    local x, y, z, yaw = front(player, 2)
    local id = makeCrate(x, y, z, yaw, roll)
    player:message("Ящик " .. id .. " (перекат: " .. roll .. "): подойди и зажми F", 0x9FD3FFFF)
end)

command("give", function(player)
    local x, y, z = player:position()
    local id = createCarry({ x = x, y = y, z = z - 90, tag = "cargo" })
    if not player:giveCarry(id) then removeCarry(id); player:message("Руки заняты", 0xFF6666FF) end
end)

command("drop", function(player) player:dropCarry() end)

command("depot", function(player)
    local x, y, z = player:position()
    depot = { x = x, y = y, z = z - 90 }
    if depotMarker then removeMarker(depotMarker) end
    depotMarker = createMarker({ x = depot.x, y = depot.y, z = depot.z, radius = 4, category = "task", style = "basic", title = "Склад", track = false })
    player:message("Точка сдачи здесь: положи ящик рядом (4 м)", 0x9FD3FFFF)
end)

on("carryPick", function(player, id)
    log(player.name .. " picks up box " .. id)
end)

on("carryDrop", function(player, id, info)
    log(player.name .. " box " .. id .. " " .. info.reason .. string.format(" at %.0f %.0f %.0f", info.x, info.y, info.z))
    if info.reason == "break" then
        local o = origin[id]
        origin[id] = nil
        player:message("Ящик разбился! Новый ждёт на месте выдачи", 0xFF9966FF)
        if o then makeCrate(o.x, o.y, o.z, o.yaw, o.roll) end
        return
    end
    if info.reason == "put" and depot then
        local dx, dy = info.x - depot.x, info.y - depot.y
        if dx * dx + dy * dy < 400 * 400 then
            removeCarry(id)   -- delivered: it leaves the hands and the world
            delivered = delivered + 1
            player:message("Ящик сдан! Всего: " .. delivered, 0x8FE388FF)
        end
    end
end)

on("playerJoin", function(player)
    player:message("Переноска: /crate [keep|drop|break], /depot, /give, /drop", 0x9FD3FFFF)
end)

-- The server console runs the same commands for the first player: "crate", "give", "drop", "depot".
on("console", function(line)
    local cmd = line:match("^(%S+)")
    local player = players()[1]
    if not player or not ({ crate = true, give = true, drop = true, depot = true })[cmd] then return end
    if cmd == "crate" then local x, y, z, yaw = front(player, 2); log("box " .. makeCrate(x, y, z, yaw, line:match("^%S+%s+(%S+)") or "keep"))
    elseif cmd == "give" then local x, y, z = player:position(); local id = createCarry({ model = line:match("^%S+%s+(%S+)") or "crate", x = x, y = y, z = z - 90 }); log("give " .. tostring(player:giveCarry(id)))
    elseif cmd == "drop" then log("drop " .. tostring(player:dropCarry()))
    else local x, y, z = player:position(); depot = { x = x, y = y, z = z - 90 }; log("depot set") end
    return true
end)

-- Boxes placed in the editor (world.json "carries"): "boxes" in the server console lists them.
on("console", function(line)
    local near = line:match("^near (%S+)")
    if near then  -- "near <key>": the first player stands 60 cm from that box, facing it
        local b, player = getCarry(near), players()[1]
        if b and player then player:setPosition(b.x - 60, b.y, b.z + 95, 0) end
        return true
    end
    local key = line:match("^take (%S+)")
    if key then  -- "take <key>": that box into the first player's hands
        local b, player = getCarry(key), players()[1]
        log("take " .. key .. ": " .. tostring(b and player and player:giveCarry(b.id)))
        return true
    end
    if line ~= "boxes" then return end
    for _, b in ipairs(carries()) do
        log(string.format("box %d key=%s model=%s tag=%s roll=%s at %.0f %.0f %.0f", b.id, tostring(b.key), b.model, b.tag, b.roll, b.x, b.y, b.z))
    end
    local d = getCarry("dock1")
    log("getCarry('dock1') -> " .. (d and d.id or "nil") .. ", carries('cargo') -> " .. #carries("cargo"))
    return true
end)
