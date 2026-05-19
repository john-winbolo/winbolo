-- =========================================================================
-- NewAutopilot/print2.lua — per-tick debug log with source:line prefix
--
-- Usage: local p2 = require("print2")
--        p2("some debug message", someVar)
--
-- Each call captures the caller's source:line via debug.getinfo and
-- appends to a per-tick buffer.  At end of tick, flush() appends the
-- buffer to a single rolling file <session>/print2.log as a delimited
-- section:
--
--   ===TICK <N>===
--   source\tline\tmessage
--   source\tline\tmessage
--   ...
--
-- One persistent file handle for the whole session — no per-tick
-- file creates, no FD churn.
--
-- Gated by _G._PRINT2_ENABLED (set by C each tick from debug flag).
-- =========================================================================

local M = {}

local buffer = {}   -- array of {source, line, msg}
local tick = 0
local tick_start = 0  -- os.clock() at set_tick

-- Persistent file handle for the rolling log + the session dir it
-- corresponds to.  Reopened lazily on first flush after the session
-- dir changes (e.g., between BrainTest sessions in the same process).
local file = nil
local file_dir = nil

-- Stats / heartbeat used to detect silent failures.
local last_successful_flush_tick = -1
local consecutive_failures        = 0

-- High-res timer (os.clock is CPU time in seconds)
local clock = os.clock

function M.set_tick(t)
  if not _G._PRINT2_ENABLED then return end
  tick = t
  tick_start = clock()
  -- Clear buffer for new tick
  for i = #buffer, 1, -1 do buffer[i] = nil end
end

-- The callable: print2(...)
local function log_msg(...)
  if not _G._PRINT2_ENABLED then return end
  local elapsed_ms = (clock() - tick_start) * 1000
  local info = debug.getinfo(3, "Sl")
  local src = info.short_src or "?"
  -- Strip [string "..."] wrapper and extract basename
  src = src:match('^%[string "(.-)"%]$') or src
  src = src:match('[/\\]([^/\\]+)$') or src
  local line = info.currentline or 0
  local parts = {}
  for i = 1, select("#", ...) do
    parts[#parts + 1] = tostring(select(i, ...))
  end
  local msg = string.format("[%.2fms] %s", elapsed_ms, table.concat(parts, ""))
  buffer[#buffer + 1] = { src = src, line = line, msg = msg }
end

-- =========================================================================
-- Failure reporting — HARD FAIL via error() so brain.think gets killed
-- and the bot manager logs the error to stderr (or VS catches it). The
-- user explicitly asked for a crash if print2 stops working — silent
-- failures are too easy to miss.
-- =========================================================================
local _fail_count = 0
local function fail_hard(reason, detail)
  _fail_count = _fail_count + 1
  consecutive_failures = consecutive_failures + 1
  local msg = string.format(
    "[print2] FATAL tick=%d reason=%s detail=%s (fail#%d consec=%d)",
    tick, tostring(reason), tostring(detail),
    _fail_count, consecutive_failures)
  -- Print to the regular log first so the message is visible even if
  -- whoever catches the error swallows it.
  print(msg)
  -- Then propagate as a Lua error. brainCoreCallThink will pcall this,
  -- log it to stderr, route it through Lua print() ([FATAL] prefix),
  -- and the bot manager will remove the bot.
  error(msg, 0)
end

-- Try to (re)open the rolling print2.log file. Hard-fails if io.open
-- returns nil — the user wants visible crashes when print2 is broken.
local function try_open(dir)
  local path = dir .. "/print2.log"
  local f, err = io.open(path, "a")
  if not f then
    fail_hard("io.open", path .. " : " .. tostring(err))
  end
  file = f
  file_dir = dir
  print(string.format("[print2] opened %s", path))
end

-- Force-close the current file handle (used after a write failure so the
-- next flush opens a fresh one).
local function force_reopen()
  if file then
    pcall(function() file:close() end)
    file = nil
    file_dir = nil
  end
end

function M.flush()
  if not _G._PRINT2_ENABLED then return end
  if #buffer == 0 then return end
  -- Fall back to cwd when no session dir is set (e.g. BrainTest run
   -- without --profile-log / --log-json). print2 should still work as
   -- long as debug mode is on.
  local dir = _G.DEBUG_SESSION_DIR
  if not dir or dir == "" then dir = "." end

  -- Lazy open / reopen on session dir change
  if not file or dir ~= file_dir then
    if file then pcall(function() file:close() end); file = nil end
    try_open(dir)  -- hard-fails on its own; only returns on success
  end

  -- Each write/flush wrapped in pcall so we can report the actual error
  -- before crashing. On any failure: force the file closed (so a future
  -- session at least tries again) and propagate.
  local ok, err = pcall(function()
    file:write("===TICK ", tick, "===\n")
    for _, entry in ipairs(buffer) do
      file:write(entry.src, "\t", entry.line, "\t", entry.msg, "\n")
    end
    file:flush()
  end)

  if not ok then
    force_reopen()
    fail_hard("write/flush", err)
  end

  last_successful_flush_tick = tick
  consecutive_failures = 0
end

-- Optional explicit close (called from Brain.close if defined)
function M.close()
  if file then
    file:close()
    file = nil
    file_dir = nil
  end
end

-- Public diagnostic — returns the tick of the last successful flush, the
-- failure count, and a human-readable status. Lets external code (or the
-- brain itself) detect "print2 has been silent for a while".
function M.diagnostic()
  return {
    last_successful_flush_tick = last_successful_flush_tick,
    consecutive_failures       = consecutive_failures,
    fail_count                 = _fail_count,
    file_open                  = file ~= nil,
    enabled                    = _G._PRINT2_ENABLED and true or false,
  }
end

-- Make the module callable as print2(...)
setmetatable(M, { __call = function(_, ...) log_msg(...) end })

return M
