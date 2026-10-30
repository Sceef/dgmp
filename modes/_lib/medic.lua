-- medic.lua — help for the wounded (setDeathMode("downed")): a downed player presses H, the medics on the
-- server get the call (a yes/no question; the one who takes it gets the way there), and a medic revives it
-- from the player menu (look at the wounded one, F, "Revive": a few seconds on the knees). With no medic on the
-- server the wounded one is told so and may give up at once (to the respawn). docs/API.md "Ранение".
--
--   local medic = require("medic")
--   medic.setup({
--       isMedic = function(p) return p.data.job == "medic" end,   -- who may take calls and revive (nil: anyone)
--       seconds = 5, health = 30,                                  -- how long reviving takes, health after it
--       use = function(medic, patient) return true end,            -- e.g. take a medkit; false, "reason": not now
--       noMedicGiveUp = true,                                      -- no medic online: giving up right away
--       command = "medic",                                         -- /medic <id>: take a call by hand
--   })

local interact = require("interact")
local M = {}
local cfg = { seconds = 5, health = 30, noMedicGiveUp = true, command = "medic" }
local calls = {}     -- patient id -> {medic = id or nil, marker = id, asked = {ids}}
local working = {}   -- medic id -> true while reviving
local function T(p, r, e) return p and p.lang == "ru" and r or e end

local function isMedic(p) return p.valid and not p.downed and (not cfg.isMedic or cfg.isMedic(p)) end
local function medics(except)
    local list = {}
    for _, p in ipairs(players()) do if p ~= except and not p.bot and isMedic(p) then list[#list + 1] = p end end
    return list
end

local function endCall(patientId)
    local c = calls[patientId]; if not c then return end
    calls[patientId] = nil
    if c.marker then removeMarker(c.marker) end
    for _, id in ipairs(c.asked) do local m = getPlayer(id); if m and m.valid then m:cancelAsk() end end
end
M.endCall = endCall

local function distance(a, b)
    local ax, ay, az = a:position(); local bx, by, bz = b:position()
    return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2) / 100
end

local function take(m, patient)
    local c = calls[patient.id]; if not c or c.medic then return end
    c.medic = m.id
    for _, id in ipairs(c.asked) do local o = getPlayer(id); if o and o ~= m and o.valid then o:cancelAsk() end end
    local x, y, z = patient:position()
    c.marker = createMarker({ x = x, y = y, z = z, world = patient.world, player = m, radius = 3, track = false, route = true,
        title = T(m, "Раненый: ", "Wounded: ") .. patient.name })
    m:message(T(m, "{7CD67C}Вызов принят. Маршрут к раненому на карте.", "{7CD67C}Call taken. The way to the wounded is on your map."))
    patient:message(T(patient, "{7CD67C}Медик " .. m.name .. " едет к вам.", "{7CD67C}Medic " .. m.name .. " is on the way."))
end

on("playerHelp", function(p)
    if calls[p.id] and calls[p.id].medic then
        return p:message(T(p, "Медик уже едет.", "A medic is already on the way."))
    end
    local list = medics(p)
    if #list == 0 then
        p:message(T(p, "{E0A070}Медиков на сервере нет. Можно сдаться: удерживайте E.", "{E0A070}No medics on the server. You may give up: hold E."))
        if cfg.noMedicGiveUp then p:setDowned({ giveUp = 0 }) end
        return
    end
    endCall(p.id)
    local c = { asked = {} }
    calls[p.id] = c
    local pid = p.id
    for _, m in ipairs(list) do
        local text = string.format(T(m, "%s ранен и зовёт на помощь (%d м). Принять вызов?", "%s is wounded and calls for help (%d m). Take the call?"), p.name, math.floor(distance(m, p)))
        if m:ask(text, { seconds = 30 }, function(med, yes)
            local patient = getPlayer(pid)
            if yes and patient and patient.valid and patient.downed then take(med, patient) end
        end) then c.asked[#c.asked + 1] = m.id
        else m:message("{E0A070}" .. text:gsub("%?$", "") .. ": /" .. cfg.command .. " " .. pid) end
    end
    p:message(T(p, "Вызов отправлен медикам: " .. #list, "Call sent to medics: " .. #list))
end)

local function revive(m, t)
    if working[m.id] then return end
    if cfg.use then
        local ok, why = cfg.use(m, t, true)  -- check only
        if ok == false then return m:message("{E07070}" .. (why or T(m, "Нечем помочь", "Nothing to help with"))) end
    end
    working[m.id] = true
    local mid, tid, started = m.id, t.id, now()
    m:playAnim("kneel", { loop = true, freeze = true, stopOnMove = true })
    t:message(T(t, m.name .. " оказывает вам помощь...", m.name .. " is helping you..."))
    local timer
    timer = every(200, function()
        local med, pat = getPlayer(mid), getPlayer(tid)
        local fine = med and pat and med.valid and pat.valid and pat.downed and not med.downed and interact.near(med, pat, 3) and med.anim
        local part = (now() - started) / cfg.seconds
        if not fine then
            cancelTimer(timer); working[mid] = nil
            if med and med.valid then med:objectiveMeter(); med:stopAnim(); med:message(T(med, "{E07070}Помощь прервана", "{E07070}Help interrupted")) end
            return
        end
        med:objectiveMeter(T(med, "Помощь раненому", "Helping the wounded"), math.min(1, part))
        if part < 1 then return end
        cancelTimer(timer); working[mid] = nil
        med:objectiveMeter(); med:stopAnim()
        if cfg.use and cfg.use(med, pat) == false then return end
        pat:revive(cfg.health)
        endCall(tid)
        pat:message(T(pat, "{7CD67C}Вас поставили на ноги. Спасибо медику " .. med.name, "{7CD67C}You are back on your feet. Thank medic " .. med.name))
        med:message(T(med, "{7CD67C}Раненый поднят: ", "{7CD67C}Revived: ") .. pat.name)
        if M.onRevive then pcall(M.onRevive, med, pat) end
    end)
end

function M.setup(c)
    for k, v in pairs(c or {}) do cfg[k] = v end
    interact.add({ id = "revive", order = 5, downed = true,
        label = { ru = "Поднять раненого", en = "Revive" },
        when = function(p, t) return t.downed and not p.downed and isMedic(p) and not working[p.id] end,
        onUse = revive })
    if cfg.command and not M.commandBound then
        M.commandBound = true
        command(cfg.command, function(p, args, who)
            local t = who and getPlayer(tonumber(who) or who)
            if t and t.downed and calls[t.id] and not calls[t.id].medic and isMedic(p) then take(p, t) return end
            p:message(T(p, "{E07070}Вызов неактуален", "{E07070}This call is no longer active"))
        end)
    end
end


on("playerQuit", function(p) endCall(p.id); working[p.id] = nil end)
on("playerDeath", function(p) endCall(p.id) end)

return M
