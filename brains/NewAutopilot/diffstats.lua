-- =========================================================================
-- NewAutopilot/diffstats.lua — per-tick accumulators for the
-- evaluate_pill_difficulty profiler.
--
-- Lives in its own module (rather than in attack.lua or pathfinder.lua)
-- to avoid a circular require: attack.lua requires pathfinder, and the
-- accumulators need to be writable from both.
--
-- Usage:
--   local diffstats = require("diffstats")
--   diffstats.reset()                           -- top of step_eval_queue
--   ... attack.evaluate_pill_difficulty calls bump fields ...
--   metrics.set("us_pool6_los_c", diffstats.us_los_c)   -- bottom of tick
-- =========================================================================

local M = {}

-- Microsecond accumulators.
M.us_los_c        = 0  -- inside cpf.simulate_shot itself (C call + return table alloc)
M.us_los_walk     = 0  -- Lua loop over returned tiles (collision check)
M.us_scan_a       = 0  -- Scan A: crossfire ellipse loop (threat.coverage_at)
M.us_scan_b       = 0  -- Scan B: maneuver ellipse loop (threat.pill_at + terrain)
M.us_dij          = 0  -- two-pass dijkstra selection (cpf.dijkstra_lookup_by_kind)

-- Counts.
M.n_calls          = 0  -- evaluate_pill_difficulty invocations
M.n_los_calls      = 0  -- pill_shots_clear invocations
M.n_simulate_shots = 0  -- cpf.simulate_shot calls
M.n_scan_tiles     = 0  -- tiles processed across Scan A + Scan B

function M.reset()
  M.us_los_c        = 0
  M.us_los_walk     = 0
  M.us_scan_a       = 0
  M.us_scan_b       = 0
  M.us_dij          = 0
  M.n_calls          = 0
  M.n_los_calls      = 0
  M.n_simulate_shots = 0
  M.n_scan_tiles     = 0
end

return M
