-- RP base: a starting point for role-play servers built only on the public Lua API (docs/API.md).
-- Accounts are kept by Steam account (by nickname when the server does not check Steam), money and the
-- last position survive quitting; chat is local (rpchat); every half hour a PayDay pays the players who
-- were on. The places come from the editor: World > Mode logic ("gunshop", "hospital", "spawn") and the
-- doors, vehicles and loot of the World sections.
local logic = require("logic")
local rpchat = require("rpchat")

local GREEN, YELLOW, RED, GREY, GOLD = "#7CD67C", "#F2D16B", "#E07070", "#A0A0A0", "#F2D16B"
local START_MONEY, PAYDAY_MINUTES, PAYDAY_MONEY = 250, 30, 100

local gunshop = logic.point("gunshop")     -- a gun dealer's counter
local hospital = logic.point("hospital")   -- where the dead wake up

rpchat.install({ radius = 20 })
-- Guns and ammo come only from the server: the shop, an admin, a job. World loot of the game is allowed
-- for ammo the player picked up himself (inventoryViolation below).
protectItems({ "kWeaponID_", "kAmmoID_" })

local function ru(p) return p.lang == "ru" end
local function say(p, en, rus, color) p:message(ru(p) and rus or en, color) end

-- ---------------------------------------------------------------- accounts
local function accountKey(p) return "acc:" .. (p.steamId or ("nick:" .. p.name:lower())) end
local function save(p)
    local acc = p.data.account
    if not acc then return end
    local x, y, z, yaw = p:position()
    acc.money = p.money
    acc.pos = { x = x, y = y, z = z, yaw = yaw }
    storage.set(accountKey(p), acc)
end

on("playerJoin", function(p)
    local acc = storage.get(accountKey(p))
    if not acc then
        acc = { money = START_MONEY, since = os.time(), played = 0 }
        say(p, "Welcome! You start with $" .. START_MONEY .. ". Type /help.", "Добро пожаловать! У вас $" .. START_MONEY .. ". Команды: /help", GREEN)
    else
        say(p, "Welcome back!", "С возвращением!", GREEN)
        if acc.pos then p:setPosition(acc.pos.x, acc.pos.y, acc.pos.z, acc.pos.yaw) end
    end
    p.data.account = acc
    p.data.since = now()
    p:setMoney(acc.money or 0)
    if not p.verified then
        say(p, "Your Steam account is not confirmed: the account is kept by nickname.",
            "Steam-аккаунт не подтверждён: аккаунт хранится по нику.", GREY)
    end
    if not p.skin then p:customize({ title = ru(p) and "Ваш персонаж" or "Your character", cancel = false }) end
end)
on("playerQuit", function(p) save(p) end)
every(60000, function() for _, p in ipairs(players()) do if not p.bot then save(p) end end end)

-- ---------------------------------------------------------------- PayDay
every(PAYDAY_MINUTES * 60000, function()
    for _, p in ipairs(players()) do
        if not p.bot and p.data.account then
            p:giveMoney(PAYDAY_MONEY)
            p:playSound("ui_tutorial_x_press")
            say(p, "PayDay: +$" .. PAYDAY_MONEY, "Зарплата: +$" .. PAYDAY_MONEY, GOLD)
            save(p)
        end
    end
end)

