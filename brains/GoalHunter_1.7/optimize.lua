-- =========================================================================
-- GoalHunter/optimize.lua — per-tick CPU phase timing log
--
-- Usage:  local opt = require("optimize")
--         opt.set_tick(now)
--         opt("phase done", " extra=", x)
--         opt.flush()
--
-- Writes to <DEBUG_SESSION_DIR>/optimize.log if set, else cwd/optimize.log.
-- Each call captures elapsed-since-tick-start (ms) automatically.
--
-- Disabled automatically when BRAIN_DEBUG_MODE is set — debug mode adds
-- print2/viz overhead that skews timing, making the log misleading.
--
-- opt.append(filename, text) — helper for one-shot diagnostic lines that
-- don't belong in the tick log.  Opens filename (relative to DEBUG_SESSION_DIR
-- or cwd) in append mode, writes text + newline, closes.  No-op in debug mode.
--
-- Survives lua_strip: call sites use the bareword "opt"/"optimize", and the
-- io.open inside this module is NOT line-leading (`local f = io.open(...)`)
-- so the stripper leaves it alone.
-- =========================================================================

local M = {}

local buffer     = {}
local tick       = 0
local tick_start = 0
local clock      = os.clock

local file     = nil
local file_dir = nil

-- C threaded writer; nil if not available (falls back to synchronous I/O).
local gh_opt_log = gh_opt_log

local function debug_mode()
  return _G.BRAIN_DEBUG_MODE
end

-- Two-flag split:
--   BRAIN_PROFILE     — measurement on (clock_us markers, opt() emits,
--                       buffer accumulation, rebuild_sections). Drives
--                       the BrainTest "Capacity tiers" panel time bar.
--   BRAIN_PROFILE_LOG — file writes on (optimize.log, opt.append). Implies
--                       BRAIN_PROFILE on the host side. Off by default in
--                       --opt mode; on automatically in dev/debug mode.
local function profile_enabled()
  return _G.BRAIN_PROFILE
end

local function profile_log_enabled()
  return _G.BRAIN_PROFILE_LOG
end

local function pick_dir()
  return _G.DEBUG_SESSION_DIR or "."
end

local function try_open(dir)
  local path = dir .. "/optimize.log"
  local f, err = io.open(path, "a")
  if not f then
    print(string.format("[optimize] open failed: %s : %s", path, tostring(err)))
    return
  end
  file     = f
  file_dir = dir
  print(string.format("[optimize] opened %s", path))
end

-- Per-tick section breakdown for the BrainTest "Capacity tiers" panel.
-- Populated as a side effect of flush() when perf-log is on. Shape:
--   { { name = "threat", ms = 0.20, subs = { { name = "pillcontrib export",
--       ms = 0.15 }, ... } }, ... }
-- The panel reads this each frame to render the stacked time-bar.
M.last_sections = {}

-- Auto-attribution for sub-sections emitted by infrastructure callers
-- (cpathfinder wrappers, etc) that don't know which brain main section
-- is currently in flight. Brain code calls opt.set_main("steer") at
-- the entry of each major phase; helpers grab the most recent value
-- via M.current_main_name() so wrapped calls (e.g. cpf.path_to) emit
-- "  <last-set-main>/path_to done X". No stack — the user wanted
-- "whatever main it came from last", which is just an overwrite-style
-- variable. Early returns don't need cleanup because next tick's
-- next set_main overwrites.
local _current_main = nil

function M.set_main(name)
  if not _G.BRAIN_PROFILE then return end
  _current_main = name
end

function M.current_main_name()
  return _current_main or "?"
end

function M.set_tick(t)
  tick       = t
  tick_start = clock()
  for i = #buffer, 1, -1 do buffer[i] = nil end
end

