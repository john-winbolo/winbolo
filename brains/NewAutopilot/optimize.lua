-- =========================================================================
-- NewAutopilot/optimize.lua — per-tick CPU phase timing log
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
local na_opt_log = na_opt_log

local function debug_mode()
  return _G.BRAIN_DEBUG_MODE
end

local function perf_log_enabled()
  return _G.BRAIN_PERF_LOG
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

function M.set_tick(t)
  tick       = t
  tick_start = clock()
  for i = #buffer, 1, -1 do buffer[i] = nil end
end

local function log_msg(...)
  if debug_mode() then return end
  local elapsed_ms = (clock() - tick_start) * 1000
  local parts = {}
  for i = 1, select("#", ...) do
    parts[#parts + 1] = tostring(select(i, ...))
  end
  buffer[#buffer + 1] = string.format("[%.2fms] %s", elapsed_ms, table.concat(parts, ""))
end

function M.flush()
  if debug_mode() or not perf_log_enabled() or #buffer == 0 then return end
  if na_opt_log then
    -- Threaded path: build full tick block in memory, enqueue for background write.
    local dir = pick_dir()
    if not file_dir or dir ~= file_dir then
      -- Start background thread on first flush (lazy, same as before).
      local path = dir .. "/optimize.log"
      if na_opt_log.open(path) then
        file_dir = dir
      end
    end
    if file_dir then
      local parts = { "===TICK ", tostring(tick), "===\n" }
      for i = 1, #buffer do
        parts[#parts + 1] = buffer[i]
        parts[#parts + 1] = "\n"
      end
      na_opt_log.write(table.concat(parts))
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
  if na_opt_log then na_opt_log.close() end
  if file then file:close(); file = nil; file_dir = nil end
  file_dir = nil
end

-- Append a single line to an arbitrary log file (relative to DEBUG_SESSION_DIR).
-- Use for one-shot diagnostic messages that don't belong in the tick log.
-- Always writes regardless of BRAIN_DEBUG_MODE — diagnostics are useful in
-- both debug and production runs. Only the tick timing log (log_msg/flush)
-- is suppressed in debug mode since debug overhead skews the measurements.
function M.append(filename, text)
  if not perf_log_enabled() then return end
  local dir  = pick_dir()
  local path = dir .. "/" .. filename
  if na_opt_log then
    na_opt_log.append(path, text)
  else
    local f = io.open(path, "a")
    if f then f:write(text, "\n"); f:close() end
  end
end

setmetatable(M, { __call = function(_, ...) log_msg(...) end })

return M
