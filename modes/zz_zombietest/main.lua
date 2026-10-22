-- temporary: freaker sync test (server.ini Zombies=1); docs/zombies.md
-- /z [count] [type] [metres]: zombies in front of you; /zclear; /zlist
command("z", function(player, args)
    local count, kind, dist = args:match("^(%d*)%s*(%a*)%s*(%d*)")
    count = tonumber(count) or 1; kind = kind ~= "" and kind or "Swarmer"; dist = (tonumber(dist) or 15) * 100
    local px, py, pz, pyaw = player:position()
    local yaw = math.rad(pyaw)
    for i = 1, count do
        local side = (i - (count + 1) / 2) * 150
        local x = px + math.cos(yaw) * dist - math.sin(yaw) * side
        local y = py + math.sin(yaw) * dist + math.cos(yaw) * side
        local id = createZombie(x, y, pz, kind, pyaw + 180, player.world)
        if not id then return player:message("createZombie failed (server.ini Zombies=1?)", "#FF7070") end
    end
    player:message(("%d %s"):format(count, kind))
end)
command("zclear", function(player)
    for _, z in ipairs(zombies()) do removeZombie(z.id) end
end)
command("zlist", function(player)
    for _, z in ipairs(zombies()) do
        player:message(("%d %s hp %s owner %s%s"):format(z.id, z.type, tostring(z.health), z.owner and z.owner.name or "-", z.dead and " dead" or ""))
    end
end)
on("zombieDeath", function(z, killer)
    broadcast(("Zombie %d killed by %s"):format(z.id, killer and killer.name or "nobody"), "#F2D16B")
end)
