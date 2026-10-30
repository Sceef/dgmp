local L=require("i18n")
-- Pure rules: no DGMP globals. Persistent changes stay in one snapshot.
local D = {}
function D.integer(v, lo, hi)
    return type(v) == "number" and v == v and v >= lo and v <= hi and v % 1 == 0
end
function D.identity(p)
    if p.verified and p.steamId and p.steamId ~= "" then return "steam:" .. p.steamId end
    return "guest:" .. p.name:lower()
end
function D.newState()
    return { version = 1, accounts = {}, camp = { level = 1, treasury = 500,
        food = 80, medicine = 50, parts = 100, fuel = 50 }, points = {}, ledger = {}, sequence = 0 }
end
function D.account(cfg, timestamp)
    return { money = cfg.startMoney, bank = 0, role = "resident", rep = 0,
        hunger = 100, thirst = 100, played = 0, payday = 0, created = timestamp,
        bag = { food = 2, water = 2 }, stash = {}, intro = false, bike = false, cooldowns = {} }
end
function D.weight(cfg, bag)
    local n = 0
    for id, count in pairs(bag) do n = n + (cfg.items[id] and cfg.items[id].weight or 0) * count end
    return n
end
function D.add(cfg, bag, id, count, maxWeight)
    if not cfg.items[id] or not D.integer(count, 1, 100) then return false, L("Неверный предмет или количество.","Invalid item or quantity.") end
    if maxWeight and D.weight(cfg, bag) + cfg.items[id].weight * count > maxWeight then return false, L("В рюкзаке нет места.","Not enough space in your backpack.") end
    bag[id] = (bag[id] or 0) + count
    return true
end
function D.take(bag, id, count)
    if not D.integer(count, 1, 100) or (bag[id] or 0) < count then return false, L("Не хватает предметов.","Not enough items.") end
    bag[id] = bag[id] - count
    if bag[id] == 0 then bag[id] = nil end
    return true
end
function D.transfer(cfg, a, b, amount)
    if a == b or not D.integer(amount, 1, cfg.maxMoney) then return false, L("Неверная сумма.","Invalid amount.") end
    if a.money < amount then return false, L("Не хватает наличных.","Not enough cash.") end
    if b.money + amount > cfg.maxMoney then return false, L("Лимит кошелька получателя.","The recipient's wallet is full.") end
    a.money, b.money = a.money - amount, b.money + amount
    return true
end
function D.bank(cfg, a, amount, withdraw)
    if not D.integer(amount, 1, cfg.maxMoney) then return false, L("Неверная сумма.","Invalid amount.") end
    local from, to = withdraw and "bank" or "money", withdraw and "money" or "bank"
    if a[from] < amount or a[to] + amount > cfg.maxMoney then return false, L("Недостаточный баланс или превышен лимит.","Insufficient balance or account limit exceeded.") end
    a[from], a[to] = a[from] - amount, a[to] + amount
    return true
end
function D.buy(cfg, a, camp, id)
    local it = cfg.items[id]
    if not it then return false, L("Нет такого товара.","No such product.") end
    if a.money < it.price then return false, L("Не хватает наличных.","Not enough cash.") end
    if camp[it.stock] < 1 then return false, L("Запасы исчерпаны: выполните снабжение лагеря.","Out of stock: deliver supplies to camp.") end
    local ok, err = D.add(cfg, a.bag, id, 1, cfg.bagWeight)
    if not ok then return false, err end
    a.money, camp.treasury = a.money - it.price, camp.treasury + it.price
    camp[it.stock] = camp[it.stock] - 1
    return true
end
function D.move(cfg, a, id, toStash)
    local src, dst = toStash and a.bag or a.stash, toStash and a.stash or a.bag
    if (src[id] or 0) < 1 then return false, L("Нет этого предмета.","You do not have this item.") end
    local ok, err = D.add(cfg, dst, id, 1, toStash and 100 or cfg.bagWeight)
    if not ok then return false, err end
    D.take(src, id, 1)
    return true
end
function D.upgrade(camp)
    if camp.level >= 3 then return false, L("Лагерь уже достиг третьего уровня.","The camp has already reached level three.") end
    local cost, parts = camp.level * 500, camp.level * 20
    if camp.treasury < cost or camp.parts < parts then return false, L("Нужно $","Required: $") .. cost .. L(" в казне и деталей: "," in the treasury and parts: ") .. parts end
    camp.treasury, camp.parts, camp.level = camp.treasury - cost, camp.parts - parts, camp.level + 1
    return true
end
function D.record(s, kind, key, detail, timestamp)
    s.sequence = s.sequence + 1
    s.ledger[#s.ledger + 1] = { id = s.sequence, at = timestamp, kind = kind, account = key, detail = detail }
    if #s.ledger > 200 then table.remove(s.ledger, 1) end
end
return D
