-- Loader job: an example of a repeatable job built on boxes to carry (docs/API.md "Переноска", "Работа: пример").
--
--   1. The foreman (an NPC) offers the job: [F] at him -> "Take the job". The player gets the work uniform,
--      markers for the warehouse and the truck, and a line on the screen.
--   2. Crates lie at the warehouse. Only a player on the job may pick one up (hold F). A crate put down
--      at the truck is delivered: it goes, $5 is added to the shift, a new crate appears at the warehouse.
--   3. [F] at the foreman again -> "Hand the job in": the shift is paid, the uniform comes off, the markers
--      and the line go. Leaving the server ends the shift unpaid; a crate in the hands goes back.
--
-- Places come from the editor (World > mode > Logic): points "loader_boss", "loader_pickup" and
-- "loader_depot". Until they are placed, the defaults below (next to the test server's spawn) are used.
local logic = require("logic")

local PAY = 5                 -- $ a crate
local CRATES = 4              -- crates waiting at the warehouse
local DEPOT_RADIUS = 4        -- m: put down this close to the truck = delivered
local UNIFORM = "m_mil_diamondlake_worker_001"
local FOREMAN_LOOK = "m_mil_diamondlake_worker_003"

local function place(name, default)
    local p = logic.point(name)
    if p.placed then return p end
    return default
end
local boss   = place("loader_boss",   { x = -85300, y = -301500, z = -5340, yaw = 180 })
local pickup = place("loader_pickup", { x = -85450, y = -302250, z = -5348, yaw = 0 })
local depot  = place("loader_depot",  { x = -84900, y = -301700, z = -5340, yaw = 0 })

local shifts = {}             -- player id -> { delivered, earned, markers = {...} }

local function ru(player) return player.lang == "ru" end
local function T(player, r, e) return ru(player) and r or e end
local function dist2(a, b) local dx, dy = a.x - b.x, a.y - b.y; return dx * dx + dy * dy end

-- ---------------------------------------------------------------- crates at the warehouse
local function crateSpot(i)
    -- A small ring around the warehouse point, so that crates do not lie inside each other.
    local a = (i or math.random(1, 360)) * 1.7
    return pickup.x + math.cos(a) * 150, pickup.y + math.sin(a) * 150, pickup.z
end
local function newCrate(i)
    local x, y, z = crateSpot(i)
    return createCarry({ model = "crate", x = x, y = y, z = z, yaw = math.random(0, 359), tag = "loader", roll = "break" })
end
local function backToWarehouse(id)
    local x, y, z = crateSpot()
    setCarry(id, { x = x, y = y, z = z })
end

on("start", function()
    for i = 1, CRATES do newCrate(i) end
end)

-- A crate left lying somewhere else (dropped on the way, its carrier left) goes back after a minute.
local strayed = {}            -- crate id -> since
every(10000, function()
    local t = now()
    for _, c in ipairs(carries("loader")) do
        local away = not c.carrier and dist2(c, pickup) > 600 * 600 and dist2(c, depot) > 600 * 600
        if away then
            strayed[c.id] = strayed[c.id] or t
            if t - strayed[c.id] > 60 then backToWarehouse(c.id); strayed[c.id] = nil end
        else strayed[c.id] = nil end
    end
end)

-- ---------------------------------------------------------------- the shift
local function shiftHud(player)
    local s = shifts[player.id]
    if not s then player:hud("loader"); return end
    player:hud("loader", {
        text = T(player, "{F2D16B}Грузчик{FFFFFF}: ящиков " .. s.delivered .. " · {7CD67C}$" .. s.earned .. "\n{A0A0A0}Несите ящики со склада к грузовику",
                         "{F2D16B}Loader{FFFFFF}: crates " .. s.delivered .. " · {7CD67C}$" .. s.earned .. "\n{A0A0A0}Carry crates from the warehouse to the truck"),
        x = 0.985, y = 0.11, align = "right", size = 1.1, bg = "#00000088",
    })
end

local function startShift(player)
    if shifts[player.id] then return end
    local s = { delivered = 0, earned = 0, markers = {} }
    shifts[player.id] = s
    player:setUniform(UNIFORM)
    s.markers[1] = createMarker({ x = pickup.x, y = pickup.y, z = pickup.z, radius = 6, category = "task", style = "search",
                                  player = player, track = false, title = T(player, "Склад", "Warehouse") })
    s.markers[2] = createMarker({ x = depot.x, y = depot.y, z = depot.z, radius = DEPOT_RADIUS, category = "task", style = "basic", route = true,
                                  player = player, track = false, title = T(player, "Грузовик", "Truck") })
    shiftHud(player)
    player:tip(T(player, "Поднимите ящик на складе (зажмите F) и отнесите к грузовику", "Pick up a crate at the warehouse (hold F) and carry it to the truck"), 6)
end

-- Ends the shift: pays what was earned (pay = false: nothing), takes the uniform, the markers and the line away.
local function endShift(player, pay)
    local s = shifts[player.id]
    if not s then return end
    shifts[player.id] = nil
    for _, m in ipairs(s.markers) do removeMarker(m) end
    if player.valid then
        if player.carrying then backToWarehouse(player.carrying) end
        player:hud("loader")
        player:setUniform(nil)
        if pay and s.earned > 0 then
            player:giveMoney(s.earned)
            player:message(T(player, "Смена сдана: ящиков " .. s.delivered .. ", заработано $" .. s.earned,
                                     "Shift handed in: " .. s.delivered .. " crates, earned $" .. s.earned), 0x8FE388FF)
        end
    end
end

-- The foreman and his [F].
createActor({ x = boss.x, y = boss.y, z = boss.z, yaw = boss.yaw, look = FOREMAN_LOOK, anim = "idle", name = "Foreman" })
createInteraction({
    x = boss.x, y = boss.y, z = boss.z, radius = 2.5, text = "Foreman / Бригадир",
    onUse = function(player)
        local s = shifts[player.id]
        if not s then
            player:dialog({
                title = T(player, "Работа грузчиком", "Loader job"),
                text = T(player, "Перенесите ящики со склада к грузовику.\n{7CD67C}$" .. PAY .. "{FFFFFF} за каждый ящик, оплата при сдаче смены.\nУроните ящик в перекате — разобьётся.",
                                 "Carry crates from the warehouse to the truck.\n{7CD67C}$" .. PAY .. "{FFFFFF} a crate, paid when you hand the job in.\nRoll with a crate and it breaks."),
                button1 = T(player, "Взять работу", "Take the job"), button2 = T(player, "Нет", "No"),
                onResponse = function(p, ok) if ok then startShift(p) end end,
            })
        else
            player:dialog({
                title = T(player, "Сдать работу", "Hand the job in"),
                text = T(player, "Ящиков: " .. s.delivered .. "\nК выплате: {7CD67C}$" .. s.earned, "Crates: " .. s.delivered .. "\nTo be paid: {7CD67C}$" .. s.earned),
                button1 = T(player, "Сдать", "Hand in"), button2 = T(player, "Работать дальше", "Keep working"),
                onResponse = function(p, ok) if ok then endShift(p, true) end end,
            })
        end
    end,
})

-- ---------------------------------------------------------------- crates: who may take them, where they count
on("carryPick", function(player, id)
    local c = getCarry(id)
    if c and c.tag == "loader" and not shifts[player.id] then
        player:tip(T(player, "Сначала возьмите работу у бригадира", "Take the job from the foreman first"), 4)
        return false
    end
end)

on("carryDrop", function(player, id, info)
    local c = getCarry(id)
    if not c or c.tag ~= "loader" then return end
    local s = shifts[player.id]
    if info.reason == "put" and s and dist2(info, depot) < (DEPOT_RADIUS * 100) ^ 2 then
        removeCarry(id)                 -- delivered: it leaves the hands and the world
        newCrate()                      -- and the warehouse gets a new one
        s.delivered, s.earned = s.delivered + 1, s.earned + PAY
        shiftHud(player)
        player:tip(T(player, "Ящик доставлен: +$" .. PAY, "Crate delivered: +$" .. PAY), 3)
    elseif info.reason == "break" then
        newCrate()                      -- (the broken one is removed by the server)
        player:tip(T(player, "Ящик разбит! Возьмите новый на складе", "The crate broke! Take a new one at the warehouse"), 4)
    elseif info.reason == "quit" or info.reason == "death" then
        backToWarehouse(id)
    end
end)

on("playerQuit", function(player) endShift(player, false) end)

on("playerJoin", function(player)
    player:message(T(player, "Работа грузчиком: подойдите к бригадиру и нажмите F", "Loader job: walk up to the foreman and press F"), 0x9FD3FFFF)
end)
