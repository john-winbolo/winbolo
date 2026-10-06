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
--   * the shell trace fails (the C call errors)         -> 0   "error"
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
--          (and on to its first square), or park where it is.  Not there
--          after DECOY_GETAWAY_MOVE_TICKS (300): it parks on the square it
--          is on and scans again from there (THE MOVE TIMEOUT, Sep 26).
--          THE BLOCKER STEP (Andrew, Sep 24: move on when the blocker has
--          "2 or less shots left"): parked on a chain square (never the
--          decoy square: the first step still waits for a hit), every think
--          takes the CLOSEST counted pill to the PARK square (edist, ties
--          to the lower id; the other pills are ignored for this check
--          only, the scan does not change) and walks its shell line to the
--          park square (cpf.simulate_shot, the scan's own trace, walked
--          without stopping).  THE PARK SQUARE, not the tank's square
--          (Andrew, Sep 24: "It should stay counting even if it got pushed
--          off and head to the next spot"): a knock off the park square
--          changes neither the line, nor the last blocker, nor its hit
--          ledger.  The hold goal still points at the park square, and when
--          the count fires the move drives to the next square from wherever
--          the tank is (the goto_tile path starts at the tank; the arrival
--          test is the tank's square only).  Each blocker on it is worth the pill shells
--          it still stops: a full wall, a damaged wall, a live pill of
--          ours or an ally's (a tree is not a blocker).  Only the LAST
--          blocker is counted: with 2 or more on the line it holds
--          (M.blockers).  A wall's shells are COUNTED, not read (Andrew,
--          Sep 24, "count hits"): see THE HIT LEDGER below M.blockers.  The
--          count is the SHOTS LEFT before the tank is open.  Shots left <=
--          DECOY_GETAWAY_BLOCKER_SHOTS (0 = no blocker left) moves it on at
--          once.  A hit still moves it too.  A shell that runs out before
--          the park square: no count, no step.  DECOY_GETAWAY_BLOCKER_STEP
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
  local reached, sx, sy, err = G.sea_shot_reaches(world, U.m2w(p.mx), U.m2w(p.my),
                                                  mx, my, cpf.SHOT_PILL, WALL_STOP)
  if reached then return 0, "open" end
  -- A failed trace is not a short shell: no cover (the safe side).
  if err then return 0, "error" end
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
-- for the overlay) and the number of edges it took (steps from a reached
-- square onto a getaway square of the next ring).
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
  -- No chain before, a chain now: the hits taken while there was no way
  -- out are spent.  A new armour baseline, so the first step waits for a
  -- fresh hit.
  if path and not ga.path then
    ga.hits, ga.hit_tick = 0, nil
    ga.arm = (info and info.armour) or ga.arm
  end
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

-- THE DIAGONAL STEP (C.DECOY_GETAWAY_DIAGONAL).  goal is the hold goal
-- (orders.decoy_goal); it carries _getaway only while the chain moves.
-- Returns the goal square when it is diagonal to the tank's square
-- (tmx,tmy) and the tank can drive straight to it, else nil.  The side
-- squares follow the pathfinder's own corner rule: no speed-0 square
-- (wall, half wall, pill) and no deep sea on either side.  They must also
-- pass the chain search's own test (M.passable: no known mine, no live
-- pill on them; world = the brain's world, nil = terrain only).  Not in a
-- boat.
local function drivable(mx, my)
  local t = U.ttype(mx, my)
  return t ~= C.T_DEEPSEA and (C.MAP_SPEED[t] or 12) > 0
end
function M.diagonal_next(goal, tmx, tmy, in_boat, world)
  if not (C.DECOY_GETAWAY and C.DECOY_GETAWAY_DIAGONAL) then return nil end
  if not (goal and goal._getaway and goal.mx) or in_boat then return nil end
  local dx, dy = goal.mx - tmx, goal.my - tmy
  if (dx ~= 1 and dx ~= -1) or (dy ~= 1 and dy ~= -1) then return nil end
  local w = world or {}
  if not (drivable(goal.mx, goal.my)
          and drivable(goal.mx, tmy) and M.passable(w, goal.mx, tmy, false)
          and drivable(tmx, goal.my) and M.passable(w, tmx, goal.my, false)) then
    return nil
  end
  return goal.mx, goal.my
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

