-- =========================================================================
-- GoalHunter/print2.lua — per-tick debug log with source:line prefix
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

-- Per-bot identity for per-file routing. Each Lua state runs one bot,
-- so this module-level local is naturally per-bot. Set via M.set_bot()
-- at brain startup; filename becomes print2_bot<bot_idx>.log so
-- concurrent bots don't share one file.
local bot_idx = nil

-- Persistent file handle for the rolling log. Opened once on first
-- flush, kept open for the process lifetime — never closed/reopened.
local file = nil

-- Stats / heartbeat used to detect silent failures.
local last_successful_flush_tick = -1
local consecutive_failures        = 0

-- High-res timer (os.clock is CPU time in seconds), used for the
-- per-line [x.xxms] within-tick stamp.
local clock = os.clock

-- C threaded log writer (global injected by C; same one optimize.lua
-- uses). nil when unavailable → synchronous file fallback below.
local na_opt_log = na_opt_log

-- Design B batching: each tick's lines are serialized into one block and
-- appended to `pending`. The accumulated blocks are handed to the writer
-- thread (off the timed think path) only every FLUSH_INTERVAL_S seconds
-- of wall time, or immediately on force_flush() (called by the host when
-- the sim is paused so the log is current while you read it).
local pending           = {}      -- array of per-tick block strings
local last_handoff      = 0       -- os.time() of last handoff (0 = never)
local log_path          = nil     -- resolved per-bot path, set on first handoff
local writer_started    = false   -- have we ensured the na_opt_log thread is up?
local FLUSH_INTERVAL_S  = 1   -- (was 5) tighter so --max-ticks exit drops only ~1s of tail
local wallclock         = os.time

function M.set_tick(t)
  if not _G._PRINT2_ENABLED then return end
  tick = t
  tick_start = clock()
  -- Clear buffer for new tick
  for i = #buffer, 1, -1 do buffer[i] = nil end
end

-- Stamp this Lua state with its owning bot index. Call once at brain
-- startup (Brain.think tick 1) — the file path uses this to route
-- each bot's lines to its own print2_bot<N>.log.
function M.set_bot(n)
  bot_idx = n
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

-- Try to open the per-bot rolling log file. Hard-fails if io.open
-- returns nil — the user wants visible crashes when print2 is broken.
-- Filename is print2_bot<N>.log so each bot writes to its own file.
local function try_open_path(path)
  local f, err = io.open(path, "a")
  if not f then
    fail_hard("io.open", path .. " : " .. tostring(err))
  end
  -- Full-buffer: the handoff explicitly flushes once per batch (~5s), so
  -- we don't want per-line OS writes. The threaded path (na_opt_log) is
  -- preferred and avoids this handle entirely.
  if f.setvbuf then pcall(f.setvbuf, f, "full") end
  file = f
  print(string.format("[print2] opened %s", path))
end

-- Resolve the per-bot log path once. Falls back to cwd when no session
-- dir is set (e.g. BrainTest run without --profile-log / --log-json).
local function resolve_path()
  if log_path then return log_path end
  local dir = _G.DEBUG_SESSION_DIR
  if not dir or dir == "" then dir = "." end
  local tag = bot_idx and tostring(bot_idx) or "unknown"
  log_path = string.format("%s/print2_bot%s.log", dir, tag)
  return log_path
end

