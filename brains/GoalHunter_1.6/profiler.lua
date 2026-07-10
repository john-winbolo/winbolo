-- =========================================================================
-- GoalHunter/profiler.lua — instruction-count sampling profiler
--
-- Wraps Lua's debug.sethook("count", N) to attribute work to source:line
-- without per-callsite instrumentation. Off by default; toggled via
-- _G.PROFILER_ENABLED before Brain.open is called.
--
-- Usage from init.lua:
--   local prof = require("profiler")
--   prof.configure({ step = 1000, dump_every = 500, dir = ".", prefix = "p0" })
--   prof.start()                 -- at top of Brain.think
--   ... brain work ...
--   prof.stop(tick)              -- at bottom of Brain.think; rotates dump
--   prof.shutdown()              -- in Brain.close
--
-- Output: <dir>/<prefix>_profile.tsv  (tab-separated)
--   Columns: samples  source:line  func
--   Rows sorted by samples desc, refreshed every dump_every ticks.
--
-- The hook only runs while start/stop wraps Brain.think, so other Lua
-- work (settings dialog, debug helpers) is excluded.
-- =========================================================================

local M = {}

local STEP        = 1000     -- one sample every N VM instructions
local DUMP_EVERY  = 500      -- rewrite profile.tsv every N stops
local file_path   = nil      -- where to write the profile dump
local samples     = {}       -- key (source:line) → { count, func }
local total_hits  = 0
local stops_since_dump = 0
local enabled     = false    -- set true by configure(); start() is a no-op
                             -- otherwise so disabled bots pay nothing.

-- The hook is called from inside Brain.think while it's running. Keep it
-- branch-light: any work here is multiplied by the sample count.
local function hook()
  local info = debug.getinfo(2, "Sln")
  if not info then return end
  local src  = info.short_src or "?"
  local line = info.currentline or 0
  if line < 0 then line = 0 end
  local key  = src .. ":" .. line
  local entry = samples[key]
  if entry then
    entry.count = entry.count + 1
  else
    samples[key] = {
      count = 1,
      func  = info.name or (info.what or "?"),
    }
  end
  total_hits = total_hits + 1
end

local function dump()
  if not file_path then return end
  local f = io.open(file_path, "w")
  if not f then return end

  -- Sort entries by count desc — Python analyzer trims to top-N, but we
  -- keep all rows so callers can re-rank if they want.
  local rows = {}
  for k, v in pairs(samples) do
    rows[#rows + 1] = { key = k, count = v.count, func = v.func }
  end
  table.sort(rows, function(a, b) return a.count > b.count end)

  f:write(string.format("# total_samples=%d step=%d\n", total_hits, STEP))
  f:write("samples\tlocation\tfunc\n")
  for i = 1, #rows do
    local r = rows[i]
    f:write(string.format("%d\t%s\t%s\n", r.count, r.key, r.func or ""))
  end
  f:close()
end

function M.configure(opts)
  opts = opts or {}
  STEP       = opts.step       or STEP
  DUMP_EVERY = opts.dump_every or DUMP_EVERY
  if opts.dir and opts.prefix then
    file_path = opts.dir .. "/" .. opts.prefix .. "_profile.tsv"
  end
  enabled = true
end

function M.start()
  if not enabled then return end
  debug.sethook(hook, "", STEP)
end

function M.stop(tick)
  if not enabled then return end
  debug.sethook()
  stops_since_dump = stops_since_dump + 1
  if stops_since_dump >= DUMP_EVERY then
    dump()
    stops_since_dump = 0
  end
end

function M.shutdown()
  if not enabled then return end
  debug.sethook()
  dump()
end

return M
