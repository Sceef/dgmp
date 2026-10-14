-- quest.lua — quests made of steps, shown with the game's own objective HUD and quest markers.
--
--   local Quest = require("quest")
--   local q = Quest.start(player, { title = "Припасы для лагеря", category = "camp" })
--   q:step("Доберитесь до заправки", { x = ..., y = ..., z = ..., radius = 5,
--          onEnter = function(q) q:step("Обыщите здание") end })
--   q:hold("Обыщите место", { x = ..., y = ..., z = ..., radius = 6, seconds = 10,
--          onDone = function(q, player) q:complete(150) end })   -- meter fills only inside the zone
--   q:complete()        -- or q:fail()
--
-- Categories: story, camp, infestation, ambush, nero, horde, campmission, task.
-- Styles (marker / objective look): basic, kill, killarea, defend, defendarea, search, track, nokill.

local Quest = {}
Quest.__index = Quest

function Quest.start(player, opts)
    opts = opts or {}
    local q = setmetatable({
        player = player,
        title = opts.title or "",
        category = opts.category or "story",
        style = opts.style or "basic",
        marker = nil,
        active = true,
    }, Quest)
    player:objective({ title = q.title, text = opts.text or "", state = "new", category = q.category, style = q.style })
    return q
end

-- Removes the current step's marker, if any.
function Quest:clearMarker()
    if self.marker then removeMarker(self.marker); self.marker = nil end
end

-- Shows a new step. `target` (optional): {x, y, z, radius = 3 (m), route = true, style,
-- onEnter = function(quest, player) ... end} puts a quest marker with a road route there.
function Quest:step(text, target)
    if not self.active or not self.player.valid then return end
    self:clearMarker()
    self:clearHold()
    self:clearCollect()
    self.player:objective({ title = self.title, text = text, state = "update", category = self.category, style = (target and target.style) or self.style })
    if target then
        local quest = self
        self.marker = createMarker({
            x = target.x, y = target.y, z = target.z, radius = target.radius or 3,
            category = self.category, style = target.style or self.style,
            route = target.route ~= false, player = self.player,
            title = self.title, description = text,
            track = target.onEnter ~= nil,
            onEnter = target.onEnter and function(player) if quest.active then target.onEnter(quest, player) end end,
        })
    end
end

-- A step that fills a meter only while the player stays in the zone ("search the place",
-- "defend the area"). target: {x, y, z, radius = 6 (m), seconds = 10, meter = "Поиск",
-- leaveTip = "...", style = "search", onDone = function(quest, player) end}.
function Quest:hold(text, target)
    if not self.active or not self.player.valid then return end
    self:clearMarker()
    self:clearHold()
    self:clearCollect()
    local quest, player = self, self.player
    local seconds = target.seconds or 10
    local meter = target.meter or "Поиск"
    local hold = { inside = false, progress = 0 }
    self.holdState = hold
    player:objective({ title = self.title, text = text, state = "update", category = self.category, style = target.style or "search" })
    self.marker = createMarker({
        x = target.x, y = target.y, z = target.z, radius = target.radius or 6,
        category = self.category, style = target.style or "search", route = false, player = player,
        title = self.title, description = text, track = true,
        onEnter = function() hold.inside = true end,
        onLeave = function()
            hold.inside = false
            if quest.active and quest.holdState == hold then
                player:tip(target.leaveTip or "Вернитесь в зону, чтобы продолжить", 3)
            end
        end,
    })
    local step = 0.25
    hold.timer = every(math.floor(step * 1000), function()
        if not quest.active or quest.holdState ~= hold or not player.valid then cancelTimer(hold.timer); return end
        if not hold.inside then return end
        hold.progress = math.min(1, hold.progress + step / seconds)
        player:objectiveMeter(meter, hold.progress)
        if hold.progress >= 1 then
            quest:clearHold()
            quest:clearMarker()
            player:objectiveMeter()
            if target.onDone then target.onDone(quest, player) end
        end
    end)
    player:objectiveMeter(meter, 0)
end

function Quest:clearHold()
    if self.holdState then
        if self.holdState.timer then cancelTimer(self.holdState.timer) end
        self.holdState = nil
    end
end