-- SHOTS LEFT in one blocker AS FIRST SEEN on a parked square: the pill
-- shells it still stops, the last one included (the shell that knocks a
-- wall down or kills a pill is stopped too; the next one goes through).
-- The engine's numbers (src/bolo/building.c buildingAddItem,
-- src/bolo/shells.c, src/bolo/pillbox.c pillsDamagePos):
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
-- and DECOY_GETAWAY_PILL_SHELL_DAMAGE (PILLBOX_SHELL_DAMAGE 1).  A wall's
-- count after that is THE HIT LEDGER's (below M.blockers).
function M.shots_left(kind, pill)
  if kind == "wall" then return (C.DECOY_GETAWAY_WALL_LIFE or 4) + 1 end
  if kind == "wall_damaged" then return 1 end
  local d = C.DECOY_GETAWAY_PILL_SHELL_DAMAGE or 1
  return math.ceil((pill and pill.health or 0) / d)
end

-- THE BLOCKERS on pill p's shell line to (mx,my): the tiles that
-- cpf.simulate_shot crosses (the trace G.sea_shot_reaches walks) between the
-- pill's square and (mx,my), walked to the end without stopping.  A full or
-- damaged wall and a live pill of ours or an ally's are blockers.
-- THE LAST BLOCKER ONLY (Andrew, Sep 24: "we don't need to count until
-- it's the last blocker between pill & us"): the blockers nearer the pill
-- take its shells first, so with 2 or more on the line nothing is counted
-- and the tank holds.  Returns shots, list, why:
--   0 blockers      0, {}, "open"          (it moves)
--   1 blocker       its shots left, { b }, "last"
--   2 or more       nil, list, "wait"      (b.shots not set)
--   shell short     nil, list, "short"     (runs out before (mx,my))
-- b = { mx, my, kind, shots }.  With a hit ledger (led) the last wall's
-- shots come from it (M.wall_shots), and b gets start / hits / park too.
-- A pill of ours or an ally's uses its live armour (M.shots_left).
function M.blockers(world, p, mx, my, led)
  local list = {}
  local ok, tiles = pcall(cpf.simulate_shot, U.m2w(p.mx), U.m2w(p.my),
                          U.m2w(mx), U.m2w(my), cpf.SHOT_PILL, 0)
  if not ok or not tiles then return nil, list, "short" end
  for i = 1, #tiles do
    local st = tiles[i]
    if st.mx == mx and st.my == my then
      if #list == 0 then return 0, list, "open" end
      if #list >= 2 then return nil, list, "wait" end
      local b = list[1]
      if led and b.kind ~= "pill" then
        M.wall_shots(led, b)
      else
        b.shots = M.shots_left(b.kind, b.q)
      end
      b.q = nil
      return b.shots, list, "last"
    end
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
        list[#list + 1] = { mx = st.mx, my = st.my, kind = kind, q = q }
      end
    end
  end
  for _, b in ipairs(list) do b.q = nil end
  return nil, list, "short"
end

-- THE HIT LEDGER (Andrew, Sep 24, "count hits"): the brain cannot read a
-- wall's life, so it counts the shells that stop on the LAST wall while
-- the tank is parked, from what a player can see and hear.  One ledger per
-- parked square (ga.led, new in park_on), keyed by the wall's square:
--   first seen    the first think a wall is the LAST blocker on the
--                 closest pill's shell line while parked (the count starts
--                 then).  A full wall starts at WALL_LIFE + 1 (5): it moves
--                 at 5 - hits <= 2, after 3 hits.  A wall that is ALREADY
--                 damaged then has a life the brain cannot know (the
--                 engine's building list), so it keeps 1, the worst case,
--                 and no hit is taken off it.
--   a hit         each wall-hit sound on the wall's square (M.hear) is one
--                 shell stopped on it.  A full wall first seen full and
--                 now damaged with no sound heard counts that change as 1
--                 hit (the sound can be lost, see M.hear).  A wall still
--                 standing always stops at least 1 more shell: never
--                 below 1.
--   gone          a wall that is rubble or grass now is not on the line:
--                 it counts 0.
-- A pill of ours or an ally's is not in the ledger: it uses its live
-- armour, as before.  With no blocker left it moves (M.blockers "open").
-- b gets start, hits, park ("wall" or "wall_damaged", the state first
-- seen) and shots.
-- The ledger entry of wall b: made the first time it is seen (and again
-- for a wall rebuilt full after it was seen damaged).  M.update makes it
-- BEFORE it hears the sounds of the think, so a hit in the same think the
-- wall became the last blocker is counted.
function M.led_entry(led, b)
  local k = b.my * 256 + b.mx
  local e = led[k]
  if e and b.kind == "wall" and e.dmg_seen then e = nil end  -- rebuilt
  if not e then
    e = { mx = b.mx, my = b.my, park = b.kind, start = M.shots_left(b.kind),
          hits = 0, ticks = {} }
    led[k] = e
  end
  return e
end

function M.wall_shots(led, b)
  local e = M.led_entry(led, b)
  if b.kind == "wall_damaged" then
    e.dmg_seen = true
    if e.park == "wall" and e.hits == 0 then e.hits = 1 end
  end
  b.start, b.hits, b.park = e.start, e.hits, e.park
  if e.park == "wall" then
    b.shots = math.max(1, e.start - e.hits)
  else
    b.shots = 1
  end
  return b.shots
end

-- HEARING THE HITS: a shell that stops on a wall plays shotBuildingNear on
-- the wall's square (src/bolo/shells.c, the BUILDING and HALFBUILDING
-- cases).  The server sends it as EVENT_SOUND [soundId, mx, my, player]
-- (server_sim_callbacks.c serverSimCbSoundDist), and a bot keeps the
-- square (input_packet.h, sound payloads; server_sim_snapshot.c
-- serverSimRecipientKeepsSoundSquares).  info.events has it
-- (brain_data.c, the EVENT_SOUND case).  Each such sound on a wall in the
-- ledger is one hit.  Note: the server sends only the CLOSEST sound of each
-- id per snapshot (soundPickOffer), so two walls hit in the same snapshot
-- give one sound; the damaged-change check in M.wall_shots covers the
-- first hit only.  Every shell counts, whoever fired it (a pill, an enemy
-- tank, this tank, an ally): the engine takes one life off the wall for
-- each shell that stops on it (shells.c, buildingAddItem in the BUILDING
-- and HALFBUILDING cases, with no owner test).  So a shell of ours or an
-- ally's that hits the wall uses up its cover exactly as a pill shell
-- does, and the ledger counts the wall's life, not the pill's shells.
-- Returns the number of hits counted.
function M.hear(led, events, now)
  if not (led and events) then return 0 end
  local n = 0
  local near, far = SND_SHOT_BUILDING_NEAR, SND_SHOT_BUILDING_FAR
  for _, ev in ipairs(events) do
    local d = ev.data
    if ev.type == EVENT_SOUND and d and (d[1] == near or d[1] == far) then
      local e = led[(d[3] or -1) * 256 + (d[2] or -1)]
      if e then
        e.hits = e.hits + 1
        e.ticks[#e.ticks + 1] = now
        n = n + 1
      end
    end
  end
  return n
end

-- Parked on (mx,my): a new armour baseline, no hits yet.
local function park_on(ga, mx, my, arm)
  ga.park_mx, ga.park_my = mx, my
  ga.hits, ga.arm = 0, arm
  ga.hit_tick = nil
  ga.led = {}   -- THE HIT LEDGER of this square
end

-- The last blocker's count as text: a wall first seen full "5-3=2", one
-- first seen damaged "dmg=1", a pill of ours "pill=N".
function M.blk_one(b)
  if b.park == "wall" then
    return string.format("%d-%d=%d", b.start or 0, b.hits or 0, b.shots or 0)
  elseif b.park == "wall_damaged" or b.kind == "wall_damaged" then
    return string.format("dmg=%d", b.shots or 0)
  elseif b.kind == "pill" then
    return string.format("pill=%d", b.shots or 0)
  end
  return tostring(b.shots or 0)
end
-- How far the tank is from the park square's centre, in world units
-- (256 to a square), from a ga.blk.
function M.off_park(bk)
  return (bk.twx or 0) - (bk.tx * 256 + 128), (bk.twy or 0) - (bk.ty * 256 + 128)
end
-- The whole count as text, for the GO log: "last(129,125)5-3=2",
-- "open", "2 blockers, waiting" or "short".
function M.blk_txt(bk)
  if bk.why == "last" and bk.list[1] then
    local b = bk.list[1]
    return string.format("last(%d,%d)%s", b.mx, b.my, M.blk_one(b))
  elseif bk.why == "wait" then
    return string.format("%d blockers, waiting", #bk.list)
  end
  return tostring(bk.why)
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
  -- For the overlays only: where the tank is, and the tick (time left).
  ga.tank_wx, ga.tank_wy, ga.now = info.tankx, info.tanky, now
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
      ga.led = ga.led or {}
      -- From the PARK square: a knock off it keeps the line, the last
      -- blocker and the ledger (see THE BLOCKER STEP above).  tx/ty on
      -- ga.blk is the park square (the end of the line); twx/twy is where
      -- the tank really is, for the overlay.
      local kx, ky = ga.park_mx, ga.park_my
      local pid, p = M.closest_pill(world, kx, ky)
      -- The last blocker first (no ledger yet), then its ledger entry, then
      -- the sounds of this think, then its count: a hit in the think the
      -- wall became the last blocker is not lost.
      local n, list, why
      if p then
        n, list, why = M.blockers(world, p, kx, ky, nil)
        if why == "last" and list[1].kind ~= "pill" then M.led_entry(ga.led, list[1]) end
      end
      if M.hear(ga.led, info.events, now) > 0 then
        for _, e in pairs(ga.led) do
          if e.ticks[#e.ticks] == now then
            print2(string.format("DECOY_GETAWAY_WALLHIT t=%d oid=%d wall=(%d,%d) hits=%d first=%s",
                   now or -1, h.oid or 0, e.mx, e.my, e.hits, e.park))
          end
        end
      end
      if p then
        if why == "last" and list[1].kind ~= "pill" then n = M.wall_shots(ga.led, list[1]) end
        ga.blk = { id = pid, mx = p.mx, my = p.my, shots = n, list = list,
                   why = why, tx = kx, ty = ky,
                   twx = info.tankx, twy = info.tanky, off = (tx ~= kx or ty ~= ky) }
      end
    end
    local by_hit = ga.hits >= (C.DECOY_GETAWAY_HITS or 1)
    local by_blk = (ga.blk and ga.blk.shots
                    and ga.blk.shots <= (C.DECOY_GETAWAY_BLOCKER_SHOTS or 2)) and true or false
    if ga.path and ga.path[ga.idx] and (by_hit or by_blk) then
      ga.phase = "move"
      -- For THE MOVE TIMEOUT: when the move began and the square it began on.
      ga.move_tick, ga.move_mx, ga.move_my = now, tx, ty
      local trig = by_hit and "hit" or "blk"
      ga.trigs = ga.trigs or {}
      ga.trigs[#ga.trigs + 1] = trig
      local t = ga.path[ga.idx]
      local bs = "-"
      if ga.blk then
        bs = string.format("p%s(%d,%d):%s", tostring(ga.blk.id), ga.blk.mx,
                           ga.blk.my, M.blk_txt(ga.blk))
        if ga.blk.off then
          local dx, dy = M.off_park(ga.blk)
          bs = bs .. string.format(",tank_off_park(%d,%d_wu)", dx, dy)
        end
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
    elseif now - (ga.move_tick or now) >= (C.DECOY_GETAWAY_MOVE_TICKS or 300) then
      -- THE MOVE TIMEOUT (Sep 26).  The next square is still drivable but
      -- the tank has not got there (an enemy tank on it, a long pathfinder
      -- detour).  Without a cap it drove about under fire for the rest of
      -- the hold, with hits not counted and fights refused.  After
      -- DECOY_GETAWAY_MOVE_TICKS it parks on the square it is on, as if it
      -- had arrived there: a new baseline and a new hit ledger, and the
      -- steps that are left come from a fresh scan from that square.  The
      -- move counts as a step only when the tank left the square it began
      -- on.  No chain from there: done, parked where it is.
      print2(string.format("DECOY_GETAWAY_MOVE_TIMEOUT t=%d oid=%d to=(%d,%d) tank=(%d,%d) ticks=%d",
             now or -1, h.oid or 0, t.mx, t.my, tx, ty, now - (ga.move_tick or now)))
      if tx ~= ga.move_mx or ty ~= ga.move_my then ga.used = ga.used + 1 end
      park_on(ga, tx, ty, arm)
      local left = max_steps - ga.used
      if M.rescan(world, info, h, tx, ty, left, now, "move_timeout") then
        ga.phase = "wait"
      else
        ga.phase = "done"
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
-- OVERLAYS (viz ids decoy_chain, decoy_scan_cells, decoy_score_terms,
-- decoy_pill_lines, decoy_blocker_count, decoy_status; BrainTest category
-- "Decoy").  The numbers are the scan's own cells, so each panel is what
-- the code computed.  What each one draws is written above its draw
-- function below.  Chain squares are numbered from the decoy square
-- (steps already taken + i).
-- ---------------------------------------------------------------------------
local WHY_TXT = {
  range = "out of range", short = "shell short", open = "open",
  wall = "wall full", wall_damaged = "wall damaged",
  enemy_pill = "enemy pill", base = "base", error = "trace error",
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

-- The six overlays (viz.lua, BrainTest category "Decoy").  Each one draws
-- only its own part; together they are the whole getaway.  Text places, so
-- that all six can be on at once: the search box is the ring `steps`
-- around the scan's start square.  Status lines sit above the box, the
-- blocker line below it, the score panel to the right of it.  Inside a
-- square: the chain label at the top (+0.25), the scan-from label at the
-- bottom (+0.75, the start square is never a scan cell), a scan cell's
-- value at +0.8.
local VIZ_CHAIN   = "decoy_chain"
local VIZ_CELLS   = "decoy_scan_cells"
local VIZ_TERMS   = "decoy_score_terms"
local VIZ_LINES   = "decoy_pill_lines"
local VIZ_BLOCKER = "decoy_blocker_count"
local VIZ_STATUS  = "decoy_status"
M.VIZ_IDS = { VIZ_CHAIN, VIZ_CELLS, VIZ_TERMS, VIZ_LINES, VIZ_BLOCKER, VIZ_STATUS }

-- Decoy: scan cells.  Every square the search worked out, and the box.
local function draw_cells(viz, v)
  local ID = VIZ_CELLS
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
  local R = v.steps or 0
  if R > 0 then
    viz.rect(ID, v.sx - R, v.sy - R, v.sx + R + 1, v.sy + R + 1, 90, 160, 255, 140, false)
  end
end

-- Decoy: chain.  The scan's start square (blue, "scan from"), the chain
-- squares in order (green) with their number, the trigger that sent the
-- tank there and "park" on the square it is parked on; in a move the
-- target square (cyan), a line from the tank to it and "MOVING to (x,y)".
local function draw_chain(viz, h, ga, v)
  local ID = VIZ_CHAIN
  viz.rect(ID, v.sx, v.sy, v.sx + 1, v.sy + 1, 90, 160, 255, 110, true)
  viz.text(ID, v.sx + 0.5, v.sy + 0.75, "scan from", "center", 150, 200, 255, 255)
  local pmx, pmy = ga.park_mx or h.mx, ga.park_my or h.my
  if ga.phase == "move" then pmx, pmy = nil, nil end
  local base = v.step0 or 0
  local px, py = v.sx, v.sy
  local park_on_chain = false
  for i, c in ipairs(v.path or {}) do
    local drove = i < (ga.idx or 1)
    viz.rect(ID, c.mx, c.my, c.mx + 1, c.my + 1, 60, 230, 90, drove and 60 or 120, true)
    viz.line(ID, px + 0.5, py + 0.5, c.mx + 0.5, c.my + 0.5, 60, 255, 90, 230)
    px, py = c.mx, c.my
    local k = base + i
    local lab = "#" .. k
    local trig = ga.trigs and ga.trigs[k]
    if trig then lab = lab .. " " .. trig end
    if pmx == c.mx and pmy == c.my then
      lab = lab .. " park"
      park_on_chain = true
    end
    viz.text(ID, c.mx + 0.5, c.my + 0.25, lab, "center", 150, 255, 170, 255)
  end
  if pmx then
    viz.rect(ID, pmx + 0.05, pmy + 0.05, pmx + 0.95, pmy + 0.95, 255, 255, 255, 200, false)
    if not park_on_chain then
      viz.text(ID, pmx + 0.5, pmy + 0.25, "park", "center", 255, 255, 255, 255)
    end
  end
  local t = ga.phase == "move" and ga.path and ga.path[ga.idx]
  if t then
    viz.rect(ID, t.mx, t.my, t.mx + 1, t.my + 1, 40, 230, 255, 110, true)
    if ga.tank_wx then
      local rx, ry = ga.tank_wx / 256, ga.tank_wy / 256
      local cx, cy = t.mx + 0.5, t.my + 0.5
      viz.line(ID, rx, ry, cx, cy, 40, 230, 255, 255)
      viz.text(ID, (rx + cx) / 2, (ry + cy) / 2 - 0.35,
               string.format("MOVING to (%d,%d)", t.mx, t.my), "center", 40, 230, 255, 255)
    end
  end
end

-- Decoy: pill lines.  The counted pills (P), a line from each to each
-- chain square (red = its shell reaches the square, grey = blocked) and an
-- orange box on each shield square.
local function draw_lines(viz, v)
  local ID = VIZ_LINES
  for _, tp in ipairs(v.P or {}) do
    viz.circle(ID, tp.pill.mx + 0.5, tp.pill.my + 0.5, 0.6, 255, 60, 60, 230, false, false)
    viz.text(ID, tp.pill.mx + 0.5, tp.pill.my - 0.5, "P p" .. tostring(tp.id),
             "center", 255, 120, 120, 255)
  end
  for _, c in ipairs(v.path or {}) do
    for _, tm in ipairs(c.terms or {}) do
      if tm.sx then
        viz.rect(ID, tm.sx + 0.3, tm.sy + 0.3, tm.sx + 0.7, tm.sy + 0.7,
                 255, 150, 40, 220, false)
      end
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
  end
end

-- Decoy: score terms.  A panel to the right of the search box: the chain
-- score, then for each chain square its safety sum, one line per counted
-- pill, the closeness term and the tile value.  Every number of the chain
-- score is on it.  The same lines go in each square's hover panel.
local function draw_terms(viz, v)
  local ID = VIZ_TERMS
  local np = #(v.P or {})
  local LW = C.DECOY_GETAWAY_LAST_WEIGHT or 2.0
  local R = v.steps or 0
  local x = v.sx + R + 1.4
  local y = v.sy - R
  local DY = 0.35
  if v.path then
    viz.text(ID, x, y, string.format("score = %s = %.3f", score_terms(v.path), v.score or 0),
             "topleft", 150, 200, 255, 255)
    y = y + DY
  end
  local base = v.step0 or 0
  for i, c in ipairs(v.path or {}) do
    local head = string.format("#%d %s%s", base + i, safety_txt(c, np),
                               i == #v.path and string.format(" x%g (last)", LW) or "")
    local lines = { head }
    viz.text(ID, x, y, head, "topleft", 150, 255, 170, 255)
    y = y + DY
    for _, tm in ipairs(c.terms or {}) do
      local tt = term_txt(tm)
      lines[#lines + 1] = tt
      viz.text(ID, x + 0.3, y, tt, "topleft", 220, 220, 220, 230)
      y = y + DY
    end
    for _, tx in ipairs({ prox_txt(c), tile_txt(c) }) do
      lines[#lines + 1] = tx
      viz.text(ID, x + 0.3, y, tx, "topleft", 200, 220, 255, 230)
      y = y + DY
    end
    viz.detail(string.format("decoy_getaway_%d_%d", c.mx, c.my), "rect",
               { c.mx, c.my, c.mx + 1, c.my + 1 },
               string.format("GETAWAY #%d (%d,%d)", base + i, c.mx, c.my), lines)
  end
end

-- Decoy: blocker count (THE BLOCKER STEP).  The closest pill's shell line
-- to the PARK square (magenta; the count is made on it) and a magenta box
-- on each blocker on it; the last blocker's box shows its count ("5-3=2
-- shots": start - hits counted = shots left).  The ledger's heard wall
-- hits on each wall, with their ticks.  A tank knocked off the park square
-- gets a thin line from the pill to where it really is, with its offset
-- from the park square's centre: that line is NOT counted.  Under the
-- search box: the state of the count and the trigger of every step so far.
local function draw_blocker(viz, ga, v)
  local ID = VIZ_BLOCKER
  local bk = ga.blk
  if bk then
    viz.line(ID, bk.mx + 0.5, bk.my + 0.5, bk.tx + 0.5, bk.ty + 0.5, 255, 80, 255, 200)
    if bk.off and bk.twx then
      local rx, ry = bk.twx / 256, bk.twy / 256
      local dx, dy = M.off_park(bk)
      viz.line(ID, bk.mx + 0.5, bk.my + 0.5, rx, ry, 255, 180, 255, 110)
      viz.text(ID, rx, ry + 0.45, string.format("tank off park (%d,%d wu)", dx, dy),
               "center", 255, 180, 255, 230)
    end
    for _, b in ipairs(bk.list or {}) do
      viz.rect(ID, b.mx + 0.15, b.my + 0.15, b.mx + 0.85, b.my + 0.85,
               255, 80, 255, 230, false)
      if bk.why == "last" then
        viz.text(ID, b.mx + 0.5, b.my + 0.6, M.blk_one(b) .. " shots",
                 "center", 255, 150, 255, 255)
      end
    end
  end
  -- The ledger is the count's own record, so it shows while the count is
  -- made (parked, the wait phase).
  if ga.phase == "wait" and ga.led then
    for _, e in pairs(ga.led) do
      local n = #(e.ticks or {})
      if n > 0 then
        local tk = {}
        for i = math.max(1, n - 3), n do tk[#tk + 1] = tostring(e.ticks[i]) end
        viz.text(ID, e.mx + 0.5, e.my + 0.3,
                 string.format("heard %d: t%s", n, table.concat(tk, ",")),
                 "center", 255, 200, 255, 255)
      end
    end
  end
  if not C.DECOY_GETAWAY_BLOCKER_STEP then return end
  local tl = "-"
  if ga.trigs and #ga.trigs > 0 then tl = table.concat(ga.trigs, " ") end
  local lim = C.DECOY_GETAWAY_BLOCKER_SHOTS or 2
  local what
  if bk and bk.why == "last" then
    what = string.format("closest p%s (%d,%d) last blocker %s shots, step at <=%d",
                         tostring(bk.id), bk.mx, bk.my, M.blk_one(bk.list[1]), lim)
  elseif bk and bk.why == "open" then
    what = string.format("closest p%s (%d,%d) no blocker left, step",
                         tostring(bk.id), bk.mx, bk.my)
  elseif bk and bk.why == "wait" then
    what = string.format("closest p%s (%d,%d) %d blockers, waiting",
                         tostring(bk.id), bk.mx, bk.my, #bk.list)
  elseif bk then
    what = string.format("closest p%s (%d,%d) shell short: no count",
                         tostring(bk.id), bk.mx, bk.my)
  elseif ga.phase == "wait" and (ga.used or 0) == 0 then
    what = "blocker step: off on the decoy square"
  else
    what = "blocker step: not counting"
  end
  local R = v.steps or 0
  viz.text(ID, v.sx + 0.5, v.sy + R + 1.3, string.format("%s | steps by: %s", what, tl),
           "center", 255, 150, 255, 255)
end

-- Decoy: status header.  Above the search box: the hold (ORDER decoy, its
-- square, the time left), the step and phase, and the scan cost.  Up to
-- three lines, not one: an overlay text is cut at OVERLAY_TEXT_MAX (128)
-- bytes, in the game, in BrainTest and in a recording alike.
local function draw_status(viz, h, ga, v)
  local ID = VIZ_STATUS
  local np = #(v.P or {})
  local base = v.step0 or 0
  local lines = {}
  local left = (h.expiry and ga.now) and math.max(0, h.expiry - ga.now) or nil
  lines[1] = string.format("ORDER decoy (%d,%d) %s s left, %d pills counted (P)",
                           h.mx or -1, h.my or -1,
                           left and tostring(math.floor(left / 50)) or "?", np)
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
    lines[2] = string.format("GETAWAY step %d/%d %s", ga.used, base + #v.path, doing)
    lines[3] = string.format("P=%d tiles=%d edges=%d traces=%d %dus",
                             np, #(v.cells or {}), v.edges or 0, v.traces or 0, v.us or -1)
  else
    lines[2] = string.format("GETAWAY none (P=%d tiles=%d) -- the hold stays put",
                             np, #(v.cells or {}))
  end
  local R = v.steps or 0
  local y = v.sy - R - 0.4 - 0.45 * (#lines - 1)
  for i, s in ipairs(lines) do
    viz.text(ID, v.sx + 0.5, y + 0.45 * (i - 1), s, "center", 150, 200, 255, 255)
  end
end

function M.draw(viz, state)
  if not viz or not viz.is_on then return end
  local on_chain, on_cells = viz.is_on(VIZ_CHAIN), viz.is_on(VIZ_CELLS)
  local on_terms, on_lines = viz.is_on(VIZ_TERMS), viz.is_on(VIZ_LINES)
  local on_blk, on_status  = viz.is_on(VIZ_BLOCKER), viz.is_on(VIZ_STATUS)
  if not (on_chain or on_cells or on_terms or on_lines or on_blk or on_status) then
    return
  end
  local h = state and state.orders and state.orders.held
  local ga = h and h.decoy and h.ga
  local v = ga and ga.viz
  if not v then return end
  if on_cells  then draw_cells(viz, v) end
  if on_lines  then draw_lines(viz, v) end
  if on_chain  then draw_chain(viz, h, ga, v) end
  if on_terms  then draw_terms(viz, v) end
  if on_blk    then draw_blocker(viz, ga, v) end
  if on_status then draw_status(viz, h, ga, v) end
end

return M
