-- jobs.lua — starts every job file of the quests folder (quests/<id>.json with "kind": "job",
-- docs/quests.md) in this mode: the givers, their [F] dialogs, the boxes of deliver steps.
--
--   require("jobs")                         -- in main.lua: all jobs
--   local jobs = require("jobs")            -- jobs.running[id] -> the job (job:stop())
--
-- One job only: Quest.runJob("loader") (modes/_lib/quest.lua). Without code: meta.ini [Mode] Jobs=all
-- (or Jobs=loader,miner). Saving a job file reloads the mode.
local Quest = require("quest")

local M = { running = {} }
local ids = Quest.list()
table.sort(ids)
for _, id in ipairs(ids) do
    local def = Quest.load(id)
    if def and def.kind == "job" and def.enabled ~= false then M.running[id] = Quest.runJob(def) end
end
if quests.watch then quests.watch() end
return M