-- A step done by doing something `count` times (game events, see docs/API.md "gameEvent"):
--   event = "pickup" | "carry" | "search" | "body" | "interact" | "door" | "explosion" | "fire" (or a list of them)
--   item  = item key or a list of keys (pickup), class = part of the object class name,
--   ids   = list of world object names (ev.id): only these objects count, e.g. two given car trunks,
--   spawn = { {item = "kWeaponID_Molotov_Small", count = 1, x = .., y = .., z = ..}, ... }: the step
--           places these pickups for the player; only they count (count defaults to how many) and
--           whatever was not taken disappears when the step or the quest ends,
--   area  = {x, y, z, radius (m)}: only events there count; marker = true shows a quest marker
--           over the area (route = true for a road route to it),
--   count = 1, onDone = function(quest, player) end.
-- The objective line shows "(done/count)" when count > 1. The same object counts once.
--   q:collect("Обыщите багажники", { event = "search", count = 3, onDone = ... })
--   q:collect("Возьмите коктейль Молотова", { event = "pickup", item = "kConsumableID_Molotov" })
--   q:collect("Подожгите сарай", { event = "fire", area = { x = .., y = .., z = .., radius = 15 }, marker = true })
local collecting = {}  -- player id -> quest with an active collect step
function Quest:collect(text, spec)
    if not self.active or not self.player.valid then return end
    self:clearMarker()
    self:clearHold()
    self:clearCollect()
    local spawned, ids, markers, markerOf = {}, nil, {}, {}
    if spec.spawn then
        ids = {}
        for _, s in ipairs(spec.spawn) do
            local id = spawnItem({ item = s.item, count = s.count or 1, x = s.x, y = s.y, z = s.z, player = self.player })
            spawned[#spawned + 1] = id
            ids[#ids + 1] = "dgmp_item_" .. id
            -- A quest marker over each item (removed when it is taken or the step ends).
            markers[#markers + 1] = createMarker({ x = s.x, y = s.y, z = s.z, radius = 2, category = self.category, style = self.style,
                                                   player = self.player, title = self.title, description = text, track = false })
            markerOf["dgmp_item_" .. id] = markers[#markers]
        end
    end
    local count = spec.count or (spec.spawn and #spec.spawn) or 1
    local state = { spec = spec, text = text, count = count, done = 0, seen = {}, spawned = spawned, ids = ids, markers = markers, markerOf = markerOf }
    self.collectState = state
    collecting[self.player.id] = self
    self:showCollect()
    if spec.area and spec.marker then
        self.marker = createMarker({
            x = spec.area.x, y = spec.area.y, z = spec.area.z, radius = spec.area.radius or 10,
            category = self.category, style = spec.style or self.style, route = spec.route == true,
            player = self.player, title = self.title, description = text, track = false,
        })
    end
end

function Quest:showCollect()
    local c = self.collectState
    local line = c.count > 1 and string.format("%s (%d/%d)", c.text, c.done, c.count) or c.text
    self.player:objective({ title = self.title, text = line, state = "update", category = self.category, style = c.spec.style or self.style })
end

local function oneOf(want, value)  -- want: a value or a list of values
    if type(want) ~= "table" then return want == value end
    for _, v in ipairs(want) do if v == value then return true end end
    return false
end

local function matches(spec, ev, spawnedIds)
    if spec.event and not oneOf(spec.event, ev.type) then return false end
    if spec.item and not oneOf(spec.item, ev.item) then return false end
    if spec.class and not string.find(ev.class or "", spec.class, 1, true) then return false end
    if spec.ids and not oneOf(spec.ids, ev.id) then return false end
    if spawnedIds and not oneOf(spawnedIds, ev.id) then return false end
    if spec.area then
        local a = spec.area
        local dx, dy, dz = ev.x - a.x, ev.y - a.y, ev.z - (a.z or ev.z)
        local r = (a.radius or 10) * 100
        if dx * dx + dy * dy + dz * dz > r * r then return false end
    end
    return true
end

on("gameEvent", function(player, ev)
    local q = collecting[player.id]
    if not q or not q.active or not q.collectState or q.player.id ~= player.id then return end
    local c = q.collectState
    local key = ev.type .. ":" .. (ev.id ~= "" and ev.id or string.format("%d,%d", ev.x // 100, ev.y // 100))
    if c.seen[key] or not matches(c.spec, ev, c.ids) then return end
    c.seen[key] = true
    c.done = c.done + 1
    if c.markerOf and c.markerOf[ev.id] then removeMarker(c.markerOf[ev.id]); c.markerOf[ev.id] = nil end
    if c.done >= c.count then
        q:clearCollect()
        q:clearMarker()
        if c.spec.onDone then c.spec.onDone(q, player) end
    else
        q:showCollect()
    end
end)
on("playerQuit", function(player) collecting[player.id] = nil end)

function Quest:clearCollect()
    if self.collectState then
        for _, id in ipairs(self.collectState.spawned or {}) do despawnItem(id) end
        for _, id in ipairs(self.collectState.markers or {}) do removeMarker(id) end
        self.collectState = nil
        if collecting[self.player.id] == self then collecting[self.player.id] = nil end
    end
end

-- Progress bar under the objective; percent 0..1, nil hides it.
function Quest:progress(title, percent)
    if percent == nil then self.player:objectiveMeter() else self.player:objectiveMeter(title, percent) end
end

function Quest:finish(state)
    if not self.active then return end
    self.active = false
    self:clearMarker()
    self:clearHold()
    self:clearCollect()
    if self.player.valid then
        self.player:objectiveMeter()
        self.player:objective({ title = self.title, text = "", state = state, category = self.category, style = self.style, xp = self.xp or 0 })
        -- The completion callout stays a moment, then the panel is released.
        local player, category = self.player, self.category
        after(6000, function() if player.valid then player:objective({ state = "remove", category = category }) end end)
    end
end

-- complete([xp]): the completion callout shows the XP number when given
function Quest:complete(xp) self.xp = xp; self:finish("complete") end
function Quest:fail() self:finish("failed") end

return Quest
