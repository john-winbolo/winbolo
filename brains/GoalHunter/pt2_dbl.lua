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
-- before the first run). PT2_DBL_SPLIT: a run is a job of parts, one v2
-- evaluate each, spread evenly over PT2_DBL_SPREAD_TICKS ticks and dropped
-- after PT2_DBL_SPLIT_MAX_TICKS. The cache ticks, the runs per tick
-- (one-go path), the spread and the cap come from the capacity tier row
-- (tier_val), else the knobs.
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
          -- PT2_DBL_SPLIT: the open job, parts this tick, part stats
          job = nil, est_part_ms = 0,
          n_parts = 0, part_us_sum = 0, part_us_max = 0, n_jobs = 0, n_drop = 0,
          n_drop_cap = 0, n_wait = 0, n_big = 0, big_us_sum = 0, job_ticks = {},
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
  -- PT2_DBL_SPLIT: the open job, near the target or a pill of its group
  local job = s.job
  if job and not job.dead then
    local list = { job.inp.A }
    for _, p in ipairs(job.g.list or {}) do list[#list + 1] = p end
    for _, p in ipairs(list) do
      if math.abs(p.mx - tx) <= R and math.abs(p.my - ty) <= R then job.dead = "wall"; break end
    end
  end
end

-- PT2_DBL_RUN / PT2_DBL_DROP / PT2_DBL_CPU lines: print2 in debug builds;
-- with PT2_DBL_STDERR_LOG a plain print() (a release DS drops it), and with
-- PT2_DBL_EVENT_FILE set to a file name, also a line in
-- <brain dir>/fs_events/<name> (as FOCUS_SPIKE_EVENT_FILE). Log only.
local function logline(line)
  if BRAIN_DEBUG_MODE then print2(line) end
  if not C.PT2_DBL_STDERR_LOG then return end
  print(line)
  local L = C.PT2_DBL_EVENT_FILE
  if type(L) == "string" and L:match("^[%w_%.%-]+$") then
    local dir = (rawget(_G, "BRAIN_DIR") or "."):gsub("[/\\]opt$", "")
    local f = io.open(dir .. "/fs_events/" .. L, "a")
    if f then f:write(line, "\n"); f:close() end
  end
end

-- A double-take value for this capacity tier (C.BRAIN_CAPACITY_LEVELS row
-- field), else the knob. Fields: dbl_cache (PT2_DBL_CACHE_TICKS),
-- dbl_evals (PT2_DBL_EVALS_PER_TICK), dbl_spread (PT2_DBL_SPREAD_TICKS),
-- dbl_split (PT2_DBL_SPLIT_MAX_TICKS).
local function tier_val(state, field, knob, dflt)
  local cap = state and state._capacity
  local v = cap and cap[field]
  if v == nil then v = C[knob] end
  if v == nil then v = dflt end
  return v
end
M._tier_val = tier_val

-- Still true to the world? nil = keep, else the reason it is dropped.
local function stale_why(e, world, info, now, state)
  if now - e.tick > tier_val(state, "dbl_cache", "PT2_DBL_CACHE_TICKS", 250) then return "age" end
  if (info.carried_pills or 0) ~= e.carried then return "carried" end
  if (info.trees or 0) ~= e.trees then return "trees" end
  for i, id in ipairs(e.ids) do
    local p = world.pills and world.pills[id]
    if not p or (p.health or 0) ~= e.hp[i] or p.owner ~= e.owner[i] then return "pill" end
  end
  return nil
end

-- The entry for a run on pill id: the inputs it saw, and no take yet.
local function new_entry(info, id, pill, now)
  return { tick = now, carried = info.carried_pills or 0, trees = info.trees or 0,
           ids = { id }, hp = { pill.health or 0 }, owner = { pill.owner },
           pos = { { pill.mx, pill.my } },
           safe = false, kills = 0, have = 0, need = 0, pills = 0, walls = 0, trees_need = 0 }
end

-- The v2 inputs for a shadow run on pill, or nil.
local function make_inp(state, world, info, pill, now)
  local PA = require("pt2_attack")
  local no_builder = info.man_status == C.LGM_DEAD
  local npb = math.min(info.carried_pills or 0, C.PPT_PILL_BLOCKERS_MAX or 2)
  local inp = PA.build_inp(state, {}, info, world, pill, no_builder, npb, C.PPT_STANDOFF, now)
  -- PT2_CHAINS_ONLY (S10): the entry counts only as a safe chain, so v2
  -- plans no single take here (the old evaluator ignores the field)
  if inp and inp.chains_only_runs then inp.chains_only = true end
  return inp
end

-- Fill entry e from v2's group result.
local function fill_entry(e, res, inp, id)
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
  -- PT2_BLOCKER_MARK: the plan's blocker pill squares (packed my*256+mx),
  -- as to_scan_result reads them: an ally pill on a ray that reaches the
  -- tank, and a square where the plan places a pill. attack.take_blocker_tiles
  -- marks them while this plan prices the best pool-6 row.
  local blk, seen = nil, {}
  for _, r in ipairs(b.rays or {}) do
    if r.hit == "tank" then
      for k = 1, r.n_block or 0 do
        local bl = r.block[k]
        if bl.kind == "ally_pill" and not seen[bl.sq] then
          seen[bl.sq] = true
          blk = blk or {}
          blk[#blk + 1] = bl.sq
        end
      end
    end
  end
  for _, s in ipairs(b.plan or {}) do
    local sq = s.kind == "pill" and s.mx and s.my and (s.my * 256 + s.mx)
    if sq and not seen[sq] then
      seen[sq] = true
      blk = blk or {}
      blk[#blk + 1] = sq
    end
  end
  e.blk = blk
  -- the chain's pills, the target first (an unsafe chain too: the entry
  -- goes stale when one of them changes)
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
M._fill_entry = fill_entry

-- The shadow run in one go. Returns the entry (always, a no-chain answer too).
local function run(state, world, info, id, pill, now)
  local V2 = require("pt2_eval")
  -- debug profile (C.PT2_PROF_LOG, PT2_DBL_PROF line): the parts of one run, in us
  local G = BRAIN_DEBUG_MODE and C.PT2_PROF_LOG and rawget(_G, "gh_pt2_old")
  local prof = G and G.prof_take and {}
  if prof then G.prof(true); G.prof_take() end
  local t0 = prof and clock_us()
  local inp = make_inp(state, world, info, pill, now)
  local PA = package.loaded["pt2_attack"]
  if prof then prof.build_inp = clock_us() - t0; prof.world_inp = PA and PA.prof_world_us or 0 end
  local e = new_entry(info, id, pill, now)
  if not inp then return e end
  local t1 = prof and clock_us()
  local res = V2.evaluate_group(inp)
  if prof then
    prof.group = clock_us() - t1
    prof.c = G.prof_take()
    M.last_prof = prof
  end
  return fill_entry(e, res, inp, id)
end

-- ── PT2_DBL_SPLIT: one shadow run as a job of parts over ticks ──────────
-- Part 1 builds the inputs (a snapshot: the pill list, our tank, armour,
-- anger and banned angles at the start tick) and runs v2 on the target;
-- each later part runs v2 with one more pill of the group as A
-- (pilltake_v2_old.group_step). One part = one whole v2 evaluate. The parts are
-- spread evenly over dbl_spread ticks (PT2_DBL_SPREAD_TICKS): part k of n
-- (k from 0) is due on tick t0 + floor(k * spread / n). A due part also
-- waits while this think so far plus its estimated cost would reach the
-- tick target (the safety net); it runs on a later tick. The job is dropped
-- when a pill of the snapshot changes (health, owner, square, carried),
-- when the entry would be dropped (stale_why: our carried pills or trees),
-- when a wall square changes in pill range of the group (M.on_tile), on a
-- budget kill, or when it is not done in dbl_split ticks
-- (PT2_DBL_SPLIT_MAX_TICKS); a dropped pill gets a new job on a later
-- pool-6 visit. The terrain memory and the Dijkstra travel costs are read
-- live, as the one-go run reads them.
local function snap_pills(inp)
  local sn = {}
  for i, p in ipairs(inp.pills) do
    sn[i] = { p, p.health or 0, p.owner, p.mx, p.my, p.in_tank and true or false }
  end
  return sn
end

local function job_why(job, world, info, now)
  if job.dead then return job.dead end
  local w = stale_why(job.e, world, info, now, job.state)
  if w then return w end
  for _, r in ipairs(job.snap) do
    local p = r[1]
    if (p.health or 0) ~= r[2] or p.owner ~= r[3] or p.mx ~= r[4] or p.my ~= r[5]
       or (p.in_tank and true or false) ~= r[6] then
      return "pill"
    end
  end
  if now - job.t0 >= tier_val(job.state, "dbl_split", "PT2_DBL_SPLIT_MAX_TICKS", 10) then
    return "cap"
  end
  return nil
end

-- The cost of the next part in ms: the EWMA of our parts. Before our first
-- part: the last real v2 run of a take (state._pt2_v2_last_us), else 0.
local function est_part_ms(s, state)
  if s.n_parts > 0 then return s.est_part_ms end
  local seed = state._pt2_v2_last_us
  return seed and seed / 1000 or 0
end
M._est_part_ms = est_part_ms

-- The safety net: this think so far (state._think_t0_us, from Brain.think)
-- plus the part's estimated cost must stay under the tick target. No
-- target published: no limit.
local function may_part(s, state)
  local br = _G.brain
  local tgt = br and br.targetMs or 0
  if tgt <= 0 then return true end
  local t0 = state._think_t0_us
  local so_far = t0 and (clock_us() - t0) / 1000 or 0
  -- PT2_DBL_PART_LEAVE_REST: leave room for the rest of the think too.
  local rest = C.PT2_DBL_PART_LEAVE_REST and s.rest_ms or 0
  return so_far + est_part_ms(s, state) + rest < tgt
end

-- PT2_DBL_PART_LEAVE_REST: the rest-of-think account. mark() at the shadow
-- pump (each tick the pool queue runs), parts add their us, and
-- note_think_end() at the main think exit takes the time since the mark,
-- minus this tick's parts, into the EWMA s.rest_ms. A killed think never
-- reaches the exit and gives no sample.
local function rest_mark(s, now)
  s.rest_mark_tick, s.rest_mark_us, s.rest_part_us = now, clock_us(), 0
end

function M.note_think_end(state, now)
  local s = state and state._pt2_dbl
  if not s or s.rest_mark_tick ~= now then return end
  s.rest_mark_tick = nil
  local ms = (clock_us() - s.rest_mark_us - (s.rest_part_us or 0)) / 1000
  if ms < 0 then ms = 0 end
  local a = C.CAPACITY_EWMA_ALPHA or 0.2
  s.rest_ms = s.rest_ms and (s.rest_ms * (1 - a) + ms * a) or ms
end

-- Is the job's next part due on tick now? Part k of n is due on tick
-- t0 + floor(k * spread / n). Before part 1 has run, n is not known.
local function part_due(job, now)
  if not job.total then return true end
  if job.n >= job.total then return false end
  local spread = tier_val(job.state, "dbl_spread", "PT2_DBL_SPREAD_TICKS", 5)
  return now - job.t0 >= floor(job.n * spread / job.total)
end
M._part_due = part_due

-- PT2_DBL_PART_SLICE_TICKS: is a part of this job sliced? Only an
-- evaluator with a slicer (pilltake_v2) is sliced.
local function slice_ticks(job)
  local n = C.PT2_DBL_PART_SLICE_TICKS or 0
  if n <= 0 or coroutine == nil then return 0 end
  local g = job.g
  if not (g and g.ev and g.ev.slicer) then return 0 end
  return n
end

-- One call of a part. Sliced: the part runs in a coroutine and the
-- slicer (pilltake_v2.slicer, fixed work units, no timers) yields when
-- the part's work for this tick is used; the next call resumes it.
-- Returns ok, done, yielded.
local function part_call(job, V2)
  local n = slice_ticks(job)
  if n <= 0 and not job.co then
    local ok, done = pcall(V2.group_step, job.g)
    return ok, done, false
  end
  if not job.co then
    local g = job.g
    job.co = coroutine.create(function() return V2.group_step(g) end)
    job.inp.v2_slice = g.ev.slicer(job.inp, n, coroutine.yield).fn
    job.slices = (job.slices or 0)
  end
  local ok, done = coroutine.resume(job.co)
  job.slices = job.slices + 1
  if ok and coroutine.status(job.co) ~= "dead" then return true, false, true end
  job.co, job.inp.v2_slice = nil, nil
  return ok, done, false
end

-- Run one part of the job (PT2_DBL_PART_SLICE_TICKS: one slice of it).
-- Stores the entry when the job is done.
local function job_part(s, state, job, now)
  local V2 = require("pt2_eval")
  local t0 = clock_us()
  local ok, done, yielded = part_call(job, V2)
  local us = clock_us() - t0
  if s.rest_part_us then s.rest_part_us = s.rest_part_us + us end
  local a = C.CAPACITY_EWMA_ALPHA or 0.2
  s.est_part_ms = (s.n_parts == 0) and us / 1000 or s.est_part_ms * (1 - a) + (us / 1000) * a
  s.n_parts = s.n_parts + 1
  s.part_us_sum = s.part_us_sum + us
  if us > s.part_us_max then s.part_us_max = us end
  if us > 2000 then s.n_big, s.big_us_sum = s.n_big + 1, s.big_us_sum + us end  -- log only
  job.us = job.us + us
  if yielded then job.slice_tick = now; return end   -- the part goes on next tick
  job.n = job.n + 1
  if not ok then
    s.job = nil
    local msg = tostring(done)
    if msg:find("tick_budget_exceeded", 1, true) then error(done, 0) end
    print("[pt2_dbl] shadow part error: " .. msg)
    return
  end
  if not job.total then job.total = job.n + V2.group_parts_left(job.g) end
  if not done then return end
  s.job = nil
  local e = fill_entry(job.e, job.g.res, job.inp, job.id)
  s.by_id[job.id] = e
  s.n_runs = s.n_runs + 1
  s.run_us_sum = s.run_us_sum + job.us
  if job.us > s.run_us_max then s.run_us_max = job.us end
  local ticks = now - job.t0 + 1
  s.job_ticks[ticks] = (s.job_ticks[ticks] or 0) + 1
  if (BRAIN_DEBUG_MODE or C.PT2_DBL_STDERR_LOG) then
    local br = _G.brain
    local line = string.format("PT2_DBL_RUN p=%s t=%d pill#%s chain=%s safe=%s kills=%d have=%d need=%d pills=%d/%d walls=%d trees=%d/%d us=%d parts=%d ticks=%d last_ms=%.2f tgt_ms=%.2f est_part_ms=%.2f",
      tostring(s.pn), now, tostring(job.id), table.concat(e.ids, "-"), tostring(e.safe), e.kills, e.have, e.need,
      e.pills, e.carried, e.walls, e.trees, e.trees_need, job.us, job.n, ticks,
      br and br.lastThinkMs or 0, br and br.targetMs or 0, s.est_part_ms)
    if job.slices then line = line .. string.format(" slices=%d", job.slices) end
    logline(line)
  end
end

-- PT2_DBL_SPLIT: run the open job's due parts. goals.lua calls it once per
-- tick before the pool queue.
function M.pump(state, world, info, now)
  if not on() or not C.PT2_DBL_SPLIT then return end
  local s = state and state._pt2_dbl
  if s and C.PT2_DBL_PART_LEAVE_REST then rest_mark(s, now) end
  local job = s and s.job
  if not job then return end
  s.pn = info.player_number
  local t0 = clock_us()
  local why = job_why(job, world, info, now)
  if why then
    s.job = nil
    s.n_drop = s.n_drop + 1
    if why == "cap" then s.n_drop_cap = s.n_drop_cap + 1 end
    if (BRAIN_DEBUG_MODE or C.PT2_DBL_STDERR_LOG) then
      local line = string.format("PT2_DBL_DROP p=%s t=%d pill#%s why=%s parts=%d/%d ticks=%d us=%d",
        tostring(s.pn), now, tostring(job.id), why, job.n, job.total or 0, now - job.t0, job.us)
      logline(line)
    end
  else
    -- PT2_DBL_PART_SLICE_TICKS: a part that yielded on an earlier tick
    -- goes on first (it is due: it started); a part that yields ends this
    -- tick's work.
    while s.job == job and (job.co and job.slice_tick ~= now or not job.co and part_due(job, now)) do
      if not may_part(s, state) then s.n_wait = s.n_wait + 1; break end
      job_part(s, state, job, now)
      if job.co then break end
    end
  end
  M.charge(state, now, clock_us() - t0)
end

-- PT2_DBL_SPLIT: open a job on pill id and run its first part. Returns the
-- entry when the job is done at once, else nil.
local function start_job(s, state, world, info, id, pill, now)
  local V2 = require("pt2_eval")
  local t0 = clock_us()
  local inp = make_inp(state, world, info, pill, now)
  local e = new_entry(info, id, pill, now)
  if not inp then s.by_id[id] = e; return e end
  local job = { id = id, state = state, t0 = now, e = e, inp = inp, snap = snap_pills(inp),
                g = V2.group_begin(inp), us = clock_us() - t0, n = 0, total = nil }
  s.job = job
  s.n_jobs = s.n_jobs + 1
  job_part(s, state, job, now)
  if s.by_id[id] == e then return e end
  return nil
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
  if s.runs >= tier_val(state, "dbl_evals", "PT2_DBL_EVALS_PER_TICK", 1) then return false end
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
-- PT2_DBL_SPLIT: a new run is a job of parts (M.pump runs the rest); the
-- answer is there on a later call, once the job is done.
function M.get(state, world, info, id, pill, now)
  if not on() or not live_enemy(pill) then return nil end
  local s = store(state)
  s.pn = info.player_number
  local e = s.by_id[id]
  if e and stale_why(e, world, info, now, state) then
    s.by_id[id] = nil
    e = nil
  end
  if e then return e end
  if not M.has_partner(world, pill) then return nil end
  if C.PT2_DBL_SPLIT then
    if s.job then return nil end   -- one job at a time
    if not may_part(s, state) then return nil end
    return start_job(s, state, world, info, id, pill, now)
  end
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
  local pf = M.last_prof
  M.last_prof = nil
  if BRAIN_DEBUG_MODE and pf then
    -- one line per run: Lua parts, then every C part as name=n/us
    local parts = {}
    for k, v in pairs(pf.c or {}) do parts[#parts + 1] = string.format("%s=%d/%.0f", k, v.n, v.us) end
    table.sort(parts)
    print2(string.format("PT2_DBL_PROF t=%d pill#%s us=%d build_inp=%.0f world_inp=%.0f group=%.0f %s",
      now, tostring(id), us, pf.build_inp or 0, pf.world_inp or 0, pf.group or 0, table.concat(parts, " ")))
  end
  if BRAIN_DEBUG_MODE then
    local br = _G.brain
    print2(string.format("PT2_DBL_RUN p=%s t=%d pill#%s chain=%s safe=%s kills=%d have=%d need=%d pills=%d/%d walls=%d trees=%d/%d us=%d last_ms=%.2f tgt_ms=%.2f est_ms=%.2f",
      tostring(s.pn), now, tostring(id), table.concat(r.ids, "-"), tostring(r.safe), r.kills, r.have, r.need,
      r.pills, r.carried, r.walls, r.trees, r.trees_need, us,
      br and br.lastThinkMs or 0, br and br.targetMs or 0, s.est_ms))
  end
  return r
end

-- CPU account: us this feature added on tick now (shadow runs, the
-- double-take diff sweep). Debug builds (or PT2_DBL_STDERR_LOG, plain
-- print) log PT2_DBL_CPU every 500 ticks:
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
  if (BRAIN_DEBUG_MODE or C.PT2_DBL_STDERR_LOG) and now - s.t_log >= 500 then
    s.t_log = now
    local ticks = math.max(1, now - s.t_first + 1)
    -- ticks per finished job: mean and p90
    local nj, sj, p90 = 0, 0, 0
    for t, n in pairs(s.job_ticks) do nj, sj = nj + n, sj + t * n end
    if nj > 0 then
      local acc = 0
      for t = 1, 1000 do
        acc = acc + (s.job_ticks[t] or 0)
        if acc >= 0.9 * nj then p90 = t; break end
      end
    end
    local line = string.format("PT2_DBL_CPU p=%s t=%d ticks=%d runs=%d run_mean_us=%.0f run_max_us=%d mean_us_per_tick=%.1f max_tick_us=%d jobs=%d drops=%d parts=%d part_mean_us=%.0f part_max_us=%d",
      tostring(s.pn), now, ticks, s.n_runs, s.n_runs > 0 and s.run_us_sum / s.n_runs or 0, s.run_us_max,
      s.us_sum / ticks, math.max(s.us_tick_max, s.us_cur), s.n_jobs, s.n_drop, s.n_parts,
      s.n_parts > 0 and s.part_us_sum / s.n_parts or 0, s.part_us_max)
      .. string.format(" drops_cap=%d waits=%d big2ms=%d big_mean_us=%.0f job_ticks_mean=%.2f job_ticks_p90=%d est_part_ms=%.2f rest_ms=%.2f",
      s.n_drop_cap, s.n_wait, s.n_big, s.n_big > 0 and s.big_us_sum / s.n_big or 0,
      nj > 0 and sj / nj or 0, p90, s.est_part_ms, s.rest_ms or 0)
    logline(line)
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

-- BLITZ_NO_DOUBLE_TAKE (2026-11-22): is the attack_pill goal g under a
-- double-take plan? A double take needs the pills to shoot in set
-- directions. Other tanks at the pill pull its fire away and break the
-- plan, so such a take is never a blitz. Two signs, either one is enough:
--   "plan"    the take's own v2 plan kills 2 or more pills (goal._pt2,
--             set by pt2_attack.finish only for a safe plan);
--   "shadow"  this bot's own double-take cache has a fresh entry on the
--             pill that qualifies (M.qualifies: safe, 2+ kills, the pills
--             and trees the plan needs are on hand).
-- The answer latches on the goal (g._blitz_dbl) until the goal ends, so an
-- entry that ages out cannot turn the take back into a blitz.
-- A blitz-only bot (squad.blitz_only) attacks only in a blitz, so the rule
-- does not apply to it. Only this bot's own map memory and pills feed both
-- signs. Returns the reason or nil.
function M.no_blitz(state, g, world, info, now)
  if not C.BLITZ_NO_DOUBLE_TAKE then return nil end
  if not (g and g.kind == "attack_pill" and g.target_id) then return nil end
  if g._blitz_dbl then return g._blitz_dbl end
  if require("squad").blitz_only(state) then return nil end
  local why
  local p2 = g._pt2
  if p2 and (p2.kills or 0) >= 2 then
    why = "plan"
  else
    local s = state and state._pt2_dbl
    local e = s and s.by_id[g.target_id]
    if e and world and info and not stale_why(e, world, info, now or 0, state)
       and M.qualifies(e)
       -- the pool's PT2_CHAIN_CLAIM rule: a chain pill an ally claims
       -- spoils the double take (no discount, and no sign here)
       and not (C.PT2_CHAIN_CLAIM and require("pt2_claim").chain_ally_claimed(
                  state, e.ids, now or 0, info.player_number or -1)) then
      why = "shadow"
    end
  end
  if why then g._blitz_dbl = why end
  return why
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
