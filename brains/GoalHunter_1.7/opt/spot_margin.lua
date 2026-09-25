local bit = require('bitcompat')
-- =========================================================================
-- spot_margin.lua — blitz spot line-of-sight margin (2026-09-24)
--
-- One shared "can this BLITZ spot shoot the pill, with room to spare?" test,
-- used by the soldier's spot pick (attack.blitz_pick_from_scan and the
-- soldier's replan in plan_position), the commander's arbiter
-- (squad.blitz_spot_shot_blocked) and the blitz GO gate (attack commit_fire).
-- One implementation, so commander and soldier never disagree.
--
-- Knob C.BLITZ_SPOT_LOS_MARGIN (tiles). 0 = check off (KEEL).
--
-- THE RULE
--   A shot line runs from the tank's origin point O to an aim point A on the
--   target pill. L = |OA|. For every blocker tile near the line (a built wall
--   or half-wall, a live deployed pill of any owner, a base of any owner — the
--   same set the scan's shell test blocks on):
--     off  = distance from the line SEGMENT to the blocker's tile SQUARE
--            (its edges, not its centre)
--     d    = distance from A, measured along the line, to the line point
--            closest to the blocker
--     need = MARGIN * d / L
--   The line FAILS when off < need for any blocker. The target pill's own tile
--   is never a blocker, and neither is the origin tile (the shell test skips
--   it too).
--
-- WHY THE MARGIN TAPERS
--   A tank that stops a little off its spot shifts the line sideways, and the
--   shift shrinks to zero at the aim point (the line pivots on A). So a blocker
--   far from the pill needs a lot of room, and a blocker right beside the pill
--   needs almost none: it fails only when the line really touches it.
--   (20260924_224514 bot0: the scan tested (126.03,144.45), the tank stopped at
--   (126.75,144.63), and from there the line ran into our own pill #15 at
--   (126,142). The tile-centre line cleared it by only 0.13 tile at d=6.0.)
--
-- A SPOT PASSES when at least ONE of the five aim points (pill centre + four
-- corners, attack_shield.AIM_OFFSETS_TILE_FIRE — the set clear_aim_from_world
-- uses) passes BOTH the margin and the real shell test (aim_line_trees). The
-- margin is pure Lua maths over the few tiles around the line, so it runs
-- FIRST and a shell simulation is only paid for an aim that passed it.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local cpf    = require("cpathfinder")
local shield = require("attack_shield")
local print2 = require("print2")

local M = {}

local sqrt, floor, huge = math.sqrt, math.floor, math.huge

-- ── Pure geometry ────────────────────────────────────────────────────────

-- Liang-Barsky clip step. Returns the narrowed (t0, t1) or nil when the
-- segment misses this slab.
local function clip(p, q, t0, t1)
  if p == 0 then
    if q < 0 then return nil end
    return t0, t1
  end
  local r = q / p
  if p < 0 then
    if r > t1 then return nil end
    if r > t0 then t0 = r end
  else
    if r < t0 then return nil end
    if r < t1 then t1 = r end
  end
  return t0, t1
end

-- Squared distance from point (px,py) to the square [bx,bx+1] x [by,by+1].
local function pt_square_d2(px, py, bx, by)
  local dx = 0
  if px < bx then dx = bx - px elseif px > bx + 1 then dx = px - bx - 1 end
  local dy = 0
  if py < by then dy = by - py elseif py > by + 1 then dy = py - by - 1 end
  return dx * dx + dy * dy
end

-- seg_square_dist(ax,ay, ox,oy, bx,by) -> off, d, L
--   Segment from the AIM point A=(ax,ay) to the ORIGIN O=(ox,oy), tile coords.
--   Blocker tile square [bx,bx+1] x [by,by+1].
--   off = shortest distance from the segment to the square (0 = touches it)
--   d   = distance from A along the segment to the segment point nearest the
--         square (when the segment enters the square: the entry point
--         nearest A)
--   L   = segment length
function M.seg_square_dist(ax, ay, ox, oy, bx, by)
  local vx, vy = ox - ax, oy - ay
  local L = sqrt(vx * vx + vy * vy)
  if L < 1e-9 then
    return sqrt(pt_square_d2(ax, ay, bx, by)), 0, 0
  end
  -- Does the segment enter the square at all?
  local t0, t1 = 0, 1
  t0, t1 = clip(-vx, ax - bx, t0, t1)
  if t0 then t0, t1 = clip(vx, bx + 1 - ax, t0, t1) end
  if t0 then t0, t1 = clip(-vy, ay - by, t0, t1) end
  if t0 then t0, t1 = clip(vy, by + 1 - ay, t0, t1) end
  if t0 then return 0, t0 * L, L end
  -- No: the nearest pair is a square corner against the segment, or a segment
  -- end against the square.
  local ux, uy = vx / L, vy / L
  local best_d2, best_t = huge, 0
  for c = 1, 4 do
    local cx = (c == 2 or c == 4) and (bx + 1) or bx
    local cy = (c >= 3) and (by + 1) or by
    local t = (cx - ax) * ux + (cy - ay) * uy
    if t < 0 then t = 0 elseif t > L then t = L end
    local qx, qy = cx - (ax + ux * t), cy - (ay + uy * t)
    local d2 = qx * qx + qy * qy
    if d2 < best_d2 then best_d2, best_t = d2, t end
  end
  local da = pt_square_d2(ax, ay, bx, by)
  if da < best_d2 then best_d2, best_t = da, 0 end
  local dob = pt_square_d2(ox, oy, bx, by)
  if dob < best_d2 then best_d2, best_t = dob, L end
  return sqrt(best_d2), best_t, L
end

-- ── Blocker lookup ───────────────────────────────────────────────────────

-- Pending pill at tile (x,y): a pill that is not on the map YET but will be
-- soon -- our man (or an ally's, from its lgmd advert) is walking there to
-- build or repair it, or we carry a pill to a placement target. Filled per
-- tick by squad.update_pending_pills into world.pending_pill_at (packed
-- y*256+x). The table is nil when C.BLITZ_SPOT_PENDING_PILLS is off (KEEL),
-- so every check below is then exactly the old code.
-- (20260925_105315 bot1/bot9: p1 picked (143.73,123.27) at t=3022 across the
-- tile p9's man was walking to; the pill appeared at t=3032 and the commander
-- rejected the spot every tick after.)
function M.pending_pill_at(world, x, y)
  local pp = world and world.pending_pill_at
  return pp ~= nil and pp[y * 256 + x] ~= nil
end

-- Blocker kind at tile (x,y): "wall", "base", "pill", "pill_pending" or false.
-- Reads terrain through U.ttype_peek (a pure read): U.ttype is a change
-- DETECTOR and priming new tiles from here would move threat recomputes.
local function world_blocker(world, x, y)
  if x < 0 or x > 255 or y < 0 or y > 255 then return false end
  local tt = U.ttype_peek(x, y)
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return "wall" end
  local key = y * 256 + x
  local be = world and world.base_at and world.base_at[key]
  if be and be.base then return "base" end
  local plist = world and world.pill_at and world.pill_at[key]
  if plist then
    local pills = world.pills
    for _, e in ipairs(plist) do
      -- Live-table check, same as aim_line_trees: only a DEPLOYED, alive pill
      -- actually on this tile counts.
      local p = (e.id and pills and pills[e.id]) or e.pill
      if p and not p.in_tank and (p.health or 0) > 0
         and (p.mx == nil or (p.mx == x and p.my == y)) then
        return "pill"
      end
    end
  end
  if M.pending_pill_at(world, x, y) then return "pill_pending" end
  return false
end

-- A lookup context for one target pill: memoizes the per-tile blocker kind so
-- a caller that tests many spots around the same pill (the soldier's pick
-- walks every scan spot) reads each tile once. `lookup` (optional) replaces
-- the world read — the unit tests use it.
function M.new_ctx(world, pmx, pmy, lookup)
  return { world = world, pmx = pmx, pmy = pmy, memo = {}, lookup = lookup }
end

local function blocker_at(ctx, x, y)
  local key = y * 256 + x
  local v = ctx.memo[key]
  if v == nil then
    if ctx.lookup then v = ctx.lookup(x, y) or false
    else v = world_blocker(ctx.world, x, y) end
    ctx.memo[key] = v
  end
  return v
end

-- line_margin(ctx, ofx,ofy, afx,afy, margin) -> ok, fail
--   Origin (ofx,ofy) and aim (afx,afy) in float tile coords.
--   ok   = true when no blocker is closer to the line than need = margin*d/L.
--   fail = when not ok, the WORST blocker (largest need - off):
--          { bx, by, kind, off, d, need, L }
--   Only tiles inside the segment's bounding box grown by `margin` (+1 for
--   the tile size) can fail — need never exceeds margin — so only those are
--   read.
function M.line_margin(ctx, ofx, ofy, afx, afy, margin)
  if not margin or margin <= 0 then return true end
  local pmx, pmy = ctx.pmx, ctx.pmy
  local omx, omy = floor(ofx), floor(ofy)
  local x0 = floor((ofx < afx and ofx or afx) - margin) - 1
  local x1 = floor((ofx > afx and ofx or afx) + margin)
  local y0 = floor((ofy < afy and ofy or afy) - margin) - 1
  local y1 = floor((ofy > afy and ofy or afy) + margin)
  local worst, worst_short = nil, 0
  for by = y0, y1 do
    for bx = x0, x1 do
      if not (bx == pmx and by == pmy) and not (bx == omx and by == omy) then
        local kind = blocker_at(ctx, bx, by)
        if kind then
          local off, d, L = M.seg_square_dist(afx, afy, ofx, ofy, bx, by)
          local need = (L > 0) and (margin * d / L) or 0
          if off < need then
            local short = need - off
            if not worst or short > worst_short then
              worst_short = short
              worst = { bx = bx, by = by, kind = kind, off = off, d = d, need = need, L = L }
            end
          end
        end
      end
    end
  end
  if worst then return false, worst end
  return true
end

-- ── Shell test ───────────────────────────────────────────────────────────

-- One aim point, one shell simulation. Returns the forest-tile count on the
-- line (0 = perfectly clear) plus the aim point in world units, or nil when the
-- line is BLOCKED / never reaches the pill. Moved here unchanged from
-- attack.lua (attack.lua aliases it) so squad.lua's arbiter can share it.
-- `wallset` (optional, packed my*256+mx) adds a commander's broadcast shield
-- walls as blockers; nil = exactly the old behaviour.
function M.aim_line_trees(ox, oy, omx, omy, pmx, pmy, world, i, wallset)
  local off = shield.AIM_OFFSETS_TILE_FIRE[i]
  local awx = bit.lshift(pmx, 8) + math.floor(off[1] * 256)
  local awy = bit.lshift(pmy, 8) + math.floor(off[2] * 256)
  local tiles = cpf.simulate_shot(ox, oy, awx, awy, cpf.SHOT_TANK, 0)
  if not tiles then return nil end
  local pill_at = world and world.pill_at
  local base_at = world and world.base_at
  local pills   = world and world.pills
  local pending = world and world.pending_pill_at
  local blocked, reached, trees = false, false, 0
  for ti = 1, #tiles do
    local t = tiles[ti]
    if t.mx == pmx and t.my == pmy then reached = true; break end
    -- Our own tile never obstructs our own shot.
    if t.mx ~= omx or t.my ~= omy then
      local tt = U.ttype(t.mx, t.my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        blocked = true; break
      elseif tt == C.T_FOREST then
        trees = trees + 1
      end
      local key = t.my * 256 + t.mx
      if wallset and wallset[key] then blocked = true; break end
      local be = base_at and base_at[key]
      if be and be.base then blocked = true; break end
      if pending and pending[key] then blocked = true; break end
      local plist = pill_at and pill_at[key]
      if plist then
        for _, e in ipairs(plist) do
          -- Live-table check (see shot_path_obstacle_count): a pill_at entry
          -- can point at a stale copy of a pill an ally has since driven
          -- over, and a phantom hp-15 blocker would throw away good spots.
          local p = (e.id and pills and pills[e.id]) or e.pill
          if p and not p.in_tank and (p.health or 0) > 0
             and (p.mx == nil or (p.mx == t.mx and p.my == t.my)) then
            blocked = true; break
          end
        end
        if blocked then break end
      end
    end
  end
  if reached and not blocked then return trees, awx, awy end
  return nil
end

-- ── Spot test: margin + shell, best aim ──────────────────────────────────

-- clear_aim_margin(ox, oy, pmx, pmy, world, margin, opts)
--   (ox,oy) origin in WORLD units; pill tile (pmx,pmy).
--   opts (all optional):
--     ctx          a new_ctx for this pill (reused across spots)
--     prefer_idx   try this aim first and take it if it passes
--     trusted_idx  an aim the caller already shell-tested from THIS exact
--                  origin (the scan): skip its simulation
--     trusted_trees  that aim's tree count
--     wallset      commander shield walls for the shell test
--     site, tick   for the reject log line
--     spot_fx/fy   spot float coords to print (defaults to the origin)
--     quiet        no reject line (a probe, not a real spot test)
--   Returns idx, awx, awy, trees when an aim passes both tests, else nil.
--   Aim preference matches clear_aim_from_world: prefer_idx if it passes, else
--   a clear centre, else a clear corner, else the fewest trees.
--   Prints one BLITZ_SPOT_MARGIN_REJECT line when the margin is what killed
--   the spot, with every factor so the verdict can be checked by hand.
function M.clear_aim_margin(ox, oy, pmx, pmy, world, margin, opts)
  opts = opts or {}
  local ctx = opts.ctx or M.new_ctx(world, pmx, pmy)
  local ofx, ofy = ox / 256.0, oy / 256.0
  local omx, omy = bit.rshift(ox, 8), bit.rshift(oy, 8)
  local offs = shield.AIM_OFFSETS_TILE_FIRE
  local prefer = opts.prefer_idx
  local n_margin, n_shell = 0, 0
  local first_fail, first_fail_i = nil, nil
  local best_i, best_trees, best_wx, best_wy = nil, nil, nil, nil
  -- Test order: prefer_idx first, then 1..5 (centre first).
  for k = 0, 5 do
    local i = (k == 0) and prefer or k
    if i and offs[i] and not (k > 0 and i == prefer) then
      local awx = bit.lshift(pmx, 8) + floor(offs[i][1] * 256)
      local awy = bit.lshift(pmy, 8) + floor(offs[i][2] * 256)
      local ok, fail = M.line_margin(ctx, ofx, ofy, awx / 256.0, awy / 256.0, margin)
      if not ok then
        n_margin = n_margin + 1
        if not first_fail then first_fail, first_fail_i = fail, i end
      else
        local trees
        if opts.trusted_idx and i == opts.trusted_idx then
          trees = opts.trusted_trees or 0
        else
          trees = M.aim_line_trees(ox, oy, omx, omy, pmx, pmy, world, i, opts.wallset)
        end
        if not trees then
          n_shell = n_shell + 1
        else
          if k == 0 then return i, awx, awy, trees end          -- preferred aim holds
          if i == 1 and trees == 0 then return 1, awx, awy, 0 end -- clear centre
          if best_trees == nil or trees < best_trees then
            best_i, best_trees, best_wx, best_wy = i, trees, awx, awy
            if trees == 0 then break end                         -- clear corner
          end
        end
      end
    end
  end
  if best_i then return best_i, best_wx, best_wy, best_trees end
  if first_fail and not opts.quiet then
    local f = first_fail
    local off_i = offs[first_fail_i]
  end
  return nil
end

return M
