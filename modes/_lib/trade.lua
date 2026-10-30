-- trade.lua — an exchange between two players: each puts things (and money) on the table from its bag, both
-- see both offers, both press "Ready"; any change takes "Ready" back from both. Then the server checks again
-- that each still has what it offers and swaps it all at once. docs/API.md "Обмен".
--
-- The mode says what a bag is (its own inventory, the outpost's account, the inventory library...):
--   local trade = require("trade")
--   trade.setup({
--       list = function(p) return { { id = "food", name = "Консервы", count = 3, icon = "food.png" }, ... } end,
--       take = function(p, id, count) return true end,    -- false: it does not have it (the trade stops)
--       give = function(p, id, count) end,
--       money = { get = function(p) return 100 end, take = function(p, n) return true end, give = function(p, n) end },  -- optional
--       allowed = function(p, t) return true end,          -- optional: may these two trade now
--       fits = function(p, gets, gives) return true end,   -- optional: p can hold what it gets ({id = count}) after giving away `gives`
--   })
-- With interact.lua the player menu gets "Offer a trade" (asks the other one). trade.open(a, b) from code.

local interact = require("interact")
local M = {}
local cfg = nil
local deals = {}   -- player id -> deal
local function T(p, r, e) return p and p.lang == "ru" and r or e end

local function other(d, id) return d.a == id and d.b or d.a end
local function offerOf(d, id) return d.offer[id] end

local function stop(d, why)
    for _, id in ipairs({ d.a, d.b }) do
        deals[id] = nil
        local p = getPlayer(id)
        if p and p.valid then
            p:closeItemWindow()
            if why then p:message("{E0A070}" .. why(p)) end
        end
    end
end

local function bagRows(d, p)
    local rows, offered = {}, offerOf(d, p.id).items
    for _, it in ipairs(cfg.list(p) or {}) do
        local left = (it.count or 1) - (offered[it.id] or 0)
        if left > 0 then rows[#rows + 1] = { id = it.id, name = it.name or it.id, count = left, icon = it.icon, text = it.text } end
    end
    return rows
end

local function names(p)
    local byId = {}
    for _, it in ipairs(cfg.list(p) or {}) do byId[it.id] = it end
    return byId
end

local function show(d)
    for _, id in ipairs({ d.a, d.b }) do
        local p, t = getPlayer(id), getPlayer(other(d, id))
        if not (p and t) then return stop(d) end
        local mine, theirs = offerOf(d, id), offerOf(d, t.id)
        local myNames, theirNames = names(p), names(t)
        local table_ = {}
        local function readyText(o) return o.ready and T(p, "{7CD67C}готов", "{7CD67C}ready") or T(p, "{E0A070}не готов", "{E0A070}not ready") end
        table_[#table_ + 1] = { id = "ready", name = T(p, "Ваше предложение — ", "Your offer — ") .. readyText(mine),
            text = T(p, "Сделка пройдёт, когда готовы оба", "The trade happens when both are ready"),
            actions = { { id = "ready", label = mine.ready and T(p, "Не готов", "Not ready") or T(p, "Готов", "Ready") } } }
        if cfg.money then
            table_[#table_ + 1] = { id = "money", name = T(p, "Деньги: $", "Money: $") .. mine.money,
                actions = { { id = "m10", label = "+$10" }, { id = "m100", label = "+$100" }, { id = "m0", label = T(p, "Убрать", "Clear") } } }
        end
        for item, n in pairs(mine.items) do
            local it = myNames[item] or {}
            table_[#table_ + 1] = { id = "mine:" .. item, name = it.name or item, count = n, icon = it.icon }
        end
        table_[#table_ + 1] = { id = "sep", name = "{AAAAAA}" .. t.name .. " — " .. readyText(theirs) }
        if cfg.money and theirs.money > 0 then table_[#table_ + 1] = { id = "their:money", name = "{7CD67C}$" .. theirs.money } end
        for item, n in pairs(theirs.items) do
            local it = theirNames[item] or {}
            table_[#table_ + 1] = { id = "their:" .. item, name = "{7CD67C}" .. (it.name or item), count = n, icon = it.icon }
        end
        d.rows[id] = { bag = bagRows(d, p), table = table_ }
        p:itemWindow({
            title = T(p, "Обмен с ", "Trade with ") .. t.name,
            hint = T(p, "Двойной щелчок — положить на стол / забрать", "Double click: put on the table / take back"),
            panes = { { id = "bag", title = T(p, "Рюкзак", "Bag"), items = d.rows[id].bag, empty = T(p, "Пусто", "Empty") },
                      { id = "table", title = T(p, "Стол", "Table"), items = d.rows[id].table } },
        }, function(q, action) M.action(q, action) end)
    end
end

local function changed(d)
    offerOf(d, d.a).ready = false; offerOf(d, d.b).ready = false
    show(d)
end

local function finish(d)
    local a, b = getPlayer(d.a), getPlayer(d.b)
    if not (a and b) or not interact.near(a, b, 6) then return stop(d, function(p) return T(p, "Обмен отменён: слишком далеко", "Trade cancelled: too far apart") end) end
    -- Check everything first, then take, then give: nobody is left with half a trade.
    for _, pair in ipairs({ { a, offerOf(d, a.id) }, { b, offerOf(d, b.id) } }) do
        local p, o = pair[1], pair[2]
        local have = {}
        for _, it in ipairs(cfg.list(p) or {}) do have[it.id] = it.count or 1 end
        for item, n in pairs(o.items) do
            if (have[item] or 0) < n then return stop(d, function(q) return T(q, "Обмен отменён: у ", "Trade cancelled: ") .. p.name .. T(q, " нет того, что он предложил", " no longer has what it offered") end) end
        end
        if cfg.money and o.money > cfg.money.get(p) then return stop(d, function(q) return T(q, "Обмен отменён: у ", "Trade cancelled: ") .. p.name .. T(q, " не хватает денег", " lacks the money") end) end
    end
    if cfg.fits then
        for _, pair in ipairs({ { a, b }, { b, a } }) do
            local p, from = pair[1], pair[2]
            if not cfg.fits(p, offerOf(d, from.id).items, offerOf(d, p.id).items) then
                return stop(d, function(q) return T(q, "Обмен отменён: ", "Trade cancelled: ") .. p.name .. T(q, " не унесёт столько", " cannot carry that much") end)
            end
        end
    end
    local taken = {}
    for _, pair in ipairs({ { a, offerOf(d, a.id) }, { b, offerOf(d, b.id) } }) do
        local p, o = pair[1], pair[2]
        for item, n in pairs(o.items) do
            if not cfg.take(p, item, n) then
                for _, back in ipairs(taken) do if back.money then cfg.money.give(back.p, back.money) else cfg.give(back.p, back.item, back.n) end end
                return stop(d, function(q) return T(q, "Обмен отменён", "Trade cancelled") end)
            end
            taken[#taken + 1] = { p = p, item = item, n = n }
        end
        if cfg.money and o.money > 0 then
            if not cfg.money.take(p, o.money) then
                for _, back in ipairs(taken) do if back.money then cfg.money.give(back.p, back.money) else cfg.give(back.p, back.item, back.n) end end
                return stop(d, function(q) return T(q, "Обмен отменён", "Trade cancelled") end)
            end
            taken[#taken + 1] = { p = p, money = o.money }
        end
    end
    for item, n in pairs(offerOf(d, a.id).items) do cfg.give(b, item, n) end
    for item, n in pairs(offerOf(d, b.id).items) do cfg.give(a, item, n) end
    if cfg.money then
        if offerOf(d, a.id).money > 0 then cfg.money.give(b, offerOf(d, a.id).money) end
        if offerOf(d, b.id).money > 0 then cfg.money.give(a, offerOf(d, b.id).money) end
    end
    log("[trade] " .. a.name .. " <-> " .. b.name)
    if M.onTrade then pcall(M.onTrade, a, b, offerOf(d, a.id), offerOf(d, b.id)) end
    stop(d, function(p) return T(p, "{7CD67C}Обмен состоялся", "{7CD67C}Trade done") end)
end

function M.action(p, action)
    local d = deals[p.id]; if not d then return end
    if action.type == "close" then
        local name = p.name
        return stop(d, function(q) return q.id == p.id and T(q, "Обмен отменён", "Trade cancelled") or name .. T(q, " отменил обмен", " cancelled the trade") end)
    end
    local t = getPlayer(other(d, p.id))
    if not t or not interact.near(p, t, 6) then return stop(d, function(q) return T(q, "Обмен отменён: слишком далеко", "Trade cancelled: too far apart") end) end
    local rows, mine = d.rows[p.id], offerOf(d, p.id)
    if not rows then return end
    if action.type == "move" and action.pane == 1 then
        local row = rows.bag[action.item]; if not row then return end
        local n = math.max(1, math.min(math.floor(tonumber(action.count) or 1), row.count))
        mine.items[row.id] = (mine.items[row.id] or 0) + n
        return changed(d)
    end
    if action.type == "move" and action.pane == 2 then
        local row = rows.table[action.item]; if not row then return end
        local item = row.id:match("^mine:(.+)$"); if not item then return end
        local n = math.max(1, math.min(math.floor(tonumber(action.count) or 1), mine.items[item] or 0))
        mine.items[item] = (mine.items[item] or 0) - n
        if mine.items[item] <= 0 then mine.items[item] = nil end
        return changed(d)
    end
    if action.type == "action" then
        if action.id == "ready" then
            mine.ready = not mine.ready
            if mine.ready and offerOf(d, t.id).ready then return finish(d) end
            return show(d)
        end
        if cfg.money and (action.id == "m10" or action.id == "m100" or action.id == "m0") then
            local add = action.id == "m10" and 10 or action.id == "m100" and 100 or -mine.money
            mine.money = math.max(0, math.min(mine.money + add, cfg.money.get(p)))
            return changed(d)
        end
    end
end

function M.open(a, b)
    if deals[a.id] or deals[b.id] then return a:message(T(a, "{E0A070}Кто-то из вас уже в обмене", "{E0A070}One of you is already trading")) end
    local d = { a = a.id, b = b.id, offer = { [a.id] = { items = {}, money = 0 }, [b.id] = { items = {}, money = 0 } }, rows = {} }
    deals[a.id] = d; deals[b.id] = d
    show(d)
end

function M.setup(c)
    cfg = c
    interact.add({
        id = "trade", order = 30,
        label = { ru = "Предложить обмен", en = "Offer a trade" },
        when = function(p, t) return not deals[p.id] and not deals[t.id] and (not cfg.allowed or cfg.allowed(p, t)) end,
        consent = { ru = "%s предлагает обмен", en = "%s offers a trade" },
        onUse = function(p, t) M.open(p, t) end,
    })
end

on("playerQuit", function(p)
    local d = deals[p.id]
    if d then local name = p.name; stop(d, function(q) return name .. T(q, " ушёл — обмен отменён", " left: trade cancelled") end) end
end)
on("playerDeath", function(p)
    local d = deals[p.id]
    if d then stop(d, function(q) return T(q, "Обмен отменён", "Trade cancelled") end) end
end)

return M
