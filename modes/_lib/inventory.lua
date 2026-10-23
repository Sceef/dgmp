-- inventory.lua — the mode's own inventory: items the mode defines (a gas can, a radio, a key), containers that
-- hold them (a player's bag, a vehicle's trunk, a stash in a house) and the item window to look into one or two
-- of them and move things between them. Everything is decided here, on the server; the game's own backpack
-- (ammo, weapons: player:giveItem) is separate. docs/API.md "Свой инвентарь".
--
--   local inventory = require("inventory")
--   inventory.define("gas", { name = { en = "Gas can", ru = "Канистра" }, icon = "gas.png", weight = 5, stack = 1,
--       text = { en = "10 l of fuel", ru = "10 л бензина" },
--       actions = { { id = "refuel", label = { en = "Refuel", ru = "Заправить" },
--                     fn = function(player, ctx) ... return true end } } })   -- true: one of it is used up
--   local bag = inventory.of(player)                       -- the player's bag (saved between visits)
--   bag:add("gas", 1)
--   inventory.open(player, bag, inventory.trunk(vehicleId))   -- the window: bag | trunk
--
-- Containers: inventory.container(id, {title=, maxWeight= (kg, nil: no limit), slots= (different items, nil: no
-- limit), persist = true (kept in storage as "inv:<id>"), accepts = function(itemId) return bool end}).
-- inventory.batch(fn): several changes saved and shown once. Methods: add(item, count [, data]) -> added, remove(item, count) -> removed, count(item), has(item [, count]),
-- weight(), canAdd(item, count) -> how many fit, list() -> {{id, count, data}}, clear(), set(list).
-- Events (optional): inventory.onChange = function(container) end, inventory.onMove = function(player, item,
-- count, from, to) return false end (false refuses the move).

local M = { onChange = nil, onMove = nil }
M.bag = { maxWeight = 30, slots = 24 }       -- every player's bag (change before the first inventory.of)
M.trunkSize = { maxWeight = 60, slots = 20 } -- a trunk unless inventory.trunk says otherwise

local defs = {}        -- item id -> definition
local containers = {}  -- container id -> container
local open = {}        -- player id -> {player, a, b, opts}: what its item window shows

local function L(player, v)
    if type(v) == "table" then return v[player and player.lang or "en"] or v.en or v.ru or "" end
    return v
end
local function ru(player) return player and player.lang == "ru" end

-- ---------------------------------------------------------------- items
function M.define(id, def)
    assert(type(id) == "string" and id ~= "", "inventory.define: an item needs an id")
    def = def or {}
    def.id = id
    def.weight = def.weight or 0
    def.stack = def.stack or 1000000   -- how many make one slot (a stack); a second stack takes another slot
    defs[id] = def
    return def
end
function M.item(id) return defs[id] end

-- ---------------------------------------------------------------- containers
local Container = {}
Container.__index = Container

local holding, held = 0, {}  -- inside M.batch: what changed, told once at its end
local function saved(c)
    if c.persist then storage.set("inv:" .. c.id, c.items) end
    if M.onChange then M.onChange(c) end
end
local function showChanges(set)  -- every window showing one of these, updated once
    for _, w in pairs(open) do
        if set[w.a] or (w.b and set[w.b]) then M.refresh(w.player) end
    end
end
local function changed(c)
    if holding > 0 then held[c] = true return end
    saved(c)
    showChanges({ [c] = true })
end

function Container:weight()
    local w = 0
    for _, it in ipairs(self.items) do w = w + (defs[it.id] and defs[it.id].weight or 0) * it.count end
    return w
end
function Container:count(item)
    local n = 0
    for _, it in ipairs(self.items) do if it.id == item then n = n + it.count end end
    return n
end
function Container:has(item, count) return self:count(item) >= (count or 1) end
function Container:list()
    local out = {}
    for i, it in ipairs(self.items) do out[i] = { id = it.id, count = it.count, data = it.data } end
    return out
end
-- How many of the item fit (weight, free slots, what it accepts).
function Container:canAdd(item, count)
    local def = defs[item]
    if not def or (self.accepts and not self.accepts(item)) then return 0 end
    count = count or 1
    local fit = count
    if self.maxWeight and def.weight > 0 then fit = math.min(fit, math.floor((self.maxWeight - self:weight()) / def.weight + 1e-6)) end
    if self.slots then
        local room, used = 0, #self.items
        for _, it in ipairs(self.items) do if it.id == item and not it.data then room = room + def.stack - it.count end end
        room = room + math.max(0, self.slots - used) * def.stack
        fit = math.min(fit, room)
    end
    return math.max(0, fit)
