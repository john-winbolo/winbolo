-- =========================================================================
-- GoalHunter/pt2_dbl.lua -- PT2_DOUBLE_TAKE_DISCOUNT: the double-take cache
-- pool 6 (attack_pill) reads to price a pill lower when pill take v2 finds
-- a safe double take on it.
-- =========================================================================
-- One shadow v2 run per pill (pt2_attack.build_inp + V2.evaluate_group, the
-- same calls the real take makes), only when another live hostile or neutral
-- pill is in chain reach of the pill. The answer is cached in
-- state._pt2_dbl.by_id[id] = { safe, kills, ids (chain world ids), have,
-- need, pills, walls, trees_need, tick, hp, owner, carried, trees }. An entry is
-- dropped when a chain pill's health or owner changes, our carried pills or
-- trees change, a wall square changes in pill range of a chain pill
-- (M.on_tile from threat.update), or it is PT2_DBL_CACHE_TICKS old.
-- At most PT2_DBL_EVALS_PER_TICK runs per tick, and only when the last think
-- plus the run's cost fits under the tick target (see may_run for the cost
-- before the first run).
-- The module keeps no state of its own: all of it lives in state, so a new
-- game starts empty. goals.lua and threat.lua call it through require() so
-- think() gets no new upvalue.
-- =========================================================================

local C      = require("constants")
local print2 = require("print2")

local M = {}

local floor = math.floor
local clock_us = clock_us or function() return floor(os.clock() * 1e6) end

local function on()
  return C.PT2_DOUBLE_TAKE_DISCOUNT and C.PILLTAKE_V2_EVALUATOR and C.PT2_CHAINS
end
M.on = on

local function store(state)
  local s = state._pt2_dbl
  if not s then
    s = { by_id = {}, runs_tick = -1, runs = 0, est_ms = 0,
          n_runs = 0, run_us_sum = 0, run_us_max = 0,
          -- CPU account (M.charge): every us this feature adds, per tick
          t_first = nil, t_cur = -1, us_cur = 0, us_sum = 0, us_tick_max = 0, t_log = 0 }
    state._pt2_dbl = s
  end
  return s
end

local function live_enemy(p)
  return p and (p.owner == "hostile" or p.owner == "neutral") and (p.health or 0) > 0
    and not p.in_tank
end

-- Cheap filter: another live hostile or neutral pill within chain reach
-- (pill range + PT2_RANGE_MARGIN_WU, as V2.evaluate_group and the v2 ray
-- list use).
function M.has_partner(world, pill)
  local pr = require("shadow").PILL_RANGE + (C.PT2_RANGE_MARGIN_WU or 128)
  local pr2 = pr * pr
  for _, p in pairs(world.pills or {}) do
    if p ~= pill and live_enemy(p) then
      local dx, dy = (p.mx - pill.mx) * 256, (p.my - pill.my) * 256
      if dx * dx + dy * dy <= pr2 then return true end
    end
  end
  return false
end

