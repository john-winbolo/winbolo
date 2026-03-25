-- =========================================================================
-- NewAutopilot/metrics.lua — lightweight per-tick performance counters
--
-- Usage:
--   local metrics = require("metrics")
--   metrics.inc("sq_cost")           -- count a call
--   metrics.inc("pill_cache_hit")    -- count a cache hit
--   metrics.finish_tick(tick_number) -- print summary every N ticks
--
-- Output: one line per report with all counters + per-tick averages.
-- =========================================================================

local C = require("constants")
local TAG = "[" .. C.BRAIN_NAME .. "]"

local M = {}

local counters = {}      -- current tick's counters
local accum    = {}      -- accumulated since last report
local maxvals  = {}      -- worst-case per report period
local ticks    = 0       -- ticks since last report
-- Uses global print which is silenced for non-debug bots by init.lua

local REPORT_INTERVAL = 50  -- print every 50 ticks (~1 second at 50 tps)

function M.inc(name, amount)
  counters[name] = (counters[name] or 0) + (amount or 1)
end

function M.get(name)
  return counters[name] or 0
end

-- Set a gauge value (replaces previous value for this tick, not additive)
function M.set(name, value)
  counters[name] = value
end

-- Track worst-case (max) value across the report period
function M.max(name, value)
  local cur = maxvals[name]
  if cur == nil or value > cur then maxvals[name] = value end
end

function M.finish_tick(tick)
  ticks = ticks + 1
  for k, v in pairs(counters) do
    accum[k] = (accum[k] or 0) + v
  end
  counters = {}

  if ticks >= REPORT_INTERVAL then
    local parts = {}
    -- Sort keys for stable output
    local keys = {}
    for k in pairs(accum) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do
      local total = accum[k]
      local avg = total / ticks
      parts[#parts + 1] = string.format("%s=%.0f(%.0f/t)", k, total, avg)
    end
    -- Append worst-case values
    local mkeys = {}
    for k in pairs(maxvals) do mkeys[#mkeys + 1] = k end
    table.sort(mkeys)
    for _, k in ipairs(mkeys) do
      parts[#parts + 1] = string.format("%s_max=%.0f", k, maxvals[k])
    end
    print(string.format(TAG .. " METRICS t=%d [%dt]: %s",
      tick, ticks, table.concat(parts, "  ")))
    accum = {}
    maxvals = {}
    ticks = 0
  end
end

return M
