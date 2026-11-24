-- =========================================================================
-- pt2_eval.lua -- the pill take evaluator that PT2_EVALUATOR picks
-- (2026-11-23): "v2" = pilltake_v2.lua, else pilltake_v2_old.lua.
--
-- A field read here comes from the picked module at the time of the read,
-- so a cfg change takes effect on the next call. A group job remembers
-- the module that began it (job.ev): its steps stay with that module.
--
-- P2 (PT2_EVAL_BY_TIER): the capacity tier row's pt2_eval ("v2" or "old")
-- wins over PT2_EVALUATOR, as dbl_evals wins over its knob. M.use(state)
-- (pt2_attack.build_inp) names the bot state whose tier is read; the inp
-- keeps the module it was built for (inp._pt2_ev), so a tier change
-- between the build and the run does not mix the two.
-- =========================================================================

local C   = require("constants")
local OLD = require("pilltake_v2_old")
local NEW = nil

local cur_state = nil

-- The evaluator name: the tier row's pt2_eval (PT2_EVAL_BY_TIER), else
-- PT2_EVALUATOR.
local function choice()
  if C.PT2_EVAL_BY_TIER then
    local cap = cur_state and cur_state._capacity
    local e = cap and cap.pt2_eval
    if e ~= nil then return e end
  end
  return C.PT2_EVALUATOR
end

local function pick()
  if choice() == "v2" then
    NEW = NEW or require("pilltake_v2")
    return NEW
  end
  return OLD
end

local M = {}

function M.use(state) cur_state = state end
M.choice = choice

function M.group_begin(inp)
  local ev = inp._pt2_ev or pick()
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

function M.evaluate_group(inp)
  return (inp._pt2_ev or pick()).evaluate_group(inp)
end

M.pick = pick

return setmetatable(M, { __index = function(_, k) return pick()[k] end })