-- Wall squares changed this tick (threat.update's terrain loop): drop the
-- entries with a chain pill in pill range of the square.
function M.on_tile(state, tx, ty, ntt)
  local s = state and state._pt2_dbl
  if not s then return end
  if ntt ~= C.T_BUILDING and ntt ~= C.T_HALFBUILD and ntt ~= C.T_RUBBLE then return end
  local R = C.PILL_RANGE_MAP or 9
  for id, e in pairs(s.by_id) do
    for _, p in ipairs(e.pos) do
      if math.abs(p[1] - tx) <= R and math.abs(p[2] - ty) <= R then
        s.by_id[id] = nil
        break
      end
    end
  end
end

-- Still true to the world? nil = keep, else the reason it is dropped.
local function stale_why(e, world, info, now)
  if now - e.tick > (C.PT2_DBL_CACHE_TICKS or 250) then return "age" end
  if (info.carried_pills or 0) ~= e.carried then return "carried" end
  if (info.trees or 0) ~= e.trees then return "trees" end
  for i, id in ipairs(e.ids) do
    local p = world.pills and world.pills[id]
    if not p or (p.health or 0) ~= e.hp[i] or p.owner ~= e.owner[i] then return "pill" end
  end
  return nil
end

-- The shadow run. Returns the entry (always, a no-chain answer too).
local function run(state, world, info, id, pill, now)
  local PA = require("pt2_attack")
  local V2 = require("pilltake_v2")
  local no_builder = info.man_status == C.LGM_DEAD
  local npb = math.min(info.carried_pills or 0, C.PPT_PILL_BLOCKERS_MAX or 2)
  local radius = C.PPT_STANDOFF
  local inp = PA.build_inp(state, {}, info, world, pill, no_builder, npb, radius, now)
  local e = { tick = now, carried = info.carried_pills or 0, trees = info.trees or 0,
              ids = { id }, hp = { pill.health or 0 }, owner = { pill.owner },
              pos = { { pill.mx, pill.my } },
              safe = false, kills = 0, have = 0, need = 0, pills = 0, walls = 0, trees_need = 0 }
  if not inp then return e end
  local res = V2.evaluate_group(inp)
  local b = res and res.best
  if not b then return e end
  e.safe = b.safe and b.chain_mode and true or false
  e.kills = b.kills or 0
  e.have, e.need = b.have or 0, b.need or 0
  for _, s in ipairs(b.plan or {}) do
    if s.kind == "wall" then e.walls = e.walls + 1
    elseif s.kind == "pill" then e.pills = e.pills + 1 end
  end
  e.trees_need = e.walls * (C.WALL_SHIELD_BUILD_COST or 2)
  -- the chain's pills, the target first
  local ids = inp.ids or {}
  for _, p in ipairs(b.chain or {}) do
    local pid = ids[p]
    if pid and pid ~= id then
      e.ids[#e.ids + 1] = pid
      e.hp[#e.hp + 1] = p.health or 0
      e.owner[#e.owner + 1] = p.owner
      e.pos[#e.pos + 1] = { p.mx, p.my }
    end
  end
  return e
end

-- The run cost may_run checks, in ms. After our first run: the EWMA of our
-- runs. Before it: the last real v2 run of a take (pt2_attack writes
-- state._pt2_v2_last_us), else the last think's own ms, so the first run
-- waits for a tick that used under half the target.
local function est_ms(s, state, last)
  if s.n_runs > 0 then return s.est_ms end
  local seed = state._pt2_v2_last_us
  if seed then return seed / 1000 end
  return last
end
M._est_ms = est_ms

-- May a shadow run go this tick? One per PT2_DBL_EVALS_PER_TICK, never on a
-- tick with no measured last think (the first tick of a round), and the last
-- think plus the run cost under the tick target.
local function may_run(s, state, now)
  if s.runs_tick ~= now then s.runs_tick, s.runs = now, 0 end
  if s.runs >= (C.PT2_DBL_EVALS_PER_TICK or 1) then return false end
  local br = _G.brain
  local last, tgt = br and br.lastThinkMs or 0, br and br.targetMs or 0
  if tgt > 0 then
    if last <= 0 then return false end
    if last + est_ms(s, state, last) >= tgt then return false end
  end
  return true
end

-- The cached answer for pill id, fresh, or nil. Runs v2 when the entry is
-- missing or stale and this tick allows a run.
function M.get(state, world, info, id, pill, now)
  if not on() or not live_enemy(pill) then return nil end
  local s = store(state)
  local e = s.by_id[id]
  if e and stale_why(e, world, info, now) then
    s.by_id[id] = nil
    e = nil
  end
  if e then return e end
  if not M.has_partner(world, pill) then return nil end
  if not may_run(s, state, now) then return nil end
  s.runs = s.runs + 1
  local t0 = clock_us()
  local ok, r = pcall(run, state, world, info, id, pill, now)
  local us = clock_us() - t0
  local a = C.CAPACITY_EWMA_ALPHA or 0.2
  -- the first run is the EWMA's start, not a blend with 0
  s.est_ms = (s.n_runs == 0) and us / 1000 or s.est_ms * (1 - a) + (us / 1000) * a
  s.n_runs = s.n_runs + 1
  s.run_us_sum = s.run_us_sum + us
  if us > s.run_us_max then s.run_us_max = us end
  if not ok then
    local msg = tostring(r)
    if msg:find("tick_budget_exceeded", 1, true) then error(r, 0) end
    print("[pt2_dbl] shadow run error: " .. msg)
    return nil
  end
  s.by_id[id] = r
  if BRAIN_DEBUG_MODE then
    local br = _G.brain
    print2(string.format("PT2_DBL_RUN t=%d pill#%s chain=%s safe=%s kills=%d have=%d need=%d pills=%d/%d walls=%d trees=%d/%d us=%d last_ms=%.2f tgt_ms=%.2f est_ms=%.2f",
      now, tostring(id), table.concat(r.ids, "-"), tostring(r.safe), r.kills, r.have, r.need,
      r.pills, r.carried, r.walls, r.trees, r.trees_need, us,
      br and br.lastThinkMs or 0, br and br.targetMs or 0, s.est_ms))
  end
  return r
end

-- CPU account: us this feature added on tick now (shadow runs, the
-- double-take diff sweep). Debug builds log PT2_DBL_CPU every 500 ticks:
-- mean us per tick since the first charge, and the dearest tick.
function M.charge(state, now, us)
  local s = store(state)
  if not s.t_first then s.t_first, s.t_log = now, now end
  if s.t_cur ~= now then
    if s.us_cur > s.us_tick_max then s.us_tick_max = s.us_cur end
    s.t_cur, s.us_cur = now, 0
  end
  s.us_cur = s.us_cur + us
  s.us_sum = s.us_sum + us
  if BRAIN_DEBUG_MODE and now - s.t_log >= 500 then
    s.t_log = now
    local ticks = math.max(1, now - s.t_first + 1)
    print2(string.format("PT2_DBL_CPU t=%d ticks=%d runs=%d run_mean_us=%.0f run_max_us=%d mean_us_per_tick=%.1f max_tick_us=%d",
      now, ticks, s.n_runs, s.n_runs > 0 and s.run_us_sum / s.n_runs or 0, s.run_us_max,
      s.us_sum / ticks, math.max(s.us_tick_max, s.us_cur)))
  end
end

-- The row condition: v2's plan is safe (every ray covered once the plan's
-- blockers stand), kills at least two pills, and we hold what the plan
-- uses now: the carried pills it drops and the trees for its walls.
-- A's standing cover (have/need) is not checked: the plan's blockers are
-- what make the take real. Returns true, or false and the reason.
function M.qualifies(e)
  if not e then return false, "none" end
  if not e.safe then return false, "unsafe" end
  if e.kills < 2 or #e.ids < 2 then return false, "kills" end
  if e.pills > e.carried then return false, "pills" end
  if e.trees < e.trees_need then return false, "trees" end
  return true
end

-- Exclusions for the row: the summed contrib of the target and every other
-- chain pill (threat.pill_contrib), and the other chain pills' coverage
-- disks ({tile_key = n}). Cached in the entry; the contrib sum is built
-- again when a source table changed (threat rebuild).
function M.exclusions(e, threat)
  local pc = threat.pill_contrib or {}
  local src = {}
  for i, p in ipairs(e.pos) do src[i] = pc[p[2] * 256 + p[1]] or false end
  local same = e._src ~= nil
  if same then
    for i = 1, #src do if src[i] ~= e._src[i] then same = false; break end end
  end
  if not same then
    local sum = {}
    for i = 1, #src do
      local t = src[i]
      if t then for k, v in pairs(t) do sum[k] = (sum[k] or 0) + v end end
    end
    e._src, e._contrib, e._other = src, sum, nil
  end
  if not e._other then
    local other = {}
    for i = 2, #src do
      local t = src[i]
      if t then for k, v in pairs(t) do other[k] = (other[k] or 0) + v end end
    end
    e._other = other
  end
  if not e._cov then
    local R = C.PILL_RANGE_MAP or 9
    local R2 = R * R
    local cov = {}
    for i = 2, #e.pos do
      local px, py = e.pos[i][1], e.pos[i][2]
      for dy = -R, R do
        for dx = -R, R do
          local nx, ny = px + dx, py + dy
          if dx * dx + dy * dy <= R2 and nx >= 0 and nx < 256 and ny >= 0 and ny < 256 then
            local k = ny * 256 + nx
            cov[k] = (cov[k] or 0) + 1
          end
        end
      end
    end
    e._cov = cov
  end
  -- contrib: target + others (diff scan B, spot, travel); other: the other
  -- chain pills only; cov_excl: their coverage disks (diff scan A)
  return { contrib = e._contrib, cov_excl = e._cov }, e._other
end

-- Ids of the other chain pills as a set, for the xfire loop.
function M.other_set(e)
  if not e._set then
    local set = {}
    for i = 2, #e.ids do set[e.ids[i]] = true end
    e._set = set
  end
  return e._set
end

-- Run stats for the CPU report: runs, mean us, max us.
function M.stats(state)
  local s = state and state._pt2_dbl
  if not s or s.n_runs == 0 then return 0, 0, 0 end
  return s.n_runs, s.run_us_sum / s.n_runs, s.run_us_max
end

return M
