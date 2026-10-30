-- interact.lua — the menu at another player: look at a player close by, press F, pick an action. The mode (or
-- another library: squad, radio, trade) adds the actions; each says when it shows, whether the other player has
-- to agree first (a yes/no question on its screen, Y / N) and what it does. docs/API.md "Меню игрока".
--
--   local interact = require("interact")
--   interact.add({
--       id = "squad_invite", order = 10,
--       label = { ru = "Пригласить в отряд", en = "Invite to the squad" },
--       when = function(player, target) return not sameSquad(player, target) end,   -- shown only then
--       consent = { ru = "%s зовёт вас в отряд", en = "%s invites you to the squad" }, -- %s: the asking player
--       onUse = function(player, target) join(target, player) end,                    -- after a yes
--       onRefuse = function(player, target, reason) end,                              -- "no", "timeout", "busy"
--   })
--   interact.remove("squad_invite")
--
-- Fields: label (string or {ru, en} or function(player, target)), order (the menu is sorted by it), range
-- (metres, 3), when, consent (text or function(player, target) -> text), seconds (to answer, 15), cooldown
-- (seconds between two questions of this action to one player, 10), downed (also shown on a downed player;
-- otherwise hidden on one), self = false. interact.setup({key = "F", label = ..., range = 2.5}) changes the
-- key and the prompt (before or after adding). interact.open(player, target) shows the menu from code.

local M = {}
local actions, order = {}, {}
local menu = { key = "F", label = { ru = "Взаимодействие", en = "Interact" }, range = 2.5 }
local asked = {}   -- "action:asker:target" -> time of the last question
local enabled = false

local function L(player, v, ...)
    if type(v) == "function" then return v(player, ...) end
    if type(v) == "table" then return v[player and player.lang or "en"] or v.en or v.ru or "" end
    return v or ""
end
local function ru(player) return player and player.lang == "ru" end

local function near(a, b, metres)
    if not (a.valid and b.valid) or a.world ~= b.world then return false end
    local ax, ay, az = a:position()
    local bx, by, bz = b:position()
    local dx, dy = ax - bx, ay - by
    return dx * dx + dy * dy <= (metres * 100) ^ 2 and math.abs(az - bz) < 300
end
M.near = near

local function sorted()
    table.sort(order, function(a, b) return (actions[a].order or 100) < (actions[b].order or 100) end)
end
local function apply()
    if #order > 0 then setPlayerMenu(menu); enabled = true
    elseif enabled then setPlayerMenu(nil); enabled = false end
end

function M.setup(opts)
    for k, v in pairs(opts or {}) do menu[k] = v end
    if enabled then setPlayerMenu(menu) end
end

function M.add(def)
    assert(type(def) == "table" and type(def.id) == "string", "interact.add: an action needs an id")
    assert(type(def.onUse) == "function", "interact.add: an action needs onUse")
    if not actions[def.id] then order[#order + 1] = def.id end
    actions[def.id] = def
    sorted(); apply()
    return def
end

function M.remove(id)
    if not actions[id] then return end
    actions[id] = nil
    for i, v in ipairs(order) do if v == id then table.remove(order, i) break end end
    apply()
end

-- The actions this player may take on that one right now.
function M.available(player, target)
    local list = {}
    for _, id in ipairs(order) do
        local a = actions[id]
        local ok = near(player, target, a.range or 3)
        if ok and target.downed and not a.downed then ok = false end
        if ok and a.when then
            local fine, result = pcall(a.when, player, target)
            if not fine then log("interact: when of " .. id .. ": " .. tostring(result)) end
            ok = fine and result
        end
        if ok then list[#list + 1] = a end
    end
    return list
end

local function run(a, player, target)
    local fine, err = pcall(a.onUse, player, target)
    if not fine then log("interact: onUse of " .. a.id .. ": " .. tostring(err)) end
end

local function use(a, player, target)
    if not near(player, target, (a.range or 3) + 1) then
        return player:message(ru(player) and "{E07070}Слишком далеко" or "{E07070}Too far away")
    end
    if a.when and not a.when(player, target) then return end
    if not a.consent then return run(a, player, target) end
    local key = a.id .. ":" .. player.id .. ":" .. target.id
    if asked[key] and now() - asked[key] < (a.cooldown or 10) then
        return player:message(ru(player) and "{E07070}Подождите, прежде чем спрашивать снова" or "{E07070}Wait before asking again")
    end
    local text = string.format(L(target, a.consent, player, target), player.name)
    local pid, tid = player.id, target.id
    local ok = target:ask(text, { seconds = a.seconds or 15 }, function(t, yes, reason)
        local p = getPlayer(pid)
        if not (p and p.valid and t.valid and t.id == tid) then return end
        if yes and near(p, t, (a.range or 3) + 3) then return run(a, p, t) end
        if yes then reason = "far" end
        p:message(reason == "timeout" and (ru(p) and ("{E0A070}" .. t.name .. " не ответил") or ("{E0A070}" .. t.name .. " did not answer"))
            or reason == "far" and (ru(p) and "{E07070}Слишком далеко" or "{E07070}Too far away")
            or (ru(p) and ("{E07070}" .. t.name .. " отказался") or ("{E07070}" .. t.name .. " said no")))
        if a.onRefuse then pcall(a.onRefuse, p, t, yes and "far" or reason == "timeout" and "timeout" or "no") end
    end)
    if not ok then
        player:message(ru(player) and ("{E0A070}" .. target.name .. " сейчас отвечает на другой вопрос") or ("{E0A070}" .. target.name .. " is answering another question"))
        if a.onRefuse then pcall(a.onRefuse, player, target, "busy") end
        return
    end
    asked[key] = now()
    player:message(ru(player) and ("{AAAAAA}Ждём ответа " .. target.name .. "...") or ("{AAAAAA}Waiting for " .. target.name .. "..."))
end

function M.open(player, target)
    local list = M.available(player, target)
    if #list == 0 then
        return player:tip(ru(player) and "Здесь нечего сделать" or "Nothing to do here", 3)
    end
    local items = {}
    for i, a in ipairs(list) do items[i] = L(player, a.label, target) end
    local ids = {}
    for i, a in ipairs(list) do ids[i] = a.id end
    local tid = target.id
    player:dialog({
        style = "list", title = target.name, items = items,
        button1 = ru(player) and "Выбрать" or "Choose",
        onResponse = function(p, ok, item)
            if not ok or not item then return end
            local t, a = getPlayer(tid), actions[ids[item]]
            if t and t.valid and a then use(a, p, t) end
        end,
    })
end

on("playerInteract", function(player, target) M.open(player, target) end)
on("playerQuit", function(player)
    for key in pairs(asked) do
        if key:find(":" .. player.id .. ":", 1, true) or key:sub(-#tostring(player.id) - 1) == ":" .. player.id then asked[key] = nil end
    end
end)

return M