end
-- Adds what fits; items with their own data (a radio's frequency) each take a slot of their own.
function Container:add(item, count, data)
    count = math.min(count or 1, self:canAdd(item, count or 1))
    if count <= 0 then return 0 end
    local def, left = defs[item], count
    if not data then
        for _, it in ipairs(self.items) do
            if left <= 0 then break end
            if it.id == item and not it.data and it.count < def.stack then
                local k = math.min(left, def.stack - it.count)
                it.count, left = it.count + k, left - k
            end
        end
    end
    while left > 0 do
        local k = data and left or math.min(left, def.stack)
        self.items[#self.items + 1] = { id = item, count = k, data = data }
        left = left - k
    end
    changed(self)
    return count
end
function Container:remove(item, count)
    count = count or 1
    local left = count
    for i = #self.items, 1, -1 do
        local it = self.items[i]
        if left <= 0 then break end
        if it.id == item then
            local k = math.min(left, it.count)
            it.count, left = it.count - k, left - k
            if it.count <= 0 then table.remove(self.items, i) end
        end
    end
    if left < count then changed(self) end
    return count - left
end
function Container:clear() self.items = {}; changed(self) end
function Container:set(list) self.items = list or {}; changed(self) end

-- Several changes, one save and one window update per container (a move takes and puts).
function M.batch(fn)
    holding = holding + 1
    local ok, err = pcall(fn)
    holding = holding - 1
    if holding == 0 then
        local list = held; held = {}
        for c in pairs(list) do saved(c) end
        showChanges(list)
    end
    if not ok then error(err, 0) end
end

function M.container(id, opts)
    local c = containers[id]
    if c then return c end
    opts = opts or {}
    c = setmetatable({ id = id, title = opts.title, maxWeight = opts.maxWeight, slots = opts.slots, persist = opts.persist,
                       accepts = opts.accepts, items = {} }, Container)
    if c.persist then c.items = storage.get("inv:" .. id) or {} end
    containers[id] = c
    return c
end
function M.get(id) return containers[id] end

-- A player's bag: kept under the Steam account (or the nickname without one).
function M.of(player)
    local key = player.verified and player.steamId or ("nick:" .. player.name:lower())
    local c = M.container("player:" .. key, { title = { en = "Backpack", ru = "Рюкзак" }, maxWeight = M.bag.maxWeight, slots = M.bag.slots, persist = true })
    return c
end
-- A vehicle's trunk. opts.key: a name that outlives the vehicle id (a world.json vehicle, a player's own car) to keep it.
function M.trunk(vehicleId, opts)
    opts = opts or {}
    local id = opts.key and ("trunk:" .. opts.key) or ("vehicle:" .. vehicleId)
    return M.container(id, { title = opts.title or { en = "Trunk", ru = "Багажник" }, maxWeight = opts.maxWeight or M.trunkSize.maxWeight,
                             slots = opts.slots or M.trunkSize.slots, persist = opts.key ~= nil, accepts = opts.accepts })
end

-- ---------------------------------------------------------------- the window
local function pane(player, c, which)
    local items = {}
    for _, it in ipairs(c.items) do
        local def = defs[it.id] or { name = it.id }
        local acts = {}
        for _, a in ipairs(def.actions or {}) do
            if not a.when or a.when(player, c) then acts[#acts + 1] = { id = a.id, label = L(player, a.label or a.id) } end
        end
        local text = type(def.text) == "function" and def.text(it, player) or L(player, def.text)
        items[#items + 1] = { id = it.id, name = L(player, def.name or it.id), count = it.count, icon = def.icon, text = text, actions = acts }
    end
    local info
    if c.maxWeight then info = string.format(ru(player) and "%.1f / %g кг" or "%.1f / %g kg", c:weight(), c.maxWeight) end
    return { id = which, title = L(player, c.title or c.id), info = info, items = items,
             empty = ru(player) and "Пусто" or "Empty" }
end

function M.refresh(player)
    local w = open[player.id]
    if not w or not player.valid then return end
    local panes = { pane(player, w.a, "a") }
    if w.b then panes[2] = pane(player, w.b, "b") end
    player:itemWindow({ title = L(player, w.opts.title), panes = panes, hint = L(player, w.opts.hint) }, function(p, action) M.handle(p, action) end)
end

-- The window over one container, or two side by side (things move between them). opts: title, hint.
function M.open(player, a, b, opts)
    open[player.id] = { player = player, a = a, b = b, opts = opts or {} }
    M.refresh(player)
end
function M.close(player)
    if open[player.id] then open[player.id] = nil; player:closeItemWindow() end
end
function M.isOpen(player) return open[player.id] ~= nil end

-- Moves count of the i-th item of `from` to `to` (as much as fits). -> moved
function M.move(player, from, i, to, count)
    local it = from.items[i]
    if not it then return 0 end
    count = math.min(count or it.count, it.count, to:canAdd(it.id, count or it.count))
    if count <= 0 then
        player:message(ru(player) and "Не помещается." or "It does not fit.", "#E07070")
        return 0
    end
    if M.onMove and M.onMove(player, it.id, count, from, to) == false then return 0 end
    local id, data = it.id, it.data
    M.batch(function()
        -- Taken from this very stack (an item with data is its own stack).
        it.count = it.count - count
        if it.count <= 0 then table.remove(from.items, i) end
        changed(from)
        to:add(id, count, data)
    end)
    return count
end

function M.handle(player, action)
    local w = open[player.id]
    if not w then return end
    if action.type == "close" then open[player.id] = nil; return end
    local from = action.pane == 2 and w.b or w.a
    local other = action.pane == 2 and w.a or w.b
    if not from then return end
    if action.type == "move" and other then
        M.move(player, from, tonumber(action.item), other, tonumber(action.count))
    elseif action.type == "action" then
        local it = from.items[tonumber(action.item) or 0]
        local def = it and defs[it.id]
        for _, a in ipairs(def and def.actions or {}) do
            if a.id == action.id and a.fn and (not a.when or a.when(player, from)) then
                local used = a.fn(player, { item = it, container = from, other = other })
                if used then from:remove(it.id, type(used) == "number" and used or 1) end
                break
            end
        end
    end
end

on("playerQuit", function(player) open[player.id] = nil end)

return M
