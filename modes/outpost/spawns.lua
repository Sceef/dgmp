local R=require("core")
for _,zone in ipairs(R.cfg.spawns.zones) do
    if getZombieZone(zone.name) then setZombieZone(zone.name,zone) else createZombieZone(zone) end
end
