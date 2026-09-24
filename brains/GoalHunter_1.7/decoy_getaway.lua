-- =========================================================================
-- GoalHunter/decoy_getaway.lua -- THE DECOY GETAWAY (Andrew, 2026-09-24)
--
-- A decoy hold (orders.lua, GO-THERE DECOY HARD HOLD) parks a bot on a
-- square a person pinged beside enemy pills, so the pills shoot the bot.
-- The getaway ADDS a way out to that hold.  It does not change when the
-- hold starts or ends.
--
-- THE PILLS (P).  The pills the hold counts (orders.decoy_pills: not ours,
-- not an ally's, not carried, alive, edist <= PILL_FIRE_RANGE of the decoy
-- square) whose shell really ARRIVES on the decoy square.  Sorted by id.
-- No pill in P = no getaway, and the hold is what it was before.
--
-- BLOCK(p, t), how well square t is shielded from pill p:
--   * t is out of p's range (goals.sea_cover_limit: 2048 wu to the tile
--     centre, one tile more for a heated pill)          -> 1.0 "range"
--   * p's shell runs out before t                       -> 1.0 "short"
--   * the shell arrives on t                            -> 0   "open"
--   * the shell stops on a full wall (T_BUILDING)       -> DECOY_GETAWAY_WALL_FULL
--   * the shell stops on a damaged wall (T_HALFBUILD)   -> DECOY_GETAWAY_WALL_DAMAGED
--   * the shell stops on OUR (or an ally's) live pill   -> health / PILLS_MAX_HEALTH
--   * the shell stops on an enemy pill or on a base     -> 0
-- The shell is the real one: goals.sea_shot_reaches, with WALLS ONLY as the
-- terrain that eats it.  A tree or a boat is shot away, so it is not cover:
-- the trace flies over it.
--
-- SAFETY(t) = sum over p in P of block(p, t) / #P.  t is a GETAWAY SQUARE
-- when at least one block is above 0, and the tank can drive on it: not a
-- wall (TERRAIN_COST_LAND 9999), no known mine, no live pill on it, and no
-- deep sea unless the tank is in a boat.
--
-- CLOSENESS (Andrew: "Decoying is easier when the decoyer is close to the
-- pillbox").  d(t) = the straight-line distance in tiles from t's centre to
-- the centre of the NEAREST pill in P.  prox(t) = max(0, 1 - d(t) /
-- PILL_FIRE_RANGE).  The TILE VALUE is
--   tile(t) = safety(t) + DECOY_GETAWAY_PROX_WEIGHT * prox(t).
-- Closeness never makes an open square a getaway square: that still needs a
-- block above 0.
--
-- THE CHAIN.  From the start square, a chain of 1..MAX_STEPS getaway
-- squares where EVERY step goes one ring further out: square i is at
-- Chebyshev distance i from the start (Andrew: "There's no use doubling back
-- on a path because the wall blockers would be destroyed already").  A step
-- is one of the 8 neighbour moves (C.DIRS8 order) that lands on the next
-- ring, and a diagonal step only when both squares beside it can be driven
-- on (the pathfinder's on-foot corner rule).  No square can come twice.
-- Its score is the sum of the tile values of its squares with the LAST one
-- counted DECOY_GETAWAY_LAST_WEIGHT times.  The best score wins; a tie goes
-- to the shorter chain, then to the lower key (my * 256 + mx) of the first
-- square, then to the chain found first.
--
-- THE SEARCH is a DP ring by ring (M.search).  Ring 0 is the start with the
-- value 0.  For a getaway square t on ring k:
--   best(t) = tile(t) + max over the ring k-1 squares u that step to t of
--             best(u)
-- with a tie in best(u) going to the lower first-square key.  Every chain to
-- a ring k square has k squares, so the length tie-break is between rings.
-- Every square is a candidate end: end(t) = best(u*) + LAST_WEIGHT *
-- tile(t) = best(t) + (LAST_WEIGHT - 1) * tile(t).  A square is worked
-- out only when a reached square on the ring inside it steps to it, so an
-- open field with no blocker costs the 8 squares of ring 1.  The order is
-- fixed (ring order, C.DIRS8), so every run gives the same chain.
--
-- WHAT THE BOT DOES (M.update, from orders.decoy_lock every think).  ONE HIT,
-- ONE STEP (or THE BLOCKER STEP, below):
--   wait   parked (on the decoy square, then on each chain square it
--          reached), turned to face the next square of the chain (M.keys).
--          It shoots what the hold lets it shoot.  The armour on arrival is
--          the baseline; DECOY_GETAWAY_HITS armour losses from then on move
--          it one square.  A fresh scan from the square it is on, with the
--          steps that are left, at most every DECOY_GETAWAY_RESCAN_TICKS and
--          only when something changed: the start square, the counted
--          pills, or a shield on the chain.
--   move   driving to the next square.  Hits on the way do not count.  It
--          never leaves the chain to fight.  On that square (the tank's
--          square is it) it parks: wait again, with a new baseline, facing
--          the square after it.  If the square can no longer be driven on:
--          a fresh scan from where the tank is with the steps that are left
--          (and on to its first square), or park where it is.
--          THE BLOCKER STEP (Andrew, Sep 24: move on when the blocker has
--          "2 or less shots left"): parked on a chain square (never the
--          decoy square: the first step still waits for a hit), every think
--          takes the CLOSEST counted pill to the tank's square (edist, ties
--          to the lower id; the other pills are ignored for this check
--          only, the scan does not change) and walks its shell line to the
--          tank's square (cpf.simulate_shot, the scan's own trace, walked
--          without stopping).  Each blocker on it is worth the pill shells
--          it still stops (M.shots_left): a full wall, a damaged wall, a
--          live pill of ours or an ally's (a tree is not a blocker).  The
--          sum is the SHOTS LEFT before the tank is open.  Shots left <=
--          DECOY_GETAWAY_BLOCKER_SHOTS (0 = no blocker left) moves it on at
--          once.  A hit still moves it too.  A shell that runs out before
--          the tank's square: no count, no step.  DECOY_GETAWAY_BLOCKER_STEP
--          false: hits only.
--   done   parked on the last square.  The hold goes on and ends the way it
--          always does (clock, pills down, caution, cancel, new order, death).
--
-- All the state is one table on the held order slot (h.ga), so it dies with
-- the order.  DECOY_GETAWAY false: nothing here runs and h.ga is never made.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local print2 = require("print2")
local bit    = require("bitcompat")

local M = {}

-- SPEED.  A scan runs once when the hold starts and again only when the
-- picture changes, so every scan is a cold one.  LuaJIT spends most of a
-- cold scan recording and compiling traces for code that then never runs
-- again (measured: about 900 us with the JIT, about 350 us without it; a
-- warm repeat is about 80 us with it, 170 us without it).  So this file runs
-- in the interpreter.  The shell traces it calls (goals.lua, cpathfinder)
-- keep the JIT.
if jit and jit.off then jit.off(true, true) end

-- goals.lua is big and loads orders.lua lazily; ask for it when it is
-- needed, once.
local goals_mod = nil
local function goals()
  if not goals_mod then goals_mod = require("goals") end
  return goals_mod
end

-- The terrain that eats a pill's shell for the getaway: walls only.
local WALL_STOP = { [C.T_BUILDING] = true, [C.T_HALFBUILD] = true }
M.WALL_STOP = WALL_STOP

local EPS = 1e-9

local function tile_of(w) return bit.rshift(w, 8) end

-- The hold's own pill rule (orders.decoy_pills).
local function counted(p, mx, my)
  return p.owner ~= "friendly" and p.owner ~= "allied" and not p.in_tank
         and (p.health or 0) > 0 and p.mx and p.my
         and U.edist(mx, my, p.mx, p.my) <= (C.PILL_FIRE_RANGE or 8)
end

local function counted_ids(world, mx, my)
  local ids = {}
  for pid, p in pairs((world and world.pills) or {}) do
    if counted(p, mx, my) then ids[#ids + 1] = pid end
  end
  table.sort(ids)
  return ids
end

-- P: the counted pills whose shell arrives on (mx,my).  Sorted by id.
function M.pill_set(world, mx, my)
  local G = goals()
  local out = {}
  for _, pid in ipairs(counted_ids(world, mx, my)) do
    local p = world.pills[pid]
    if G.sea_shot_reaches(world, U.m2w(p.mx), U.m2w(p.my), mx, my,
                          cpf.SHOT_PILL, WALL_STOP) then
      out[#out + 1] = { id = pid, pill = p }
    end
  end
  return out
end

local function live_pill_at(world, mx, my)
  local plist = world.pill_at and world.pill_at[my * 256 + mx]
  if not plist then return nil end
  for _, pe in ipairs(plist) do
    local q = pe.pill
    if q and (q.health or 0) > 0 and not q.in_tank then return q end
  end
  return nil
end

-- block(p, t).  Returns the value, the reason, and the tile that stopped
-- the shell (and that pill's health for a pill) when there is one.
function M.block(world, p, mx, my)
  local G = goals()
  local dwx = U.m2w(p.mx) - U.m2w(mx)
  local dwy = U.m2w(p.my) - U.m2w(my)
  local d = math.sqrt(dwx * dwx + dwy * dwy)
  if d > G.sea_cover_limit(p) then return 1.0, "range" end
  local reached, sx, sy = G.sea_shot_reaches(world, U.m2w(p.mx), U.m2w(p.my),
                                             mx, my, cpf.SHOT_PILL, WALL_STOP)
  if reached then return 0, "open" end
  if not sx then return 1.0, "short" end
  local tt = U.ttype(sx, sy)
  if tt == C.T_BUILDING then
    return C.DECOY_GETAWAY_WALL_FULL or 1.0, "wall", sx, sy
  end
  if tt == C.T_HALFBUILD then
    return C.DECOY_GETAWAY_WALL_DAMAGED or 0.5, "wall_damaged", sx, sy
  end
  local q = live_pill_at(world, sx, sy)
  if q then
    if q.owner == "friendly" or q.owner == "allied" then
      local b = q.health / (C.PILLS_MAX_HEALTH or 15)
      if b > 1 then b = 1 end
      return b, "pill", sx, sy, q.health
    end
    return 0, "enemy_pill", sx, sy
  end
  return 0, "base", sx, sy
end

-- Can this tank drive on (mx,my)?  The pathfinder's land cost table (9999 =
-- a wall, a half wall or deep sea), deep sea allowed in a boat, no known mine
-- and no live pill on the square.
function M.passable(world, mx, my, in_boat)
  if not U.in_map(mx, my) then return false end
  local raw = U.traw(mx, my)
  local mf = TERRAIN_MINE_FLAG
  if mf and bit.band(raw, mf) ~= 0 then return false end
  local tt = bit.band(raw, TERRAIN_MASK)
  if tt == C.T_DEEPSEA then return in_boat and true or false end
  if (C.TERRAIN_COST_LAND[tt] or 0) >= 9999 then return false end
  if live_pill_at(world, mx, my) then return false end
  return true
end

-- One square of the scan, worked out once per scan (so each (pill, square)
-- shell trace runs once).
local function cell_at(ctx, mx, my)
  local k = my * 256 + mx
  local c = ctx.cells[k]
  if c then return c end
  c = { mx = mx, my = my, key = k, s = 0, v = 0 }
  if not M.passable(ctx.world, mx, my, ctx.in_boat) then
    c.ok, c.why = false, "no_drive"
  else
    local sum, any, terms = 0, false, {}
    for i, tp in ipairs(ctx.P) do
      local b, why, sx, sy, hp = M.block(ctx.world, tp.pill, mx, my)
      -- M.block checks the range first and traces the shell only in range.
      if why ~= "range" then ctx.traces = ctx.traces + 1 end
      terms[i] = { id = tp.id, b = b, why = why, sx = sx, sy = sy, hp = hp }
      sum = sum + b
      if b > 0 then any = true end
    end
    c.terms = terms
    c.sum   = sum
    c.s     = sum / #ctx.P
    c.ok    = any
    if not any then c.why = "open" end
    -- CLOSENESS: d to the nearest pill in P, prox, and the tile value.
    local d = nil
    for _, tp in ipairs(ctx.P) do
      local ddx, ddy = tp.pill.mx - mx, tp.pill.my - my
      local dd = math.sqrt(ddx * ddx + ddy * ddy)
      if not d or dd < d then d = dd end
    end
    local R = C.PILL_FIRE_RANGE or 8
    local prox = d and (1 - d / R) or 0
    if prox < 0 then prox = 0 end
    c.d, c.prox = d, prox
    c.pw = (C.DECOY_GETAWAY_PROX_WEIGHT or 0) * prox
    c.v  = c.s + c.pw
  end
  ctx.cells[k] = c
  ctx.list[#ctx.list + 1] = c
  return c
end

-- A corner square only has to be drivable (the tank does not stop on it).
local function pass_at(ctx, mx, my)
  local k = my * 256 + mx
  local v = ctx.pass[k]
  if v == nil then
    local c = ctx.cells[k]
    if c then v = (c.why ~= "no_drive")
    else v = M.passable(ctx.world, mx, my, ctx.in_boat) end
    ctx.pass[k] = v
  end
  return v
end

-- The chain search: the outward DP described at the top.  Returns the best
-- chain (a list of cells, nil = none), its score, the scan context (cells
-- for the overlay) and the number of steps it tried.
function M.search(world, P, sx, sy, steps, in_boat)
  local ctx = { world = world, P = P, in_boat = in_boat,
                cells = {}, list = {}, pass = {}, traces = 0 }
  local LW   = C.DECOY_GETAWAY_LAST_WEIGHT or 2.0
  local DIRS = C.DIRS8
  local best, blen, bfirst, bend = -1, 0, 0, nil
  local edges = 0
  -- Ring 0: the start.  An entry: the cell, best, the first-square key and
  -- the entry it came from.
  local ring = { { x = sx, y = sy, best = 0, first = nil, prev = nil } }
  for k = 1, steps do
    local nxt, at = {}, {}
    for _, u in ipairs(ring) do
      for d = 1, 8 do
        local dx, dy = DIRS[d][1], DIRS[d][2]
        local nx, ny = u.x + dx, u.y + dy
        local ax, ay = nx - sx, ny - sy
        if ax < 0 then ax = -ax end
        if ay < 0 then ay = -ay end
        -- OUTWARD ONLY: the step must land on ring k.
        if (ax > ay and ax or ay) == k and U.in_map(nx, ny) then
          local c = cell_at(ctx, nx, ny)
          if c.ok and (dx == 0 or dy == 0
                       or (pass_at(ctx, u.x + dx, u.y) and pass_at(ctx, u.x, u.y + dy))) then
            edges = edges + 1
            local v = u.best + c.v
            local first = u.first or c.key
            local e = at[c.key]
            if not e then
              e = { x = nx, y = ny, c = c, best = v, first = first, prev = u }
              at[c.key] = e
              nxt[#nxt + 1] = e
            elseif v > e.best + EPS
                   or (v >= e.best - EPS and first < e.first) then
              e.best, e.first, e.prev = v, first, u
            end
          end
        end
      end
    end
    -- Every reached square is a candidate end.
    for _, e in ipairs(nxt) do
      local score = e.best + (LW - 1) * e.c.v
      if score > best + EPS
         or (score >= best - EPS
             and (k < blen or (k == blen and e.first < bfirst))) then
        best, blen, bfirst, bend = score, k, e.first, e
      end
    end
    if #nxt == 0 then break end
    ring = nxt
  end
  local bpath = nil
  if bend then
    bpath = {}
    local e = bend
    for i = blen, 1, -1 do bpath[i] = e.c; e = e.prev end
  end
  return bpath, (bpath and best or nil), ctx, edges
end

-- The score of a chain written out, the way the overlay and the log say it.
local function score_terms(path)
  local LW = C.DECOY_GETAWAY_LAST_WEIGHT or 2.0
  local t = {}
  for i, c in ipairs(path) do
    if i == #path then t[i] = string.format("%gx%.3f", LW, c.v)
    else t[i] = string.format("%.3f", c.v) end
  end
  return table.concat(t, " + ")
end
M.score_terms = score_terms

local function path_str(path)
  local t = {}
  for i, c in ipairs(path or {}) do
    t[i] = string.format("(%d,%d)s=%.3f+[d=%.2f prox=%.3f x%g=%.3f]=%.3f",
                         c.mx, c.my, c.s, c.d or -1, c.prox or 0,
                         C.DECOY_GETAWAY_PROX_WEIGHT or 0, c.pw or 0, c.v)
  end
  return table.concat(t, " ")
end

-- What M.update compares to decide on a fresh scan: the tank's square, the
-- counted pills, and every shield square of the chain (its terrain and the
-- health of a pill on it).
function M.signature(world, h, ga, tx, ty)
  local parts = { tx .. "," .. ty }
  for _, pid in ipairs(counted_ids(world, h.mx, h.my)) do
    parts[#parts + 1] = tostring(pid)
  end
  for _, c in ipairs(ga.path or {}) do
    for _, tm in ipairs(c.terms or {}) do
      if tm.sx then
        local q = live_pill_at(world, tm.sx, tm.sy)
        parts[#parts + 1] = string.format("%d,%d:%d:%s:%d", tm.sx, tm.sy,
            U.ttype(tm.sx, tm.sy), q and tostring(q.owner) or "-",
            q and (q.health or 0) or 0)
      end
    end
  end
  return table.concat(parts, "|")
end

-- A scan from (sx,sy) with `steps` left.  Writes ga.path (nil = no chain),
-- ga.score and the overlay data, and logs DECOY_GETAWAY_SCAN.
function M.rescan(world, info, h, sx, sy, steps, now, why)
  local ga = h.ga
  local t0 = clock_us and clock_us() or nil
  local P = M.pill_set(world, h.mx, h.my)
  -- Shell traces this scan: one per counted pill for P, then one per
  -- (pill in P, worked-out square) in range.
  local ptr = #counted_ids(world, h.mx, h.my)
  local path, score, ctx, edges = nil, nil, nil, 0
  if #P > 0 and steps > 0 then
    path, score, ctx, edges =
      M.search(world, P, sx, sy, steps, info and info.inboat)
  end
  local us = t0 and (clock_us() - t0) or -1
  ga.path, ga.score, ga.idx = path, score, 1
  ga.scan_tick = now
  ga.check = now
  ga.sig = M.signature(world, h, ga, sx, sy)
  local pids = {}
  for i, tp in ipairs(P) do pids[i] = tostring(tp.id) end
  ga.viz = { tick = now, sx = sx, sy = sy, P = P, path = path, score = score,
             cells = ctx and ctx.list or {}, edges = edges, us = us, why = why,
             traces = ptr + (ctx and ctx.traces or 0),
             step0 = ga.used or 0, steps = steps }
  print2(string.format("DECOY_GETAWAY_SCAN t=%d oid=%d why=%s from=(%d,%d) steps=%d P=[%s] tiles=%d edges=%d traces=%d best=%s path=%s score=%s us=%d",
         now or -1, h.oid or 0, tostring(why), sx, sy, steps,
         table.concat(pids, ","), ctx and #ctx.list or 0, edges,
         ptr + (ctx and ctx.traces or 0),
         score and string.format("%.3f", score) or "none",
         path and path_str(path) or "-",
         path and score_terms(path) or "-", us))
  return path
end

-- The square the hold goal points at: the next square of the chain while it
-- moves, the chain square it parked on after that, the decoy square before
-- the first step.
function M.park_tile(h)
  local ga = C.DECOY_GETAWAY and h and h.ga
  if ga then
    if ga.phase == "move" and ga.path and ga.path[ga.idx] then
      return ga.path[ga.idx].mx, ga.path[ga.idx].my
    end
    if ga.park_mx then return ga.park_mx, ga.park_my end
  end
  return h.mx, h.my
end

function M.driving(h)
  return (C.DECOY_GETAWAY and h and h.ga and h.ga.phase == "move") and true or false
end

-- THE BLOCKER STEP's pill: the closest counted pill to (mx,my), by edist,
-- ties to the lower id.  Returns the id, the pill and the distance.
function M.closest_pill(world, mx, my)
  local bid, bp, bd = nil, nil, nil
  for _, pid in ipairs(counted_ids(world, mx, my)) do
    local p = world.pills[pid]
    local d = U.edist(mx, my, p.mx, p.my)
    if not bd or d < bd - EPS then bid, bp, bd = pid, p, d end
  end
  return bid, bp, bd
end

-- SHOTS LEFT in one blocker: the pill shells it still stops, the last one
-- included (the shell that knocks a wall down or kills a pill is stopped
-- too; the next one goes through).  The engine's numbers
-- (src/bolo/building.c buildingAddItem, src/bolo/shells.c, src/bolo/pillbox.c
-- pillsDamagePos):
--   wall (full)    the first shell makes it a damaged wall and gives it a
--                  life of building_life; each shell after takes one life,
--                  and at 0 it is rubble.  building_life + 1 shells.
--   wall_damaged   its life left: 1 .. building_life, or building_life + 1
--                  for a damaged wall that no shell has touched yet.  The
--                  life is the engine's own list and the brain cannot see
--                  it, so the SMALLEST value is used: 1 (move sooner).
--   pill           its armour, pill_shell_damage (1) off per shell:
--                  ceil(armour / pill_shell_damage).
-- building_life and pill_shell_damage are rules the brain cannot read, so
-- they are the engine defaults: DECOY_GETAWAY_WALL_LIFE (BUILDING_LIFE 4)
-- and DECOY_GETAWAY_PILL_SHELL_DAMAGE (PILLBOX_SHELL_DAMAGE 1).
function M.shots_left(kind, pill)
  if kind == "wall" then return (C.DECOY_GETAWAY_WALL_LIFE or 4) + 1 end
  if kind == "wall_damaged" then return 1 end
  local d = C.DECOY_GETAWAY_PILL_SHELL_DAMAGE or 1
  return math.ceil((pill and pill.health or 0) / d)
end

-- THE BLOCKERS on pill p's shell line to (mx,my): the tiles that
-- cpf.simulate_shot crosses (the trace G.sea_shot_reaches walks) between the
-- pill's square and (mx,my), walked to the end without stopping.  A full or
-- damaged wall and a live pill of ours or an ally's are blockers, each with
-- its shots left.  Returns the shots left in all of them, the list of
-- blockers ({ mx, my, kind, shots }), or nil and the list when the shell
-- runs out before (mx,my).
function M.blockers(world, p, mx, my)
  local list = {}
  local ok, tiles = pcall(cpf.simulate_shot, U.m2w(p.mx), U.m2w(p.my),
                          U.m2w(mx), U.m2w(my), cpf.SHOT_PILL, 0)
  if not ok or not tiles then return nil, list end
  local sum = 0
  for i = 1, #tiles do
    local st = tiles[i]
    if st.mx == mx and st.my == my then return sum, list end
    if st.mx ~= p.mx or st.my ~= p.my then
      local tt = U.ttype(st.mx, st.my)
      local kind, q = nil, nil
      if tt == C.T_BUILDING then
        kind = "wall"
      elseif tt == C.T_HALFBUILD then
        kind = "wall_damaged"
      else
        q = live_pill_at(world, st.mx, st.my)
        if q and (q.owner == "friendly" or q.owner == "allied") then kind = "pill" end
      end
      if kind then
        local n = M.shots_left(kind, q)
        list[#list + 1] = { mx = st.mx, my = st.my, kind = kind, shots = n }
        sum = sum + n
      end
    end
  end
  return nil, list
end

-- Parked on (mx,my): a new armour baseline, no hits yet.
local function park_on(ga, mx, my, arm)
  ga.park_mx, ga.park_my = mx, my
  ga.hits, ga.arm = 0, arm
  ga.hit_tick = nil
end

-- Every think of a standing decoy hold (orders.decoy_lock, after the
-- pills-down end).  See WHAT THE BOT DOES above.
function M.update(state, world, info, h, now)
  if not C.DECOY_GETAWAY then return end
  if not (h and info and info.tankx and info.tanky) then return end
  local tx, ty = tile_of(info.tankx), tile_of(info.tanky)
  local arm = info.armour or 0
  local ga = h.ga
  local max_steps = C.DECOY_GETAWAY_MAX_STEPS or 5
  if not ga then
    ga = { phase = "wait", hits = 0, arm = arm, used = 0, idx = 1, trigs = {} }
    h.ga = ga
    M.rescan(world, info, h, tx, ty, max_steps, now, "arrival")
  end
  -- ARMOUR LOSS since the last think.  Counted only while parked (wait):
  -- the baseline is the armour on arrival at the square.
  if ga.phase == "wait" and arm < ga.arm then
    ga.hits = ga.hits + 1
    if not ga.hit_tick then
      ga.hit_tick, ga.hit_before, ga.hit_after = now, ga.arm, arm
    end
  end
  ga.arm = arm
  -- The blocker count is only made while parked (wait); the overlay must
  -- not show an old one while it moves or once it is done.
  if ga.phase ~= "wait" then ga.blk = nil end
  if ga.phase == "wait" then
    -- The scan starts from the square it parked on; before the first step,
    -- from the tank's square (the decoy park allows one square of slack).
    local fx, fy = tx, ty
    if ga.used > 0 then fx, fy = ga.park_mx, ga.park_my end
    if now - (ga.check or now) >= (C.DECOY_GETAWAY_RESCAN_TICKS or 50) then
      ga.check = now
      if M.signature(world, h, ga, fx, fy) ~= ga.sig then
        M.rescan(world, info, h, fx, fy, max_steps - ga.used, now, "changed")
      end
    end
    -- THE BLOCKER STEP: on a chain square only (ga.used > 0), with a next
    -- square to go to.  ga.blk is what the overlay shows.
    ga.blk = nil
    if C.DECOY_GETAWAY_BLOCKER_STEP and ga.used > 0 and ga.path and ga.path[ga.idx] then
      local pid, p = M.closest_pill(world, tx, ty)
      if p then
        local n, list = M.blockers(world, p, tx, ty)
        ga.blk = { id = pid, mx = p.mx, my = p.my, shots = n, list = list,
                   tx = tx, ty = ty }
      end
    end
    local by_hit = ga.hits >= (C.DECOY_GETAWAY_HITS or 1)
    local by_blk = (ga.blk and ga.blk.shots
                    and ga.blk.shots <= (C.DECOY_GETAWAY_BLOCKER_SHOTS or 2)) and true or false
    if ga.path and ga.path[ga.idx] and (by_hit or by_blk) then
      ga.phase = "move"
      local trig = by_hit and "hit" or "blk"
      ga.trigs = ga.trigs or {}
      ga.trigs[#ga.trigs + 1] = trig
      local t = ga.path[ga.idx]
      local bs = "-"
      if ga.blk then
        bs = string.format("p%s(%d,%d):%s", tostring(ga.blk.id), ga.blk.mx,
                           ga.blk.my, ga.blk.shots and (tostring(ga.blk.shots) .. "shots") or "short")
      end
      print2(string.format("DECOY_GETAWAY_GO t=%d oid=%d step=%d/%d to=(%d,%d) trigger=%s blk=%s hit_t=%d armour=%d->%d hits=%d path=%s",
             now or -1, h.oid or 0, ga.used + 1, ga.used + #ga.path - ga.idx + 1,
             t.mx, t.my, trig, bs, ga.hit_tick or -1, ga.hit_before or -1,
             ga.hit_after or -1, ga.hits, path_str(ga.path)))
    end
  end
  if ga.phase == "move" then
    local path = ga.path
    local t = path[ga.idx]
    if t.mx == tx and t.my == ty then
      -- ARRIVED: park here with a new baseline, facing the square after it.
      ga.used = ga.used + 1
      ga.idx = ga.idx + 1
      park_on(ga, t.mx, t.my, arm)
      if ga.idx > #path or ga.used >= max_steps then
        ga.phase = "done"
        print2(string.format("DECOY_GETAWAY_PARK t=%d oid=%d tile=(%d,%d) steps=%d -- end of the chain",
               now or -1, h.oid or 0, t.mx, t.my, ga.used))
      else
        ga.phase = "wait"
        local n = path[ga.idx]
        print2(string.format("DECOY_GETAWAY_STEP t=%d oid=%d tile=(%d,%d) steps=%d next=(%d,%d) armour=%d -- waiting for a hit or the blocker step",
               now or -1, h.oid or 0, t.mx, t.my, ga.used, n.mx, n.my, arm))
      end
    elseif not M.passable(world, t.mx, t.my, info.inboat) then
      -- The next square is no longer drivable: a fresh chain from here with
      -- the steps that are left (and on to its first square: the hit that
      -- started this step is spent), or park where the tank is.
      local left = max_steps - ga.used
      if not M.rescan(world, info, h, tx, ty, left, now, "next_blocked") then
        ga.phase = "done"
        park_on(ga, tx, ty, arm)
        print2(string.format("DECOY_GETAWAY_PARK t=%d oid=%d tile=(%d,%d) steps=%d -- no chain left",
               now or -1, h.oid or 0, tx, ty, ga.used))
      end
    end
  end
end

-- TURN TO FACE THE WAY OUT.  Parked in the wait phase with a chain and the
-- hold goal (not a fight: attack_tank / kill_lgm aim on their own), the
-- turn keys point the tank at the centre of the next square.  Called by
-- orders.decoy_keys on the final keys.
function M.keys(state, info, h, keys, taps)
  local ga = C.DECOY_GETAWAY and h and h.ga
  if not (ga and ga.phase == "wait" and ga.path and ga.path[ga.idx]) then
    return keys, taps
  end
  -- The hold goal is a goto_tile on the park square: decoy_goal's, or the
  -- ping order's own goto_tile that decoy_lock leaves in place.
  local g = state and state.goal
  local pmx, pmy = M.park_tile(h)
  if not (g and g.kind == "goto_tile" and g.mx == pmx and g.my == pmy) then
    return keys, taps
  end
  local KL, KR = _G.KEY_TURNLEFT, _G.KEY_TURNRIGHT
  if not (KL and KR and info and info.direction) then return keys, taps end
  local t = ga.path[ga.idx]
  local aim = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                         t.mx + 0.5, t.my + 0.5)
  local hb, tb = U.aim_turn_bits(U.adiff(info.direction, aim), 6, 1)
  local mask = bit.bnot(bit.bor(KL, KR))
  keys = bit.bor(bit.band(keys or 0, mask), hb)
  taps = bit.bor(bit.band(taps or 0, mask), tb)
  return keys, taps
end

-- ---------------------------------------------------------------------------
-- OVERLAY (viz id "decoy_getaway_viz").  The numbers are the scan's own
-- cells, so the panel is what the code computed.  Every scanned square:
-- yellow with its safety (a getaway square) or grey with why not ("open" =
-- no pill is blocked, "no_drive" = the tank cannot drive on it).  The chain:
-- green squares joined by a line from the tank's square, each one labelled
-- with its number and "s = (b1 + b2 ...) / #P = safety", and under it one
-- line per pill: its block and why (wall full, wall damaged, pill h/15, out
-- of range, shell short, open, enemy pill, base).  An orange box on every
-- shield square, a line from each pill to each chain square (red = open,
-- grey = blocked).  The header on the scan's start square: the step it is
-- on ("waiting for hit N" or "moving to square i"), the score written out
-- (last square x LAST_WEIGHT) and the scan cost.  Chain squares are
-- numbered from the decoy square (steps already taken + i).
-- ---------------------------------------------------------------------------
local WHY_TXT = {
  range = "out of range", short = "shell short", open = "open",
  wall = "wall full", wall_damaged = "wall damaged",
  enemy_pill = "enemy pill", base = "base",
}
local function term_txt(tm)
  local w = WHY_TXT[tm.why] or tostring(tm.why)
  if tm.why == "pill" then
    w = string.format("pill hp %d/%d", tm.hp or 0, C.PILLS_MAX_HEALTH or 15)
  end
  if tm.sx then w = string.format("%s @(%d,%d)", w, tm.sx, tm.sy) end
  return string.format("p%s %s = %.3f", tostring(tm.id), w, tm.b)
end
M.term_txt = term_txt

local function safety_txt(c, np)
  local t = {}
  for i, tm in ipairs(c.terms or {}) do t[i] = string.format("%.3f", tm.b) end
  return string.format("s = (%s) / %d = %.3f", table.concat(t, " + "), np, c.s)
end
M.safety_txt = safety_txt

-- The closeness term and the tile value, written out.
local function prox_txt(c)
  local W = C.DECOY_GETAWAY_PROX_WEIGHT or 0
  local R = C.PILL_FIRE_RANGE or 8
  return string.format("prox: d=%.2f, max(0, 1 - %.2f/%d) = %.3f, x%g = %.3f",
                       c.d or -1, c.d or -1, R, c.prox or 0, W, c.pw or 0)
end
M.prox_txt = prox_txt

local function tile_txt(c)
  return string.format("tile = %.3f + %.3f = %.3f", c.s, c.pw or 0, c.v or c.s)
end
M.tile_txt = tile_txt

function M.draw(viz, state)
  if not viz or not viz.is_on or not viz.is_on("decoy_getaway_viz") then return end
  local h = state and state.orders and state.orders.held
  local ga = h and h.decoy and h.ga
  local v = ga and ga.viz
  if not v then return end
  local ID = "decoy_getaway_viz"
  local np = #(v.P or {})
  for _, c in ipairs(v.cells or {}) do
    if c.ok then
      viz.rect(ID, c.mx, c.my, c.mx + 1, c.my + 1, 230, 210, 60, 60, true)
      viz.text(ID, c.mx + 0.5, c.my + 0.8, string.format("%.2f+%.2f", c.s, c.pw or 0),
               "center", 240, 230, 150, 200)
    else
      viz.rect(ID, c.mx, c.my, c.mx + 1, c.my + 1, 110, 110, 110, 50, true)
      viz.text(ID, c.mx + 0.5, c.my + 0.8, tostring(c.why),
               "center", 170, 170, 170, 180)
    end
  end
  -- The search bound: ring `steps` around the scan's start square.
  local R = v.steps or 0
  if R > 0 then
    viz.rect(ID, v.sx - R, v.sy - R, v.sx + R + 1, v.sy + R + 1, 90, 160, 255, 140, false)
  end
  -- The pills in P.
  for _, tp in ipairs(v.P or {}) do
    viz.circle(ID, tp.pill.mx + 0.5, tp.pill.my + 0.5, 0.6, 255, 60, 60, 230, false, false)
    viz.text(ID, tp.pill.mx + 0.5, tp.pill.my - 0.5, "P p" .. tostring(tp.id),
             "center", 255, 120, 120, 255)
  end
  local LW = C.DECOY_GETAWAY_LAST_WEIGHT or 2.0
  local px, py = v.sx, v.sy
  local base = v.step0 or 0
  for i, c in ipairs(v.path or {}) do
    local drove = i < (ga.idx or 1)
    viz.rect(ID, c.mx, c.my, c.mx + 1, c.my + 1, 60, 230, 90, drove and 60 or 120, true)
    viz.line(ID, px + 0.5, py + 0.5, c.mx + 0.5, c.my + 0.5, 60, 255, 90, 230)
    px, py = c.mx, c.my
    local head = string.format("#%d %s%s", base + i, safety_txt(c, np),
                               i == #v.path and string.format(" x%g (last)", LW) or "")
    viz.text(ID, c.mx + 0.5, c.my + 0.15, head, "center", 150, 255, 170, 255)
    local lines = { head }
    local nt = #(c.terms or {})
    for j, tm in ipairs(c.terms or {}) do
      local tt = term_txt(tm)
      lines[#lines + 1] = tt
      viz.text(ID, c.mx + 0.5, c.my + 0.15 + 0.3 * j, tt, "center",
               220, 220, 220, 230)
      if tm.sx then
        viz.rect(ID, tm.sx + 0.3, tm.sy + 0.3, tm.sx + 0.7, tm.sy + 0.7,
                 255, 150, 40, 220, false)
      end
    end
    -- The closeness term and the tile value the chain score adds up.
    for j, tx in ipairs({ prox_txt(c), tile_txt(c) }) do
      lines[#lines + 1] = tx
      viz.text(ID, c.mx + 0.5, c.my + 0.15 + 0.3 * (nt + j), tx, "center",
               200, 220, 255, 230)
    end
    for _, tp in ipairs(v.P or {}) do
      local open = false
      for _, tm in ipairs(c.terms or {}) do
        if tm.id == tp.id and tm.b == 0 then open = true end
      end
      if open then
        viz.line(ID, tp.pill.mx + 0.5, tp.pill.my + 0.5, c.mx + 0.5, c.my + 0.5,
                 255, 60, 60, 150)
      else
        viz.line(ID, tp.pill.mx + 0.5, tp.pill.my + 0.5, c.mx + 0.5, c.my + 0.5,
                 150, 150, 150, 90)
      end
    end
    viz.detail(string.format("decoy_getaway_%d_%d", c.mx, c.my), "rect",
               { c.mx, c.my, c.mx + 1, c.my + 1 },
               string.format("GETAWAY #%d (%d,%d)", base + i, c.mx, c.my), lines)
  end
  -- THE BLOCKER STEP: the closest pill's shell line to the tank (magenta),
  -- and a magenta box on each blocker it counted, with its shots left.
  local bk = ga.blk
  if bk then
    viz.line(ID, bk.mx + 0.5, bk.my + 0.5, bk.tx + 0.5, bk.ty + 0.5, 255, 80, 255, 200)
    for _, b in ipairs(bk.list or {}) do
      viz.rect(ID, b.mx + 0.15, b.my + 0.15, b.mx + 0.85, b.my + 0.85,
               255, 80, 255, 230, false)
      viz.text(ID, b.mx + 0.5, b.my + 0.6, string.format("%d shots", b.shots or 0),
               "center", 255, 150, 255, 255)
    end
  end
  -- Up to four header lines, not one: an overlay text is cut at
  -- OVERLAY_TEXT_MAX (128) bytes, in the game, in BrainTest and in a
  -- recording alike.
  local hdr, hdr2, hdr3, hdr4
  if v.path then
    local need = C.DECOY_GETAWAY_HITS or 1
    local doing
    if ga.phase == "move" then
      doing = string.format("moving to square #%d", ga.used + 1)
    elseif ga.phase == "done" then
      doing = string.format("done: parked on square #%d", ga.used)
    else
      doing = string.format("on square #%d, waiting for hit %d of %d",
                            ga.used, (ga.hits or 0) + 1, need)
    end
    hdr = string.format("GETAWAY step %d/%d %s", ga.used, base + #v.path, doing)
    hdr2 = string.format("score = %s = %.3f", score_terms(v.path), v.score or 0)
    hdr3 = string.format("P=%d tiles=%d edges=%d traces=%d %dus",
                         np, #(v.cells or {}), v.edges or 0, v.traces or 0, v.us or -1)
  else
    hdr = string.format("GETAWAY none (P=%d tiles=%d) -- the hold stays put",
                        np, #(v.cells or {}))
  end
  -- The blocker step line: the closest pill, its count, and the trigger of
  -- every step so far ("hit" or "blk").
  if C.DECOY_GETAWAY_BLOCKER_STEP then
    local tl = "-"
    if ga.trigs and #ga.trigs > 0 then tl = table.concat(ga.trigs, " ") end
    local lim = C.DECOY_GETAWAY_BLOCKER_SHOTS or 2
    local what
    if bk and bk.shots then
      what = string.format("closest p%s (%d,%d) shots left=%d, step at <=%d",
                           tostring(bk.id), bk.mx, bk.my, bk.shots, lim)
    elseif bk then
      what = string.format("closest p%s (%d,%d) shell short: no count",
                           tostring(bk.id), bk.mx, bk.my)
    elseif ga.phase == "wait" and (ga.used or 0) == 0 then
      what = "blocker step: off on the decoy square"
    else
      what = "blocker step: not counting"
    end
    hdr4 = string.format("%s | steps by: %s", what, tl)
  end
  viz.rect(ID, v.sx, v.sy, v.sx + 1, v.sy + 1, 90, 160, 255, 110, true)
  local ty = v.sy - 0.7 - (hdr3 and 1.0 or 0) - (hdr4 and 0.5 or 0)
  viz.text(ID, v.sx + 0.5, ty, hdr, "center", 150, 200, 255, 255)
  if hdr2 then
    viz.text(ID, v.sx + 0.5, ty + 0.5, hdr2, "center", 150, 200, 255, 255)
  end
  if hdr3 then
    viz.text(ID, v.sx + 0.5, ty + 1.0, hdr3, "center", 150, 200, 255, 255)
  end
  if hdr4 then
    viz.text(ID, v.sx + 0.5, ty + (hdr3 and 1.5 or 0.5), hdr4, "center",
             255, 150, 255, 255)
  end
end

return M
