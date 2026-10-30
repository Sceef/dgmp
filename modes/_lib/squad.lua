-- squad.lua — squads: players who see each other on the big map and the radar (a mark that follows each
-- member), a squad chat and, if the mode wants, a squad radio channel. Joining needs the invited player's
-- yes (through the player menu of interact.lua). docs/API.md "Отряд".
--
--   local squad = require("squad")
--   squad.setup({ max = 6, chat = "s", radio = false, color = "#7CD67C" })
--   squad.of(player)            -- the player's squad {id, leader = id, members = {id, ...}} or nil
--   squad.same(a, b)            -- in one squad
--   squad.leave(player)
--   squad.onChange = function(s) end   -- members changed (s.members empty: it is gone)
--
-- With interact.lua the player menu gets "Invite to the squad" (asks the other one) and "Leave the squad".
-- Commands: /<chat> text (squad chat), /squad (members), /squad leave, /squad kick <id>, /squad lead <id>.

local interact = require("interact")
local M = { onChange = nil }
local opts = { max = 6, chat = "s", radio = false, color = "#7CD67C", icon = "MAP_player_marker" }
local squads, ofPlayer = {}, {}   -- squad id -> squad; player id -> squad id
local nextId = 1
local marks = {}                  -- squad id -> {[viewer id] = {[member id] = blip}}

local function ru(p) return p and p.lang == "ru" end
local function T(p, r, e) return ru(p) and r or e end

function M.of(p) local id = p and ofPlayer[p.id]; return id and squads[id] end
function M.same(a, b) local s = M.of(a); return s ~= nil and s == M.of(b) end

local function channel(s) return "Squad " .. s.id end

-- Every member sees every other one: a mark that follows it, only for that viewer.
local function refresh(s)
    marks[s.id] = marks[s.id] or {}
    local m = marks[s.id]
    local inSquad = {}
    for _, id in ipairs(s.members) do inSquad[id] = true end
    for viewer, list in pairs(m) do
        for member, blip in pairs(list) do
            if not inSquad[viewer] or not inSquad[member] then removeBlip(blip); list[member] = nil end
        end
        if not inSquad[viewer] then m[viewer] = nil end
    end
    for _, viewer in ipairs(s.members) do
        local v = getPlayer(viewer)
        if v then
            m[viewer] = m[viewer] or {}
            for _, member in ipairs(s.members) do
                local t = getPlayer(member)
                if member ~= viewer and t and not m[viewer][member] then
                    m[viewer][member] = createBlip({ follow = t, player = v, icon = opts.icon, color = opts.color, title = t.name,
                        description = T(v, "Отряд", "Squad"), map = true, radar = true, edge = true })
                end
            end
        end
    end
    if M.onChange then pcall(M.onChange, s) end
end

local function tell(s, text)
    for _, id in ipairs(s.members) do
        local p = getPlayer(id)
        if p then p:message("{7CD67C}[" .. T(p, "Отряд", "Squad") .. "] {FFFFFF}" .. (type(text) == "function" and text(p) or text)) end
    end
end

local function create(leader)
    local s = { id = nextId, leader = leader.id, members = { leader.id } }
    nextId = nextId + 1
    squads[s.id] = s; ofPlayer[leader.id] = s.id
    if opts.radio then leader:joinVoiceChannel(channel(s), "radio") end
    return s
end

function M.join(p, leader)
    if M.of(p) then M.leave(p) end
    local s = M.of(leader) or create(leader)
    if #s.members >= opts.max then
        leader:message(T(leader, "{E07070}Отряд полон", "{E07070}The squad is full")); return false
    end
    s.members[#s.members + 1] = p.id; ofPlayer[p.id] = s.id
    if opts.radio then p:joinVoiceChannel(channel(s), "radio") end
    tell(s, function(v) return p.name .. T(v, " вступил в отряд", " joined the squad") end)
    refresh(s)
    return true
end

function M.leave(p, why)
    local s = M.of(p); if not s then return end
    for i, id in ipairs(s.members) do if id == p.id then table.remove(s.members, i) break end end
    ofPlayer[p.id] = nil
    if opts.radio and p.valid then p:leaveVoiceChannel(channel(s)) end
    if p.valid then p:message(T(p, "{7CD67C}Вы вышли из отряда", "{7CD67C}You left the squad")) end
    if #s.members <= 1 then
        -- One left: no squad any more.
        for _, id in ipairs(s.members) do
            ofPlayer[id] = nil
            local q = getPlayer(id)
            if q then
                if opts.radio then q:leaveVoiceChannel(channel(s)) end
                q:message(T(q, "{7CD67C}Отряд распущен", "{7CD67C}The squad is disbanded"))
            end
        end
        s.members = {}
        refresh(s); squads[s.id] = nil; marks[s.id] = nil
        return
    end
    if s.leader == p.id then s.leader = s.members[1] end
    local name = p.name
    tell(s, function(v) return name .. (why == "kick" and T(v, " исключён из отряда", " was removed from the squad") or T(v, " покинул отряд", " left the squad")) end)
    refresh(s)
end

function M.setup(o)
    for k, v in pairs(o or {}) do opts[k] = v end
    if opts.chat and not M.chatBound then
        M.chatBound = true
        command(opts.chat, function(p, args)
            local s = M.of(p)
            if not s then return p:message(T(p, "{E07070}Вы не в отряде", "{E07070}You are not in a squad")) end
            if args == "" then return end
            tell(s, p.name .. ": " .. args)
        end)
    end
end

interact.add({
    id = "squad_invite", order = 10,
    label = { ru = "Пригласить в отряд", en = "Invite to the squad" },
    when = function(p, t)
        local s = M.of(p)
        return not M.same(p, t) and (not s or (s.leader == p.id and #s.members < opts.max))
    end,
    consent = { ru = "%s зовёт вас в отряд", en = "%s invites you to the squad" },
    onUse = function(p, t) M.join(t, p) end,
})
interact.add({
    id = "squad_leave", order = 11,
    label = { ru = "Выйти из отряда", en = "Leave the squad" },
    when = function(p, t) return M.same(p, t) end,
    onUse = function(p) M.leave(p) end,
})

command("squad", function(p, args, what)
    local s = M.of(p)
    if not s then return p:message(T(p, "Вы не в отряде. Пригласить: посмотрите на игрока и нажмите F.", "You are not in a squad. To invite: look at a player and press F.")) end
    local rest = args:match("^%S+%s+(.+)$")
    if what == "leave" then return M.leave(p) end
    if what == "kick" or what == "lead" then
        if s.leader ~= p.id then return p:message(T(p, "{E07070}Только лидер отряда", "{E07070}Only the squad leader")) end
        local t = rest and getPlayer(tonumber(rest) or rest)
        if not t or not M.same(p, t) or t == p then return p:message(T(p, "{E07070}Нет такого в отряде", "{E07070}No such member")) end
        if what == "kick" then return M.leave(t, "kick") end
        s.leader = t.id
        return tell(s, function(v) return t.name .. T(v, " теперь лидер отряда", " now leads the squad") end)
    end
    local names = {}
    for _, id in ipairs(s.members) do
        local q = getPlayer(id)
        if q then names[#names + 1] = q.id .. " " .. q.name .. (id == s.leader and T(p, " (лидер)", " (leader)") or "") end
    end
    p:message("{7CD67C}" .. T(p, "Отряд: ", "Squad: ") .. "{FFFFFF}" .. table.concat(names, ", "))
end)

on("playerQuit", function(p) M.leave(p, "quit") end)

return M
