local L=require("i18n")
local R=require("core")
local S=require("services")
local C, D = R.cfg, R.rules
-- The player menu (look at a player, F): squad, trade, radio, money, and the duty actions of medics and guards.
local interact=require("interact")
local squad=require("squad")
local trade=require("trade")
local medic=require("medic")

-- A lethal hit lays the player down: a medic on duty revives it (a medkit), else it bleeds out or gives up
-- (to the clinic, as before). Finishing off a wounded player kills it.
setDeathMode("downed", { bleedout = 240, giveUpAfter = 60, finish = true })
medic.setup({
    isMedic = function(p) local a, s = R.acc(p), R.session(p); return a ~= nil and s ~= nil and a.role == "medic" and s.duty end,
    seconds = 6, health = 35,
    use = function(m, t, check)
        local a = R.acc(m)
        if not a or (a.bag.medkit or 0) < 1 then return false, L.resolve(L("Нужна аптечка в рюкзаке.", "You need a medkit in your backpack."), m) end
        if check then return true end
        D.take(a.bag, "medkit", 1); R.commit("revive", m, t.name); S.calls[t.id] = nil
        return true
    end,
})
on("playerDowned", function(p)
    local s = R.session(p); if not s then return end
    R.msg(p, L("Вы тяжело ранены. H — позвать медика, через минуту можно сдаться (E).", "You are badly wounded. H: call a medic; after a minute you may give up (E)."), false)
end)

squad.setup({ max = 6, chat = "sq", radio = false, color = "#7CD67C" })

local function duty(p, role) local a, s = R.acc(p), R.session(p); return a and s and a.role == role and s.duty end
local function both(p, t) return R.ready(p) and R.ready(t) end
local function hasRadio(p) local a = R.acc(p); return a and (a.bag.radio or 0) > 0 end
local function radioChannel(p)
    for name, how in pairs(p:voiceChannels()) do if how == "radio" and name:find("^Rubezh %d+$") then return name end end
end