-- Hand the accumulated `pending` blocks to the writer thread (or the
-- synchronous fallback) and reset. The disk I/O happens off the timed
-- think path when na_opt_log is present, so this is cheap to call.
local function handoff()
  if #pending == 0 then
    last_handoff = wallclock()
    return
  end
  local text = table.concat(pending)
  for i = #pending, 1, -1 do pending[i] = nil end
  local path = resolve_path()

  if na_opt_log then
    -- Threaded path: writer thread opens path, appends text, closes.
    -- The writer thread is started lazily by whoever needs it first. We
    -- can't assume the profiler (optimize.lua) started it — append is a
    -- silent no-op when the thread isn't running, which would drop the
    -- whole log. So ensure it ourselves. open() is idempotent (returns
    -- false if already running) and we only use append (per-path), never
    -- the single shared "main file", so this never collides with the
    -- profiler's own na_opt_log usage.
    if not writer_started then
      na_opt_log.open(path)
      writer_started = true
    end
    na_opt_log.append(path, text)
  else
    -- Synchronous fallback: persistent handle, full-buffered, one
    -- flush per handoff (every ~5s) — not per tick.
    if not file then try_open_path(path) end
    local ok, err = pcall(function()
      file:write(text)
      file:flush()
    end)
    if not ok then
      fail_hard("write/flush", err)
    end
  end

  last_handoff = wallclock()
  last_successful_flush_tick = tick
  consecutive_failures = 0
end

-- Reset the per-bot log target so the NEXT handoff re-resolves the path from
-- the (possibly just-updated) DEBUG_SESSION_DIR. The host calls this on a new
-- game start (via serverSimBotExecLua) so each game's print2 lands in that
-- game's fresh debug_sessions/<TS>/ dir instead of appending to the previous
-- game's file. Flushes any pending tail to the OLD path first so the finished
-- game's log is complete, then drops the cached path/handle.
function M.reset_log()
  if #pending > 0 then pcall(handoff) end          -- flush tail to old path
  if file then pcall(function() file:close() end) end
  file           = nil
  log_path       = nil
  writer_started = false
end

-- Per-tick: serialize this tick's buffer into one block string, append
-- it to `pending`, and hand the batch to the writer only every
-- FLUSH_INTERVAL_S seconds. Near-zero cost on the ticks in between.
function M.flush()
  if not _G._PRINT2_ENABLED then return end
  if #buffer > 0 then
    local parts = { "===TICK ", tostring(tick), "===\n" }
    for _, entry in ipairs(buffer) do
      parts[#parts + 1] = entry.src
      parts[#parts + 1] = "\t"
      parts[#parts + 1] = tostring(entry.line)
      parts[#parts + 1] = "\t"
      parts[#parts + 1] = entry.msg
      parts[#parts + 1] = "\n"
    end
    pending[#pending + 1] = table.concat(parts)
  end

  if last_handoff == 0 then last_handoff = wallclock() end
  if (wallclock() - last_handoff) >= FLUSH_INTERVAL_S then
    handoff()
  end
end

-- Host-invoked: drain `pending` to disk right now, ignoring the timer.
-- Called when the sim is paused (think() has stopped, so the per-tick
-- flush path is dormant) so the log reflects the latest tick on screen.
function M.force_flush()
  if not _G._PRINT2_ENABLED then return end
  handoff()
end

-- Optional explicit close (called from Brain.close if defined). Drains
-- any pending blocks first so nothing is lost at shutdown.
function M.close()
  handoff()
  if file then
    file:close()
    file = nil
  end
end

-- Public diagnostic — returns the tick of the last successful flush, the
-- failure count, and a human-readable status. Lets external code (or the
-- brain itself) detect "print2 has been silent for a while".
function M.diagnostic()
  -- Wall-clock seconds since the last handoff to the writer. Batching is
  -- time-based (every FLUSH_INTERVAL_S), so the watchdog must reason in
  -- seconds, not ticks. 0 before the first tick has set last_handoff.
  local secs = (last_handoff > 0) and (wallclock() - last_handoff) or 0
  return {
    last_successful_flush_tick = last_successful_flush_tick,
    seconds_since_handoff      = secs,
    flush_interval_s           = FLUSH_INTERVAL_S,
    pending_blocks             = #pending,
    consecutive_failures       = consecutive_failures,
    fail_count                 = _fail_count,
    file_open                  = file ~= nil,
    enabled                    = _G._PRINT2_ENABLED and true or false,
  }
end

-- Make the module callable as print2(...)
setmetatable(M, { __call = function(_, ...) log_msg(...) end })

return M
