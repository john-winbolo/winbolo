-- =========================================================================
-- replan_prof.lua -- REPLAN_PROF: the time of each part of a goal replan
-- (C.REPLAN_PROF_LOG, debug or -brain-profile-log only). Log only: it
-- changes no decision.
--
-- On the first replan with the knob on, the functions of a few modules are
-- put inside timers: goals (its M functions and each POOL_EVALUATORS
-- entry), pathfinder, cpathfinder, threat, danger, attack, exploration,
-- squad, builder, pill_portfolio, builder_pool. A timer runs only between
-- M.begin and M.finish (the replan block of init.lua); outside it a timed
-- function costs one more Lua call. With the knob off nothing is wrapped.
--
-- Times are self times: a stack of open timers, and the time since the last
-- push or pop goes to the timer on top. replan_other is the time of the
-- replan block in no timed function, so the rows add up to the replan's
-- total. util and world are not timed (their functions are small and are
-- called very often, so a timer would cost more than the function).
--
-- One REPLAN_PROF line per replan (tier, budget, total, then each row as
-- name=calls/us, most time first). REPLAN_PROF_GAME (the sums of the game)
-- every REPLAN_PROF_GAME_EVERY replans. A replan that a budget kill cut
-- leaves its timers open: the next M.begin counts it (killed=) and starts
-- from zero.
-- =========================================================================

local C = require("constants")

local M = {}

local clock = rawget(_G, "clock_us") or function() return os.clock() * 1e6 end

local ACTIVE = false
local stk, sp, last = {}, 0, 0
local cur = {}          -- this replan: name -> { n, us }
local game = {}         -- the game: name -> { n, us }
local game_n, game_us, killed, replans = 0, 0, 0, 0
local t_begin, r_tick, r_info = 0, 0, nil
local wrapped = false

local function row(t, name)
  local r = t[name]
  if not r then r = { n = 0, us = 0 }; t[name] = r end
  return r
end

local function push(name)
  local t = clock()
  local top = stk[sp]
  local r = row(cur, top)
  r.us = r.us + (t - last)
  last = t
  sp = sp + 1
  stk[sp] = name
  local q = row(cur, name)
  q.n = q.n + 1
end

local function pop_ret(...)
  local t = clock()
  local r = row(cur, stk[sp])
  r.us = r.us + (t - last)
  last = t
  if sp > 1 then sp = sp - 1 end
  return ...
end

-- M.orig[timer] = the function inside it, so that code which checks "is
-- this still the module's own function?" (pill_portfolio PP_FRONT_C) can
-- look through a timer. Weak keys: it keeps no function alive.
M.orig = setmetatable({}, { __mode = "k" })

local function wrap(name, f)
  local w = function(...)
    if not ACTIVE then return f(...) end
    push(name)
    return pop_ret(f(...))
  end
  M.orig[w] = f
  return w
end

local MODULES = { "goals", "pathfinder", "cpathfinder", "threat", "danger", "attack",
                  "exploration", "squad", "builder", "pill_portfolio", "builder_pool" }
local SHORT = { goals = "goals", pathfinder = "PF", cpathfinder = "cpf", threat = "threat",
                danger = "danger", attack = "attack", exploration = "expl", squad = "squad",
                builder = "builder", pill_portfolio = "PP", builder_pool = "bpool" }

local function wrap_all()
  wrapped = true
  for _, mod in ipairs(MODULES) do
    local t = package.loaded[mod]
    if type(t) == "table" then
      local names = {}
      for k, v in pairs(t) do
        if type(k) == "string" and type(v) == "function" then names[#names + 1] = k end
      end
      for _, k in ipairs(names) do t[k] = wrap(SHORT[mod] .. "." .. k, t[k]) end
    end
  end
  -- the pool evaluators (goals.lua POOL_EVALUATORS): one row per pool
  local G = package.loaded["goals"]
  local ev, nm = G and G._POOL_EVALUATORS, G and G._POOL_EVALUATOR_NAMES
  if ev then
    for i = 1, #ev do ev[i] = wrap("pool." .. ((nm and nm[i]) or tostring(i)), ev[i]) end
  end
end

-- Is the profile on? (the knob, in a debug brain or a profile-log run)
function M.want()
  return C.REPLAN_PROF_LOG and (BRAIN_DEBUG_MODE or BRAIN_PROFILE_LOG) and true or false
end

local function out(line)
  local dbg = rawget(_G, "BRAIN_DEBUG_MODE")
  if dbg then
    local p2 = require("print2")
    p2(line)
  elseif BRAIN_PROFILE_LOG then
    require("optimize").append("optimize.log", "  " .. line)
  end
end

local function rows_line(t, total)
  local rows = {}
  for k, v in pairs(t) do rows[#rows + 1] = { k, v.n, v.us } end
  table.sort(rows, function(a, b) if a[3] ~= b[3] then return a[3] > b[3] end return a[1] < b[1] end)
  local parts = {}
  for i, r in ipairs(rows) do parts[i] = string.format("%s=%d/%.0f", r[1], r[2], r[3]) end
  return table.concat(parts, " ")
end

-- The start of the replan block. state: the bot state (tier, tick).
function M.begin(state, now)
  if not wrapped then wrap_all() end
  if ACTIVE then
    -- the last replan did not reach M.finish: a budget kill cut it
    killed = killed + 1
    out(string.format("REPLAN_PROF_KILLED t=%d last_replan=%d", now or 0, r_tick))
  end
  cur = {}
  stk[1] = "replan_other"
  sp = 1
  row(cur, "replan_other").n = 1
  local br = rawget(_G, "brain")
  r_tick = now or 0
  r_info = { tier = state._capacity_tier or -1, tgt = br and br.targetMs or 0,
             last = br and br.lastThinkMs or 0, wk = br and br.wasKilled or false,
             goal = state.goal and state.goal.kind or "-" }
  t_begin = clock()
  last = t_begin
  ACTIVE = true
end

-- The end of the replan block.
function M.finish(state, now)
  if not ACTIVE then return end
  local t = clock()
  local r = row(cur, stk[sp])
  r.us = r.us + (t - last)
  ACTIVE = false
  local total = t - t_begin
  replans = replans + 1
  game_us = game_us + total
  for k, v in pairs(cur) do
    local g = row(game, k)
    g.n = g.n + v.n
    g.us = g.us + v.us
  end
  local i = r_info
  out(string.format("REPLAN_PROF t=%d tier=%d tgt_ms=%.1f prev_ms=%.1f prev_killed=%s goal=%s->%s us=%.0f | %s",
    now or 0, i.tier, i.tgt, i.last, tostring(i.wk), i.goal,
    tostring(state.goal and state.goal.kind), total, rows_line(cur, total)))
  local every = C.REPLAN_PROF_GAME_EVERY or 10
  if every > 0 and replans % every == 0 then
    out(string.format("REPLAN_PROF_GAME t=%d replans=%d killed=%d us=%.0f | %s",
      now or 0, replans, killed, game_us, rows_line(game, game_us)))
  end
end

M.is_active = function() return ACTIVE end

return M