local function log_msg(...)
  if not profile_enabled() then return end
  local elapsed_ms = (clock() - tick_start) * 1000
  local parts = {}
  for i = 1, select("#", ...) do
    parts[#parts + 1] = tostring(select(i, ...))
  end
  buffer[#buffer + 1] = string.format("[%.2fms] %s", elapsed_ms, table.concat(parts, ""))
end

-- Walk the tick's buffer and build a hierarchical section list for the
-- panel. Lines look like "[0.00ms] <indent><name> done <ms> ms". A
-- non-indented line is a main section; preceding indented lines roll up
-- as its `subs`. Subs always appear before their parent in the buffer
-- because opt() is called when each phase ends — depth-first ordering.
local function rebuild_sections()
  M.last_sections = {}
  -- Three indent levels:
  --   0 spaces -> main section.    Attaches accumulated `pending_subs`.
  --   2 spaces -> sub section.     Attaches accumulated `pending_subsubs`.
  --   4+ spaces -> subsub section. Goes into `pending_subsubs`.
  -- Order matters: each emitter writes its children BEFORE itself, so by
  -- the time we see a parent line the kids are already buffered.
  local pending_subs    = {}
  local pending_subsubs = {}
  for i = 1, #buffer do
    local L = buffer[i]
    -- Match: "[<elapsed>ms] <indent>(name) done <ms> ms"
    -- Lua patterns are greedy: `%s*` followed by `(%s*)` would eat all
    -- the whitespace into the first capture and leave the second empty.
    -- We anchor on a literal space after the closing bracket and capture
    -- only the *extra* whitespace as the indent.
    local indent, name, ms = L:match("^%[[^%]]+%] (%s*)(.-) done%s+([%d.]+)%s+ms")
    if name and ms and name ~= "" then
      local ind = #(indent or "")
      local entry = { name = name, ms = tonumber(ms) or 0 }
      if ind == 0 then
        entry.subs = pending_subs
        pending_subs = {}
        pending_subsubs = {}  -- defensive: drop any orphans
        M.last_sections[#M.last_sections + 1] = entry
      elseif ind <= 2 then
        entry.subs = pending_subsubs
        pending_subsubs = {}
        pending_subs[#pending_subs + 1] = entry
      else
        pending_subsubs[#pending_subsubs + 1] = entry
      end
    end
  end
end

function M.flush()
  -- Profiling-only: parse buffer into last_sections so the panel can
  -- read it. Skip the file write portion when profile-log is off.
  if not profile_enabled() or #buffer == 0 then return end
  rebuild_sections()
  if not profile_log_enabled() then return end
  if gh_opt_log then
    -- Threaded path: build full tick block in memory, enqueue for background write.
    local dir = pick_dir()
    if not file_dir or dir ~= file_dir then
      -- Start background thread on first flush (lazy, same as before).
      local path = dir .. "/optimize.log"
      if gh_opt_log.open(path) then
        file_dir = dir
      end
    end
    if file_dir then
      local parts = { "===TICK ", tostring(tick), "===\n" }
      for i = 1, #buffer do
        parts[#parts + 1] = buffer[i]
        parts[#parts + 1] = "\n"
      end
      gh_opt_log.write(table.concat(parts))
    end
  else
    -- Synchronous fallback.
    local dir = pick_dir()
    if not file or dir ~= file_dir then
      if file then pcall(function() file:close() end); file = nil end
      try_open(dir)
    end
    if not file then return end
    local ok = pcall(function()
      file:write("===TICK ", tick, "===\n")
      for _, msg in ipairs(buffer) do
        file:write(msg, "\n")
      end
      file:flush()
    end)
    if not ok then
      pcall(function() file:close() end)
      file     = nil
      file_dir = nil
    end
  end
end

function M.close()
  -- FLUSH, not close: the writer thread is process-global and shared with
  -- print2 and with the other bots' brains. Brain.close runs once per bot, so
  -- stopping the thread here would leave every bot that closes after this one
  -- with a dead writer (and print2 hard-fails when its append is refused).
  -- The thread has no OS resources to leak -- it fflushes after every batch and
  -- exits with the process.
  if gh_opt_log then gh_opt_log.flush() end
  if file then file:close(); file = nil; file_dir = nil end
  file_dir = nil
end

-- Append a single line to an arbitrary log file (relative to DEBUG_SESSION_DIR).
-- Use for one-shot diagnostic messages that don't belong in the tick log.
-- Always writes regardless of BRAIN_DEBUG_MODE — diagnostics are useful in
-- both debug and production runs. Only the tick timing log (log_msg/flush)
-- is suppressed in debug mode since debug overhead skews the measurements.
function M.append(filename, text)
  if not profile_log_enabled() then return end
  local dir  = pick_dir()
  local path = dir .. "/" .. filename
  if gh_opt_log then
    gh_opt_log.append(path, text)
  else
    local f = io.open(path, "a")
    if f then f:write(text, "\n"); f:close() end
  end
end

setmetatable(M, { __call = function(_, ...) log_msg(...) end })

return M
