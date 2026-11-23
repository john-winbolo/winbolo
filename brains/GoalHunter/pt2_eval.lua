-- =========================================================================
-- pt2_eval.lua -- the pill take evaluator that PT2_EVALUATOR picks
-- (2026-11-23): "v2" = pilltake_v2.lua, else pilltake_v2_old.lua.
--
-- A field read here comes from the picked module at the time of the read,
-- so a cfg change takes effect on the next call. A group job remembers
-- the module that began it (job.ev): its steps stay with that module.
-- =========================================================================

local C   = require("constants")
local OLD = require("pilltake_v2_old")
local NEW = nil

local function pick()
  if C.PT2_EVALUATOR == "v2" then
    NEW = NEW or require("pilltake_v2")
    return NEW
  end
  return OLD
end

local M = {}

function M.group_begin(inp)
  local ev = pick()
  local job = ev.group_begin(inp)
  job.ev = ev
  return job
end

function M.group_step(job)
  return (job.ev or OLD).group_step(job)
end

function M.group_parts_left(job)
  return (job.ev or OLD).group_parts_left(job)
end

M.pick = pick

return setmetatable(M, { __index = function(_, k) return pick()[k] end })
