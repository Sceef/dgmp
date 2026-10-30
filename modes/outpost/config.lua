local L=require("i18n")
-- РУБЕЖ: all distances in metres, positions in Unreal units (100 = 1 m).
return {
    title = L("РУБЕЖ · DGMP RP","RUBEZH · DGMP RP"), version = 1,
    startMoney = 450, maxMoney = 10000000, bagWeight = 30,
    paydaySeconds = 900, payday = 60, needsSeconds = 180,
    jobSeconds = 12, jobCooldown = 90, missionSeconds = 1200,
    bikePrice = 350, treatmentPrice = 35, respawnFee = 50, tutorialAllowance = 80,
    -- Fallback matches the reviewed Iron Mike layout; editor world.json takes precedence.
    points = {
        spawn = { x = -4825.37744, y = -89710.0703, z = -1944.09045, yaw = -50.0427628 },
        office = { x = -2895.55151, y = -93327.0156, z = -1759.42883, yaw = 114.152916 },
        shop = { x = 4952.65332, y = -87537.8906, z = -2639.19629, yaw = -146.058838 },
        clinic = { x = 1690.2428, y = -87989.1797, z = -2631.97729, yaw = -10.9191141 },
        garage = { x = 1471.30396, y = -85516.4531, z = -2643.45825, yaw = -101.22625 },
        warehouse = { x = 5749.02588, y = -93901.4609, z = -2824.84961, yaw = 89.0990295 },
        cargo = { x = 5936.24805, y = -87650.1406, z = -2663.52441, yaw = -111.125465 },
        salvage = { x = 34680.5469, y = -122137.938, z = -3067.43115, yaw = 51.4036064 },
        patrol1 = { x = 6496.28809, y = -87699.6641, z = -2380.22754, yaw = -23.0301456 },
        patrol2 = { x = -12561.959, y = -80536.3594, z = -2340.74927, yaw = 132.778961 },
        expedition = { x = 78825, y = -155558.156, z = -3635.64697, yaw = 145.174561 },
        helipad = { x = -8051.26562, y = -99224.8516, z = -2926.50732, yaw = -173.14621 },
    },
    items = {
        food = { name = L("Паёк","Ration"), weight = 0.5, price = 15, stock = "food" },
        water = { name = L("Фляга воды","Water flask"), weight = 0.5, price = 10, stock = "food" },
        medkit = { name = L("Аптечка","Medkit"), weight = 1, price = 45, stock = "medicine" },
        scrap = { name = L("Детали","Parts"), weight = 1, price = 20, stock = "parts" },
        fuel = { name = L("Канистра","Fuel can"), weight = 5, price = 40, stock = "fuel" },
        radio = { name = L("Рация","Radio"), weight = 1, price = 80, stock = "parts" },
        -- Quest item: from an expedition, for the clinic (not sold, given away or traded).
        nero_meds = { name = L("Медикаменты NERO","NERO medical supplies"), weight = 2, price = 0, stock = "medicine", quest = true },
    },
    questItems = { "nero_meds" },
    itemOrder = { "food", "water", "medkit", "scrap", "fuel", "radio" },
    roles = {
        resident = { name = L("Житель","Resident"), uniform = false },
        supplier = { name = L("Снабженец","Supplier"), uniform = "m_mil_diamondlake_worker_001" },
        medic = { name = L("Медик","Medic"), uniform = "f_mil_diamondlake_medic_001" },
        mechanic = { name = L("Механик","Mechanic"), uniform = "m_mil_diamondlake_worker_003" },
        guard = { name = L("Охрана","Guard"), uniform = "m_sur_copeland_guard_08" },
    },
    roleOrder = { "resident", "supplier", "medic", "mechanic", "guard" },
    weapons = {
        { name = L("M9 · 30 патронов","M9 · 30 rounds"), key = "kWeaponID_M9", ammo = 30, price = 180, rep = 0 },
        { name = L("1887 · 12 патронов","1887 · 12 rounds"), key = "kWeaponID_1887", ammo = 12, price = 350, rep = 10 },
        { name = L("АК-47 · 60 патронов","AK-47 · 60 rounds"), key = "kWeaponID_AK47", ammo = 60, price = 750, rep = 30 },
    },
}