-- Trade: the outpost backpack and cash.
trade.setup({
    list = function(p)
        local a, out = R.acc(p), {}
        if not a then return out end
        for _, id in ipairs(C.itemOrder) do
            if (a.bag[id] or 0) > 0 then out[#out + 1] = { id = id, name = L.resolve(C.items[id].name, p), count = a.bag[id] } end
        end
        return out
    end,
    take = function(p, id, n) local a = R.acc(p); return a ~= nil and D.take(a.bag, id, n) end,
    give = function(p, id, n) local a = R.acc(p); if a then a.bag[id] = (a.bag[id] or 0) + n end end,
    fits = function(p, gets, gives)
        local a = R.acc(p); if not a then return false end
        local w = D.weight(C, a.bag)
        for id, n in pairs(gives) do w = w - (C.items[id] and C.items[id].weight or 0) * n end
        for id, n in pairs(gets) do w = w + (C.items[id] and C.items[id].weight or 0) * n end
        return w <= C.bagWeight
    end,
    money = {
        get = function(p) local a = R.acc(p); return a and a.money or 0 end,
        take = function(p, n) local a = R.acc(p); if not a or a.money < n then return false end; a.money = a.money - n; return true end,
        give = function(p, n) local a = R.acc(p); if a then a.money = math.min(C.maxMoney, a.money + n) end end,
    },
    allowed = both,
})
trade.onTrade = function(a, b) R.commit("trade", a, b.name) end

interact.add({ id = "radio_share", order = 20,
    label = { ru = "Поделиться частотой рации", en = "Share your radio channel" },
    when = function(p, t) return both(p, t) and hasRadio(p) and hasRadio(t) and radioChannel(p) and radioChannel(p) ~= radioChannel(t) end,
    consent = function(p, t)
        local n = radioChannel(p):match("%d+")
        return L.resolve(L("%s предлагает настроить вашу рацию на канал " .. n, "%s offers to tune your radio to channel " .. n), t)
    end,
    onUse = function(p, t)
        local ch = radioChannel(p); if not ch then return end
        t:setVoiceChannel(ch)
        R.msg(t, L("Рация настроена: канал ", "Radio tuned to channel ") .. ch:match("%d+") .. L(". Говорить — K.", ". Talk: K."))
        R.msg(p, t.name .. L(" теперь на вашем канале.", " is on your channel now."))
    end })

interact.add({ id = "pay", order = 25,
    label = { ru = "Передать деньги", en = "Give money" },
    when = function(p, t) return both(p, t) and R.acc(p).money > 0 end,
    onUse = function(p, t)
        local tid = t.id
        R.dialog(p, { style = "input", title = L("Передать деньги: ", "Give money: ") .. t.name, text = L("Сумма, $", "Amount, $"),
            button1 = L("Передать", "Give"), onResponse = function(q, ok, _, text)
                local r = getPlayer(tid)
                if not ok or not r or not both(q, r) or not interact.near(q, r, 4) then return end
                local amount = tonumber(text)
                local fine, err = D.transfer(C, R.acc(q), R.acc(r), amount and math.floor(amount))
                if R.result(q, fine, err, "pay", r.name .. " " .. tostring(amount)) then
                    R.msg(q, L("Передано $", "Paid $") .. math.floor(amount)); R.msg(r, q.name .. L(" передал вам $", " paid you $") .. math.floor(amount))
                end
            end })
    end })

interact.add({ id = "aid", order = 40,
    label = { ru = "Оказать помощь (аптечка)", en = "Treat (medkit)" },
    when = function(p, t) return both(p, t) and duty(p, "medic") and t.health < 95 end,
    onUse = function(p, t)
        local a, s = R.acc(p), R.session(p)
        if now() < (s.aidAt or 0) then return R.msg(p, L("Подождите немного.", "Wait a moment."), false) end
        local ok, err = D.take(a.bag, "medkit", 1); if not ok then return R.msg(p, err, false) end
        s.aidAt = now() + 10; t:heal(); p:playAnim("kneel", { loop = false }); S.calls[t.id] = nil
        R.commit("aid", p, t.name)
        R.msg(t, L("Медик ", "Medic ") .. p.name .. L(" оказал помощь.", " treated you."))
    end })

-- Guards on duty: papers (the other one shows them), a search (asks, unless the hands are up or it is down).
interact.add({ id = "papers", order = 50,
    label = { ru = "Проверить документы", en = "Check papers" },
    when = function(p, t) return both(p, t) and duty(p, "guard") end,
    consent = { ru = "Охранник %s просит показать документы", en = "Guard %s asks for your papers" },
    onUse = function(p, t)
        local a = R.acc(t)
        R.dialog(p, { title = L("Документы: ", "Papers: ") .. t.name, button1 = L("Вернуть", "Hand back"),
            text = L.format(L("{D9C898}Имя:{FFFFFF} %s\n{D9C898}Номер:{FFFFFF} %d\n{D9C898}Профессия:{FFFFFF} %s\n{D9C898}Пропуск лагеря:{FFFFFF} %s\n{D9C898}Репутация:{FFFFFF} %d",
                "{D9C898}Name:{FFFFFF} %s\n{D9C898}Number:{FFFFFF} %d\n{D9C898}Profession:{FFFFFF} %s\n{D9C898}Camp pass:{FFFFFF} %s\n{D9C898}Reputation:{FFFFFF} %d"),
                t.name, t.id, C.roles[a.role].name, a.intro and L("есть", "yes") or L("нет", "none"), a.rep) })
        R.msg(t, L("Вы показали документы охраннику ", "You showed your papers to guard ") .. p.name)
    end })

local function frisk(p, t)
    local a, lines = R.acc(t), {}
    for _, id in ipairs(C.itemOrder) do
        if (a.bag[id] or 0) > 0 then lines[#lines + 1] = L.resolve(C.items[id].name, p) .. " × " .. a.bag[id] end
    end
    for key, n in pairs(t:inventory()) do
        if key:find("^kWeaponID_") and n > 0 then lines[#lines + 1] = "{E0A070}" .. key:gsub("^kWeaponID_", "") end
    end
    R.dialog(p, { title = L("Обыск: ", "Search: ") .. t.name, button1 = L("Закончить", "Done"),
        text = (#lines > 0 and table.concat(lines, "\n") or L.resolve(L("Ничего", "Nothing"), p)) .. "\n\n$" .. a.money })
    R.msg(t, L("Вас обыскал ", "You were searched by ") .. p.name)
    R.commit("frisk", p, t.name)
end
interact.add({ id = "frisk", order = 51,
    label = { ru = "Обыскать", en = "Search" },
    when = function(p, t) return both(p, t) and duty(p, "guard") and (t.anim == "dcn_surrender" or t.downed) end,
    downed = true,
    onUse = frisk })
interact.add({ id = "frisk_ask", order = 52,
    label = { ru = "Попросить показать вещи", en = "Ask to show belongings" },
    when = function(p, t) return both(p, t) and duty(p, "guard") and t.anim ~= "dcn_surrender" and not t.downed end,
    consent = { ru = "Охранник %s просит показать вещи", en = "Guard %s asks to see your belongings" },
    onUse = frisk })

-- The Tab panel: profession and duty.
setScoreboard({ columns = { "Профессия / Role" } })
local function board(p)
    local a, s = R.acc(p), R.session(p)
    if a then p:setScoreboard({ L.resolve(C.roles[a.role].name, p) .. (s.duty and " ●" or "") }) end
end
on("playerJoin", function(p) after(500, function() if p.valid then board(p) end end) end)
every(5000, function() for _, p in ipairs(players()) do if p.valid then board(p) end end end)
