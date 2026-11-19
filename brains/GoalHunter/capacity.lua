-- =========================================================================
-- GoalHunter/capacity.lua — how many GoalHunter bots this machine can run
-- at full strength. The engine calls Brain.capacity(machine, measured) once
-- when the brain loads (see tests/README.md, "Brain capacity"); init.lua
-- sets Brain.capacity to M.capacity. It lives in its own module so that
-- Brain.think, which is at Lua's 60-upvalue cap, gains no upvalue.
--
-- machine: cpu_name, logical_cores, physical_cores, speed_1t,
--          speed_all, scaling, worker_threads, on_battery, version.
--          speed_1t is 0 when the CPU speed test has not run here yet.
-- measured: nil for now (think times per bot count, not measured yet).
--
-- The answer: physical cores, scaled by how fast one core is next to the
-- reference machine (C.CAPACITY_REF_1T, a 4-core i5-6600 scores about
-- 25,000), times C.CAPACITY_K, rounded down and clamped to 1..16. With no
-- speed test result it returns nil, and the engine uses its default
-- (logical cores - 1, at least 1).
-- =========================================================================

local C = require("constants")

local M = {}

local CAPACITY_MIN = 1
local CAPACITY_MAX = 16

function M.capacity(machine, measured)
  if type(machine) ~= "table" then return nil end
  local physical = tonumber(machine.physical_cores) or 0
  local speed1t = tonumber(machine.speed_1t) or 0
  local ref = tonumber(C.CAPACITY_REF_1T) or 0
  local k = tonumber(C.CAPACITY_K) or 0
  if physical <= 0 or speed1t <= 0 or ref <= 0 then return nil end
  local n = math.floor(physical * (speed1t / ref) * k)
  if n < CAPACITY_MIN then n = CAPACITY_MIN end
  if n > CAPACITY_MAX then n = CAPACITY_MAX end
  return n
end

return M
