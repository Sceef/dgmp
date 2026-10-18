-- Logic objects of a mode: named triggers and points that the mode's code asks for and the editor
-- places (World > mode > Logic; stored in modes/<mode>/world.json, "logic").
--
--   local logic = require("logic")
--   local start  = logic.trigger("start", { radius = 8 })       -- one trigger
--   local checks = logic.triggers("checkpoint", { radius = 12 }) -- an ordered list of triggers
--   local lobby  = logic.point("lobby")                           -- one point (x, y, z, yaw)
--   local grid   = logic.points("grid")                           -- an ordered list of points
--   local ava    = logic.actor("shopkeeper")                       -- an NPC placed in the editor (World > NPC)
--   local guards = logic.actors("guard")                           -- every NPC of the group "guard"
--
--   start:onEnter(function(player) ... end)
--   start:onLeave(function(player) ... end)
--   if start:inside(player) then ... end
--
-- The names must be written as plain strings ("start", not a variable): the editor finds them in
-- the mode's files. Until a name is placed in the editor, `placed` is false and its position is 0.
-- Radii are in metres. Saving in the editor reloads the mode with the new positions.

local logic = {}
local data = worldLogic()
local triggers = {}   -- every trigger made, for the enter/leave tracking

local Trigger = {}
Trigger.__index = Trigger

local function entry(name, index)
    local v = data[name]
    if type(v) ~= "table" then return nil end
    if v[1] ~= nil or next(v) == nil then v = v[index or 1] end   -- a list
    return v
end

local function makePoint(v)
    local p = { x = 0, y = 0, z = 0, yaw = 0, placed = false }
    if v then p.x, p.y, p.z, p.yaw, p.placed = v.x or 0, v.y or 0, v.z or 0, v.yaw or 0, true end
    return p
end

local function makeTrigger(name, v, opts)
    local t = makePoint(v)
    t.name = name
    t.radius = (v and v.radius) or (opts and opts.radius) or 5
    t.height = (opts and opts.height) or math.max(t.radius, 5)   -- metres above and below the point
    t.enter, t.leave, t.players = {}, {}, {}
    setmetatable(t, Trigger)
    triggers[#triggers + 1] = t
    return t
end

-- `fn(player, trigger)` when a player comes in / goes out.
function Trigger:onEnter(fn) self.enter[#self.enter + 1] = fn; return self end
function Trigger:onLeave(fn) self.leave[#self.leave + 1] = fn; return self end

-- Whether the player stands in the trigger now: a cylinder of `radius`, `height` up and down.
function Trigger:inside(player)
    if not self.placed or not player.valid then return false end
    local x, y, z = player:position()
    local dx, dy = x - self.x, y - self.y
    local r = self.radius * 100
    return dx * dx + dy * dy <= r * r and math.abs(z - self.z) <= self.height * 100
end

-- A quest marker on the trigger (createMarker options; position and radius come from the trigger).
function Trigger:marker(opts)
    opts = opts or {}
    opts.x, opts.y, opts.z = self.x, self.y, self.z
    opts.radius = opts.radius or self.radius
    if opts.track == nil then opts.track = false end   -- the trigger itself reports enter/leave
    return createMarker(opts)
end

-- NPCs placed in the editor (World > NPC, world.json "actors"): one by its id, or all of a group.
-- Each is {id (for setActor/removeActor), key, group, x, y, z, yaw, name, world, placed}.
function logic.actor(name)
    return getActor(name) or { id = nil, key = name, x = 0, y = 0, z = 0, yaw = 0, placed = false }
end
function logic.actors(group)
    local list = {}
    for _, a in ipairs(worldActors()) do if a.group == group then list[#list + 1] = a end end
    return list
end

function logic.trigger(name, opts) return makeTrigger(name, entry(name), opts) end
function logic.point(name) return makePoint(entry(name)) end

function logic.triggers(name, opts)
    local list, v = {}, data[name]
    if type(v) == "table" and v[1] ~= nil then
        for i, e in ipairs(v) do local t = makeTrigger(name, e, opts); t.index = i; list[i] = t end
    elseif type(v) == "table" and next(v) ~= nil then
        local t = makeTrigger(name, v, opts); t.index = 1; list[1] = t
    end
    return list
end

function logic.points(name)
    local list, v = {}, data[name]
    if type(v) == "table" and v[1] ~= nil then
        for i, e in ipairs(v) do list[i] = makePoint(e) end
    elseif type(v) == "table" and next(v) ~= nil then
        list[1] = makePoint(v)
    end
    return list
end

-- Enter / leave tracking, 10 times a second.
local acc = 0
on("tick", function(dt)
    acc = acc + dt
    if acc < 0.1 or #triggers == 0 then return end
    acc = 0
    local all = players()
    for _, t in ipairs(triggers) do
        if t.placed and (#t.enter > 0 or #t.leave > 0) then
            for _, p in ipairs(all) do
                local now = t:inside(p)
                local was = t.players[p.id]
                if now and not was then
                    t.players[p.id] = true
                    for _, fn in ipairs(t.enter) do fn(p, t) end
                elseif was and not now then
                    t.players[p.id] = nil
                    for _, fn in ipairs(t.leave) do fn(p, t) end
                end
            end
        end
    end
end)

on("playerQuit", function(player)
    for _, t in ipairs(triggers) do t.players[player.id] = nil end
end)

return logic
