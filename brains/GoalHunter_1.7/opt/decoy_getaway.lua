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
-- ONE STEP:
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

-- goals.lua is big and loads orders.lua lazily; ask for it when it is needed.
local function goals() return require("goals") end

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
    ga = { phase = "wait", hits = 0, arm = arm, used = 0, idx = 1 }
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
    if ga.path and ga.path[ga.idx] and ga.hits >= (C.DECOY_GETAWAY_HITS or 1) then
      ga.phase = "move"
      local t = ga.path[ga.idx]
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
      else
        ga.phase = "wait"
        local n = path[ga.idx]
      end
    elseif not M.passable(world, t.mx, t.my, info.inboat) then
      -- The next square is no longer drivable: a fresh chain from here with
      -- the steps that are left (and on to its first square: the hit that
      -- started this step is spent), or park where the tank is.
      local left = max_steps - ga.used
      if not M.rescan(world, info, h, tx, ty, left, now, "next_blocked") then
        ga.phase = "done"
        park_on(ga, tx, ty, arm)
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
    else
    end
  end
  -- The search bound: ring `steps` around the scan's start square.
  local R = v.steps or 0
  if R > 0 then
  end
  -- The pills in P.
  for _, tp in ipairs(v.P or {}) do
  end
  local LW = C.DECOY_GETAWAY_LAST_WEIGHT or 2.0
  local px, py = v.sx, v.sy
  local base = v.step0 or 0
  for i, c in ipairs(v.path or {}) do
    local drove = i < (ga.idx or 1)
    px, py = c.mx, c.my
    local head = string.format("#%d %s%s", base + i, safety_txt(c, np),
                               i == #v.path and string.format(" x%g (last)", LW) or "")
    local lines = { head }
    local nt = #(c.terms or {})
    for j, tm in ipairs(c.terms or {}) do
      local tt = term_txt(tm)
      lines[#lines + 1] = tt
      if tm.sx then
      end
    end
    -- The closeness term and the tile value the chain score adds up.
    for j, tx in ipairs({ prox_txt(c), tile_txt(c) }) do
      lines[#lines + 1] = tx
    end
    for _, tp in ipairs(v.P or {}) do
      local open = false
      for _, tm in ipairs(c.terms or {}) do
        if tm.id == tp.id and tm.b == 0 then open = true end
      end
      if open then
      else
      end
    end
  end
  local hdr
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
    hdr = string.format("GETAWAY step %d/%d %s: score = %s = %.3f  (P=%d tiles=%d edges=%d traces=%d %dus)",
                        ga.used, base + #v.path, doing, score_terms(v.path),
                        v.score or 0, np, #(v.cells or {}), v.edges or 0,
                        v.traces or 0, v.us or -1)
  else
    hdr = string.format("GETAWAY none (P=%d tiles=%d) -- the hold stays put",
                        np, #(v.cells or {}))
  end
end

return M