-- ---------------------------------------------------------------- gun shop
local SHOP = {
    { key = "kWeaponID_M9", en = "M9 pistol", ru = "Пистолет M9", price = 150, ammo = 30 },
    { key = "kWeaponID_1887", en = "1887 shotgun", ru = "Дробовик 1887", price = 300, ammo = 12 },
    { key = "kWeaponID_AK47", en = "AK-47", ru = "АК-47", price = 800, ammo = 60 },
}
local function openShop(p)
    local items = {}
    for _, it in ipairs(SHOP) do items[#items + 1] = { ru(p) and it.ru or it.en, "{7CD67C}$" .. it.price } end
    p:dialog({
        style = "list", title = ru(p) and "Оружейный" or "Gun shop", items = items,
        headers = { ru(p) and "Товар" or "Item", ru(p) and "Цена" or "Price" },
        button1 = ru(p) and "Купить" or "Buy",
        onResponse = function(p, ok, index)
            if not ok or not index then return end
            local it = SHOP[index]
            if p.money < it.price then return say(p, "Not enough money", "Не хватает денег", RED) end
            p:giveMoney(-it.price)
            p:giveWeapon(it.key, it.ammo)
            say(p, "Bought: " .. it.en, "Куплено: " .. it.ru, GREEN)
            save(p)
        end,
    })
end
if gunshop.placed then
    createInteraction({ x = gunshop.x, y = gunshop.y, z = gunshop.z, radius = 2, text = "Gun shop", onUse = openShop })
    createBlip({ x = gunshop.x, y = gunshop.y, z = gunshop.z, icon = "MAP_objective_encampment_merchant", color = GOLD })
end
on("inventoryViolation", function(p, v)
    -- Ammo found in the world is fine; guns come only from the shop.
    if v.pickedUp and v.item:find("^kAmmoID_") then return true end
end)

-- ---------------------------------------------------------------- death and the hospital
on("playerRespawn", function(p)
    if not hospital.placed then return end
    p:setPosition(hospital.x, hospital.y, hospital.z + 100, hospital.yaw)
    local fee = math.min(p.money, 50)
    if fee > 0 then p:giveMoney(-fee) end
    say(p, "You woke up in the hospital. Treatment: $" .. fee, "Вы очнулись в больнице. Лечение: $" .. fee, GREY)
    return true
end)
if hospital.placed then createBlip({ x = hospital.x, y = hospital.y, z = hospital.z, icon = "MAP_player_safehouse", color = "#E07070" }) end

-- ---------------------------------------------------------------- animations
local ANIMS = { sit = "sit", hands = "surrender", wave = "wave", kneel = "kneel", lay = "lie", talk = "talk", guard = "guard" }
for cmd, anim in pairs(ANIMS) do command(cmd, function(p) p:playAnim(anim, { loop = cmd ~= "wave" }) end) end
command("stop", function(p) p:stopAnim() end)

-- ---------------------------------------------------------------- money and info
command("pay", function(p, args, who)
    local amount = tonumber(args:match("^%S+%s+(%d+)"))
    local target = who and getPlayer(tonumber(who) or who)
    if not target or not amount or amount <= 0 then return say(p, "Usage: /pay <id> <amount>", "Используйте: /pay <id> <сумма>", RED) end
    if target == p or p.money < amount then return say(p, "Cannot pay that", "Так нельзя", RED) end
    local px, py, pz = p:position(); local tx, ty, tz = target:position()
    if (px - tx) ^ 2 + (py - ty) ^ 2 + (pz - tz) ^ 2 > 500 ^ 2 then return say(p, "Come closer", "Подойдите ближе", RED) end
    p:giveMoney(-amount); target:giveMoney(amount)
    rpchat.near(p, "* " .. rpchat.name(p) .. " " .. (ru(p) and "передаёт деньги" or "hands over some money") .. " " .. rpchat.name(target))
    save(p); save(target)
end)
command("stats", function(p)
    local acc = p.data.account or {}
    say(p, ("Money: $%d · on the server since %s"):format(p.money, os.date("%d.%m.%Y", acc.since or os.time())),
        ("Деньги: $%d · на сервере с %s"):format(p.money, os.date("%d.%m.%Y", acc.since or os.time())), YELLOW)
end)
command("help", function(p)
    say(p, "Chat: /me /do /try /b /s /w /pm /o · /pay /stats · /sit /hands /wave /kneel /lay /talk /guard /stop · /skin",
        "Чат: /me /do /try /b /s /w /pm /o · /pay /stats · /sit /hands /wave /kneel /lay /talk /guard /stop · /skin", YELLOW)
    if p.admin then say(p, "Admin: /goto /gethere /money /kick /ban /unban /heal", "Админ: /goto /gethere /money /kick /ban /unban /heal", YELLOW) end
end)
command("skin", function(p) p:customize({ title = ru(p) and "Магазин одежды" or "Clothes shop" }) end)

-- ---------------------------------------------------------------- admin tools
local function admin(name, fn)
    command(name, function(p, args, who)
        if not p.admin then return say(p, "Admins only", "Только для администраторов", RED) end
        fn(p, args, who)
    end)
end
local function target(p, who)
    local t = who and getPlayer(tonumber(who) or who)
    if not t then say(p, "No such player", "Нет такого игрока", RED) end
    return t
end
admin("goto", function(p, args, who) local t = target(p, who); if t then local x, y, z = t:position(); p:setWorld(t.world); p:setPosition(x + 100, y, z) end end)
admin("gethere", function(p, args, who) local t = target(p, who); if t then local x, y, z = p:position(); t:setWorld(p.world); t:setPosition(x + 100, y, z) end end)
admin("money", function(p, args, who) local t = target(p, who); local n = tonumber(args:match("^%S+%s+(-?%d+)")); if t and n then t:giveMoney(n); save(t) end end)
admin("heal", function(p, args, who) local t = who and target(p, who) or p; if t then t:heal() end end)
admin("kick", function(p, args, who) local t = target(p, who); if t then t:kick(args:match("^%S+%s+(.+)$") or "") end end)
admin("ban", function(p, args, who)
    local t = target(p, who)
    local minutes, reason = args:match("^%S+%s+(%d+)%s*(.*)$")
    if t then t:ban(reason or args:match("^%S+%s+(.+)$") or "", tonumber(minutes) or 0) end
end)
admin("unban", function(p, args) local n = unban(args); say(p, n .. " ban(s) lifted", "Снято банов: " .. n, YELLOW) end)
on("cheat", function(p, kind, info)
    for _, a in ipairs(players()) do if a.admin then a:message("[AC] " .. p.name .. ": " .. kind .. " — " .. info.detail, RED) end end
end)
