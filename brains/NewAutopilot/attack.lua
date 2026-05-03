-- =========================================================================
-- NewAutopilot/attack.lua — pill attack position planning + substate machines
-- =========================================================================

local C      = require("constants")
local TAG    = "[" .. C.BRAIN_NAME .. "]"
local U      = require("util")
local PF     = require("pathfinder")
local cpf    = require("cpathfinder")
local log    = require("logger")
local threat = require("threat")
local shot_tracker = require("shot_tracker")
local shield = require("attack_shield")
local viz    = require("viz")

local smart_cost = cpf.smart_cost
local KIND_PILL  = cpf.KIND_PILL

local print2 = require("print2")

local M = {}

-- "Effectively stopped" gate for the substate transitions in approach
-- and in_range_position. Returns true if EITHER the reported speed is
-- at/below speed_tol OR the tank's wu position hasn't changed for the
-- last `still_ticks` ticks. Catches the corner case where info.speed
-- reads a low value (e.g. 4) but the tank is no longer making any
-- actual progress (terrain friction, wedged against a tile boundary,
-- etc.). Per-key trackers live on `state.attack_motion` so they
-- survive the per-tick goal-table swap without polluting the goal.
-- Pick which side to swerve toward based on perpendicular cover.
-- Samples 2 tiles perpendicular to the tank->pill line, walks each
-- back to the pill counting cover (forest/walls/friendly pills) and
-- subtracting hazards (water/swamp/rubble/enemy bases). The side with
-- more cover wins. Sets goal._best_swerve_dir (1=LEFT-sample,
-- -1=RIGHT-sample) and goal._swerve_viz for the HUD overlay.
-- Called from BOTH the legacy aim->swerve path AND PPT shoot_pill->
-- swerve so PPT doesn't fall back to a random direction.
local function compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
  local dx = pmx - tmx
  local dy = pmy - tmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len <= 0.01 then return end
  local ux, uy = dx / len, dy / len
  local tcx, tcy = tmx + 0.5, tmy + 0.5
  local pcx, pcy = pmx + 0.5, pmy + 0.5
  local lfx = tcx + (-uy) * 2
  local lfy = tcy + ux * 2
  local rfx = tcx + uy * 2
  local rfy = tcy + (-ux) * 2

  local fire_r2 = C.PILL_FIRE_RANGE * C.PILL_FIRE_RANGE
  local function in_fire_range(bx, by)
    local ddx = bx - pmx
    local ddy = by - pmy
    return ddx * ddx + ddy * ddy <= fire_r2
  end
  local function count_cover(fx0, fy0)
    local n = 0
    U.line_walk(fx0, fy0, pcx, pcy, function(bx, by)
      if not U.in_map(bx, by) then return end
      local tt = U.ttype(bx, by)
      -- pill_at[k] is a list of {id=,pill=} records (see world.lua:32) —
      -- old code did world.pills[<list>] which is always nil, so the
      -- friendly_pill cover bonus never fired.
      local plist = world.pill_at[by * 256 + bx]
      local friendly_pill = false
      if plist then
        for _, e in ipairs(plist) do
          if e.pill and e.pill.owner == "friendly" then
            friendly_pill = true; break
          end
        end
      end
      local base_entry = world.base_at[by * 256 + bx]
      local enemy_base = base_entry and base_entry.base
                         and base_entry.base.owner == "hostile"
      if in_fire_range(bx, by) then
        if tt == C.T_FOREST or tt == C.T_BUILDING or tt == C.T_HALFBUILD then
          n = n + 1
        end
        if friendly_pill then n = n + 3 end
        if enemy_base then n = n - 10 end
      else
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD
           or tt == C.T_RIVER  or tt == C.T_DEEPSEA
           or tt == C.T_SWAMP  or tt == C.T_RUBBLE then
          n = n - 10
        end
        if friendly_pill then n = n - 10 end
        if enemy_base then n = n - 10 end
      end
    end)
    return n
  end

  local left_cover  = count_cover(lfx, lfy)
  local right_cover = count_cover(rfx, rfy)
  goal._best_swerve_dir = left_cover >= right_cover and 1 or -1
  goal._swerve_viz = {
    lfx = lfx, lfy = lfy, left_cover = left_cover,
    rfx = rfx, rfy = rfy, right_cover = right_cover,
    chosen = goal._best_swerve_dir,
    pcx = pcx, pcy = pcy,
  }
  print(string.format(TAG .. " ATTACK: swerve calc L(%.2f,%.2f)=%d R(%.2f,%.2f)=%d -> %s",
        lfx, lfy, left_cover, rfx, rfy, right_cover,
        goal._best_swerve_dir == 1 and "LEFT" or "RIGHT"))
end

-- Clear the active goal, idle the pathfinder, and wipe ALL transient
-- attack-side state that downstream code shouldn't see after we
-- abandon. Centralises the bug class where one site clears 25
-- fields by hand and another site clears just `kind`, leaving stale
-- _shield_scan / _aim_locked / etc. to leak into the next goal that
-- happens to inherit the same goal table.
--
-- Mutates state.goal IN PLACE (preserves table identity) so callers
-- that pre-cached `local goal = state.goal` continue to see "none"
-- on subsequent reads. Only the core identity fields are kept so
-- the new goal selector can re-derive everything else cleanly.
local CORE_GOAL_FIELDS = {
  kind = true, mx = true, my = true, wx = true, wy = true,
  target_id = true,
}
local function clear_attack_goal(state, reason)
  -- Snapshot what we're killing BEFORE we wipe it, so the overlay can
  -- show "attack_pill/in_range_aim_finetune cleared at t=N because X".
  local kind_was = state.goal and state.goal.kind or nil
  local sub_was  = state.goal and state.goal.substate or nil
  local mx_was   = state.goal and state.goal.mx or nil
  local my_was   = state.goal and state.goal.my or nil
  if state.goal then
    for k in pairs(state.goal) do
      if not CORE_GOAL_FIELDS[k] then state.goal[k] = nil end
    end
    state.goal.kind = "none"
  else
    state.goal = { kind = "none" }
  end
  if state.pf then state.pf.status = "idle" end
  -- Auto-derive a caller location if the call site didn't pass a reason,
  -- so the overlay still tells you which line cleared the goal.
  local r = reason
  if not r then
    local info = debug.getinfo(2, "Sl")
    if info then r = string.format("(no reason) called from %s:%d",
                                   info.short_src or "?", info.currentline or 0)
    else r = "(no reason)" end
  end
  state._last_attack_clear = {
    tick = state.tick or 0,
    reason = r,
    kind_was = kind_was, sub_was = sub_was,
    mx_was = mx_was, my_was = my_was,
  }
  if reason then
    print(string.format("[clear_attack_goal] %s", reason))
  end
end
M.clear_attack_goal = clear_attack_goal

-- STILL_POS_TOL: max world-unit drift over the still-window that
-- still counts as "stopped". Without this, a 1-wu-per-tick jitter
-- (common on tree/swamp tiles where info.speed lies about actual
-- ground motion) keeps resetting `since` and effectively_stopped
-- never returns true even when the tank is visually frozen.
-- 4 wu = ¼ game-pixel — well below anything that matters for aim.
local STILL_POS_TOL = 4
local function effectively_stopped(state, info, now, speed_tol, still_ticks, key)
  if info.speed <= speed_tol then return true end
  state.attack_motion = state.attack_motion or {}
  local sub = state.attack_motion[key] or {}
  state.attack_motion[key] = sub
  -- Anchor on first call OR when drift exceeds tolerance.
  if not sub.wx then
    sub.wx, sub.wy, sub.since = info.tankx, info.tanky, now
    return false
  end
  local dx = info.tankx - sub.wx
  local dy = info.tanky - sub.wy
  if dx * dx + dy * dy > STILL_POS_TOL * STILL_POS_TOL then
    sub.wx, sub.wy, sub.since = info.tankx, info.tanky, now
    return false
  end
  return (now - sub.since) >= still_ticks
end

-- Attack pill constants (from constants.lua)
-- ATTACK_PILL_STANDOFF: distance to stand from pill
-- ATTACK_PILL_RANGE: max distance to start shooting
-- ATTACK_CURVE_AFTER_HITS: hits before curving away
-- ATTACK_CURVE_TICKS: duration of curve-away maneuver

-- Find a pill at (mx, my) via spatial index. Returns the pill entry or nil.
-- Emit the candidate-spot overlay (LOS box, danger box, maneuver
-- ellipse, maneuver tiles, score label) for a list of spots around a
-- pill. Mirrors the inline draw block at the bottom of
-- update_attack_substate so the same visual style is used for both
-- the goal pill (where show_all gates non-chosen) and the per-pill
-- emission from the pool-6 evaluator (where every spot draws).
-- viz_id picks which V-dialog row gates the emission.
-- mode (optional): "all" | "bucket" | "winner". Default "all" draws
-- every spot. "bucket" filters to s.in_bucket; "winner" filters to
-- s.deg == chosen_deg.
-- alpha_scale (optional, default 1.0): multiplier on every alpha so
-- the goal-pill emission can render bolder than the pool-6 candidates
-- without duplicating the renderer.
function M.draw_pill_eval_spots(spots, pmx, pmy, viz_id, mode, chosen_deg, alpha_scale)
  if not spots then return end
  mode = mode or "all"
  alpha_scale = alpha_scale or 1.0
  local safe_r = C.ATTACK_SAFE_RADIUS
  local function a(v)
    local x = math.floor(v * alpha_scale + 0.5)
    if x > 255 then x = 255 elseif x < 0 then x = 0 end
    return x
  end
  for _, s in ipairs(spots) do
    if s.cx
       and (mode ~= "bucket" or s.in_bucket)
       and (mode ~= "winner" or s.deg == chosen_deg) then
      -- Inner box: green=LOS, red=no LOS
      local cr, cg = s.has_los and 0 or 200, s.has_los and 200 or 0
      viz.rect(viz_id, s.cx - 0.15, s.cy - 0.15,
                       s.cx + 0.15, s.cy + 0.15, cr, cg, 0, a(200))
      if s.has_los then
        local sr, sg = (s.total_score or 999) < 10 and 0 or 255,
                       (s.total_score or 999) < 10 and 200 or 165
        viz.rect(viz_id, s.cx - 0.25, s.cy - 0.25,
                         s.cx + 0.25, s.cy + 0.25, sr, sg, 0, a(120))
        if s.maneuver_tiles then
          for _, t in ipairs(s.maneuver_tiles) do
            viz.rect(viz_id, t.x, t.y, t.x + 1, t.y + 1, 255, 255, 0, a(80))
          end
        end
        -- Maneuver ellipse outline. Radii match score_attack_spot
        -- (r_long = ATTACK_SAFE_RADIUS, r_short = that / 3) so the
        -- drawn shape tracks the actual scoring geometry.
        do
          local edx = pmx + 0.5 - s.cx
          local edy = pmy + 0.5 - s.cy
          local elen = math.sqrt(edx * edx + edy * edy)
          if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
          local ux, uy = edx / elen, edy / elen
          local vx, vy = -uy, ux
          local rl, rs = safe_r, safe_r / 3.0
          local segs = 24
          local px, py
          for i = 0, segs do
            local ang = (i / segs) * 2 * math.pi
            local eu = math.cos(ang) * rl
            local ev = math.sin(ang) * rs
            local nx = s.cx + eu * ux + ev * vx
            local ny = s.cy + eu * uy + ev * vy
            if px then
              local er, eg = (s.total_score or 999) < 10 and 0 or 255,
                             (s.total_score or 999) < 10 and 200 or 165
              viz.line(viz_id, px, py, nx, ny, er, eg, 0, a(80))
            end
            px, py = nx, ny
          end
        end
        -- Score label. Pink for everyone; bucket members get a dark
        -- purple overdraw on top so the contenders stand out from the
        -- losers at a glance.
        if (s.total_score or 999) < 900 then
          local label = string.format("A%.0f+B%.0f+D%.0f+E%.0f=%.0f",
            s.score_a or 0, s.score_b or 0, s.score_d or 0,
            s.score_e or 0, s.total_score or 0)
          viz.text(viz_id, s.cx - 1, s.cy - 0.5, label,
                   "topleft", 255, 0, 255, a(200))
          if s.in_bucket then
            viz.text(viz_id, s.cx - 1, s.cy - 0.5, label,
                     "topleft", 60, 0, 100, 255)
            local dij_label = string.format("dij=%.0f", s._dij or 0)
            viz.text(viz_id, s.cx - 1, s.cy - 0.1, dij_label,
                     "topleft", 60, 0, 100, 255, 0.6)
          end
        end
      end
    end
  end
end

function M.find_pill_at(world, mx, my)
  local entries = world.pill_at[my * 256 + mx]
  if not entries then return nil end
  return entries[1] and entries[1].pill or nil
end

-- =========================================================================
-- Pill attack position planner
-- =========================================================================

-- Check every intermediate tile between (x0,y0) and (x1,y1) is water.
-- When true a boat shell travels over them and strikes the first land (the pill).
local water_corridor_to = U.water_corridor_to

-- Simulate the tank shot from the chosen standoff to the target pill and
-- return a reason string if the path is blocked, or nil if it is clear.
-- Blocking conditions:
--   * any pill (friendly or enemy) other than the target on the path
--   * more than one wall tile on the outgoing path
-- Walls are allowed up to a count of 1 (one shield wall sitting on the
-- path edge is acceptable; two means something went badly wrong with the
-- setup).  Called every tick in the pre-fire substates so a newly-placed
-- strategic pill that landed in the corridor triggers an immediate replan.
local function standoff_shot_obstacle(goal, pill, world)
  if not (goal.standoff_fx and goal.standoff_fy) then return nil end
  local pmx, pmy = pill.mx, pill.my
  -- Aim: winning PPT corner if available, otherwise pill center.
  local target_wx, target_wy
  if goal._shield_scan and goal._shield_scan.best then
    local w   = goal._shield_scan.best
    local off = shield.AIM_OFFSETS_TILE_FIRE[w.best_aim_idx or 1]
                or shield.AIM_OFFSETS_TILE_FIRE[1]
    target_wx = (pmx << 8) + math.floor(off[1] * 256)
    target_wy = (pmy << 8) + math.floor(off[2] * 256)
  else
    target_wx = (pmx << 8) | 128
    target_wy = (pmy << 8) | 128
  end
  local spot_wx  = math.floor(goal.standoff_fx * 256 + 0.5)
  local spot_wy  = math.floor(goal.standoff_fy * 256 + 0.5)
  local origin_mx = spot_wx >> 8
  local origin_my = spot_wy >> 8
  local tiles = cpf.simulate_shot(spot_wx, spot_wy,
                                  target_wx, target_wy,
                                  cpf.SHOT_TANK, 0)
  if not tiles then return nil end
  local wall_n = 0
  for _, t in ipairs(tiles) do
    if t.mx == pmx and t.my == pmy then break end  -- reached target pill tile
    if t.mx ~= origin_mx or t.my ~= origin_my then
      -- Pill in path (other than target)?
      local plist = world.pill_at and world.pill_at[t.my * 256 + t.mx]
      if plist then
        for _, e in ipairs(plist) do
          if e.pill then
            return string.format("pill at (%d,%d) in shot path", t.mx, t.my)
          end
        end
      end
      -- Wall count gate.
      local tt = U.ttype(t.mx, t.my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        wall_n = wall_n + 1
        if wall_n > 1 then
          return string.format("2+ walls in shot path (at (%d,%d))", t.mx, t.my)
        end
      end
    end
  end
  return nil
end

-- Count forest tiles on the Bresenham line from (x0,y0) to (x1,y1), excluding
-- endpoints.  Each one would be destroyed by a shell fired along this path —
-- a resource cost since the brain farms trees for road building and pill repair.
-- Counts forest tiles on the line from (x0,y0) to (x1,y1).
-- If `do_viz` is true, draws a white outline on each forest tile considered.
local function forest_tiles_on_path(x0, y0, x1, y1, do_viz)
  local count = 0
  -- Use precise sub-pixel walk: tile centers at +0.5
  U.line_walk(x0 + 0.5, y0 + 0.5, x1 + 0.5, y1 + 0.5, function(cx, cy)
    if U.ttype(cx, cy) == C.T_FOREST then
      count = count + 1
      if do_viz then
        viz.rect("forest_path_tiles", cx,        cy,        cx + 1,    cy + 1,    255, 255, 255, 220)
        viz.rect("forest_path_tiles", cx + 0.05, cy + 0.05, cx + 0.95, cy + 0.95, 255, 255, 255, 220)
      end
    end
  end)
  return count
end

-- Score a single candidate standoff tile (cx, cy) for attacking `pill`.
-- Returns a score (lower is better) or math.huge if the position is unusable.
-- When orbit_radius is non-nil, also penalizes slow/hazardous terrain on the
-- orbit arc (for circle-strafe attacks like bpc).
local function score_standoff(world, cx, cy, pill, info, orbit_radius)
  if not U.in_map(cx, cy) then return math.huge end

  -- Must have clear line of sight to the pill (no walls)
  if PF.wall_hp_between(cx, cy, pill.mx, pill.my) > 0 then return math.huge end

  -- Forest tiles on the shot path are destroyed by each shell fired.
  -- Prefer angles that don't waste trees the brain may want to farm later.
  local trees_destroyed = forest_tiles_on_path(cx, cy, pill.mx, pill.my)

  local tt        = U.ttype(cx, cy)
  local land_cost = C.TERRAIN_COST_LAND[tt] or 9999

  -- Walls/impassable non-water: completely unusable
  if land_cost >= 9999 and not U.is_water(tt) then
    return math.huge
  end

  -- Water standoff: only valid when every tile between here and the pill is also
  -- water, meaning the shell clears the water and strikes the pill at the edge.
  -- Otherwise the shell hits the first land square it encounters, not the pill —
  -- making this position completely useless for the attack.
  if U.is_water(tt) then
    if not water_corridor_to(cx, cy, pill.mx, pill.my) then
      return math.huge
    end
  end

  -- Water at standoff (edge case — pill is right at water boundary):
  -- penalise heavily because the boat can be destroyed by return fire and
  -- the tank ends up stranded in open water.
  local water_pen = 0
  if tt == C.T_RIVER   then water_pen = 150 end
  if tt == C.T_DEEPSEA then water_pen = 300 end

  -- Pushback: terrain in the direction the tank gets shoved when hit.
  -- Any terrain that slows or blocks movement is dangerous: buildings trap you
  -- while the pill keeps firing, slow terrain (swamp/rubble/crater) leaves you
  -- unable to manoeuvre away, water kills you outright.
  -- Penalty per tile type — higher = harder to escape while taking damage.
  local PUSH_PEN = {
    [C.T_DEEPSEA]   = 400,  -- instant death without boat
    [C.T_RIVER]     = 200,  -- very slow without boat, easily killed
    [C.T_BUILDING]  = 350,  -- impassable: completely stuck while being shot
    [C.T_HALFBUILD] = 350,  -- impassable: same
    [C.T_SWAMP]     = 100,  -- speed 4 — sitting duck
    [C.T_RUBBLE]    = 100,  -- speed 4 — sitting duck
    [C.T_CRATER]    = 100,  -- speed 4 — sitting duck
    [C.T_FOREST]    = 20,   -- slightly slow but provides cover
    -- ROAD/GRASS/REFBASE/BOAT = 0 (fast escape)
  }
  local pdx_raw = cx - pill.mx
  local pdy_raw = cy - pill.my
  local plen    = math.max(1, math.sqrt(pdx_raw * pdx_raw + pdy_raw * pdy_raw))
  local push_dx = pdx_raw / plen
  local push_dy = pdy_raw / plen
  local pushback_pen = 0
  for step = 1, 5 do
    local bx = U.mclamp(math.floor(cx + push_dx * step + 0.5))
    local by = U.mclamp(math.floor(cy + push_dy * step + 0.5))
    local bt = U.ttype(bx, by)
    local pen = PUSH_PEN[bt] or 0
    if pen > 0 then
      -- Closer steps hurt more: weight 1.0 / 0.7 / 0.5 / 0.35 / 0.2
      local weight = math.max(0.2, 1.0 - (step - 1) * 0.2)
      pushback_pen = pushback_pen + pen * weight
    end
  end
  -- Proximity scan: hazardous terrain within radius 4 adds penalty regardless
  -- of exact pushback direction.  Multiple hits from the pill can push the tank
  -- sideways, the boat gets dropped on the first land tile (leaving it behind),
  -- and even a single recoil can reach water 3 tiles away.
  -- Weight falls off with Chebyshev distance so adjacent tiles hurt most.
  for dy = -4, 4 do
    for dx = -4, 4 do
      local d = math.max(math.abs(dx), math.abs(dy))  -- Chebyshev distance
      if d >= 1 then
        local nx, ny = U.mclamp(cx + dx), U.mclamp(cy + dy)
        local npen = PUSH_PEN[U.ttype(nx, ny)] or 0
        if npen > 0 then
          -- weight: 0.5 at d=1, 0.35 at d=2, 0.2 at d=3, 0.1 at d=4
          local w = math.max(0.1, 0.65 - d * 0.15)
          pushback_pen = pushback_pen + npen * w
        end
      end
    end
  end

  -- Crossfire from all OTHER hostile/neutral pills in range of this tile
  -- (the target pill itself is excluded — we expect to be shot at by it).
  -- Weighted heavily (×8): taking fire from a second pill while engaged
  -- with the target is devastating and worth a significant detour to avoid.
  local crossfire = 0
  for _, pm in pairs(world.pills) do
    if (pm.mx ~= pill.mx or pm.my ~= pill.my)
       and (pm.owner == "hostile" or pm.owner == "neutral")
       and pm.health > 0 then
      local d = U.mdist(cx, cy, pm.mx, pm.my)
      if d <= C.PILL_RANGE_MAP then
        local proximity = 1.0 - d / (C.PILL_RANGE_MAP + 1)
        crossfire = crossfire + (C.PILL_DANGER_BASE
                    + C.PILL_DANGER_ANGER * (pm.anger or 0)) * proximity * 8
      end
    end
  end

  -- Approach cost from the tank's current position.
  -- Use KIND_NORMAL Dijkstra (full danger) — we're navigating to a firing
  -- position, not through the pill, so full danger applies.
  -- Falls back to straight-line estimate if Dijkstra hasn't reached this tile.
  local tmx    = info.tankx >> 8
  local tmy    = info.tanky >> 8
  local ammo   = (info.shells or 0) + (info.mines or 0)
  local approach = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, cx, cy, info.inboat and 1 or 0)
  if approach >= 1e29 then
    approach = cpf.estimate_cost(tmx, tmy, cx, cy, info.inboat and 1 or 0)
  end

  -- Approach exposure: penalise routes that cross slow terrain within the
  -- target pill's firing range.  On swamp/rubble/crater the tank moves at
  -- speed 4, too slow to dodge pill shots — each such tile means eating
  -- unavoidable damage on the way in.  Fast terrain (grass/road) is fine:
  -- the tank can outrun pill aim at range.
  local SLOW_IN_RANGE = {
    [C.T_SWAMP]  = true,
    [C.T_RUBBLE] = true,
    [C.T_CRATER] = true,
  }
  local approach_exposure = 0
  local adist = U.mdist(tmx, tmy, cx, cy)
  if adist > 0 then
    local asteps = math.min(adist, 30)
    for i = 1, asteps do
      local t  = i / asteps
      local ax = U.mclamp(math.floor(tmx + (cx - tmx) * t + 0.5))
      local ay = U.mclamp(math.floor(tmy + (cy - tmy) * t + 0.5))
      if SLOW_IN_RANGE[U.ttype(ax, ay)]
         and U.mdist(ax, ay, pill.mx, pill.my) <= C.PILL_RANGE_MAP then
        approach_exposure = approach_exposure + C.APPROACH_SLOW_IN_RANGE_PEN
      end
    end
  end

  -- Escape cost: always in land mode — the tank will be on foot when it needs
  -- to escape (boat is dropped on first land tile when approaching standoff).
  local escape_cost = 0
  local best_escape = math.huge
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" or b.owner == "neutral" then
      local ec = cpf.estimate_cost(cx, cy, b.mx, b.my, 0)
      if ec < best_escape then best_escape = ec end
    end
  end
  if best_escape < math.huge then escape_cost = best_escape * 0.2 end

  -- Each forest tile on the shot path costs ~80: meaningful enough to prefer a
  -- clear angle, but less than the water/pushback penalties so it doesn't block
  -- the only viable position on an enclosed map.
  local tree_pen = trees_destroyed * 80

  -- Orbit path penalty: for circle-strafe attacks (bpc), check the terrain the
  -- tank will drive through while orbiting.  Sample 8 points on the orbit arc
  -- (±90° from the candidate's angle around the pill) and penalize slow or
  -- hazardous tiles.  This ensures the planner picks a starting position where
  -- the tank can orbit smoothly on fast terrain.
  local orbit_pen = 0
  if orbit_radius then
    local base_angle = math.atan(cx - pill.mx, -(cy - pill.my))
    local ORBIT_PEN = {
      [C.T_DEEPSEA]   = 500,
      [C.T_RIVER]     = 250,
      [C.T_BUILDING]  = 400,
      [C.T_HALFBUILD] = 400,
      [C.T_SWAMP]     = 120,
      [C.T_RUBBLE]    = 120,
      [C.T_CRATER]    = 120,
      [C.T_FOREST]    = 15,
    }
    -- Sample a half-circle in each direction (the tank will orbit through these)
    for step = -4, 4 do
      if step ~= 0 then
        local a = base_angle + step * (math.pi / 4)  -- 45° increments, ±180°
        local ox = U.mclamp(math.floor(pill.mx + math.sin(a) * orbit_radius + 0.5))
        local oy = U.mclamp(math.floor(pill.my - math.cos(a) * orbit_radius + 0.5))
        local ott = U.ttype(ox, oy)
        local open = ORBIT_PEN[ott] or 0
        if open > 0 then
          -- Closer arc steps (±45°, ±90°) matter more than far ones (±135°, ±180°)
          local weight = math.max(0.3, 1.0 - math.abs(step) * 0.15)
          orbit_pen = orbit_pen + open * weight
        end
      end
    end
  end

  -- Threat map penalty: use the unified threat grid (pills + enemy tanks) to
  -- penalise standoff positions in high-threat areas.  The crossfire term above
  -- only considers pills visible in world.pills; the threat grid also captures
  -- tank presence zones.  Scale by 2× so it meaningfully influences the score
  -- without dominating terrain/pushback penalties.
  local threat_pen = threat.at(cx, cy) * 2

  -- Influence bias: prefer standing on the friendly side of the pill so a
  -- retreat after the exchange lands in our territory, not deep in the
  -- enemy's. Friendly influence is positive, hostile is negative — negating
  -- converts to a penalty (hostile tile bad, friendly tile bonus).
  local influence_pen = -(cpf.influence_at(cx, cy) or 0)
                        * C.ATTACK_STANDOFF_INFLUENCE_WEIGHT

  -- Friendly pill as barrier bonus (aIndy): if a friendly pill is between us
  -- and the target, it absorbs enemy fire — discount the position.
  local fpill_barrier_bonus = 0
  for _, fp in pairs(world.pills) do
    if fp.owner == "friendly" and fp.health > 0 then
      -- Check if friendly pill is roughly on the line target→standoff
      local d_fp_target = U.mdist(fp.mx, fp.my, pill.mx, pill.my)
      local d_fp_us = U.mdist(fp.mx, fp.my, cx, cy)
      local d_total = U.mdist(cx, cy, pill.mx, pill.my)
      -- Friendly pill is "between" if both distances are less than total
      if d_fp_target < d_total and d_fp_us < d_total and d_fp_target >= 1 then
        fpill_barrier_bonus = fpill_barrier_bonus + C.FPILL_BARRIER_BONUS
      end
    end
  end

  return approach + water_pen + pushback_pen + crossfire + escape_cost + tree_pen
       + approach_exposure + orbit_pen + threat_pen + influence_pen
       - fpill_barrier_bonus
end

-- Enumerate candidate standoff positions around `pill` and pick the best scored one.
-- Samples multiple radii (max range down to max-2) so that if the ring at exactly
-- shell range lands on walls, nearby passable tiles are still considered.
-- Falls back to the closest candidate if none have finite scores (e.g. pill in open water).
function M.pick_standoff(world, info, pill, state, standoff_override, orbit_radius)
  local R_MAX = standoff_override or C.ATTACK_PILL_STANDOFF
  local R_MIN = R_MAX   -- stay on the circle edge, don't go closer
  local N     = C.ATTACK_PLAN_DIRS
  local tmx   = info.tankx >> 8
  local tmy   = info.tanky >> 8

  local best_score = math.huge
  local best_mx, best_my = nil, nil
  local fallback_dist = math.huge
  local fallback_mx, fallback_my = nil, nil

  local seen = {}
  for R = R_MAX, R_MIN, -1 do
    -- Slight penalty for shorter radii: each tile closer = more pill damage taken.
    -- 20 per tile makes radius 6 cost +20 and radius 5 cost +40 vs radius 7.
    local range_pen = (R_MAX - R) * 20
    for i = 0, N - 1 do
      local angle = i * (2 * math.pi / N)
      local cx = U.mclamp(math.floor(pill.mx + math.sin(angle) * R + 0.5))
      local cy = U.mclamp(math.floor(pill.my - math.cos(angle) * R + 0.5))
      local ck = U.mkey(cx, cy)
      if not seen[ck] then
        seen[ck] = true
        -- Always track closest as fallback
        local d = U.mdist(tmx, tmy, cx, cy)
        if d < fallback_dist then
          fallback_dist = d; fallback_mx = cx; fallback_my = cy
        end
        local score = score_standoff(world, cx, cy, pill, info, orbit_radius)
        if score < math.huge then score = score + range_pen end
        local tt   = U.ttype(cx, cy)
        local trees_on_path = forest_tiles_on_path(cx, cy, pill.mx, pill.my)
        if C.LOG_STANDOFF_CANDIDATES then
          print(string.format(
            TAG .. "   cand (%d,%d) R=%d tt=%d score=%s trees_hit=%d",
            cx, cy, R, tt,
            score >= math.huge and "INF" or string.format("%.1f", score),
            trees_on_path))
        end
        if score < best_score then
          best_score = score; best_mx = cx; best_my = cy
        end

        -- Overlay: show candidate positions with color-coded scores
        if score >= math.huge then
          -- Unreachable: dim red
          viz.rect("pill_take_target", cx, cy, cx + 1, cy + 1, 100, 0, 0, 60)
        elseif score == best_score then
          -- Currently best: bright green (will be overwritten by final pick)
        else
          -- Scored: yellow-to-red gradient based on relative cost
          local rel = math.min(1.0, score / math.max(1, best_score * 3))
          local r = math.floor(255 * rel)
          local g = math.floor(255 * (1 - rel))
          viz.rect("pill_take_target", cx, cy, cx + 1, cy + 1, r, g, 0, 50)
        end
      end
    end
  end

  local smx = best_mx or fallback_mx
  local smy = best_my or fallback_my
  if smx then
    -- Overlay: mark chosen standoff with bright green circle
    viz.circle("pill_take_target", smx + 0.5, smy + 0.5, 0.45, 0, 255, 0, 220)
    -- Line from pill to chosen standoff (green)
    viz.line("pill_take_target", pill.mx + 0.5, pill.my + 0.5, smx + 0.5, smy + 0.5, 0, 255, 0, 100)
    print(string.format(TAG .. " ATTACK PLAN: pill@(%d,%d) standoff=(%d,%d) score=%s",
          pill.mx, pill.my, smx, smy,
          best_score >= math.huge and "INF(fallback)" or string.format("%.1f", best_score)))
  end
  return smx, smy
end

-- =========================================================================
-- Wall-shield attack planner
-- =========================================================================
-- Pick a wall tile 1 tile from the pill, between the pill and the tank's
-- approach direction.  Then pick a standoff tile ~5 tiles from the pill
-- behind the wall on the same line.  Returns wall_mx, wall_my, stand_mx,
-- stand_my or nil if no valid wall-shield position exists.

local function pick_wall_shield(world, info, pill, state)
  if not C.WALL_SHIELD_ENABLED then return nil end
  if (info.trees or 0) < C.WALL_SHIELD_MIN_TREES then return nil end
  -- Don't attempt in a boat
  if info.inboat then return nil end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local pmx, pmy = pill.mx, pill.my

  -- Direction from pill to tank (approach angle)
  local dx = tmx - pmx
  local dy = tmy - pmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 2 then return nil end  -- too close already

  local ux, uy = dx / len, dy / len

  -- Try the primary angle and rotations of ±30°, ±60° to find a buildable wall tile
  local candidates = {}
  local angles = { 0, 0.52, -0.52, 1.05, -1.05 }  -- radians: 0, ±30°, ±60°
  for _, ang in ipairs(angles) do
    local cos_a = math.cos(ang)
    local sin_a = math.sin(ang)
    local rx = ux * cos_a - uy * sin_a
    local ry = ux * sin_a + uy * cos_a

    -- Wall tile: 1 tile from pill in this direction
    local wmx = math.floor(pmx + rx * C.WALL_SHIELD_WALL_DIST + 0.5)
    local wmy = math.floor(pmy + ry * C.WALL_SHIELD_WALL_DIST + 0.5)
    wmx = U.mclamp(wmx); wmy = U.mclamp(wmy)

    -- Wall tile must be buildable (grass, rubble, swamp, crater, road, or already a wall)
    local wtt = U.ttype(wmx, wmy)
    local buildable = (wtt == C.T_GRASS or wtt == C.T_RUBBLE or wtt == C.T_SWAMP
                       or wtt == C.T_CRATER or wtt == C.T_ROAD or wtt == C.T_BUILDING
                       or wtt == C.T_HALFBUILD or wtt == C.T_FOREST)
    -- Can't build on water, deep sea, bases, pills
    if not buildable then goto next_angle end
    -- Wall tile must not be the pill tile itself
    if wmx == pmx and wmy == pmy then goto next_angle end

    -- Standoff tile: offset ~25° from the wall direction so shells clear the wall.
    -- The wall blocks pill return fire (adjacent to pill on our side) but the tank
    -- shoots at an angle that misses the wall tile.  Try +25° and -25° offsets at
    -- distances 7 then 6 (max shell range down to 1 less) to find a position with
    -- clear LOS to the pill that doesn't pass through the wall.
    local smx, smy = nil, nil
    local off_rad = math.rad(C.WALL_SHIELD_STANDOFF_ANGLE_OFFSET)
    -- Try offset angles first, then 0° as fallback (directly behind wall)
    local offsets = { off_rad, -off_rad, off_rad * 2, -off_rad * 2, 0 }
    for _, sdist in ipairs({ C.WALL_SHIELD_STANDOFF, C.WALL_SHIELD_STANDOFF - 1, C.WALL_SHIELD_STANDOFF + 1 }) do
      for _, off in ipairs(offsets) do
        local cos_o = math.cos(off)
        local sin_o = math.sin(off)
        local ox = rx * cos_o - ry * sin_o
        local oy = rx * sin_o + ry * cos_o
        local cx = U.mclamp(math.floor(pmx + ox * sdist + 0.5))
        local cy = U.mclamp(math.floor(pmy + oy * sdist + 0.5))
        -- Must be passable land
        local ctt = U.ttype(cx, cy)
        local cland = C.TERRAIN_COST_LAND[ctt] or 9999
        if cland < 9999 and not U.is_water(ctt) then
          -- Must have clear LOS to pill: Bresenham must NOT pass through wall tile
          local hits_wall = U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
            if lx == wmx and ly == wmy then return true end
          end)
          -- Check for OTHER walls on path (exclude the wall we're building)
          local other_wall_hp = 0
          U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
            if not (lx == wmx and ly == wmy) then
              local ltt = U.ttype(lx, ly)
              if ltt == C.T_BUILDING then other_wall_hp = other_wall_hp + C.WALL_HP_FULL end
              if ltt == C.T_HALFBUILD then other_wall_hp = other_wall_hp + C.WALL_HP_HALF end
            end
          end)
          if not hits_wall and other_wall_hp == 0 then
            -- Must be within shell range
            local cd = math.sqrt((cx - pmx) * (cx - pmx) + (cy - pmy) * (cy - pmy))
            if cd <= C.ATTACK_PILL_RANGE then
              smx = cx; smy = cy
              break
            end
          end
        end
      end
      if smx then break end
    end
    if not smx then goto next_angle end

    -- Prebuild position: outside pill range on the wall direction line
    local pbmx = math.floor(pmx + rx * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5)
    local pbmy = math.floor(pmy + ry * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5)
    pbmx = U.mclamp(pbmx); pbmy = U.mclamp(pbmy)
    local pbtt = U.ttype(pbmx, pbmy)
    local pbland = C.TERRAIN_COST_LAND[pbtt] or 9999
    if pbland >= 9999 or U.is_water(pbtt) then goto next_angle end

    -- Check crossfire from other hostile pills at standoff
    local crossfire = 0
    for _, pm in pairs(world.pills) do
      if (pm.mx ~= pmx or pm.my ~= pmy)
         and (pm.owner == "hostile" or pm.owner == "neutral")
         and pm.health > 0 then
        local d = U.mdist(smx, smy, pm.mx, pm.my)
        if d <= C.PILL_RANGE_MAP then
          crossfire = crossfire + 1
        end
      end
    end

    -- LGM reachability: can the LGM walk from prebuild to wall and back?
    local lgm_to_wall = cpf.lgm_travel_ticks_map(
      pbmx, pbmy, wmx, wmy, wmx, wmy,
      C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
    if lgm_to_wall == -1 then goto next_angle end

    local lgm_return = cpf.lgm_travel_ticks_map(
      wmx, wmy, pbmx, pbmy, wmx, wmy,
      C.WALL_SHIELD_LGM_MAX_TICKS, C.WALL_SHIELD_LGM_STUCK_TICKS)
    if lgm_return == -1 then goto next_angle end

    local lgm_round_trip = lgm_to_wall + C.LGM_BUILD_TIME + lgm_return

    -- Approach cost
    local ammo = (info.shells or 0) + (info.mines or 0)
    local approach = cpf.estimate_cost(tmx, tmy, smx, smy, 0)

    -- Threat map: penalise standoff positions in high-threat areas (enemy
    -- tanks nearby, additional pill fire not captured by crossfire count).
    local ws_threat = threat.at(smx, smy) * 2

    -- Score: lower is better
    local score = approach + crossfire * 200 + math.abs(ang) * 50
                + lgm_round_trip * C.WALL_SHIELD_LGM_TRIP_WEIGHT
                + ws_threat
    candidates[#candidates + 1] = {
      wmx = wmx, wmy = wmy, smx = smx, smy = smy,
      pbmx = pbmx, pbmy = pbmy,
      score = score, ang = ang, crossfire = crossfire, lgm_trip = lgm_round_trip,
    }

    ::next_angle::
  end

  if #candidates == 0 then return nil end

  -- Pick best
  table.sort(candidates, function(a, b) return a.score < b.score end)
  local best = candidates[1]
  print(string.format(
    TAG .. " WALL-SHIELD PLAN: pill@(%d,%d) wall=(%d,%d) standoff=(%d,%d) prebuild=(%d,%d) score=%.0f crossfire=%d lgm_trip=%d",
    pmx, pmy, best.wmx, best.wmy, best.smx, best.smy,
    best.pbmx, best.pbmy, best.score, best.crossfire, best.lgm_trip))
  return best.wmx, best.wmy, best.smx, best.smy, best.pbmx, best.pbmy
end

-- Return cached or freshly computed standoff position for the given pill.
-- Invalidated when the pill target changes or every PILL_ATTACK_REPLAN_TICKS.
-- Also re-plans if the cached tile has become impassable (e.g. built over).
function M.get_standoff(world, info, pill_key, pill, state)
  local plan = state.pill_attack_plan
  local now  = state.tick or 0
  if plan and plan.pill_key == pill_key and now < plan.replan_at then
    -- Validate: cached tile must be passable dry land (not water, not a wall).
    -- TERRAIN_COST_LAND[T_RIVER] == -1 (dynamic sentinel) so we must test
    -- for water explicitly rather than relying on the cost threshold.
    local tt = U.ttype(plan.standoff_mx, plan.standoff_my)
    if not U.is_water(tt) then
      local lc = C.TERRAIN_COST_LAND[tt] or 9999
      if lc < 9999 then
        return plan.standoff_mx, plan.standoff_my
      end
    end
  end

  local smx, smy = M.pick_standoff(world, info, pill, state)
  state.pill_attack_plan = {
    pill_key    = pill_key,
    standoff_mx = smx,
    standoff_my = smy,
    replan_at   = now + C.PILL_ATTACK_REPLAN_TICKS,
  }
  return smx, smy
end

-- =========================================================================
-- Unified attack pill substate machine
-- =========================================================================
-- Substates: position → aim → engage → curve_away → (back to engage)
--            pill dies at any point → rush
--            armour critical → disengage
--
--
-- =========================================================================
-- M.evaluate_pill_difficulty(pill, world, detailed)
-- Scan the standoff circle around a pill and score how hard it is to attack.
-- Returns: best_score (lower=easier), spots (list of scored positions)
-- No pathfinding calls — only grid lookups, safe to call on all pills.
--
-- detailed=false (default): returns only best_score and minimal spots
--   (no maneuver_tiles, no cx/cy floats — lightweight for goal selection)
-- detailed=true: full data including maneuver_tiles and precise positions
--   (for visualization when actively attacking this pill)
-- =========================================================================

-- ── Precomputed ellipse stamps ──
--
-- For each scan angle, compute once: the list of integer (dx, dy) tile
-- offsets RELATIVE TO THE SPOT TILE that fall inside the maneuver
-- ellipse oriented at that angle. The runtime code iterates the stamp
-- directly instead of nested loops + tile_in_ellipse calls.
--
-- The ellipse geometry depends only on the angle (orientation) and
-- the standard radii (r_long = ATTACK_SAFE_RADIUS,
-- r_short = ATTACK_SAFE_RADIUS / 3) — same axes the inspector viz
-- uses to draw the outline.
-- The float center sub-tile offset varies per angle, so each stamp is
-- generated for that exact angle's spot center.
--
-- ELLIPSE_STAMPS_5DEG  — 72 stamps at 5° steps, used by the full
--                        plan_position scan (detailed=true case).
-- ELLIPSE_STAMPS_45DEG —  8 stamps at 45° steps, used by the quick
--                        per-candidate eval in goals.lua step_eval_queue.
local ELLIPSE_STAMPS_5DEG  = {}
local ELLIPSE_STAMPS_45DEG = {}
do
  local R       = C.ATTACK_PILL_STANDOFF
  -- r_long was hardcoded to 4 for years while the inspector viz drew
  -- the outline using ATTACK_SAFE_RADIUS; shrinking SAFE_RADIUS made
  -- the visible ellipse smaller but left the stamps the original
  -- size. Source both axes from the same constant so they track.
  local r_long  = C.ATTACK_SAFE_RADIUS
  local r_short = C.ATTACK_SAFE_RADIUS / 3.0
  local iter_r  = math.ceil(r_long) + 1

  local function build_stamp(deg)
    local rad = math.rad(deg)
    -- Spot center if pill is at (0, 0). pmx + 0.5 = 0.5, etc.
    local cx = 0.5 + math.sin(rad) * R
    local cy = 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    -- Orientation: unit vector from spot toward pill (0, 0).
    local edx = 0.5 - cx
    local edy = 0.5 - cy
    local elen = math.sqrt(edx * edx + edy * edy)
    if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
    local ux, uy = edx / elen, edy / elen   -- radial (toward pill)
    local vx, vy = -uy, ux                  -- tangential

    -- 5 sample offsets (4 corners + center) — same as the original
    -- tile_in_ellipse so the stamp matches its result exactly.
    local sample_offsets = {
      {0,   0}, {1,   0}, {0,   1}, {1,   1}, {0.5, 0.5},
    }
    local function tile_in(sx, sy)
      for _, off in ipairs(sample_offsets) do
        local rel_x = (sx + off[1]) - cx
        local rel_y = (sy + off[2]) - cy
        local pu = rel_x * ux + rel_y * uy
        local pv = rel_x * vx + rel_y * vy
        if (pu * pu) / (r_long * r_long) + (pv * pv) / (r_short * r_short) <= 1.0 then
          return true
        end
      end
      return false
    end

    local stamp = {}
    for dy = -iter_r, iter_r do
      for dx = -iter_r, iter_r do
        local sx = mx + dx
        local sy = my + dy
        if tile_in(sx, sy) then
          -- Store offsets from the SPOT TILE (mx, my) so the runtime
          -- code can do `mx_runtime + off.dx`, `my_runtime + off.dy`.
          stamp[#stamp + 1] = { dx = dx, dy = dy }
        end
      end
    end
    return stamp
  end

  for deg = 0, 359, 5 do
    ELLIPSE_STAMPS_5DEG[deg] = build_stamp(deg)
  end
  for deg = 0, 359, 45 do
    ELLIPSE_STAMPS_45DEG[deg] = build_stamp(deg)
  end
end

function M.evaluate_pill_difficulty(pill, world, detailed, scan_step, phase, state, tmx, tmy)
  local pmx, pmy = pill.mx, pill.my
  local R = C.ATTACK_PILL_STANDOFF
  -- Banned-angle map for this pill (set by approach-timeout handler).
  -- nil if state isn't passed (legacy callers / unit tests) or this
  -- pill has no bans.
  local banned_for_pill = nil
  local now_for_ban = 0
  if state and state.banned_pill_angles then
    banned_for_pill = state.banned_pill_angles[pmy * 256 + pmx]
    now_for_ban = state.tick or 0
  end
  local step_deg = scan_step or C.ATTACK_SCAN_DEGREES
  local safe_r = C.ATTACK_SAFE_RADIUS
  -- Pick the right precomputed stamp set for this scan resolution.
  -- Falls back to nil for other step values (the runtime then uses
  -- the slower tile_in_ellipse path).
  local stamps = (step_deg == 5) and ELLIPSE_STAMPS_5DEG
              or (step_deg == 45) and ELLIPSE_STAMPS_45DEG
              or nil

  local spots = detailed and {} or nil
  local best_score = math.huge
  local best_spot = nil  -- track best spot for returning
  local all_valid = {}   -- collect all LOS-valid spots for two-pass selection

  for deg = 0, 359, step_deg do
    -- Skip banned approach angles (set by approach timeout). 5° bucket.
    if banned_for_pill then
      local bucket = math.floor((deg % 360) / 5) * 5
      local exp = banned_for_pill[bucket]
      if exp and now_for_ban < exp then goto next_spot end
    end
    local rad = math.rad(deg)
    local cx = pmx + 0.5 + math.sin(rad) * R
    local cy = pmy + 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    if not U.in_map(mx, my) then goto next_spot end

    local tt = U.ttype(mx, my)
    local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
    if not passable then goto next_spot end

    -- LOS test: try a real-physics shot from the spot's tile center to
    -- the pill's center + 4 corners. As long as one of the five trial
    -- shots has a clean trajectory (no wall, no other pill in the way),
    -- the spot has line of sight. Skewed corner shots can slip past
    -- obstacles that block the direct center line.
    local spot_wx = (mx << 8) | 128
    local spot_wy = (my << 8) | 128
    local has_los = PF.pill_shots_clear(spot_wx, spot_wy, pill, world,
                                        cpf.SHOT_TANK, 0)

    local score_a, score_b, score_d, score_e, total_score = 0, 0, 0, 0, 999
    local maneuver_tiles = detailed and {} or nil
    if has_los then
      local total_danger = 0
      local safe_tiles = 0
      local terrain_penalty = 0
      -- Ellipse oriented toward the pill: long axis = radial (safe_r),
      -- short axis = tangential (safe_r / 3).  Compute unit vector from
      -- spot toward pill to define the axes.
      local edx = pmx + 0.5 - cx
      local edy = pmy + 0.5 - cy
      local elen = math.sqrt(edx * edx + edy * edy)
      if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
      local ux, uy = edx / elen, edy / elen  -- radial (toward pill)
      local vx, vy = -uy, ux                 -- tangential (perpendicular)
      -- Match the stamp + inspector viz: r_long sourced from safe_r
      -- so all three (runtime fallback, precomputed stamp, drawn
      -- ellipse) stay in lockstep.
      local r_long  = safe_r         -- radial half-length
      local r_short = safe_r / 3.0   -- tangential half-width

      -- Pre-compute crossfire: count hostile pills where ANY ellipse tile
      -- is in range.  Once a pill is marked as covering, skip to the next.
      -- Iterate up to ceil(r_long) + 1 to cover the long axis (buffer for the
      -- offset between integer tile mx,my and the precise float spot cx,cy).
      local iter_r = math.ceil(r_long) + 1
      -- Tile (sx, sy) is "inside" the ellipse if its center OR any of its 4
      -- corners falls inside.  Offsets are corner positions on the tile:
      local sample_offsets = {
        {0,   0},    -- top-left corner
        {1,   0},    -- top-right corner
        {0,   1},    -- bottom-left corner
        {1,   1},    -- bottom-right corner
        {0.5, 0.5},  -- center
      }
      local function tile_in_ellipse(sx, sy)
        for _, off in ipairs(sample_offsets) do
          -- Position relative to the precise ellipse center (cx, cy)
          local rel_x = (sx + off[1]) - cx
          local rel_y = (sy + off[2]) - cy
          local pu = rel_x * ux + rel_y * uy
          local pv = rel_x * vx + rel_y * vy
          if (pu * pu) / (r_long * r_long) + (pv * pv) / (r_short * r_short) <= 1.0 then
            return true
          end
        end
        return false
      end

      -- Use the precomputed ellipse stamp for this angle if available;
      -- otherwise fall back to the slower nested-loop + tile_in_ellipse
      -- path. The stamp is a flat list of (dx, dy) offsets relative to
      -- the spot tile (mx, my) — exactly the tiles inside the ellipse
      -- for this angle.
      local stamp = stamps and stamps[deg] or nil

      -- ── Crossfire (Scan A) ──
      -- Worst (highest-coverage) tile in the BACK HALF of the ellipse
      -- (away from the pill). The pill-side half is already expected to
      -- take fire from the target — only unexpected crossfire from behind
      -- matters for positioning. Tiles with positive projection onto ux,uy
      -- (toward pill) are skipped.
      local max_coverage = 0
      if stamp then
        for i = 1, #stamp do
          local off = stamp[i]
          -- Project offset onto radial axis (toward pill). Skip pill-side half.
          local proj = off.dx * ux + off.dy * uy
          if proj <= 0 then
            local sx2, sy2 = mx + off.dx, my + off.dy
            if U.in_map(sx2, sy2) then
              local cov = threat.coverage_at(sx2, sy2)
              if cov > max_coverage then max_coverage = cov end
            end
          end
        end
      else
        for dy2 = -iter_r, iter_r do
          for dx2 = -iter_r, iter_r do
            local proj = dx2 * ux + dy2 * uy
            if proj <= 0 then
              local sx2, sy2 = mx + dx2, my + dy2
              if tile_in_ellipse(sx2, sy2) and U.in_map(sx2, sy2) then
                local cov = threat.coverage_at(sx2, sy2)
                if cov > max_coverage then max_coverage = cov end
              end
            end
          end
        end
      end
      if max_coverage > 1 then
        score_e = 100 * (max_coverage - 1)
      end

      -- ── Maneuver area scan (Scan B) ──
      -- Sum danger + accumulate terrain penalty over the in-ellipse
      -- tiles. Per-tile work cannot be precomputed (threat / terrain
      -- change per tick), but iterating the stamp directly cuts the
      -- per-tile overhead from ~50 ops (tile_in_ellipse) to ~0.
      -- Subtract THIS pill's own contribution from each tile so the
      -- maneuver-area average reflects ambient threat rather than the
      -- target's own footprint (mirrors what self_dr does for the
      -- approach path). Same rationale: the bot is committed to
      -- killing this pill, so its danger shouldn't bully position
      -- selection AGAINST it.
      local _self_pcontrib = threat.pill_contrib[pmy * 256 + pmx]
      local function process_tile(sx, sy)
        if not U.in_map(sx, sy) then return end
        local d = threat.pill_at(sx, sy)
        if _self_pcontrib then
          local self_d = _self_pcontrib[sy * 256 + sx]
          if self_d then
            d = d - self_d
            if d < 0 then d = 0 end
          end
        end
        -- Tree cover: reduce danger based on surrounding forest count
        if d > 0 and U.ttype(sx, sy) == C.T_FOREST then
          local tree_n = 0
          if U.in_map(sx-1, sy) and U.ttype(sx-1, sy) == C.T_FOREST then tree_n = tree_n + 1 end
          if U.in_map(sx+1, sy) and U.ttype(sx+1, sy) == C.T_FOREST then tree_n = tree_n + 1 end
          if U.in_map(sx, sy-1) and U.ttype(sx, sy-1) == C.T_FOREST then tree_n = tree_n + 1 end
          if U.in_map(sx, sy+1) and U.ttype(sx, sy+1) == C.T_FOREST then tree_n = tree_n + 1 end
          if tree_n >= 2 then d = math.max(0, d - (tree_n - 1)) end
        end
        total_danger = total_danger + d
        if detailed then
          maneuver_tiles[#maneuver_tiles + 1] = { val = math.floor(d + 0.5), x = sx, y = sy }
        end
        safe_tiles = safe_tiles + 1
        local stt = U.ttype(sx, sy)
        -- Hostile bases are no-go: treat them like deep water.
        local base_entry = world.base_at[sy * 256 + sx]
        local enemy_base = base_entry and base_entry.base
                           and base_entry.base.owner == "hostile"
        -- Friendly pill on this tile? pill_at[k] is a list — walk it.
        -- Friendlies don't change tile-type the same way owned pillboxes
        -- do (engine-dependent), so the T_PILLBOX check below can miss
        -- them. Catch explicitly so the maneuver area sees them as
        -- impassable (we can't drive on a friendly pill).
        local friendly_pill_here = false
        local plist = world.pill_at[sy * 256 + sx]
        if plist then
          for _, e in ipairs(plist) do
            if e.pill and e.pill.owner == "friendly"
               and (e.pill.health or 0) > 0 then
              friendly_pill_here = true; break
            end
          end
        end
        if stt == C.T_DEEPSEA or enemy_base then
          terrain_penalty = terrain_penalty + 1000
        elseif stt == C.T_BUILDING or stt == C.T_HALFBUILD
           or stt == C.T_SWAMP or stt == C.T_RIVER
           or stt == C.T_PILLBOX
           or friendly_pill_here then
          -- Pillboxes (including friendly ones) act like walls for our
          -- purposes: the tank can't drive through them, so a spot whose
          -- maneuver ellipse overlaps a pill tile is worse for the take.
          terrain_penalty = terrain_penalty + 100
        end
      end

      if stamp then
        for i = 1, #stamp do
          local off = stamp[i]
          process_tile(mx + off.dx, my + off.dy)
        end
      else
        for dy = -iter_r, iter_r do
          for dx = -iter_r, iter_r do
            local sx, sy = mx + dx, my + dy
            if tile_in_ellipse(sx, sy) then
              process_tile(sx, sy)
            end
          end
        end
      end
      score_a = safe_tiles > 0 and (total_danger / safe_tiles) or 999
      score_b = 0
      if detailed then
        for _, t in ipairs(maneuver_tiles) do
          if t.val >= C.ATTACK_DANGER_HOTSPOT then score_b = 10; break end
        end
      else
        -- Lightweight hotspot check: recompute from max_danger
        if total_danger / math.max(1, safe_tiles) >= C.ATTACK_DANGER_HOTSPOT then
          score_b = 10
        end
      end
      score_d = terrain_penalty
      total_score = score_a + score_b + score_d + score_e
      -- Mid/late-game spots deep in enemy influence are much riskier to
      -- hold during the take — boost their cost so we prefer takes
      -- through friendlier or contested ground when one exists.
      local hostile_inf_mult = 1
      if phase and phase ~= "opening" then
        local infl = cpf.influence_at(mx, my) or 0
        if infl <= C.PILL_TAKE_HOSTILE_INF_THRESHOLD then
          hostile_inf_mult = C.PILL_TAKE_HOSTILE_INF_MULT
          total_score = total_score * hostile_inf_mult
        end
      end
      local spot_ref = nil
      if detailed then
        spot_ref = {
          cx = cx, cy = cy, mx = mx, my = my,
          has_los = has_los,
          score_a = score_a, score_b = score_b, score_d = score_d, score_e = score_e,
          total_score = total_score, deg = deg,
          maneuver_tiles = maneuver_tiles,
        }
        spots[#spots + 1] = spot_ref
      end
      all_valid[#all_valid + 1] = {
        mx = mx, my = my, cx = cx, cy = cy, score = total_score, deg = deg,
        hostile_inf_mult = hostile_inf_mult,
        spot = spot_ref,
      }
    end
    ::next_spot::
  end

  -- Two-pass selection: group valid spots by score in 50-buckets,
  -- take the best bucket, then pick the one with lowest travel cost.
  --
  -- Two-pass selection: group valid spots into 50-wide score buckets,
  -- take the best bucket, then pick the closest spot by travel cost.
  --
  -- For the travel cost we need ONE slate that covers ALL bucket spots
  -- so every comparison is within the same cost space. Try slates in
  -- order (short-range first — most current — then long-range); the
  -- first slate where every bucket spot has a finite cost wins. If no
  -- single slate covers all spots, fall back to A* for each spot.
  --   0 = KIND_NORMAL short-range   2 = KIND_PILL short-range
  --   1 = KIND_NORMAL long-range    3 = KIND_PILL long-range
  local COST_INF    = 1e29
  local SLATE_ORDER = { 0, 1, 2, 3 }

  if #all_valid > 0 then
    local min_score = math.huge
    for _, s in ipairs(all_valid) do
      if s.score < min_score then min_score = s.score end
    end
    local bucket_floor = math.floor(min_score / 50) * 50
    local bucket_ceil  = bucket_floor + 50

    -- Collect bucket members once.
    local bucket = {}
    for _, s in ipairs(all_valid) do
      if s.score >= bucket_floor and s.score < bucket_ceil then
        bucket[#bucket + 1] = s
      end
    end

    -- Find the first slate where every bucket spot has a finite cost.
    local costs      = {}   -- costs[i] = travel cost for bucket[i]
    local slate_used = nil
    for _, sl in ipairs(SLATE_ORDER) do
      local ok = true
      for i, s in ipairs(bucket) do
        local c = cpf.dijkstra_cost_at(sl, s.mx, s.my, 0)
        if c >= COST_INF then ok = false; break end
        costs[i] = c
      end
      if ok then slate_used = sl; break end
    end

    -- No slate covers all spots — fall back to A* per spot.
    if not slate_used and tmx then
      for i, s in ipairs(bucket) do
        costs[i] = cpf.estimate_cost(tmx, tmy, s.mx, s.my, 0)
      end
    end

    -- Pick the bucket spot with the lowest cost and mark viz state.
    local best_dij = math.huge
    for i, s in ipairs(bucket) do
      local dij = costs[i] or math.huge
      if s.spot then s.spot.in_bucket = true; s.spot._dij = dij end
      if dij < best_dij then
        best_dij   = dij
        best_spot  = s
        best_score = s.score
      end
    end
  end

  return best_score, spots, best_spot
end

-- =========================================================================
-- evaluate_tank_standoff — find best engagement position around enemy tank
--
-- Same concept as evaluate_pill_difficulty but for tank combat:
-- sample 8 positions at TANK_COMBAT_STANDOFF_RANGE around the target,
-- score each for terrain, maneuver space (ellipse), crossfire from
-- hostile pills, and wall obstructions. Returns the best standoff
-- tile coordinates and score, plus the A* cost to reach it.
--
-- Returns: best_mx, best_my, best_score, path_cost, shells_on_arrival
--          or nil if no valid standoff position found.
-- =========================================================================
function M.evaluate_tank_standoff(et, tmx, tmy, info, world, state)
  print2(string.format("attack_tank standoff: evaluating enemy@(%d,%d) from tank@(%d,%d) R=%d",
    et.mx, et.my, tmx, tmy, C.TANK_COMBAT_STANDOFF_RANGE))
  local R = C.TANK_COMBAT_STANDOFF_RANGE
  local safe_r = C.ATTACK_SAFE_RADIUS
  local stamps = ELLIPSE_STAMPS_45DEG
  local best_score = math.huge
  local best_mx, best_my = nil, nil
  local best_deg = 0
  local scan_spots = {}

  for deg = 0, 315, 45 do
    local rad = math.rad(deg)
    local cx = et.mx + 0.5 + math.sin(rad) * R
    local cy = et.my + 0.5 - math.cos(rad) * R
    local mx = math.floor(cx)
    local my = math.floor(cy)
    if not U.in_map(mx, my) then goto next_tank_spot end

    local tt = U.ttype(mx, my)
    local passable = (C.TERRAIN_COST_LAND[tt] or 9999) < 9999 and not U.is_water(tt)
    if not passable then
      print2(string.format("  attack_tank cand deg=%d @(%d,%d) SKIP: impassable tt=%d", deg, mx, my, tt))
      scan_spots[#scan_spots + 1] = {
        cx = cx, cy = cy, mx = mx, my = my, deg = deg,
        has_los = false, total_score = 999, reason = "impassable",
        score_danger = 0, score_crossfire = 0, score_terrain = 0, score_approach = 0,
      }
      goto next_tank_spot
    end

    -- Wall check: need clear shot to the enemy tank from this position
    local wall_hp = PF.wall_hp_between(mx, my, et.mx, et.my)
    if wall_hp > 0 then
      print2(string.format("  attack_tank cand deg=%d @(%d,%d) SKIP: wall_hp=%d", deg, mx, my, wall_hp))
      scan_spots[#scan_spots + 1] = {
        cx = cx, cy = cy, mx = mx, my = my, deg = deg,
        has_los = false, total_score = 999, reason = "wall_blocked",
        score_danger = 0, score_crossfire = 0, score_terrain = 0, score_approach = 0,
      }
      goto next_tank_spot
    end

    -- ── Maneuver ellipse scan ──
    -- Oriented toward the enemy tank, same geometry as pill attack.
    local total_danger = 0
    local safe_tiles = 0
    local terrain_penalty = 0
    local max_coverage = 0
    local maneuver_tiles = {}

    local stamp = stamps and stamps[deg] or nil
    local function process_tile(sx, sy)
      if not U.in_map(sx, sy) then return end
      local d = threat.pill_at(sx, sy)
      total_danger = total_danger + d
      safe_tiles = safe_tiles + 1
      maneuver_tiles[#maneuver_tiles + 1] = { x = sx, y = sy, val = math.floor(d + 0.5) }
      local stt = U.ttype(sx, sy)
      local base_entry = world.base_at[sy * 256 + sx]
      local enemy_base = base_entry and base_entry.base
                         and base_entry.base.owner == "hostile"
      if stt == C.T_DEEPSEA or enemy_base then
        terrain_penalty = terrain_penalty + 1000
      elseif stt == C.T_BUILDING or stt == C.T_HALFBUILD
         or stt == C.T_SWAMP or stt == C.T_RIVER then
        terrain_penalty = terrain_penalty + 100
      end
      local cov = threat.coverage_at(sx, sy)
      if cov > max_coverage then max_coverage = cov end
    end

    if stamp then
      for i = 1, #stamp do
        local off = stamp[i]
        process_tile(mx + off.dx, my + off.dy)
      end
    end

    local score_danger = safe_tiles > 0 and (total_danger / safe_tiles) or 999
    local score_crossfire = max_coverage > 0 and (100 * max_coverage) or 0
    -- Approach cost: lightweight distance tiebreaker only. The A* path_cost
    -- to the winning standoff already captures the real travel cost — the
    -- standoff score should just pick the better LOCAL position, not
    -- double-count travel via the full Dijkstra cost.
    local score_approach = U.mdist(tmx, tmy, mx, my) * 2
    local score = score_danger + score_crossfire + terrain_penalty + score_approach

    scan_spots[#scan_spots + 1] = {
      cx = cx, cy = cy, mx = mx, my = my, deg = deg,
      has_los = true, total_score = score,
      score_danger = score_danger, score_crossfire = score_crossfire,
      score_terrain = terrain_penalty, score_approach = score_approach,
      maneuver_tiles = maneuver_tiles,
    }

    print2(string.format("  attack_tank cand deg=%d @(%d,%d) danger=%.1f xfire=%.0f terrain=%.0f approach=%.0f total=%.0f",
      deg, mx, my, score_danger, score_crossfire, terrain_penalty, score_approach, score))

    -- Overlay: color-coded candidate positions
    if score < best_score then
      -- Will be best (for now) — skip, we'll draw the winner after
    else
      local rel = math.min(1.0, score / math.max(1, best_score * 3))
      local cr = math.floor(255 * rel)
      local cg = math.floor(255 * (1 - rel))
      viz.rect("tank_combat_standoff_scan", cx - 0.15, cy - 0.15, cx + 0.15, cy + 0.15, cr, cg, 0, 120)
    end

    if score < best_score then
      best_score = score
      best_mx = mx
      best_my = my
      best_deg = deg
    end

    ::next_tank_spot::
  end

  if not best_mx then
    print2("  attack_tank: NO valid standoff position found")
    return nil
  end
  print2(string.format("  attack_tank WINNER: deg=%d @(%d,%d) score=%.1f", best_deg, best_mx, best_my, best_score))

  -- Overlay: mark chosen standoff with green circle + line to enemy
  viz.circle("tank_combat_standoff_scan", best_mx + 0.5, best_my + 0.5, 0.45, 0, 255, 0, 220)
  viz.line("tank_combat_standoff_scan", et.mx + 0.5, et.my + 0.5, best_mx + 0.5, best_my + 0.5, 0, 255, 0, 100)

  -- Draw ellipse outline on winner
  do
    local best_spot = nil
    for _, s in ipairs(scan_spots) do
      if s.deg == best_deg and s.has_los then best_spot = s; break end
    end
    if best_spot then
      local edx = et.mx + 0.5 - best_spot.cx
      local edy = et.my + 0.5 - best_spot.cy
      local elen = math.sqrt(edx * edx + edy * edy)
      if elen < 0.01 then edx, edy, elen = 0, -1, 1 end
      local ux, uy = edx / elen, edy / elen
      local vx, vy = -uy, ux
      -- Same r_long source as the scoring code (was hardcoded to 4
      -- and drifted from ATTACK_SAFE_RADIUS — drawn shape now matches).
      local rl, rs = safe_r, safe_r / 3.0
      local segs = 24
      local px, py
      for i = 0, segs do
        local a = (i / segs) * 2 * math.pi
        local eu = math.cos(a) * rl
        local ev = math.sin(a) * rs
        local nx = best_spot.cx + eu * ux + ev * vx
        local ny = best_spot.cy + eu * uy + ev * vy
        if px then
          viz.line("tank_combat_standoff_scan", px, py, nx, ny, 0, 200, 0, 100)
        end
        px, py = nx, ny
      end
    end
  end

  -- Score labels on each candidate
  for _, s in ipairs(scan_spots) do
    if s.has_los and s.total_score < 900 then
      local label = string.format("D%.0f+X%.0f+T%.0f+A%.0f=%.0f",
        s.score_danger, s.score_crossfire, s.score_terrain, s.score_approach,
        s.total_score)
      viz.text("tank_combat_standoff_scan", s.cx - 1, s.cy - 0.5, label, "topleft", 255, 0, 255, 255)
    elseif not s.has_los then
      viz.text("tank_combat_standoff_scan", s.cx, s.cy - 0.3, s.reason or "blocked", "center", 200, 0, 0, 180)
    end
  end

  -- A* to the best standoff position (not the enemy tank itself)
  cpf.set_config("danger_scale", 0.1)
  local path_cost = smart_cost(KIND_PILL, tmx, tmy, best_mx, best_my, 0,
                               info.shells or 32, info.trees or 0,
                               info.mines or 0, info.armour or 40)
  cpf.set_config("danger_scale", 1.0)

  local shells_on_arrival = cpf.dijkstra_shells_at(KIND_PILL, best_mx, best_my)
                         or cpf.astar_shells_at(best_mx, best_my)

  print2(string.format("  attack_tank A* to (%d,%d): path_cost=%.1f shells_arr=%s",
    best_mx, best_my, path_cost,
    shells_on_arrival and tostring(math.floor(shells_on_arrival)) or "nil"))

  return best_mx, best_my, best_score, path_cost, shells_on_arrival, scan_spots, best_deg
end

-- position:   navigating to standoff, braking to stop
-- aim:        stopped at standoff, turning to face pill
-- engage:     firing at pill, monitoring armour
-- curve_away: evasive turn after taking hits
-- rush:       pill dead, drive to capture
-- disengage:  armour critical, flee

-- Walk shot_tracker.shots and update goal._on_target_fired (count of shots
-- fired during this attack whose crosshair-at-fire was on the target pill)
-- and goal._on_target_misses (shots that died without landing on the pill).
-- For each newly-detected miss, bump goal._bullets_needed by one so the
-- charge/engage swerve trigger waits for a replacement shot.
local function update_shot_accounting(goal, world)
  if not goal._attack_start_tick then
    goal._on_target_fired  = 0
    goal._on_target_misses = 0
    return
  end
  local pmx, pmy = goal.mx, goal.my
  local pwx = (pmx + 0.5) * 256
  local pwy = (pmy + 0.5) * 256
  local hit_tol_wu = 384        -- death within 1.5 tiles counts as a hit
  local hit2 = hit_tol_wu * hit_tol_wu
  local fired, on_target_fired, on_target_in_flight, misses = 0, 0, 0, 0
  for _, s in ipairs(shot_tracker.shots) do
    if s.fire_tick >= goal._attack_start_tick then
      -- Total shots fired during this attack (any direction).
      fired = fired + 1

      -- A shot is "on target" if its actual simulated trajectory
      -- (cpf.simulate_shot, same physics as the engine) crosses the
      -- pill tile. Stricter than the prior ray-perpendicular check —
      -- a near-miss that fires from where the trajectory would only
      -- clip the corner of an adjacent tile no longer counts.
      local sm = math.sqrt(s.step_x * s.step_x + s.step_y * s.step_y)
      local on_target = false
      if sm > 0 then
        -- Project a target point far down the firing direction so
        -- simulate_shot traces the full trajectory's range. Round to
        -- integers — cpf_simulate_shot's binding uses luaL_checkinteger
        -- so floats from the (far/sm) scale crash the brain.
        local far = 14 * 256
        local tx_w = math.floor(s.fire_fx + s.step_x * (far / sm) + 0.5)
        local ty_w = math.floor(s.fire_fy + s.step_y * (far / sm) + 0.5)
        local path = cpf.simulate_shot(s.fire_fx, s.fire_fy, tx_w, ty_w,
                                       cpf.SHOT_TANK, 14)
        if path then
          for _, t in ipairs(path) do
            if t.mx == pmx and t.my == pmy then
              on_target = true
              break
            end
          end
        end
      end
      if on_target then
        on_target_fired = on_target_fired + 1
        if s.status == "in_flight" then
          on_target_in_flight = on_target_in_flight + 1
        elseif s.status == "dead" then
          local ddx = s.dead_fx - pwx
          local ddy = s.dead_fy - pwy
          local missed = (ddx * ddx + ddy * ddy) > hit2
          if missed then
            misses = misses + 1
            if not s._attack_handled then
              -- First detection of this miss — bump the kill budget.
              goal._bullets_needed = (goal._bullets_needed or 0) + 1
            end
          end
          s._attack_handled = true
        end
      end
    end
  end
  goal._fired               = fired
  goal._on_target_fired     = on_target_fired
  goal._on_target_in_flight = on_target_in_flight
  goal._on_target_misses    = misses
end

function M.update_attack_substate(goal, state, world, info)
  if goal.kind ~= "attack_pill" then return end

  -- Tally on-target shots and bump bullets_needed for any misses.
  update_shot_accounting(goal, world)

  -- HUD: kill attempt indicator (top-left)
  if goal._kill_attempt ~= nil then
    local label = goal._kill_attempt and "KILL ATTEMPT" or "DAMAGE ONLY"
    local r, g, b = goal._kill_attempt and 100 or 255,
                    goal._kill_attempt and 255 or 200,
                    goal._kill_attempt and 100 or 50
    viz.hud_text("hud_kill_attempt", 10, 160, label, "topleft", r, g, b, 255)
    viz.hud_text("hud_kill_attempt", 10, 175,
      string.format("bullets_needed=%d pill_hp=%d", goal._bullets_needed or 0,
                    goal.target_id and (function()
                      local p = world.pills[goal.target_id]
                      return p and p.health or 0
                    end)() or 0),
      "topleft", 200, 200, 200, 255)
    viz.hud_text("hud_kill_attempt", 10, 190,
      string.format("fired=%d on_pill=%d misses=%d",
                    goal._fired or 0,
                    goal._on_target_fired or 0,
                    goal._on_target_misses or 0),
      "topleft", 0, 255, 255, 255)
  end

  -- Floating count above the target pill:
  --   "<on-target shots still in flight> / <pill HP remaining>"
  -- A tree-blocked shot drops out of the in-flight side without changing
  -- the HP side, so the indicator visibly goes down by one rather than
  -- pretending we somehow need more bullets to kill the pill.
  if goal._fired and goal._fired > 0 then
    local pill_hp = 0
    if goal.target_id then
      local p = world.pills[goal.target_id]
      pill_hp = p and p.health or 0
    end
    viz.text("pill_shot_count", goal.mx + 0.5, goal.my - 0.6,
      string.format("%d/%d", goal._on_target_in_flight or 0, pill_hp),
      "center", 0, 255, 255, 255)
  end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick
  local pmx, pmy = goal.mx, goal.my

  if not goal.substate then goal.substate = "plan_position" end

  -- Only log on substate transitions (avoid spamming every tick)
  if goal.substate ~= goal._last_logged_sub then
    goal._last_logged_sub = goal.substate
    print(string.format(TAG .. " [ATTACK] substate=%s pill=(%d,%d)", goal.substate, pmx, pmy))
  end

  -- Look up pill
  local pill = M.find_pill_at(world, pmx, pmy)

  -- PPT eligibility is decided ONCE at goal start (plan_position's
  -- scan-once block sets _is_ppt from the initial pill HP). We used
  -- to re-check every tick and demote shoot_pill -> engage if the
  -- pill dropped below threshold mid-attack — that produced a
  -- jarring "I was about to fire and now I'm in the wrong substate
  -- with the wrong aim point and the shield wall still up" moment
  -- that lost shots. Since we already paid the PPT setup cost
  -- (built shield walls, picked corner aim), riding it out for the
  -- rest of the kill is the right call. The non-PPT engage path
  -- only makes sense as the INITIAL choice for a low-HP pill.

  -- ══════════════════════════════════════════════════════════════════
  -- plan_position: full terrain analysis to find best attack spot
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "plan_position" then
    -- Only scan once, reuse stored results for drawing
    if not goal.scan_spots then
      goal._scan_tank_mx = tmx
      goal._scan_tank_my = tmy

      local best_score, spots = M.evaluate_pill_difficulty(pill, world, true, nil, state.phase, state, tmx, tmy)
      goal.scan_spots = spots

      -- Step 1: apply influence bonus/penalty to all LOS spots.
      --   friendly territory (influence > 0): -5 (cheaper)
      --   hostile territory  (influence < 0): +5 (more expensive)
      -- Stored as adj_score for all subsequent ranking.
      for _, s in ipairs(spots) do
        if s.has_los then
          s._influence = cpf.influence_at(s.mx, s.my)
          local infl_adj = 0
          if s._influence > 0 then infl_adj = -5
          elseif s._influence < 0 then infl_adj = 5
          end
          s._infl_adj = infl_adj
          s._adj_score = s.total_score + infl_adj
        end
      end

      -- Step 2: collect all "green" spots (adj_score < 10 with LOS)
      local greens = {}
      for _, s in ipairs(spots) do
        if s.has_los and s._adj_score < 10 then
          greens[#greens + 1] = s
        end
      end

      -- Step 3: no greens — pick top-scoring LOS spot + all within 25% of it.
      -- "Within 25%" means adj_score <= top_score * 1.25 (lower is better).
      if #greens == 0 then
        local los_spots = {}
        for _, s in ipairs(spots) do
          if s.has_los then los_spots[#los_spots + 1] = s end
        end
        table.sort(los_spots, function(a, b) return a._adj_score < b._adj_score end)
        if #los_spots > 0 then
          local top_score = los_spots[1]._adj_score
          local threshold = top_score * 1.25
          for _, s in ipairs(los_spots) do
            if s._adj_score <= threshold then
              greens[#greens + 1] = s
            else
              break  -- sorted, so we can stop
            end
          end
        end
      end

      local best = nil
      if #greens > 0 then
        -- Step 4: rank by real Dijkstra travel cost from the tank.
        -- The bot already maintains a KIND_NORMAL full-map Dijkstra
        -- rooted at its current position, so this is a cached lookup
        -- (no per-spot search) that beats the linear estimate the
        -- shortlist tiebreak used to use.
        local boat = info.inboat and 1 or 0
        for _, s in ipairs(greens) do
          local dij = cpf.dijkstra_lookup_by_kind(cpf.KIND_NORMAL, s.mx, s.my, boat)
          if dij >= 1e29 then
            -- Tile not yet reached by the search (rare on land paths
            -- but possible early in a search or for unreachable spots).
            -- Fall back to the linear estimate so ranking still works.
            dij = cpf.estimate_cost(tmx, tmy, s.mx, s.my, boat)
          end
          s._est = dij
        end
        table.sort(greens, function(a, b) return (a._est or math.huge) < (b._est or math.huge) end)
        best = greens[1]
      end

      -- Log selection
      print(string.format(TAG .. " PLAN SELECT: greens=%d oranges=%s best=%s score=%.1f",
            #greens, best_orange and string.format("%.1f", best_orange.total_score) or "none",
            best and string.format("(%d,%d) %.1f", best.mx, best.my, best.total_score) or "none",
            best and best.total_score or -1))

      if best then
        goal.standoff_mx = best.mx   -- integer tile for A* nav
        goal.standoff_my = best.my
        goal.standoff_fx = best.cx  -- precise float for charge/engage
        goal.standoff_fy = best.cy
        goal._chosen_deg = best.deg
        goal._scan_tick = state.tick   -- frame stamp for staged overlay reveal
        -- Approach position: extend line from pill through standoff by
        -- APPROACH_OFFSET. Stored as both float (precise final target)
        -- and clamped integer tile (for the A* path target).
        local dx = best.cx - (pmx + 0.5)
        local dy = best.cy - (pmy + 0.5)
        local d = math.sqrt(dx * dx + dy * dy)
        if d > 0.01 then
          local ux, uy = dx / d, dy / d
          goal.approach_fx = best.cx + ux * C.ATTACK_APPROACH_OFFSET
          goal.approach_fy = best.cy + uy * C.ATTACK_APPROACH_OFFSET
          goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
          goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
        else
          goal.approach_mx = best.mx
          goal.approach_my = best.my
          goal.approach_fx = best.cx
          goal.approach_fy = best.cy
        end
        if not goal._plan_logged then
          local n_los = 0
          for _, s in ipairs(spots) do if s.has_los then n_los = n_los + 1 end end
          print(string.format(TAG .. " PLAN: pill@(%d,%d) best=(%d,%d) deg=%d score=%.1f (A=%.1f B=%.0f D=%.0f) candidates=%d",
                pmx, pmy, best.mx, best.my, best.deg, best.total_score,
                best.score_a or 0, best.score_b or 0, best.score_d or 0, n_los))
          goal._plan_logged = true
        end

        -- Protected pill take (PPT) gating: high-HP pills use a tighter
        -- standoff and a slower charge so the wall-shielded angle is
        -- preserved. Determined ONCE here for the lifetime of this
        -- attack_pill goal and stashed on the goal for downstream
        -- substates / steering.
        local pill_hp = pill and pill.health or 0
        goal._is_ppt = pill_hp >= (C.PPT_HEALTH_THRESHOLD or 8)
        -- Force PPT when our armour is low enough that taking return
        -- fire during a charge could be lethal, even on a soft pill:
        --   * armour <= ARMOUR_LOW (15): one or two hits from flee.
        --   * armour <= ARMOUR_MODERATE (25) AND chosen standoff is in
        --     a hot threat zone (>= ARMOUR_MOD_PPT_DANGER): mid-armour
        --     plus dangerous approach corridor.
        if not goal._is_ppt then
          local force_low = info.armour <= (C.ARMOUR_LOW or 15)
          local force_mod = false
          if info.armour <= (C.ARMOUR_MODERATE or 25)
             and goal.standoff_mx and goal.standoff_my then
            local sd = threat.at(goal.standoff_mx, goal.standoff_my)
            if sd >= (C.ARMOUR_MOD_PPT_DANGER or 75) then
              force_mod = true
            end
          end
          if force_low or force_mod then
            goal._is_ppt = true
            print(string.format(TAG ..
              " ATTACK: forcing PPT (armour=%d hp=%d reason=%s)",
              info.armour, pill_hp,
              force_low and "LOW_ARMOUR" or "MOD_ARMOUR+HOT_STANDOFF"))
          end
        end
        local scan_radius = goal._is_ppt and C.PPT_STANDOFF
                            or C.ATTACK_PILL_STANDOFF
        -- For PPT, pull the chosen standoff in from 7.4 to 7.0 along
        -- the same angle the planner picked. evaluate_pill_difficulty
        -- already laid the standoff on the 7.4 circle; rescaling keeps
        -- the angle and just trims the radius.
        if goal._is_ppt and goal.standoff_fx and goal.standoff_fy then
          local cdx = goal.standoff_fx - (pmx + 0.5)
          local cdy = goal.standoff_fy - (pmy + 0.5)
          local clen = math.sqrt(cdx * cdx + cdy * cdy)
          if clen > 0.01 then
            local scale = scan_radius / clen
            goal.standoff_fx = (pmx + 0.5) + cdx * scale
            goal.standoff_fy = (pmy + 0.5) + cdy * scale
            goal.standoff_mx = U.mclamp(math.floor(goal.standoff_fx))
            goal.standoff_my = U.mclamp(math.floor(goal.standoff_fy))
          end
        end

        -- Wall-shielded standoff search: scan 8 nearby angles around the
        -- chosen standoff and pick a more cover-protected spot if one
        -- exists. Replaces standoff_* and recomputes approach_* in place
        -- so all downstream substates see the new spot. Stash the scan
        -- result for the upcoming build_walls substate (Phase 2) and for
        -- the in-place viz overlay below.
        -- No-builder mode: if the LGM is dead/parachuting we can't
        -- place new walls, so the shield scan must score only
        -- already-existing cover (walls + friendly pills). If
        -- nothing scores, the demote below kicks PPT off and we
        -- charge unshielded.
        local no_builder = (info.man_status == C.LGM_DEAD)
        local sscan = shield.scan(pill, world,
                                  goal.standoff_mx, goal.standoff_my,
                                  goal._chosen_deg or 0,
                                  goal.standoff_fx, goal.standoff_fy,
                                  scan_radius, no_builder)
        if no_builder and sscan and (not sscan.best or (sscan.best.score or 0) <= 0) then
          print(TAG .. " ATTACK: LGM dead and no existing cover — demoting to no-shield")
          goal._is_ppt = false
          sscan = nil
        end
        if sscan then sscan.created_tick = now end
        goal._shield_scan = sscan
        if sscan and sscan.best then
          local w = sscan.best
          local n_blockers = 0
          if w.best_aim_idx and w.aims[w.best_aim_idx] then
            n_blockers = #w.aims[w.best_aim_idx].blockers
          end
          print(string.format(TAG .. " SHIELD: replacing standoff (%d,%d)->(%d,%d) deg %.1f->%.1f score=%d blockers=%d (aim=%d)",
                goal.standoff_mx, goal.standoff_my, w.mx, w.my,
                goal._chosen_deg or 0, w.deg, w.score,
                n_blockers, w.best_aim_idx or 0))
          goal.standoff_mx = w.mx
          goal.standoff_my = w.my
          goal.standoff_fx = w.cx
          goal.standoff_fy = w.cy
          goal._chosen_deg = w.deg
          goal._scan_tick = state.tick   -- frame stamp for staged overlay reveal
          -- Recompute approach point behind the new standoff.
          local dxn = w.cx - (pmx + 0.5)
          local dyn = w.cy - (pmy + 0.5)
          local dn  = math.sqrt(dxn * dxn + dyn * dyn)
          if dn > 0.01 then
            local ux, uy = dxn / dn, dyn / dn
            goal.approach_fx = w.cx + ux * C.ATTACK_APPROACH_OFFSET
            goal.approach_fy = w.cy + uy * C.ATTACK_APPROACH_OFFSET
            goal.approach_mx = U.mclamp(math.floor(goal.approach_fx))
            goal.approach_my = U.mclamp(math.floor(goal.approach_fy))
          end
          -- Aim/engage default to pill center; override to the exact
          -- corner (or center) the shield scan chose so the tank shoots
          -- through the protected lane it picked, not down the middle.
          -- Use _TILE_FIRE (1 gu deeper than the scoring inset) so the
          -- real gun aim has extra error tolerance vs. the scoring
          -- pick. Same corner index — just shifted further inward.
          local off = shield.AIM_OFFSETS_TILE_FIRE[w.best_aim_idx or 1]
                      or shield.AIM_OFFSETS_TILE_FIRE[1]
          goal.aim_mx = pmx + off[1]
          goal.aim_my = pmy + off[2]
        end
      else
        if not goal._plan_logged then
          print(string.format(TAG .. " PLAN: no candidates for pill@(%d,%d), falling back", pmx, pmy))
          goal._plan_logged = true
        end
        local smx, smy = M.pick_standoff(world, info, pill, state)
        goal.standoff_mx = smx
        goal.standoff_my = smy
      end
    end -- if not goal.scan_spots (scan once)

    -- Transition to approach after 3 ticks. The old 50-tick (1s) hold was
    -- for letting a human inspect the spot-scoring viz; planning is now
    -- reliable enough that we don't need the pause in normal play. 3 ticks
    -- is enough to be catchable when scrubbing a replay. Bump higher
    -- (e.g. 50 for 1 second) if the scoring viz needs live dwell time.
    if goal.standoff_mx then
      if not goal._plan_show_tick then
        goal._plan_show_tick = now
      elseif (now - goal._plan_show_tick) >= 3 then
        -- Pre-flight tree check: PPT plans build wall_shields, each
        -- costs WALL_SHIELD_BUILD_COST trees. Better to gather BEFORE
        -- approaching — once at the standoff, the forest may be on the
        -- other side of the pill / a river / etc., and gathering then
        -- forces the tank to leave its hard-won standoff.
        local need_gather = false
        local trees_needed = 0
        local n_pots = 0
        if goal._is_ppt and goal._shield_scan and goal._shield_scan.best then
          local w = goal._shield_scan.best
          local pots = w.best_aim_idx and w.aims[w.best_aim_idx]
                       and w.aims[w.best_aim_idx].potential_blockers
          if pots and #pots > 0 then
            n_pots = #pots
            trees_needed = n_pots * (C.WALL_SHIELD_BUILD_COST or 2)
            if (info.trees or 0) < trees_needed then
              need_gather = true
            end
          end
        end
        if need_gather then
          goal.substate = "gather_trees"
          goal._trees_for_walls = trees_needed
          goal._gather_start    = now
          goal._gather_last_progress = now
          goal._gather_last_trees    = info.trees or 0
          print(string.format(TAG .. " ATTACK: plan_position -> gather_trees (%d/%d trees for %d walls)",
                info.trees or 0, trees_needed, n_pots))
        else
          goal.substate = "approach"
          print(string.format(TAG .. " ATTACK: plan_position -> approach, standoff=(%d,%d) precise=(%.1f,%.1f)",
                goal.standoff_mx, goal.standoff_my,
                goal.standoff_fx or goal.standoff_mx + 0.5,
                goal.standoff_fy or goal.standoff_my + 0.5))
        end
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- gather_trees: hold position before approach, let LGM harvest nearby
  -- forest until we have enough trees for the planned shield walls.
  -- The builder's existing "gather" mode (set via builder.set_mode) does
  -- the actual work — including all the smarts about reachable forest,
  -- LGM_DEPLOY_DIST cap, and danger-safe path. We just gate the
  -- transition to approach.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "gather_trees" then
    local trees_have = info.trees or 0
    local trees_need = goal._trees_for_walls or 0
    -- Track tree-count progress so we can detect "no nearby forest"
    -- via a no-progress timeout instead of waiting the full hard cap.
    if trees_have ~= goal._gather_last_trees then
      goal._gather_last_trees    = trees_have
      goal._gather_last_progress = now
    end
    local stalled = (now - (goal._gather_last_progress or now)) > 250  -- ~5 s
    local timed_out = (now - (goal._gather_start or now)) > (C.PPT_GATHER_TIMEOUT or 1500)
    if trees_have >= trees_need then
      goal.substate = "approach"
      goal._trees_for_walls = nil
      goal._gather_start    = nil
      goal._gather_last_progress = nil
      goal._gather_last_trees    = nil
      print(string.format(TAG .. " ATTACK: gather_trees -> approach (%d trees stocked)", trees_have))
    elseif stalled or timed_out then
      -- Demote out of PPT entirely so the rest of the attack runs the
      -- legacy non-PPT flow (charge -> engage -> swerve, near-edge aim
      -- via the per-tick fallback in init.lua). Wiping _shield_scan
      -- frees init.lua's aim override to set aim_mx/aim_my from the
      -- pill-edge geometry instead of the corner the scan picked,
      -- which would otherwise be unprotected without walls.
      goal.substate = "approach"
      goal._is_ppt = false
      goal._shield_scan = nil
      local why = stalled and "no nearby forest reachable" or "hard timeout"
      print(string.format(TAG .. " ATTACK: gather_trees -> approach (%s, %d/%d trees) — demoted to no-shield",
            why, trees_have, trees_need))
      goal._trees_for_walls = nil
      goal._gather_start    = nil
      goal._gather_last_progress = nil
      goal._gather_last_trees    = nil
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- Per-tick standoff shot-path sanity check.
  -- Runs for every pre-fire substate where the standoff is committed
  -- but the shot hasn't been taken yet.  Catches pills placed in the
  -- corridor (e.g. by build_pill_strategic racing with build_walls)
  -- and ≥2-wall pile-ups without waiting for LGM to return to tank.
  -- ══════════════════════════════════════════════════════════════════
  do
    local sub = goal.substate
    if pill and goal.standoff_fx and
       (sub == "approach"         or sub == "build_walls"       or
        sub == "aim"              or sub == "in_range_position"  or
        sub == "in_range_aim_pre" or sub == "in_range_aim"       or
        sub == "in_range_aim_finetune") then
      -- Rate-limit to once per ~0.5 s (25 ticks @ 50 Hz).
      goal._sanity_check_tick = goal._sanity_check_tick or 0
      local reason
      if now - goal._sanity_check_tick >= 25 then
        goal._sanity_check_tick = now
        reason = standoff_shot_obstacle(goal, pill, world)
      end
      if reason then
        print(string.format(TAG ..
          " SANITY: shot path blocked (%s) in %s — replanning", reason, sub))
        goal.substate                 = "plan_position"
        goal.scan_spots               = nil
        goal._shield_scan             = nil
        goal._plan_show_tick          = nil
        goal._plan_logged             = nil
        goal._approach_start          = nil
        goal._approach_last_progress  = nil
        goal._approach_last_dist      = nil
        goal._wall_build_list         = nil
        goal._wall_build_idx          = nil
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- approach: navigate to standoff position, brake to stop
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "approach" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      goal.scan_spots = nil
      goal._plan_show_tick = nil
      goal._plan_logged = nil
      print(TAG .. " ATTACK: approach has no standoff, replanning")
    else
      -- First-tick init for approach progress timer (used for the
      -- timeout below). Reset on every entry from another substate.
      if not goal._approach_start then
        goal._approach_start = now
        goal._approach_last_progress = now
        goal._approach_last_dist = nil
      end

      -- Reach the approach point (precise float, 1.5 tiles behind standoff).
      -- Tolerance: 64 wu (1/4 tile) AND speed <= 4.
      local afx = goal.approach_fx or (goal.approach_mx and (goal.approach_mx + 0.5))
                                    or (goal.standoff_mx and (goal.standoff_mx + 0.5))
      local afy = goal.approach_fy or (goal.approach_my and (goal.approach_my + 0.5))
                                    or (goal.standoff_my and (goal.standoff_my + 0.5))
      local awx = math.floor(afx * 256 + 0.5)
      local awy = math.floor(afy * 256 + 0.5)
      local adist = U.wdist(info.tankx, info.tanky, awx, awy)
      -- Closing the distance counts as progress and resets the timer.
      if goal._approach_last_dist == nil or adist < goal._approach_last_dist - 4 then
        goal._approach_last_progress = now
        goal._approach_last_dist     = adist
      end
      -- Was 64 (1/4 tile). At that tolerance the tank brakes early and
      -- coasts to a stop short of the approach point — visible as a
      -- noticeable gap at the green winner-marker. 16 wu (1/16 tile)
      -- forces the tank to creep right up onto the spot.
      local DIST_TOL  = 16
      -- Matches the creep target speed in steering.lua (the tank holds
      -- at 4 inside the 16-wu approach window). Insisting on 0 makes
      -- the substate hang since the tank doesn't decelerate further.
      local SPEED_TOL = 4

      -- HUD overlay near the tank: current distance + threshold so we
      -- can see live what's blocking the transition.
      do
        local twx = info.tankx / 256.0
        local twy = info.tanky / 256.0
        local dist_ok  = adist <= DIST_TOL
        local speed_ok = info.speed <= SPEED_TOL
        local dr, dg, db = dist_ok  and 100 or 255, dist_ok  and 255 or 100, 100
        local sr, sg, sb = speed_ok and 100 or 255, speed_ok and 255 or 100, 100
        viz.text("approach_dist", twx + 1.0, twy - 1.0,
          string.format("dist=%d/%d", adist, DIST_TOL),
          "topleft", dr, dg, db, 255)
        viz.text("approach_dist", twx + 1.0, twy - 0.4,
          string.format("spd=%d/%d", info.speed, SPEED_TOL),
          "topleft", sr, sg, sb, 255)
      end

      if adist <= DIST_TOL and
         effectively_stopped(state, info, now, SPEED_TOL, 5, "approach") then
        -- Shield-scan-driven wall building: prefer the explicit winner;
        -- if there's no winner, fall back to the standoff candidate so
        -- we can still build out its protection slots if any exist.
        local target_for_build = nil
        local why_no_build = "no shield scan"
        if goal._shield_scan then
          target_for_build = goal._shield_scan.best or goal._shield_scan.standoff
          if not goal._shield_scan.best then why_no_build = "no winner; using standoff" end
        end
        local pots = nil
        if target_for_build then
          if target_for_build.best_aim_idx and target_for_build.aims[target_for_build.best_aim_idx] then
            pots = target_for_build.aims[target_for_build.best_aim_idx].potential_blockers
            -- Lock the gun aim onto the chosen corner/center for the
            -- shield target. If best_aim_idx is set, that means at least
            -- one outgoing aim is clean, and that's the lane we want
            -- charge/aim/engage to all use. (Done unconditionally here
            -- in case the standoff itself ends up being target_for_build
            -- — the original best-only path doesn't set aim_mx/my then.)
            local off = shield.AIM_OFFSETS_TILE_FIRE[target_for_build.best_aim_idx]
                        or shield.AIM_OFFSETS_TILE_FIRE[1]
            goal.aim_mx = pmx + off[1]
            goal.aim_my = pmy + off[2]
          else
            why_no_build = "target has no clean aim (all blocked by wall)"
            -- No clean shot through any aim means PPT can't function
            -- (in_range_aim/shoot_pill rely on a chosen corner). Demote
            -- to non-PPT so aim transitions to the regular charge path
            -- instead of in_range_position, which would just sit idle.
            if goal._is_ppt then
              print(TAG .. " ATTACK: demoting shielded -> no-shield (no clean aim available)")
              goal._is_ppt = false
            end
          end
        end
        -- Tree budget check: each wall needs WALL_SHIELD_BUILD_COST
        -- trees. The pre-flight gather_trees substate (entered before
        -- approach in plan_position) should have stocked us by now —
        -- if we're still short here, gathering would either be wasted
        -- (no nearby forest reachable from the standoff) or undoing
        -- the approach we just finished. Degrade to no-shield aim.
        local cost_per_wall = C.WALL_SHIELD_BUILD_COST or 2
        local needs_build = pots and #pots > 0
        -- Wall-building is a PPT-only behavior. If the per-tick health
        -- check has demoted us to non-PPT (pill HP < threshold), the
        -- whole point of shielded charge is moot — slot straight into
        -- the legacy charge path instead of detouring through a build.
        if needs_build and not goal._is_ppt then
          needs_build = false
          why_no_build = "no-shield (pill HP below threshold) — no walls needed"
        end
        local trees_needed = needs_build and (#pots * cost_per_wall) or 0
        if needs_build and (info.trees or 0) < trees_needed then
          needs_build = false
          why_no_build = string.format("not enough trees (%d/%d for %d walls) at standoff",
                                       info.trees or 0, trees_needed, #pots)
        end
        local decision_msg
        if needs_build then
          goal.substate = "build_walls"
          decision_msg = string.format("BUILD_WALLS: %d slots (%s, score=%d, trees=%d/%d)",
                                       #pots, target_for_build.kind or "?",
                                       target_for_build.score or 0,
                                       info.trees or 0, trees_needed)
        else
          goal.substate = "aim"
          goal.aim_tick = now
          goal._aim_locked = nil
          if pots and #pots == 0 then why_no_build = "0 potential blockers (already all built or no slots exist)" end
          decision_msg = "skip build_walls: " .. why_no_build
        end
        -- Approach finished — clear its timer so a future re-entry
        -- starts a fresh countdown.
        goal._approach_start = nil
        goal._approach_last_progress = nil
        goal._approach_last_dist = nil
        print(TAG .. " ATTACK: at approach " .. decision_msg)
        -- Park the message on screen for ~2 seconds (100 ticks @ 50Hz)
        -- so the human can read it without scrubbing the trace.
        goal._build_decision_msg   = decision_msg
        goal._build_decision_until = now + 100
      else
        -- Hard approach timeout: if we make NO net progress closing
        -- the gap to the approach point in APPROACH_GIVE_UP_TICKS
        -- (~10 s @ 50 Hz), bail back to plan_position. Without this,
        -- the stuck-detection whitelist for "approach" can leave the
        -- tank parked indefinitely against an obstacle.
        local APPROACH_GIVE_UP_TICKS = 500
        goal._approach_timeout_total = APPROACH_GIVE_UP_TICKS
        if (now - goal._approach_last_progress) > APPROACH_GIVE_UP_TICKS then
          -- Ban this approach angle on this pill for 3 minutes (9000
          -- ticks @ 50Hz). evaluate_pill_difficulty's per-degree scan
          -- will skip banned buckets so plan_position's next pass picks
          -- a different angle. Bucket to 5° so close-but-not-identical
          -- candidate angles around the failed one are also excluded.
          if goal._chosen_deg then
            local pkey = pmy * 256 + pmx
            local pill_bans = state.banned_pill_angles[pkey]
            if not pill_bans then
              pill_bans = {}
              state.banned_pill_angles[pkey] = pill_bans
            end
            local bucket = math.floor((goal._chosen_deg % 360) / 5) * 5
            pill_bans[bucket] = now + 9000
            print(string.format(TAG .. " ATTACK: banning approach angle %d° on pill (%d,%d) for 3 min",
                  bucket, pmx, pmy))
          end
          print(string.format(TAG .. " ATTACK: approach stalled (no progress in %d ticks, dist=%d), replanning",
                APPROACH_GIVE_UP_TICKS, adist))
          goal.substate = "plan_position"
          goal.scan_spots = nil
          goal._shield_scan = nil
          goal._plan_show_tick = nil
          goal._plan_logged = nil
          goal._approach_start = nil
          goal._approach_last_progress = nil
          goal._approach_last_dist = nil
        end
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- build_walls: tank waits at approach point while LGM builds the
  -- shield walls along the chosen aim's protection slots, closest-to-
  -- pill first so the LGM never has to walk past a freshly built wall
  -- to reach the next site. Sets goal.wall_shield + goal.wall_mx/my
  -- one wall at a time; builder.lua picks them up via the wall_shield
  -- mode (which already knows how to dispatch BUILDMODE_BUILD).
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "build_walls" then
    -- Lazy init: build the wall queue once, sorted closest-to-pill first.
    if not goal._wall_build_list then
      local pots = nil
      if goal._shield_scan and goal._shield_scan.best then
        local w = goal._shield_scan.best
        if w.best_aim_idx and w.aims[w.best_aim_idx] then
          pots = w.aims[w.best_aim_idx].potential_blockers
        end
      end
      pots = pots or {}
      local sorted = {}
      for _, p in ipairs(pots) do sorted[#sorted + 1] = p end
      table.sort(sorted, function(a, b)
        local da = (a.mx - pmx) * (a.mx - pmx) + (a.my - pmy) * (a.my - pmy)
        local db = (b.mx - pmx) * (b.mx - pmx) + (b.my - pmy) * (b.my - pmy)
        return da < db
      end)
      goal._wall_build_list  = sorted
      goal._wall_build_idx   = 1
      goal._wall_build_start = now
      goal._wall_build_last_progress = now
      -- Reset the LGM-progress trackers too, otherwise they retain
      -- state from a previous build attempt on the same goal table
      -- and the give-up timer compares against stale "last seen
      -- making progress" values.
      goal._wall_build_prev_man  = nil
      goal._wall_build_prev_idx  = nil
      goal._wall_idx_started     = nil
      goal._build_decision_msg   = nil
      goal._build_decision_until = nil
      goal._build_timeout_total  = nil
      print(string.format(TAG .. " BUILD_WALLS: queued %d walls (closest-to-pill first)",
                          #sorted))
    end

    local list = goal._wall_build_list
    local idx  = goal._wall_build_idx

    -- Skip walls that are already built (could be by us, ally, or just
    -- pre-existing terrain we re-checked). Counts as progress so the
    -- give-up timer resets.
    while idx <= #list do
      local target = list[idx]
      local tt = U.ttype(target.mx, target.my)
      if tt == C.T_BUILDING or tt == C.T_HALFBUILD then
        idx = idx + 1
        goal._wall_build_last_progress = now
        goal._wall_idx_started = nil  -- reset per-wall sub-timer
      else
        break
      end
    end
    goal._wall_build_idx = idx

    -- Per-wall sub-timeout: if a single queue entry has been the
    -- current target for WALL_STALL_TICKS without finishing, skip to
    -- the next one. Catches unreachable walls (LGM can't get there),
    -- builder safety rejections (danger / no trees / pill nearby),
    -- etc., without waiting on the overall give-up window.
    local WALL_STALL_TICKS = 250  -- ~5 s @ 50 Hz
    if idx <= #list then
      local target = list[idx]
      local cur_tt = U.ttype(target.mx, target.my)
      if not goal._wall_idx_started then
        goal._wall_idx_started = now
        goal._wall_idx_prev_tt = cur_tt
      elseif goal._wall_idx_prev_tt ~= cur_tt then
        -- Tile type changed (e.g. forest harvested → grass) — that's
        -- real LGM progress on this slot. Reset the per-wall timer
        -- so the BUILD round-trip after a harvest doesn't trip the
        -- stall and skip a slot we're actively working on.
        goal._wall_idx_started = now
        goal._wall_idx_prev_tt = cur_tt
      elseif (now - goal._wall_idx_started) > WALL_STALL_TICKS then
        print(string.format(TAG ..
          " BUILD_WALLS: wall %d/%d at (%d,%d) stalled (%d ticks, tt=%d), skipping",
          idx, #list, target.mx, target.my, WALL_STALL_TICKS, cur_tt))
        idx = idx + 1
        goal._wall_build_idx = idx
        goal._wall_idx_started = nil
        goal._wall_idx_prev_tt = nil
      end
    end

    -- LGM round-trip: any change in man_status between ticks counts as
    -- progress. So a successful dispatch (in -> out) AND the LGM coming
    -- back to the tank (out -> in) both reset the give-up timer. Lets
    -- a pillbox-shielded build that requires multiple LGM trips run
    -- as long as the LGM keeps moving.
    local cur_man_status = info.man_status or 0
    if goal._wall_build_prev_man ~= nil and
       goal._wall_build_prev_man ~= cur_man_status then
      goal._wall_build_last_progress = now
      -- Also reset the per-wall stall timer: any LGM transition
      -- (going out / returning to tank) is real work on the current
      -- slot. Without this, a wall that needs harvest+build (two
      -- round-trips ≈ 10s) trips the 5s WALL_STALL even though the
      -- LGM is genuinely moving on its behalf.
      if goal._wall_idx_started then
        goal._wall_idx_started = now
      end
    end
    goal._wall_build_prev_man = cur_man_status
    -- Wall target advancement (idx incremented above) is already
    -- captured by the in-loop progress reset, but record it explicitly
    -- here too for any future case where the queue is mutated outside
    -- this block.
    if goal._wall_build_prev_idx ~= nil and
       goal._wall_build_prev_idx ~= idx then
      goal._wall_build_last_progress = now
    end
    goal._wall_build_prev_idx = idx

    -- Hard timeout: if NO progress (no wall built, no LGM transition)
    -- in BUILD_GIVE_UP_TICKS (~10 sec at 50 Hz), proceed without the
    -- rest. Avoids hanging the attack flow when trees are short, the
    -- LGM is genuinely dead, or the build site is unreachable.
    local BUILD_GIVE_UP_TICKS = 500
    local stalled = (now - goal._wall_build_last_progress) > BUILD_GIVE_UP_TICKS

    -- Stash for the per-tick draw block below; it owns rendering so
    -- the countdown shows even at the moment we transition out (and
    -- stays put across substate flips while the goal still has the
    -- build state populated).
    goal._build_timeout_total = BUILD_GIVE_UP_TICKS

    if idx > #list or stalled then
      if stalled then
        print(string.format(TAG .. " BUILD_WALLS: stalled (no wall built in %d ticks), proceeding to aim",
                            BUILD_GIVE_UP_TICKS))
      else
        print(string.format(TAG .. " BUILD_WALLS: complete after %d ticks, %d walls built, proceeding to aim",
                            now - goal._wall_build_start, #list))
      end
      goal.wall_shield = false
      goal.wall_mx = nil
      goal.wall_my = nil
      goal.substate = "aim"
      goal.aim_tick = now
      goal._aim_locked = nil
    else
      -- Point the builder at the current target wall.
      local target = list[idx]
      goal.wall_shield = true
      goal.wall_mx = target.mx
      goal.wall_my = target.my
    end
    -- Fall through to draw.
  end

  -- ══════════════════════════════════════════════════════════════════
  -- charge: accelerate toward standoff, let auto-slowdown stop us
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "charge" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
    end
    -- Record shell count and pill HP at start of charge for swerve trigger.
    -- Decide intent NOW: if bullets we'll fire >= pill HP, we're going for a
    -- confirmed kill; otherwise we're just chipping (defensive swerve later).
    if not goal._charge_shells then
      goal._charge_shells = info.shells
      local pill_hp_now = pill and pill.health or 15
      goal._bullets_needed = math.min(10, pill_hp_now)
      goal._kill_attempt = (goal._bullets_needed >= pill_hp_now)
      goal._attack_start_tick = now
      -- Stash for the swerve-duration override below — low-HP pills get
      -- shorter swerves since the kill is fast and we don't need a long
      -- evasion window.
      goal._charge_start_hp = pill_hp_now
    end

    -- Immediate swerve: pill dead OR fired enough shots
    local bullets_fired = (goal._charge_shells or info.shells) - info.shells
    local pill_hp = pill and pill.health or 0
    if pill_hp <= 0 or (goal._bullets_needed and bullets_fired >= goal._bullets_needed) then
      goal.substate = "swerve"
      goal._swerve_start = now
      -- Low-HP pills get a much shorter swerve — the kill happens fast,
      -- the pill won't get many (if any) shots off, so a long evasion
      -- just delays the next goal. Lookup by HP at start of charge.
      local LOW_HP_SWERVE = { [1] = 30, [2] = 36, [3] = 40 }
      local low_hp_ticks = LOW_HP_SWERVE[goal._charge_start_hp]
      if low_hp_ticks then
        goal._swerve_ticks_left = low_hp_ticks
        goal._swerve_turn_ticks_left = math.min(low_hp_ticks, C.SWERVE_TURN_TICKS)
      elseif goal._kill_attempt then
        goal._swerve_ticks_left = C.SWERVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
      else
        goal._swerve_ticks_left = C.SWERVE_DEFENSIVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
      end
      goal._swerve_pill_dead = (pill_hp <= 0)
      compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
      goal._swerve_dir = goal._best_swerve_dir or ((now % 2 == 0) and 1 or -1)
      goal._engage_hits = nil
      print(string.format(TAG .. " ATTACK: immediate swerve from charge (fired=%d needed=%d hp=%d kill_attempt=%s start_hp=%s)",
            bullets_fired, goal._bullets_needed or 0, pill_hp,
            tostring(goal._kill_attempt), tostring(goal._charge_start_hp)))
    end
    -- Steering handles movement and transition to engage
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- aim: stopped at standoff, turning to face pill, no shooting
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "aim" then
    -- Check if aimed (steering sets _aim_locked when corr <= 1)
    if goal._aim_locked then
      compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)

      -- Check for trees between tank and crosshairs (pill direction).
      -- Skip detree entirely for shielded pill takes — the shield scan
      -- already picked an aim corner with a clear shot through the
      -- chosen angle, so any trees on the pill-center line aren't on
      -- our actual firing path. Burning shells to clear them just
      -- wastes ammo and time before we can take the corner shot.
      local trees = goal._is_ppt and 0
                    or forest_tiles_on_path(tmx, tmy, pmx, pmy)
      if trees > 0 then
        goal.substate = "detree"
        goal._aim_locked = nil
        goal._detree_shells_at_start = info.shells  -- baseline for actual shots fired
        goal._detree_shots_needed = trees
        print(string.format(TAG .. " ATTACK: aimed, clearing %d trees", trees))
      elseif goal._is_ppt then
        -- Shielded: skip the aggressive charge — move carefully into
        -- range, re-aim precisely, then shoot.
        goal.substate = "in_range_position"
        goal._aim_locked = nil
        print(TAG .. " ATTACK: shielded aimed, moving into firing range")
      else
        goal.substate = "charge"
        goal._aim_locked = nil
        goal._charge_braking = nil
        print(TAG .. " ATTACK: aimed, charging to standoff")
      end
    -- Abort if can't aim within 3 seconds
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: aim timeout, aborting")
      clear_attack_goal(state)
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- detree: shoot trees between tank and pill until clear
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "detree" then
    -- Visualize remaining trees (recount each tick just for the viz)
    local trees_left = forest_tiles_on_path(tmx, tmy, pmx, pmy, true)
    local needed = goal._detree_shots_needed or 0
    -- Count actual shots fired by tracking shell count drops
    local fired = (goal._detree_shells_at_start or info.shells) - info.shells
    -- HUD: detree progress above the tank
    viz.text("detree_progress", tmx + 0.5, tmy - 1.2,
      string.format("DETREE %d/%d (left=%d)", fired, needed, trees_left),
      "center", 255, 255, 100, 255)
    if fired >= needed or fired >= 6 then
      if goal._is_ppt then
        goal.substate = "in_range_position"
        print(string.format(TAG .. " ATTACK: PPT detree done (shots=%d/%d), moving into range",
              fired, needed))
      else
        goal.substate = "charge"
        goal._charge_braking = nil
        print(string.format(TAG .. " ATTACK: detree done (shots=%d/%d), charging",
              fired, needed))
      end
    end
    -- Steering handles shooting; fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_position (PPT): creep to the standoff at the same speed
  -- the approach uses, hold to the same dist+spd tolerance, no firing.
  -- Steering owns the actual movement; attack.lua just decides when to
  -- transition out.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_position" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      goal.scan_spots = nil
      goal._plan_show_tick = nil
      goal._plan_logged = nil
      print(TAG .. " ATTACK: in_range_position has no standoff, replanning")
    else
      local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
      local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
      -- Round-to-nearest (not truncate) so this matches steering's
      -- in_range_position quantization (steering.lua:914-915). Truncating
      -- here while steering rounds caused a 1-wu discrepancy on sub-wu
      -- standoffs (attack viz showed dist=17/16 while steering showed
      -- sdist=16 in the same tick).
      local swx = math.floor(sfx * 256 + 0.5)
      local swy = math.floor(sfy * 256 + 0.5)
      local sdist = U.wdist(info.tankx, info.tanky, swx, swy)
      -- EXPERIMENT: use 16 wu for ALL takes, including 3-blocker. The
      -- old rule tightened to 8 wu when 3+ blockers were in play, on the
      -- theory that the gun-line geometry is more sensitive there. In
      -- practice the tighter window stalled the transition more than it
      -- helped accuracy.
      -- Revert by restoring this line:
      --   local DIST_TOL = (n_blockers >= 3) and 8 or 16
      -- with n_blockers sourced from goal._shield_scan.best.blockers_count.
      local n_blockers = (goal._shield_scan and goal._shield_scan.best
                          and goal._shield_scan.best.blockers_count) or 0
      local DIST_TOL = 12
      local SPEED_TOL = 4
      -- Mirror to a goal field so steering can match the brake
      -- threshold to the same tolerance — without this, steering
      -- brakes at the default 16 wu and a 3-blocker take with
      -- tol=8 will stall just outside the transition window.
      goal._in_range_dist_tol = DIST_TOL

      do
        local twx = info.tankx / 256.0
        local twy = info.tanky / 256.0
        local dist_ok  = sdist <= DIST_TOL
        local speed_ok = info.speed <= SPEED_TOL
        local dr, dg, db = dist_ok  and 100 or 255, dist_ok  and 255 or 100, 100
        local sr, sg, sb = speed_ok and 100 or 255, speed_ok and 255 or 100, 100
        viz.text("approach_dist", twx + 1.0, twy - 1.0,
          string.format("dist=%d/%d", sdist, DIST_TOL),
          "topleft", dr, dg, db, 255)
        viz.text("approach_dist", twx + 1.0, twy - 0.4,
          string.format("spd=%d/%d", info.speed, SPEED_TOL),
          "topleft", sr, sg, sb, 255)
      end

      if sdist <= DIST_TOL and
         effectively_stopped(state, info, now, SPEED_TOL, 5, "in_range_position") then
        -- Compute the pre-aim point: ONE GAME-PIXEL OUTSIDE the
        -- pillbox tile on the same side as the chosen aim corner.
        -- For a center aim there's no offset, the pre-aim IS the
        -- center. The idea: settle the gun on a slightly-overshot
        -- direction first, then refine to the corner inside the
        -- tile. Splits the rotation cleanly so steering doesn't
        -- have to slow down across the lock threshold while the
        -- finetune sim is also sampling the trajectory.
        local PIX = 2.0 / 16.0    -- 2 game-pixels = 32 wu = 1/8 tile
        local fx, fy = goal.aim_mx - pmx, goal.aim_my - pmy
        local pre_dx, pre_dy
        if fx < 0.5 - 1e-3 then
          pre_dx = -PIX                -- corner on left side → outside is further left
        elseif fx > 0.5 + 1e-3 then
          pre_dx = 1.0 + PIX           -- right side → outside is past the right edge
        else
          pre_dx = 0.5                 -- centered: no x offset
        end
        if fy < 0.5 - 1e-3 then
          pre_dy = -PIX
        elseif fy > 0.5 + 1e-3 then
          pre_dy = 1.0 + PIX
        else
          pre_dy = 0.5
        end
        goal.aim_pre_mx = pmx + pre_dx
        goal.aim_pre_my = pmy + pre_dy

        goal.substate        = "in_range_aim_pre"
        goal.aim_tick        = now
        goal._aim_locked     = nil
        goal._pre_aim_locked = nil
        print(string.format(TAG ..
          " ATTACK: PPT in range (%.2f,%.2f) sdist=%d spd=%d, pre-aiming to (%.3f,%.3f)",
          sfx, sfy, sdist, info.speed, goal.aim_pre_mx, goal.aim_pre_my))
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim_pre (PPT): turn to a coarse pre-aim point one game-
  -- pixel OUTSIDE the pillbox tile on the same side as the chosen
  -- corner (or the center, if the chosen aim was already center).
  -- Same lock condition as in_range_aim — once corr <= 1, hand off
  -- to in_range_aim for the final corner aim. No trajectory check
  -- here; that lives in in_range_aim_finetune.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim_pre" then
    -- pre-aim is "I'm pointed at the correct SIDE of the pillbox to
    -- begin fine aiming" — it uses its own _pre_aim_locked flag
    -- (set by steering when corr <= 1 of aim_pre_mx/my). The next
    -- substate (in_range_aim) needs its own fresh _aim_locked, so
    -- we explicitly null both here on transition.
    if goal._pre_aim_locked then
      goal.substate        = "in_range_aim"
      goal.aim_tick        = now
      goal._pre_aim_locked = nil
      goal._aim_locked     = nil
      print(TAG .. " ATTACK: pre-aim locked, refining to chosen corner")
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: shielded in_range_aim_pre timeout, aborting")
      clear_attack_goal(state)
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim (PPT): turn to face the chosen aim point exactly.
  -- Steering does the turning; once aim is locked we simulate the
  -- actual shell trajectory (using info.tank_angle = float, bit-
  -- exact with engine) and verify the pill tile is hit.
  --   - hit  → shoot_pill
  --   - miss → fall back to aiming at pill CENTER and try again
  --   - miss while already aimed at center → abort the pill take
  --     (something changed since plan-time: pill moved, blocker
  --     appeared, range insufficient, etc.)
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim" then
    -- Phase 1: turn to the chosen aim point (corner from the shield
    -- scan). Pure trig in steering — no trajectory simulation here.
    -- Hands off to in_range_aim_finetune the moment steering
    -- reports the corner is locked (corr <= 1).
    if goal._aim_locked then
      goal.substate        = "in_range_aim_finetune"
      goal._finetune_start = now
      goal._finetune_taps  = 0
      print(TAG .. " ATTACK: aim locked, entering in_range_aim_finetune (sim-verify)")
    elseif goal.aim_tick and (now - goal.aim_tick) > 150 then
      print(TAG .. " ATTACK: shielded in_range_aim timeout, aborting")
      clear_attack_goal(state)
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- in_range_aim_finetune (PPT): per-tick trajectory verify.
  --   Success: cpf.simulate_shot_angle(info.tank_angle) — using the
  --     FLOAT tank angle for bit-exact match to the engine's actual
  --     shell flight — produces a tile path that includes the pill
  --     tile. May succeed immediately on the first tick (no tap
  --     needed) if the corner aim already lines up. Otherwise
  --     steering taps 1 brad/tick toward the pill center until the
  --     sim crosses the pill.
  --   Cap: FINETUNE_MAX_TAPS taps OR FINETUNE_TIMEOUT ticks without
  --     a hit → abort the take. We won't fire on a verified miss;
  --     if the geometry won't converge after a fair attempt the
  --     plan is wrong and goal-selector should re-pick.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "in_range_aim_finetune" then
    -- 24 (was 16): _finetune_taps counts every brain tick in finetune,
    -- including the forced-idle ticks from steering's 3-emit/1-idle
    -- burst cap. 24 brain ticks ≈ 18 actual nudges, restoring the
    -- pre-cap budget of ~12+ effective taps for stubborn geometries.
    local FINETUNE_MAX_TAPS = 24
    local FINETUNE_TIMEOUT  = 100  -- ~2 s @ 50 Hz
    local angle_f = info.tank_angle or info.direction
    local path = cpf.simulate_shot_angle(info.tankx, info.tanky, angle_f,
                                         cpf.SHOT_TANK, info.gunrange or 14)
    local on_pill = false
    if path then
      for _, t in ipairs(path) do
        if t.mx == pmx and t.my == pmy then on_pill = true; break end
      end
    end
    goal._finetune_path    = path        -- viz reads these
    goal._finetune_on_pill = on_pill

    if on_pill then
      -- Lock the verified angle as the new aim point so shoot_pill's
      -- corner-correction tap (steering.lua's shoot_pill block) doesn't
      -- pull the angle back toward the original shield-scan corner —
      -- finetune just spent N taps drifting AWAY from that corner to
      -- satisfy the sim, and reverting would immediately invalidate
      -- the verified hit. Project ~16 tiles along the current angle so
      -- shoot_pill's `aim_at_f(tank → aim)` returns essentially the
      -- current direction; corr stays near 0, no further tapping.
      local far_t = 16.0
      local rad   = angle_f * (3.14159265358979323846 / 128.0)
      goal.aim_mx = info.tankx / 256.0 + math.sin(rad) * far_t
      goal.aim_my = info.tanky / 256.0 - math.cos(rad) * far_t
      goal.substate          = "shoot_pill"
      goal._shoot_armour     = info.armour
      goal._shoot_shells     = info.shells
      goal._shoot_hits_total = 0
      goal._shoot_start_tick = now
      -- First steering tick of shoot_pill is forced idle for the same
      -- reason finetune's first tick is: the previous substate may have
      -- been holding a turn key, so the engine's firstLeft/firstRight
      -- ramp counter is unknown. One blank tick guarantees the next
      -- emitted tap starts at /8 instead of full speed.
      goal._shoot_first_steer = true
      print(string.format(TAG ..
        " ATTACK: finetune verified (angle %.2f, %d taps) — opening fire",
        angle_f, goal._finetune_taps or 0))
    elseif (goal._finetune_taps or 0) >= FINETUNE_MAX_TAPS
       or (now - (goal._finetune_start or now)) > FINETUNE_TIMEOUT then
      local n_taps = goal._finetune_taps or 0
      local n_ticks = now - (goal._finetune_start or now)
      print(string.format(TAG ..
        " ATTACK: finetune couldn't line up after %d taps / %d ticks (angle %.2f, pill@(%d,%d)) — aborting pill take",
        n_taps, n_ticks, angle_f, pmx, pmy))
      clear_attack_goal(state, string.format(
        "finetune timeout: taps=%d/%d ticks=%d/%d angle=%.2f pill@(%d,%d)",
        n_taps, FINETUNE_MAX_TAPS, n_ticks, FINETUNE_TIMEOUT,
        angle_f, pmx, pmy))
    end
    -- Else: steering will tap one brad toward pill center this tick.
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- shoot_pill (PPT): park, hold aim, fire — same swerve heuristic as
  -- engage: ATTACK_CURVE_AFTER_HITS hits taken or pill killed.
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "shoot_pill" then
    if not goal._shoot_armour then goal._shoot_armour = info.armour end
    local hits_taken = goal._shoot_armour - info.armour
    goal._shoot_armour = info.armour
    goal._shoot_hits_total = (goal._shoot_hits_total or 0) + hits_taken

    local pill_hp = pill and pill.health or 0

    -- No-progress timeout. shoot_pill has no built-in escape if the
    -- shells are silently missing (trajectory off, friendly LGM in
    -- the lane, pill picked up — all leave pill HP unchanged while
    -- we keep "firing"). Track the highest pill HP we've observed
    -- and the last tick HP went DOWN. If too long without progress,
    -- abort the take so the goal selector can re-plan from scratch.
    --
    -- Threshold: 100 ticks (~2 s @ 50 Hz). Reload is ~0.5 s so we
    -- expect 4 shells fired in that window — none landing means
    -- something's actually wrong, not just bad luck.
    local SHOOT_NO_PROGRESS_TICKS = 150
    if not goal._shoot_progress_hp then
      goal._shoot_progress_hp   = pill_hp
      goal._shoot_progress_tick = now
      goal._shoot_initial_hp    = pill_hp  -- baseline for the kill bar
    elseif pill_hp < goal._shoot_progress_hp then
      goal._shoot_progress_hp   = pill_hp
      goal._shoot_progress_tick = now
    elseif (now - (goal._shoot_progress_tick or now)) > SHOOT_NO_PROGRESS_TICKS
       and pill_hp > 0 then
      print(string.format(TAG ..
        " ATTACK: shoot_pill no progress for %d ticks (pill_hp=%d) — aborting take",
        now - (goal._shoot_progress_tick or now), pill_hp))
      clear_attack_goal(state)
      -- clear_attack_goal mutates state.goal in place (wipes all
      -- non-core fields, sets kind="none"). The local `goal` here
      -- aliases the same table, so downstream reads like
      -- `goal._shoot_hits_total >= C.ATTACK_CURVE_AFTER_HITS` would
      -- compare nil and crash. Bail out of the substate handler
      -- immediately — the goal selector will pick a fresh goal next
      -- tick. (Brain crash on May 1: this exact path, line 2461 of
      -- 1c528b1.)
      return
    end

    -- HUD: live progress toward each of the three exit triggers.
    -- Bar fills as we approach the exit (kill / swerve-from-hits / abort).
    do
      local function bar(frac)
        if frac < 0 then frac = 0 elseif frac > 1 then frac = 1 end
        local n = math.floor(frac * 10 + 0.5)
        return string.rep("#", n) .. string.rep("-", 10 - n)
      end
      local init_hp     = goal._shoot_initial_hp or pill_hp
      local hits_total  = goal._shoot_hits_total or 0
      local curve_after = C.ATTACK_CURVE_AFTER_HITS or 1
      local stale_ticks = now - (goal._shoot_progress_tick or now)
      local kill_frac   = (init_hp > 0) and (1.0 - pill_hp / init_hp) or 1.0

      viz.hud_text("hud_shoot_pill_progress", 10, 210,
        "PPT shoot_pill exits:", "topleft", 255, 220, 100, 255)
      viz.hud_text("hud_shoot_pill_progress", 10, 225,
        string.format(" kill   [%s] hp=%d/%d", bar(kill_frac), pill_hp, init_hp),
        "topleft", 100, 255, 100, 255)
      viz.hud_text("hud_shoot_pill_progress", 10, 240,
        string.format(" swerve [%s] hits=%d/%d",
          bar(hits_total / curve_after), hits_total, curve_after),
        "topleft", 255, 180, 80, 255)
      viz.hud_text("hud_shoot_pill_progress", 10, 255,
        string.format(" abort  [%s] %d/%d ticks since last hp drop",
          bar(stale_ticks / SHOOT_NO_PROGRESS_TICKS),
          stale_ticks, SHOOT_NO_PROGRESS_TICKS),
        "topleft", 255, 120, 120, 255)
    end

    local should_swerve = false
    local pill_dead     = false
    if pill_hp <= 0 then
      should_swerve = true
      pill_dead     = true
    elseif goal._shoot_hits_total >= C.ATTACK_CURVE_AFTER_HITS then
      should_swerve = true
    end

    if should_swerve then
      goal.substate    = "swerve"
      goal._swerve_start = now
      if pill_dead then
        goal._swerve_ticks_left      = C.SWERVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
      else
        goal._swerve_ticks_left      = C.SWERVE_DEFENSIVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
      end
      goal._swerve_pill_dead = pill_dead
      -- PPT skips the legacy aim substate where _best_swerve_dir is
      -- normally computed, so compute it fresh here. Without this,
      -- the `or random` fallback below would coin-flip the swerve
      -- direction and could send the tank into a hazard.
      compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
      goal._swerve_dir       = goal._best_swerve_dir
                               or ((now % 2 == 0) and 1 or -1)
      print(string.format(TAG .. " ATTACK: PPT shoot_pill -> swerve (hits=%d hp=%d dead=%s)",
            goal._shoot_hits_total or 0, pill_hp, tostring(pill_dead)))
    end
    -- Steering handles braking, aim hold, and shooting; fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- engage: stationary, fire at pill
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "engage" then
    -- Track armour to detect incoming hits
    if not goal._engage_armour then goal._engage_armour = info.armour end
    local hits_taken = goal._engage_armour - info.armour
    goal._engage_armour = info.armour

    -- Track bullets fired since charge started
    local bullets_fired = (goal._charge_shells or info.shells) - info.shells
    local pill_hp = pill and pill.health or 0

    -- Immediate swerve: pill dead OR fired enough shots
    if pill_hp <= 0 or (goal._bullets_needed and bullets_fired >= goal._bullets_needed) then
      goal.substate = "swerve"
      goal._swerve_start = now
      -- Low-HP shortcut from charge_start_hp (see charge handler).
      local LOW_HP_SWERVE = { [1] = 30, [2] = 36, [3] = 40 }
      local low_hp_ticks = LOW_HP_SWERVE[goal._charge_start_hp]
      if low_hp_ticks then
        goal._swerve_ticks_left = low_hp_ticks
        goal._swerve_turn_ticks_left = math.min(low_hp_ticks, C.SWERVE_TURN_TICKS)
      elseif goal._kill_attempt then
        goal._swerve_ticks_left = C.SWERVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
      else
        goal._swerve_ticks_left = C.SWERVE_DEFENSIVE_TOTAL_TICKS
        goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
      end
      goal._swerve_pill_dead = (pill_hp <= 0)
      compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
      goal._swerve_dir = goal._best_swerve_dir or ((now % 2 == 0) and 1 or -1)
      goal._engage_hits = nil
      print(string.format(TAG .. " ATTACK: immediate swerve (fired=%d needed=%d hp=%d kill_attempt=%s start_hp=%s)",
            bullets_fired, goal._bullets_needed or 0, pill_hp,
            tostring(goal._kill_attempt), tostring(goal._charge_start_hp)))
    else
      -- Count cumulative hits taken during this engage
      goal._engage_hits = (goal._engage_hits or 0) + hits_taken

      -- Swerve early: after taking ATTACK_CURVE_AFTER_HITS hits, dodge
      -- This triggers while crosshairs are still on pill — preemptive evasion
      local should_swerve = goal._engage_hits >= C.ATTACK_CURVE_AFTER_HITS

      -- Also check if crosshairs off pill (knocked out of range)
      -- Float angle so the displayed crosshair matches the engine's
      -- actual firing direction; info.direction is a floor and would
      -- show the crosshair offset from the true shell path.
      local cx, cy = U.crosshair_at(info.tankx, info.tanky,
                                    info.tank_angle or info.direction, 7.0)
      local cdx, cdy = cx - (pmx + 0.5), cy - (pmy + 0.5)
      local crosshairs_off = math.sqrt(cdx * cdx + cdy * cdy) > 0.7

      if should_swerve or crosshairs_off then
        local pill_anger = pill and pill.anger or 0
        if pill_anger > C.ANGER_ATTACK_THRESHOLD then
          -- Pill is angry — swerve to dodge
          goal.substate = "swerve"
          goal._swerve_start = now
          -- Defensive swerve (pill still alive): longer turn
          goal._swerve_ticks_left = C.SWERVE_DEFENSIVE_TOTAL_TICKS
          goal._swerve_turn_ticks_left = C.SWERVE_DEFENSIVE_TURN_TICKS
          goal._swerve_pill_dead = false
          compute_best_swerve_dir(goal, world, pmx, pmy, tmx, tmy)
      goal._swerve_dir = goal._best_swerve_dir or ((now % 2 == 0) and 1 or -1)
          print(string.format(TAG .. " ATTACK: swerving (hits=%d anger=%.2f xhair_off=%s)",
                goal._engage_hits or 0, pill_anger, tostring(crosshairs_off)))
        else
          -- Pill is calm — go straight to loiter/refuel decision
          goal.substate = "post_engage"
          goal._post_engage_tick = now
        end
      -- Abort if can't aim within 3 seconds (~150 ticks)
      elseif goal.engage_tick and not goal._engage_aimed
             and (now - goal.engage_tick) > 150 then
        print(TAG .. " ATTACK: engage timeout — can't aim, aborting")
        clear_attack_goal(state)
      end
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- loiter: wait outside pill range for anger to decay, then re-engage
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "loiter" then
    if not pill or pill.health <= 0 then
      -- Pill killed (by us-via-leftover-shells or by an ally) while
      -- we waited. Release to capture_pill via the normal selector
      -- instead of locking into rush — see swerve completion above.
      clear_attack_goal(state)
      print(TAG .. " ATTACK: pill died during loiter — releasing to capture_pill")
    elseif pill.anger and pill.anger < C.ANGER_ATTACK_THRESHOLD then
      -- Pill calmed down — re-engage with a FULL fresh plan. We have
      -- to wipe everything plan_position will re-derive (scan grid,
      -- PPT decision, shield-scan, chosen aim, ranged-armour
      -- counters) so a take that was PPT against a tough pill
      -- doesn't re-enter PPT machinery against the same pill now at
      -- 1-2 HP — the right move on a weakened pill is the cheap
      -- engage path, but only plan_position knows that.
      goal.substate              = "plan_position"
      goal.scan_spots            = nil
      goal._plan_show_tick       = nil
      goal._plan_logged          = nil
      goal._is_ppt               = nil
      goal._shield_scan          = nil
      goal._aim_locked           = nil
      goal._pre_aim_locked       = nil
      goal._finetune_taps        = nil
      goal._finetune_start       = nil
      goal._finetune_path        = nil
      goal._finetune_on_pill     = nil
      goal.aim_mx                = nil
      goal.aim_my                = nil
      goal._shoot_armour         = nil
      goal._shoot_shells         = nil
      goal._shoot_hits_total     = nil
      goal._shoot_progress_hp    = nil
      goal._shoot_progress_tick  = nil
      goal._engage_armour        = nil
      goal._engage_hits          = nil
      goal._charge_braking       = nil
      goal._charge_shells        = nil
      goal._charge_start_hp      = nil
      goal._kill_attempt         = nil
      goal._bullets_needed       = nil
      goal._on_target_in_flight  = nil
      goal._swerve_extends       = nil
      print(string.format(TAG ..
        " ATTACK: pill cooled (anger=%.2f, hp=%d), re-planning from scratch",
        pill.anger, pill.health or 0))
    elseif goal._loiter_start and (now - goal._loiter_start) > C.ANGER_WAIT_MAX then
      -- Waited too long — give up, go refuel
      state.wounded_pill = { id = goal.target_id, mx = pmx, my = pmy, hp = pill.health, tick = now, owner = pill.owner }
      print(TAG .. " ATTACK: loiter timeout, abandoning")
      clear_attack_goal(state)
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- swerve: hard turn to dodge pill's predictive aim after engage
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "swerve" then
    -- If pill dies mid-swerve, shorten the turn portion to 25 ticks remaining
    if not goal._swerve_pill_dead then
      local pill_now_dead = (not pill or pill.health <= 0)
      if pill_now_dead then
        goal._swerve_pill_dead = true
        if (goal._swerve_turn_ticks_left or 0) > C.SWERVE_TURN_TICKS then
          goal._swerve_turn_ticks_left = C.SWERVE_TURN_TICKS
          if (goal._swerve_ticks_left or 0) > C.SWERVE_TOTAL_TICKS then
            goal._swerve_ticks_left = C.SWERVE_TOTAL_TICKS
          end
          print(TAG .. " ATTACK: swerve shortened — pill confirmed dead")
        end
      end
    end
    goal._swerve_ticks_left = (goal._swerve_ticks_left or 1) - 1
    if (goal._swerve_turn_ticks_left or 0) > 0 then
      goal._swerve_turn_ticks_left = goal._swerve_turn_ticks_left - 1
    end
    if goal._swerve_ticks_left <= 0 then
      -- Swerve done — check if pill died
      if not pill or pill.health <= 0 then
        -- Drop the attack_pill goal entirely. The dead pill will
        -- pop into pool 11 (capture_pill) on the next replan, win
        -- via normal hysteresis (it's the natural follow-on so the
        -- goal-group lock favors it), and the tank will drive in
        -- to grab it through standard nav. Going through the goal
        -- selector lets a genuinely higher-priority goal (flee,
        -- urgent rescue) interrupt — the old "rush" substate
        -- locked us to this pill no matter what.
        clear_attack_goal(state)
        print(TAG .. " ATTACK: swerve done, pill dead — releasing to capture_pill")
      elseif (goal._on_target_in_flight or 0) >= pill.health then
        -- Pill still alive but enough on-target shells are in flight
        -- to expect a kill. Don't drop into post_engage yet (it'd
        -- pick loiter/refuel based on the pill being "alive" before
        -- our inbound shells finish the job). Extend the swerve a
        -- bit and recheck — usually the in-flight shots resolve
        -- within a tick or two. Cap the extensions so a stuck
        -- in-flight count can't loop forever.
        goal._swerve_extends = (goal._swerve_extends or 0) + 1
        if goal._swerve_extends <= 30 then
          goal._swerve_ticks_left = 1
          if goal._swerve_extends == 1 then
            print(string.format(TAG ..
              " ATTACK: swerve extending — %d on-target in flight vs hp=%d",
              goal._on_target_in_flight or 0, pill.health))
          end
        else
          -- Give up extending; treat as a real swerve completion.
          if state.command_goal then
            print(TAG .. " ATTACK: command pill take done — releasing command, replanning")
            state.command_goal = nil
            clear_attack_goal(state)
          else
            goal.substate = "post_engage"
            goal._post_engage_tick = now
            print(TAG .. " ATTACK: swerve done (extends exhausted), evaluating next move")
          end
        end
      else
        -- If this was a manual command (ctrl-click), clear it so normal
        -- goal selection takes over instead of loitering/refueling.
        if state.command_goal then
          print(TAG .. " ATTACK: command pill take done — releasing command, replanning")
          state.command_goal = nil
          clear_attack_goal(state)
        else
          goal.substate = "post_engage"
          goal._post_engage_tick = now
          print(TAG .. " ATTACK: swerve done, evaluating next move")
        end
      end
    end
    -- HUD overlay: show raw swerve goal._* values (screen-relative)
    viz.hud_text("hud_swerve_debug", 10, 80,  "SWERVE", "topleft", 255, 255, 100, 255)
    viz.hud_text("hud_swerve_debug", 10, 100, "_swerve_ticks_left=" .. tostring(goal._swerve_ticks_left), "topleft", 255, 255, 100, 255)
    viz.hud_text("hud_swerve_debug", 10, 120, "_swerve_turn_ticks_left=" .. tostring(goal._swerve_turn_ticks_left), "topleft", 255, 255, 100, 255)
    viz.hud_text("hud_swerve_debug", 10, 140, "_swerve_pill_dead=" .. tostring(goal._swerve_pill_dead), "topleft", 255, 255, 100, 255)
    -- During swerve: do NOT check pill health or allow any interrupts.
    -- Swerve MUST complete to minimize damage taken.
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- post_engage: decide loiter vs refuel after engage/swerve
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "post_engage" then
    local pill_anger = pill and pill.anger or 1.0
    local ticks_to_calm = math.max(0, (pill_anger - C.ANGER_ATTACK_THRESHOLD) * C.PILL_ANGER_DECAY)

    local refuel_cost = math.huge
    for _, b in pairs(world.bases) do
      if b.owner == "friendly" then
        local there = cpf.estimate_cost(tmx, tmy, b.mx, b.my, info.inboat and 1 or 0)
        local back  = cpf.estimate_cost(b.mx, b.my, pmx, pmy, 0)
        local trip = there + back + 100
        if trip < refuel_cost then refuel_cost = trip end
      end
    end

    if ticks_to_calm < refuel_cost and ticks_to_calm < C.ANGER_WAIT_MAX then
      goal.substate = "loiter"
      goal._loiter_start = now
      print(string.format(TAG .. " ATTACK: loitering (wait=%d vs refuel=%d)",
            math.floor(ticks_to_calm), refuel_cost < math.huge and math.floor(refuel_cost) or 99999))
    else
      state.wounded_pill = { id = goal.target_id, mx = pmx, my = pmy, hp = pill and pill.health or 0, tick = now, owner = pill and pill.owner or nil }
      print(string.format(TAG .. " ATTACK: refueling (wait=%d vs refuel=%d)",
            math.floor(ticks_to_calm), refuel_cost < math.huge and math.floor(refuel_cost) or 99999))
      clear_attack_goal(state)
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- rush: pill dead, drive to pill tile to capture
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "rush" then
    if pill and pill.health > 0 then
      -- Pill came back (repaired?), re-engage
      goal.substate = "engage"
    end
    -- Fall through to draw
  end

  -- ══════════════════════════════════════════════════════════════════
  -- DRAW swerve direction choice (left/right cover sample lines)
  -- ══════════════════════════════════════════════════════════════════
  if goal._swerve_viz then
    local sv = goal._swerve_viz
    -- Color: chosen side bright green, unchosen dim red
    local lr, lg, lb = (sv.chosen == 1) and 50  or 200,
                       (sv.chosen == 1) and 255 or 50,
                       50
    local rr, rg, rb = (sv.chosen == -1) and 50  or 200,
                       (sv.chosen == -1) and 255 or 50,
                       50
    -- Mark the sample point with a small dot circle
    viz.circle("swerve_dir_choice", sv.lfx, sv.lfy, 0.25, lr, lg, lb, 220)
    viz.circle("swerve_dir_choice", sv.rfx, sv.rfy, 0.25, rr, rg, rb, 220)
    -- Blue outlines around each tile considered for cover (re-walk for viz)
    local function noop() end
    U.line_walk(sv.lfx, sv.lfy, sv.pcx, sv.pcy, noop, {50, 100, 255, 200}, "bpc_cover_samples")
    U.line_walk(sv.rfx, sv.rfy, sv.pcx, sv.pcy, noop, {50, 100, 255, 200}, "bpc_cover_samples")
    -- Draw the LOS line from each sample to the pill
    viz.line("swerve_dir_choice", sv.lfx, sv.lfy, sv.pcx, sv.pcy, lr, lg, lb, 200)
    viz.line("swerve_dir_choice", sv.rfx, sv.rfy, sv.pcx, sv.pcy, rr, rg, rb, 200)
    -- Cover scores at each sample
    viz.text("swerve_dir_choice", sv.lfx, sv.lfy - 0.3, "L=" .. sv.left_cover,  "center", lr, lg, lb, 255)
    viz.text("swerve_dir_choice", sv.rfx, sv.rfy - 0.3, "R=" .. sv.right_cover, "center", rr, rg, rb, 255)
  end

  -- Shield-scan overlay: 8 candidate spots + winner blocker tiles.
  -- Drawn while planning so a human can see where the alternate
  -- standoffs landed and what's giving cover. Draws every tick the
  -- goal still owns _shield_scan.
  if goal._shield_scan then
    shield.draw_overlay(goal._shield_scan, now)

    -- Pronounced TARGET marker on the chosen aim point. Drawn on top
    -- of the per-aim borders so the user can verify the corner the
    -- shield scan actually chose vs which one charge/shoot is using.
    -- Stays visible during all PPT substates (in_range_position /
    -- in_range_aim / shoot_pill) and the legacy aim/charge/engage
    -- substates too.
    if goal.aim_mx and goal.aim_my then
      local ax, ay = goal.aim_mx, goal.aim_my
      -- Three concentric magenta circles + crosshair lines. Sized so the
      -- visible mass fits inside a single pillbox tile — at the previous
      -- 0.45-tile radius, ~85% of the rings extended outside the tile
      -- when the aim was a corner (pmx+0.06, pmy+0.06), making the
      -- marker look like it was sitting OUTSIDE the pill.
      viz.circle("pill_take_target", ax, ay, 0.10, 255, 0, 255, 230)
      viz.circle("pill_take_target", ax, ay, 0.06, 255, 0, 255, 240)
      viz.circle("pill_take_target", ax, ay, 0.03, 255, 0, 255, 255)
      viz.line("pill_take_target", ax - 0.18, ay,        ax + 0.18, ay,        255, 0, 255, 255)
      viz.line("pill_take_target", ax,         ay - 0.18, ax,         ay + 0.18, 255, 0, 255, 255)
      viz.text("pill_take_target", ax + 0.12, ay - 0.18, "TARGET",
                   "topleft", 255, 100, 255, 255, 0.35)
      -- Live aim accuracy: how many bradians the tank's current
      -- direction is off from a perfect aim at (ax, ay), and the
      -- finetune verdict (does cpf.simulate_shot say the trajectory
      -- crosses the pill tile?). 1 brad = engine quantum, so values
      -- below 1 are unactable but useful as ground truth.
      local aim_dir = U.aim_at_f(info.tankx / 256.0, info.tanky / 256.0,
                                 ax, ay)
      local corr    = U.adiff(info.direction, aim_dir)
      local on_pill = goal._finetune_on_pill
      local on_str
      if on_pill == nil then
        on_str = ""
      elseif on_pill then
        on_str = "  [on pill]"
      else
        on_str = "  [OFF]"
      end
      local cr, cg, cb = 255, 100, 255
      if on_pill then cr, cg, cb = 80, 255, 120 end
      if on_pill == false then cr, cg, cb = 255, 200, 80 end
      viz.text("pill_take_target", ax + 0.12, ay + 0.04,
               string.format("corr=%.2f<=1%s", corr, on_str),
               "topleft", cr, cg, cb, 255, 0.30)

      -- Substate progress indicator. Surfaces:
      --   in_range_aim_pre       → PRE-AIM rotating to the right
      --                            side of the pill (orange while
      --                            still rotating, green once
      --                            _pre_aim_locked); shows the aim
      --                            timeout countdown so the user
      --                            can see how close to abort.
      --   in_range_aim           → AIM rotating to the chosen
      --                            corner. Same color/timeout
      --                            convention as PRE-AIM.
      --   in_range_aim_finetune  → FINETUNE per-tick sim verify;
      --                            green when sim hits, orange
      --                            while tapping, with tap counter.
      --   else                   → blank "(awaiting aim lock)".
      local mode_str
      local mr, mg, mb = 200, 200, 200
      if goal.substate == "in_range_aim_pre" then
        local elapsed   = (goal.aim_tick and (now - goal.aim_tick)) or 0
        local timeout   = 150
        if goal._pre_aim_locked then
          mode_str = string.format("PRE-AIM: locked (%dt → AIM)", elapsed)
          mr, mg, mb = 80, 255, 120
        else
          mode_str = string.format("PRE-AIM: rotating (%dt / %dt timeout)",
                                   elapsed, timeout)
          mr, mg, mb = 255, 200, 80
        end
      elseif goal.substate == "in_range_aim" then
        local elapsed   = (goal.aim_tick and (now - goal.aim_tick)) or 0
        local timeout   = 150
        if goal._aim_locked then
          mode_str = string.format("AIM: locked (%dt → FINETUNE)", elapsed)
          mr, mg, mb = 80, 255, 120
        else
          mode_str = string.format("AIM: rotating to corner (%dt / %dt timeout)",
                                   elapsed, timeout)
          mr, mg, mb = 255, 200, 80
        end
      elseif goal.substate == "in_range_aim_finetune" then
        if on_pill then
          mode_str = string.format("FINETUNE: locked (%d taps)",
                                   goal._finetune_taps or 0)
          mr, mg, mb = 80, 255, 120
        else
          -- burst counter: 0..3 inside the tap branch; reaches 3 right
          -- before the forced idle. "next: idle" when burst == 3,
          -- "next: tap (Nt left)" otherwise. Hold ticks set burst=3 to
          -- force an idle on the hold→tap boundary, so the same label
          -- doubles as the post-hold cool-down indicator.
          local burst = goal._finetune_burst or 0
          local next_str
          if burst >= 3 then
            next_str = "next: idle"
          else
            next_str = string.format("next: tap (%dt to skip)", 3 - burst)
          end
          mode_str = string.format("FINETUNE: tap-to-center (%d taps, %s)",
                                   goal._finetune_taps or 0, next_str)
          mr, mg, mb = 255, 200, 80
        end
      else
        mode_str = "(awaiting aim lock)"
      end
      viz.text("pill_take_target", ax + 0.12, ay + 0.16, mode_str,
               "topleft", mr, mg, mb, 255, 0.28)
    end
  end

  -- Build-walls decision banner: re-emit each tick until expiry so the
  -- text actually persists on screen (overlay commands are cleared per
  -- frame). 100 ticks ~= 2 seconds at the 50 Hz sim rate.
  if goal._build_decision_msg and goal._build_decision_until and
     now < goal._build_decision_until then
    local twx = info.tankx / 256.0
    local twy = info.tanky / 256.0
    viz.text("build_decision_banner", twx, twy - 2.0, goal._build_decision_msg,
                 "center", 255, 240, 100, 255, 0.5)
  end

  -- Build-walls status / timeout countdown. Drawn at TWO positions so
  -- it's always visible: once next to the standoff (in case the camera
  -- is on the pill), once next to the tank (in case the camera is on
  -- the tank). Shows three states:
  --   - countdown (queue active)
  --   - "build pending" (scan winner exists, queue not yet built)
  --   - nothing if no shield scan at all
  if goal._shield_scan then
    -- Color helper: drains green -> yellow -> red as fraction goes
    -- from 1 (fresh) to 0 (timeout).
    local function color_for(frac)
      if frac > 0.5 then return 100, 230, 100
      elseif frac > 0.2 then return 230, 220, 80
      else return 230, 60, 60 end
    end

    local labels = {}  -- list of {label, r, g, b, viz_id}

    -- Approach timeout while in approach substate.
    if goal.substate == "approach" and goal._approach_last_progress
       and goal._approach_timeout_total then
      local total = goal._approach_timeout_total
      local ticks_left = total - (now - goal._approach_last_progress)
      if ticks_left < 0 then ticks_left = 0 end
      local r, g, b = color_for(ticks_left / total)
      labels[#labels + 1] = {
        string.format("APPROACH t-%d (%.1fs)", ticks_left, ticks_left / 50.0),
        r, g, b, "build_status" }
    end

    -- Build-walls: countdown if active, "pending" hint otherwise.
    if goal._wall_build_list and goal._wall_build_last_progress
       and goal._build_timeout_total then
      local total = goal._build_timeout_total
      local ticks_left = total - (now - goal._wall_build_last_progress)
      if ticks_left < 0 then ticks_left = 0 end
      local r, g, b = color_for(ticks_left / total)
      local idx_now = goal._wall_build_idx or 0
      labels[#labels + 1] = {
        string.format("BUILD t-%d (%.1fs)  q=%d/%d",
                      ticks_left, ticks_left / 50.0, idx_now,
                      #goal._wall_build_list),
        r, g, b, "build_status" }
      -- Per-wall stall timer (WALL_STALL_TICKS=250). Tile-type
      -- change (forest→grass after harvest, grass→half-build after
      -- build start) resets it; only fires when the LGM is genuinely
      -- not progressing on this slot.
      if goal._wall_idx_started then
        local STALL = 250
        local stall_left = STALL - (now - goal._wall_idx_started)
        if stall_left < 0 then stall_left = 0 end
        local sr, sg, sb = color_for(stall_left / STALL)
        local cur_target = goal._wall_build_list[idx_now]
        local tt_str = (goal._wall_idx_prev_tt ~= nil)
          and string.format(" tt=%d", goal._wall_idx_prev_tt) or ""
        labels[#labels + 1] = {
          string.format("WALL t-%d (%.1fs) @(%d,%d)%s",
                        stall_left, stall_left / 50.0,
                        cur_target and cur_target.mx or -1,
                        cur_target and cur_target.my or -1,
                        tt_str),
          sr, sg, sb, "build_status" }
      end
    elseif goal.substate ~= "approach" then
      -- Don't double up with the APPROACH timer — only show the
      -- "BUILD pending" line once approach is finished.
      labels[#labels + 1] = {
        string.format("BUILD pending (sub=%s)", goal.substate or "?"),
        180, 180, 180, "build_status" }
    end

    -- Wall-shield builder skip reason — surfaces silent gate failures
    -- (no trees, angry pill, LGM can't reach, path unsafe) right next
    -- to the build queue so the user sees why a wall isn't going up
    -- instead of staring at an idle tank.
    if goal.substate == "build_walls" and state._wall_shield_skip
       and (now - state._wall_shield_skip.tick) < 30 then
      local s = state._wall_shield_skip
      local parts = {}
      if s.angry_pill_close then parts[#parts + 1] = "ANGRY_PILL" end
      if not s.has_trees    then parts[#parts + 1] =
        string.format("TREES(%d/%d)", s.trees_have, s.trees_need) end
      if not s.can_reach    then parts[#parts + 1] = "NO_REACH" end
      if not s.path_safe    then parts[#parts + 1] = "UNSAFE_PATH" end
      if #parts > 0 then
        labels[#labels + 1] = {
          "WALL_SKIP: " .. table.concat(parts, " ") ..
            string.format("  @(%d,%d)", s.wx or 0, s.wy or 0),
          230, 80, 80, "wall_skip_reason" }
      end
    end

    -- Anchor labels near the TANK (not 4 tiles diagonally away from
    -- the standoff — that put them off-screen at normal zoom and the
    -- user reported never seeing the BUILD countdown). Stack 0.6 tile
    -- per line so multiple labels (BUILD + WALL_SKIP + APPROACH) read
    -- vertically.
    do
      local twx = info.tankx / 256.0
      local twy = info.tanky / 256.0
      for i, lbl in ipairs(labels) do
        viz.text(lbl[5],
                     twx + 1.0,
                     twy + 1.5 + (i - 1) * 0.6,
                     lbl[1], "topleft", lbl[2], lbl[3], lbl[4], 255, 0.5)
      end
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- DRAW scan results every tick (persisted in goal.scan_spots)
  -- ══════════════════════════════════════════════════════════════════
  if goal.scan_spots then
    -- To see candidate spots for ALL pills (not just the goal), enable
    -- attack_scan_spots_all_pills — the pool-6 evaluator emits per-pill
    -- overlays on its eval tick.
    -- Staged reveal: ticks since the scan completed control how much
    -- of the candidate set is shown.
    --   age 0 → all spots (full scoring grid)
    --   age 1 → only spots in the winning score bucket
    --   age ≥ 2 → only the chosen winner
    -- Falls back to all-then-winner if no scan tick was stamped.
    local now_tick = state and state.tick or 0
    local age = goal._scan_tick and (now_tick - goal._scan_tick) or math.huge
    local mode
    if age <= 0 then mode = "all"
    elseif age == 1 then mode = "bucket"
    else mode = "winner" end
    -- Goal pill renders bolder than pool-6 candidates (alpha_scale 1.5)
    -- via the shared renderer, so any spot-overlay style change happens
    -- in one place.
    M.draw_pill_eval_spots(goal.scan_spots, pmx, pmy,
                           "attack_scan_spots", mode, goal._chosen_deg, 1.5)
    -- Legend for scoring components (only on the all-spots frame)
    if mode == "all" then
      local lx, ly = pmx + 12, pmy - 6
      viz.text("attack_scan_spots", lx, ly,       "Position Score Legend:", "topleft", 255, 200, 0, 255)
      viz.text("attack_scan_spots", lx, ly + 0.7, "A = avg danger in maneuver area", "topleft", 255, 0, 255, 255)
      viz.text("attack_scan_spots", lx, ly + 1.4, "B = hotspot penalty (any tile >= " .. C.ATTACK_DANGER_HOTSPOT .. ")", "topleft", 255, 0, 255, 255)
      viz.text("attack_scan_spots", lx, ly + 2.1, "D = terrain penalty", "topleft", 255, 0, 255, 255)
      viz.text("attack_scan_spots", lx, ly + 2.8, "E = crossfire 10*(N-1) pills", "topleft", 255, 0, 255, 255)
    end

    -- Draw line from scan-time tank position to chosen standoff
    -- (C scores are relative to this position, not current tank position)
    local stmx = goal._scan_tank_mx or tmx
    local stmy = goal._scan_tank_my or tmy
    if goal.standoff_mx then
      viz.line("attack_scan_spots", stmx + 0.5, stmy + 0.5,
                   goal.standoff_mx + 0.5, goal.standoff_my + 0.5, 0, 255, 100, 200)
    end
    -- Mark scan-time tank position (small white square)
    viz.rect("attack_scan_spots", stmx + 0.2, stmy + 0.2, stmx + 0.8, stmy + 0.8, 255, 255, 255, 150)
    -- Mark chosen standoff spot (green) at precise position
    if goal.standoff_fx then
      viz.circle("attack_scan_spots", goal.standoff_fx, goal.standoff_fy, 0.3, 0, 255, 100, 255)
    elseif goal.standoff_mx then
      viz.circle("attack_scan_spots", goal.standoff_mx + 0.5, goal.standoff_my + 0.5, 0.3, 0, 255, 100, 255)
    end
    -- Mark approach point (purple) at the precise float position.
    -- Falls back to tile center if only the integer fields are present.
    if (goal.approach_fx or goal.approach_mx) then
      local afx = goal.approach_fx or (goal.approach_mx + 0.5)
      local afy = goal.approach_fy or (goal.approach_my + 0.5)
      viz.circle("attack_scan_spots", afx, afy, 0.3, 180, 0, 255, 200)
      -- Line from standoff (or pill) through the approach point so the
      -- geometry is visible end-to-end without any tile-snapping.
      local sfx = goal.standoff_fx or (goal.standoff_mx and (goal.standoff_mx + 0.5)) or (goal.mx + 0.5)
      local sfy = goal.standoff_fy or (goal.standoff_my and (goal.standoff_my + 0.5)) or (goal.my + 0.5)
      viz.line("attack_scan_spots", sfx, sfy, afx, afy, 180, 0, 255, 180)
    end
  end

  -- ══════════════════════════════════════════════════════════════════
  -- Persistent chosen-standoff marker: solid beige disc + pill id label.
  -- Drawn every tick the attack goal has a standoff, so it stays visible
  -- until the standoff is replaced (e.g. shield re-pick) or the goal ends.
  -- Disc faked with concentric outline circles since the overlay primitive
  -- has no fill mode.
  -- ══════════════════════════════════════════════════════════════════
  if goal.standoff_fx or goal.standoff_mx then
    local sfx = goal.standoff_fx or (goal.standoff_mx + 0.5)
    local sfy = goal.standoff_fy or (goal.standoff_my + 0.5)
    local R, G, B = 245, 222, 179   -- beige
    for i = 0, 10 do
      viz.circle("attack_chosen_standoff_marker",
                 sfx, sfy, 0.45 - i * 0.04, R, G, B, 230)
    end
    if goal.target_id then
      viz.text("attack_chosen_standoff_marker",
               sfx, sfy, tostring(goal.target_id),
               "center", 0, 0, 0, 255)
    end
  end
end

return M
--[[ REMOVED: position/aim/engage/curve_away/rush/disengage substates
  if goal.substate == "position" then
    if not goal.standoff_mx then
      goal.substate = "plan_position"
      return
    end
    local sdist = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    if sdist <= 1 and info.speed <= 4 and not info.inboat then
      goal.substate = "aim"
      print(string.format(TAG .. " ATTACK: arrived at standoff (%d,%d)",
            goal.standoff_mx, goal.standoff_my))
      log.event("attack", "aim")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- aim: stopped, turn to face pill, no shooting
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "aim" then
    local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    local corr = math.abs(U.adiff(info.direction, aim_dir))
    local clear = PF.wall_hp_between(tmx, tmy, pmx, pmy) == 0
    if corr < 4 and clear then
      goal.substate = "engage"
      goal.engage_tick = now
      goal.engage_armour = info.armour
      goal.hits_taken = 0
      print(string.format(TAG .. " ATTACK: aimed, engaging pill@(%d,%d)", pmx, pmy))
      log.event("attack", "engage")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- engage: fire at pill, monitor armour
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "engage" then
    local hits = (goal.engage_armour or info.armour) - info.armour
    goal.hits_taken = hits

    if goal.engage_tick then
      local ticks_shooting = now - goal.engage_tick
      if ticks_shooting >= C.DRAIN_PROJECTION_MIN_TICKS and hits > 0 then
        local drain_rate = hits / ticks_shooting
        local pill_hp = pill and pill.health or 0
        local projected = info.armour - drain_rate * (pill_hp * C.TTK_TICKS_PER_HIT)
        if projected < C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN then
          goal.substate = "disengage"
          print(string.format(TAG .. " ATTACK: drain disengage arm=%d proj=%.1f", info.armour, projected))
          return
        end
      end
    end

    if hits >= C.ATTACK_CURVE_AFTER_HITS then
      goal.substate = "curve_away"
      goal.curve_tick = now
      goal.curve_dir = (math.random() > 0.5) and 1 or -1
      print(string.format(TAG .. " ATTACK: curve_away after %d hits", hits))
      log.event("attack", "curve_away")
    end
    return
  end

  -- ══════════════════════════════════════════════════════════════════
  -- curve_away: evasive turn, then replan position
  -- ══════════════════════════════════════════════════════════════════
  if goal.substate == "curve_away" then
    if now - (goal.curve_tick or now) >= C.ATTACK_CURVE_TICKS then
      goal.substate = "plan_position"
      goal.standoff_mx = nil
      goal.standoff_my = nil
      print(TAG .. " ATTACK: curve done, replanning position")
      log.event("attack", "replan")
    end
    return
  end

  -- rush: pill dead, steering handles navigation
  if goal.substate == "rush" then return end

  -- disengage: clear goal
  if goal.substate == "disengage" then
    goal.kind = "none"
    state.pf.status = "idle"
    print(TAG .. " ATTACK: disengaged")
    return
  end
end

return M
--[[ REMOVED: old wall-shield, pill-place, and legacy attack substates
      goal.standoff_mx = smx; goal.standoff_my = smy
      goal.substate = smx and "approach" or "plan"
    end
    return
  end

  if goal.substate == "approach" then
    if not goal.standoff_mx then return end

    if goal.wall_shield and goal.wall_mx then
      -- Wall-shield approach: navigate to PREBUILD position (outside pill range)
      local pb_mx = goal.prebuild_mx or goal.standoff_mx
      local pb_my = goal.prebuild_my or goal.standoff_my
      local pbdist = U.mdist(tmx, tmy, pb_mx, pb_my)
      if pbdist <= C.ATTACK_ENGAGE_RADIUS and not info.inboat then
        goal.substate      = "ws_prebuild"
        goal.ws_build_tick = now
        goal.lgm_return_tick = nil
        print(string.format(TAG .. " WALL-SHIELD: arrived at prebuild (%d,%d), dispatching LGM to build wall@(%d,%d)",
              pb_mx, pb_my, goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "approach->ws_prebuild", pbdist = pbdist,
          prebuild_mx = pb_mx, prebuild_my = pb_my,
          wall_mx = goal.wall_mx, wall_my = goal.wall_my,
        })
      end
    else
      -- Normal approach: navigate to standoff, must stop before engaging
      local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
      local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
      local in_range  = pdist_w <= C.ATTACK_PILL_RANGE * 256
      local clear_los = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
      if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range and clear_los
         and info.speed <= 4 and not info.inboat then
        goal.substate    = "engage"
        goal.engage_tick = now
        print(string.format(TAG .. " ATTACK: engage pill@(%d,%d) from (%d,%d)",
              goal.mx, goal.my, tmx, tmy))
        log.reason("attack_sub", {
          transition = "approach->engage", sdist = sdist,
          pill_x = goal.mx, pill_y = goal.my,
        })
      end
    end
    return
  end

  -- ── Wall-shield substates ───────────────────────────────────────────
  -- ws_prebuild: tank stopped OUTSIDE pill range, LGM dispatched to build wall
  -- ws_prewait:  wall built, LGM returning; tank still outside pill range
  -- ws_advance:  LGM safe in tank, wall up; tank advances to engagement standoff
  -- ws_engage:   at standoff, shooting through/past wall
  -- ws_retreat:  wall destroyed, tank retreating perpendicular to draw fire away
  -- ws_rebuild:  tank repositioned, LGM dispatched to rebuild wall

  if goal.substate == "ws_prebuild" then
    -- Wait for the wall to appear at the target tile
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
      -- Wall is up — wait for LGM to return
      goal.substate = "ws_prewait"
      goal.ws_wait_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: wall built@(%d,%d), waiting for LGM return (outside range)",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_prebuild->ws_prewait",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    elseif now - (goal.ws_build_tick or now) > 400 then
      -- Timeout: LGM failed to build, fall back to normal engage
      goal.substate = "engage"
      goal.engage_tick = now
      goal.wall_shield = false
      print(TAG .. " WALL-SHIELD: prebuild timeout, falling back to normal attack")
    end
    return
  end

  if goal.substate == "ws_prewait" then
    -- Wait for LGM to return to tank (tank still outside pill range)
    if info.man_status == C.LGM_INTANK then
      goal.lgm_return_tick = goal.lgm_return_tick or now
      local safe_ticks = now - goal.lgm_return_tick
      if safe_ticks >= C.WALL_SHIELD_LGM_SAFE_TICKS then
        goal.substate = "ws_advance"
        print(string.format(TAG .. " WALL-SHIELD: LGM safe, advancing to standoff (%d,%d) with wall@(%d,%d)",
              goal.standoff_mx, goal.standoff_my, goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "ws_prewait->ws_advance",
          standoff_mx = goal.standoff_mx, standoff_my = goal.standoff_my,
        })
      end
    else
      goal.lgm_return_tick = nil  -- LGM still out
    end
    return
  end

  if goal.substate == "ws_advance" then
    -- LGM safe in tank, wall already built — advance to engagement standoff
    local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local in_range = pdist_w <= C.ATTACK_PILL_RANGE * 256
    if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range then
      goal.substate    = "ws_engage"
      goal.engage_tick = now
      goal.first_hit_tick = nil
      goal.last_armour = info.armour
      print(string.format(TAG .. " WALL-SHIELD: engaging pill@(%d,%d) with wall@(%d,%d)",
            goal.mx, goal.my, goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_advance->ws_engage",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    end
    -- If wall got destroyed during advance, retreat back
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt ~= C.T_BUILDING and wtt ~= C.T_HALFBUILD then
      if (info.trees or 0) >= C.WALL_SHIELD_BUILD_COST then
        goal.substate = "ws_retreat"
        goal.ws_retreat_tick = now
        print(TAG .. " WALL-SHIELD: wall destroyed during advance, retreating to rebuild")
      else
        goal.substate = "engage"
        goal.engage_tick = now
        goal.wall_shield = false
        print(TAG .. " WALL-SHIELD: wall destroyed during advance, no trees, normal attack")
      end
    end
    return
  end

  if goal.substate == "ws_engage" then
    -- Shooting at pill with wall absorbing return fire.
    -- Check if wall is still intact
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    local wall_intact = (wtt == C.T_BUILDING or wtt == C.T_HALFBUILD)

    if not wall_intact then
      -- Wall destroyed — retreat first, then rebuild
      if info.man_status == C.LGM_INTANK
         and (info.trees or 0) >= C.WALL_SHIELD_BUILD_COST then
        goal.substate = "ws_retreat"
        goal.ws_retreat_tick = now
        print(string.format(TAG .. " WALL-SHIELD: wall@(%d,%d) destroyed, retreating to rebuild",
              goal.wall_mx, goal.wall_my))
        log.reason("attack_sub", {
          transition = "ws_engage->ws_retreat",
          wall_mx = goal.wall_mx, wall_my = goal.wall_my,
          trees = info.trees,
        })
      else
        if (info.trees or 0) < C.WALL_SHIELD_BUILD_COST then
          goal.substate = "engage"
          goal.engage_tick = now
          goal.wall_shield = false
          print(TAG .. " WALL-SHIELD: out of trees, switching to normal attack")
        end
      end
      return
    end

    -- Disengage check — more lenient with wall-shield (wall absorbs shots)
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " WALL-SHIELD: armour critical (%d), disengaging",
            info.armour))
      log.reason("attack_sub", {
        transition = "ws_engage->disengage",
        armour = info.armour,
      })
    end
    return
  end

  if goal.substate == "ws_retreat" then
    -- Tank retreating perpendicular to the pill->wall line to draw fire away.
    -- Once outside pill range, transition to ws_rebuild.
    local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local outside_range = pdist_w > C.PILL_FIRE_RANGE * 256
    -- Also accept if we've been retreating long enough
    local retreat_time = now - (goal.ws_retreat_tick or now)
    if outside_range or retreat_time > 150 then
      goal.substate = "ws_rebuild"
      goal.ws_rebuild_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: retreated, dispatching LGM to rebuild wall@(%d,%d)",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_retreat->ws_rebuild",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
        retreat_ticks = retreat_time,
      })
    end
    return
  end

  if goal.substate == "ws_rebuild" then
    -- Tank is repositioned outside pill range. LGM rebuilds the wall.
    local wtt = U.ttype(goal.wall_mx, goal.wall_my)
    if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
      -- Wall rebuilt! Wait for LGM return
      goal.substate = "ws_prewait"
      goal.ws_wait_tick = now
      goal.lgm_return_tick = nil
      print(string.format(TAG .. " WALL-SHIELD: wall rebuilt@(%d,%d), waiting for LGM",
            goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_rebuild->ws_prewait",
        wall_mx = goal.wall_mx, wall_my = goal.wall_my,
      })
    elseif now - (goal.ws_rebuild_tick or now) > 400 then
      goal.substate = "engage"
      goal.engage_tick = now
      goal.wall_shield = false
      print(TAG .. " WALL-SHIELD: rebuild timeout, falling back to normal attack")
    end
    return
  end

  -- ── Normal (non-wall-shield) engage ─────────────────────────────────

  if goal.substate == "engage" then
    if not goal.standoff_mx then return end
    local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    local tank_tt = U.ttype(tmx, tmy)
    local hazard  = (tank_tt == C.T_RIVER or tank_tt == C.T_DEEPSEA
                     or tank_tt == C.T_BUILDING or tank_tt == C.T_HALFBUILD)
    local min_time_met = (now - (goal.engage_tick or 0)) >= C.ATTACK_MIN_ENGAGE_TICKS

    -- Knocked too far or hazardous terrain → reposition
    if hazard or (sdist > C.ATTACK_REPOSITION_RADIUS and min_time_met) then
      goal.substate        = "reposition"
      goal.reposition_tick = now
      print(string.format(TAG .. " ATTACK: reposition pill@(%d,%d) sdist=%d hazard=%s",
            goal.mx, goal.my, sdist, tostring(hazard)))
      log.reason("attack_sub", {
        transition = "engage->reposition", sdist = sdist,
        hazard = hazard, tank_tt = tank_tt,
      })
      return
    end

    -- Time-under-fire based disengage.
    local pill = nil
    pill = M.find_pill_at(world, goal.mx, goal.my)
    local pill_anger = pill and (pill.anger or 0) or 0

    if not goal.first_hit_tick then
      if info.armour < (goal.last_armour or info.armour) then
        goal.first_hit_tick = now
        goal.engage_start_armour = goal.last_armour or info.armour
      end
    end
    goal.last_armour = info.armour

    if goal.first_hit_tick then
      local ticks_under_fire = now - goal.first_hit_tick
      local anger_scale = 1.0 - pill_anger * (1.0 - C.ENGAGE_ANGRY_FLEE_FACTOR)
      local esc_dx = tmx - goal.mx
      local esc_dy = tmy - goal.my
      local esc_len = math.max(1, math.sqrt(esc_dx * esc_dx + esc_dy * esc_dy))
      local escape_slow = false
      for step = 1, 3 do
        local ex = U.mclamp(math.floor(tmx + esc_dx / esc_len * step + 0.5))
        local ey = U.mclamp(math.floor(tmy + esc_dy / esc_len * step + 0.5))
        local ett = U.ttype(ex, ey)
        if ett == C.T_SWAMP or ett == C.T_RUBBLE or ett == C.T_CRATER
           or ett == C.T_RIVER or ett == C.T_DEEPSEA then
          escape_slow = true
          break
        end
      end
      local terrain_scale = escape_slow and 0.6 or 1.0
      local max_ticks = C.ENGAGE_MAX_INCOMING_TICKS * anger_scale * terrain_scale

      if ticks_under_fire >= max_ticks then
        goal.substate = "disengage"
        print(string.format(
          TAG .. " ATTACK: disengage pill@(%d,%d) fire=%d/%.0f anger=%.2f esc_slow=%s",
          goal.mx, goal.my, ticks_under_fire, max_ticks, pill_anger,
          tostring(escape_slow)))
        log.reason("attack_sub", {
          transition = "engage->disengage",
          ticks_under_fire = ticks_under_fire, max_ticks = max_ticks,
          pill_anger = pill_anger, escape_slow = escape_slow,
        })
        return
      end

      -- Feature 3: armour drain projection — disengage early if projected
      -- armour at kill time would be below the flee threshold
      if ticks_under_fire >= C.DRAIN_PROJECTION_MIN_TICKS then
        local armour_lost = (goal.engage_start_armour or info.armour) - info.armour
        if armour_lost > 0 then
          local drain_rate = armour_lost / ticks_under_fire
          local remaining_hp = pill and pill.health or 0
          local remaining_ttk = remaining_hp * C.TTK_TICKS_PER_HIT
          local projected_armour = info.armour - drain_rate * remaining_ttk
          local flee_with_margin = C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN
          if projected_armour < flee_with_margin then
            goal.substate = "disengage"
            print(string.format(
              TAG .. " ATTACK: drain disengage pill@(%d,%d) arm=%d proj=%.1f drain=%.3f/tick rem_hp=%d",
              goal.mx, goal.my, info.armour, projected_armour, drain_rate, remaining_hp))
            log.reason("attack_sub", {
              transition = "engage->disengage(drain)",
              armour = info.armour, projected = projected_armour,
              drain_rate = drain_rate, remaining_hp = remaining_hp,
              remaining_ttk = remaining_ttk, threshold = flee_with_margin,
            })
            return
          end
        end
      end
    end
    return
  end

  if goal.substate == "disengage" then
    -- Disengage triggers the normal flee/refuel logic by clearing the
    -- attack goal.  The dynamic flee threshold in goal_selection will
    -- pick a base to retreat to.  Clear the capture objective so goal
    -- selection doesn't immediately re-attack.
    -- The capture objective is preserved — after refueling, the brain
    -- will re-engage the pill.  Only clear attack plan so it replans
    -- the standoff from the new position.
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    print(TAG .. " ATTACK: disengaged — will refuel and re-engage")
    return
  end

  if goal.substate == "reposition" then
    -- Force replan of standoff position from current location
    local pill = nil
    pill = M.find_pill_at(world, goal.mx, goal.my)
    if pill then
      local pk = U.mkey(goal.mx, goal.my)
      state.pill_attack_plan = nil  -- force fresh computation
      local smx, smy = M.get_standoff(world, info, pk, pill, state)
      goal.standoff_mx = smx; goal.standoff_my = smy
      goal.substate = smx and "approach" or "plan"
      print(string.format(TAG .. " ATTACK: re-approach pill@(%d,%d) new standoff=(%s,%s)",
            goal.mx, goal.my, tostring(smx), tostring(smy)))
      log.reason("attack_sub", {
        transition = "reposition->approach",
        new_standoff_mx = smx, new_standoff_my = smy,
      })
    end
    return
  end
end

-- =========================================================================
-- Pill placement planner
-- =========================================================================

-- Pick the best position to place a friendly pill near a hostile target.
-- Mirrors pick_wall_shield: place 1 tile from the target on the approach
-- line, try ±30°/±60° rotations if that tile isn't valid.
-- Returns place_mx, place_my, deploy_mx, deploy_my, standoff_mx, standoff_my
-- or nil if no valid position found.
function M.pick_pill_placement(world, info, target_pill, state)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local pmx, pmy = target_pill.mx, target_pill.my

  -- Direction from pill to tank (approach angle) — same as pick_wall_shield
  local dx = tmx - pmx
  local dy = tmy - pmy
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 2 then return nil, nil, nil, nil, nil, nil end

  local ux, uy = dx / len, dy / len

  -- Try the primary angle and rotations of ±30°, ±60° — same as pick_wall_shield
  local angles = { 0, 0.52, -0.52, 1.05, -1.05 }
  for _, ang in ipairs(angles) do
    local cos_a = math.cos(ang)
    local sin_a = math.sin(ang)
    local rx = ux * cos_a - uy * sin_a
    local ry = ux * sin_a + uy * cos_a

    -- Placement tile: 1 tile from target in this direction (same as wall dist)
    local wmx = U.mclamp(math.floor(pmx + rx * C.WALL_SHIELD_WALL_DIST + 0.5))
    local wmy = U.mclamp(math.floor(pmy + ry * C.WALL_SHIELD_WALL_DIST + 0.5))

    -- Must be placeable land (no water, no existing pill/base)
    local wtt = U.ttype(wmx, wmy)
    local placeable = (wtt == C.T_GRASS or wtt == C.T_ROAD or wtt == C.T_RUBBLE
                       or wtt == C.T_SWAMP or wtt == C.T_CRATER or wtt == C.T_FOREST
                       or wtt == C.T_REFBASE)
    if not placeable then goto next_angle end
    if wmx == pmx and wmy == pmy then goto next_angle end
    if world.pill_at[wmy * 256 + wmx] then goto next_angle end
    if world.base_at[wmy * 256 + wmx] then goto next_angle end

    -- Standoff tile: offset angle so shells clear the placed pill — same as pick_wall_shield
    do
      local smx, smy = nil, nil
      local off_rad = math.rad(C.WALL_SHIELD_STANDOFF_ANGLE_OFFSET)
      local offsets = { off_rad, -off_rad, off_rad * 2, -off_rad * 2, 0 }
      for _, sdist in ipairs({ C.WALL_SHIELD_STANDOFF, C.WALL_SHIELD_STANDOFF - 1, C.WALL_SHIELD_STANDOFF + 1 }) do
        for _, off in ipairs(offsets) do
          local cos_o = math.cos(off)
          local sin_o = math.sin(off)
          local ox = rx * cos_o - ry * sin_o
          local oy = rx * sin_o + ry * cos_o
          local cx = U.mclamp(math.floor(pmx + ox * sdist + 0.5))
          local cy = U.mclamp(math.floor(pmy + oy * sdist + 0.5))
          local ctt = U.ttype(cx, cy)
          local cland = C.TERRAIN_COST_LAND[ctt] or 9999
          if cland < 9999 and not U.is_water(ctt) then
            -- LOS to target must NOT pass through placed pill tile
            local hits_pill = U.bresenham(cx, cy, pmx, pmy, function(lx, ly)
              if lx == wmx and ly == wmy then return true end
            end)
            if not hits_pill then
              local cd = math.sqrt((cx - pmx) * (cx - pmx) + (cy - pmy) * (cy - pmy))
              if cd <= C.ATTACK_PILL_RANGE then
                smx = cx; smy = cy
                break
              end
            end
          end
        end
        if smx then break end
      end
      if not smx then goto next_angle end

      -- Deploy position: outside pill range on the approach line — same as wall-shield prebuild
      local deploy_mx = U.mclamp(math.floor(pmx + rx * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local deploy_my = U.mclamp(math.floor(pmy + ry * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))

      print(string.format(
        TAG .. " [PP] PLACEMENT PLAN: target@(%d,%d) place@(%d,%d) deploy@(%d,%d) standoff@(%d,%d)",
        pmx, pmy, wmx, wmy, deploy_mx, deploy_my, smx, smy))
      return wmx, wmy, deploy_mx, deploy_my, smx, smy
    end

    ::next_angle::
  end
  return nil, nil, nil, nil, nil, nil
end

-- Find the best friendly pill to pick up for placement.
-- Only considers dead friendly pills (health == 0) since alive pills can't be picked up.
-- Returns pill entry and pill_id, or nil if none available.
function M.pick_source_pill(world, info, state)
  if (info.carried_pills or 0) > 0 then
    return nil, nil  -- already carrying a pill
  end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local best_cost = math.huge
  local best_pill, best_id = nil, nil
  local ammo = (info.shells or 0) + (info.mines or 0)
  for id, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health == 0 then
      local c = cpf.estimate_cost(tmx, tmy, p.mx, p.my, info.inboat and 1 or 0)
      if c < best_cost then
        best_cost = c
        best_pill = p
        best_id = id
      end
    end
  end
  return best_pill, best_id
end

-- =========================================================================
-- Pill placement substate machine
-- =========================================================================
-- States: select_pill → pickup → navigate → dispatch → wait_place →
--         engage → collect_target → (chain back to select_pill)
-- Also:   engage → finish (placed pill died, continue shooting with tank)
--         engage/finish → disengage (armour critical)

function M.update_pill_place_substate(goal, state, world, info)
  if goal.kind ~= "pill_place" then return end

  if M.USE_PILLPLACE_BT then
    if not pillplace_bt then pillplace_bt = require("pillplace_bt") end
    pillplace_bt.tick(goal, state, world, info)
    return
  end

  -- ── Legacy FSM (kept as fallback) ─────────────────────────────────
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  if not goal.substate then
    goal.substate = "select_pill"
  end

  -- Look up target pill
  local target = M.find_pill_at(world, goal.mx, goal.my)

  -- Target dead in any substate → collect it
  if target and target.health == 0 and goal.substate ~= "collect_target"
     and goal.substate ~= "disengage" then
    goal.substate = "collect_target"
    goal.collect_tick = now
    print(string.format(TAG .. " [PP] target@(%d,%d) dead, collecting", goal.mx, goal.my))
    log.reason("pp_sub", { transition = goal.substate .. "->collect_target" })
    return
  end

  -- Target became friendly → done
  if target and target.owner == "friendly" then
    print(string.format(TAG .. " [PP] target@(%d,%d) is now friendly, done", goal.mx, goal.my))
    goal.kind = "none"
    goal.substate = nil
    return
  end

  -- ── select_pill ─────────────────────────────────────────────────────
  if goal.substate == "select_pill" then
    -- Already carrying a pill?
    if (info.carried_pills or 0) > 0 then
      -- Pick placement position for target
      if not target then
        goal.kind = "none"; goal.substate = nil
        print(TAG .. " [PP] select_pill: target not found, aborting")
        return
      end
      local pmx, pmy, dmx, dmy, smx, smy = M.pick_pill_placement(world, info, target, state)
      if not pmx then
        goal.kind = "none"; goal.substate = nil
        print(TAG .. " [PP] select_pill: no valid placement position, aborting")
        return
      end
      goal.place_mx    = pmx
      goal.place_my    = pmy
      goal.deploy_mx   = dmx
      goal.deploy_my   = dmy
      goal.standoff_mx = smx
      goal.standoff_my = smy
      goal.substate = "navigate"
      print(string.format(TAG .. " [PP] select_pill: have pill, navigate to deploy@(%d,%d) place@(%d,%d)", dmx, dmy, pmx, pmy))
      log.reason("pp_sub", { transition = "select_pill->navigate", place_mx = pmx, place_my = pmy, deploy_mx = dmx, deploy_my = dmy })
      return
    end

    -- Find a dead friendly pill to pick up
    local src_pill, src_id = M.pick_source_pill(world, info, state)
    if not src_pill then
      -- No pills available — can't do pill placement
      goal.kind = "none"; goal.substate = nil
      print(TAG .. " [PP] select_pill: no friendly pills to pick up, aborting")
      return
    end

    goal.source_mx = src_pill.mx
    goal.source_my = src_pill.my
    goal.source_id = src_id
    goal.substate = "pickup"
    print(string.format(TAG .. " [PP] select_pill: picking up dead pill#%s@(%d,%d)",
          tostring(src_id), src_pill.mx, src_pill.my))
    log.reason("pp_sub", {
      transition = "select_pill->pickup",
      source_id = src_id, source_mx = src_pill.mx, source_my = src_pill.my,
    })
    return
  end

  -- ── pickup ──────────────────────────────────────────────────────────
  if goal.substate == "pickup" then
    -- Drive to the dead friendly pill to pick it up
    if (info.carried_pills or 0) > 0 then
      -- Got it! Now select placement position
      goal.substate = "select_pill"  -- re-enter to compute placement
      print(TAG .. " [PP] pickup: pill picked up, selecting placement")
      return
    end
    -- Check the source pill is still there and dead
    local src = M.find_pill_at(world, goal.source_mx, goal.source_my)
    if not src or src.health > 0 or src.owner ~= "friendly" then
      -- Source pill gone or revived
      goal.substate = "select_pill"
      print(TAG .. " [PP] pickup: source pill no longer available, re-selecting")
      return
    end
    -- Navigation handled by steering; just monitor arrival
    return
  end

  -- ── navigate ────────────────────────────────────────────────────────
  if goal.substate == "navigate" then
    if not goal.place_mx then
      goal.substate = "select_pill"
      return
    end
    -- Check arrival: at deploy position (outside pill range)
    local nav_mx = goal.deploy_mx or goal.place_mx
    local nav_my = goal.deploy_my or goal.place_my
    local pdist = U.mdist(tmx, tmy, nav_mx, nav_my)
    if pdist <= C.ATTACK_ENGAGE_RADIUS and info.man_status == C.LGM_INTANK
       and (info.carried_pills or 0) > 0 and not info.inboat then
      goal.substate = "dispatch"
      goal.dispatch_tick = now
      print(string.format(TAG .. " [PP] navigate: arrived at deploy@(%d,%d), dispatching LGM to place@(%d,%d)",
            nav_mx, nav_my, goal.place_mx, goal.place_my))
      log.reason("pp_sub", {
        transition = "navigate->dispatch",
        deploy_mx = nav_mx, deploy_my = nav_my,
        place_mx = goal.place_mx, place_my = goal.place_my,
      })
    end
    return
  end

  -- ── dispatch ────────────────────────────────────────────────────────
  if goal.substate == "dispatch" then
    -- Builder handles the actual BUILDMODE_PBOX command.
    -- Transition to wait_place once LGM leaves the tank.
    if info.man_status ~= C.LGM_INTANK then
      goal.substate = "wait_place"
      goal.wait_tick = now
      print(TAG .. " [PP] dispatch: LGM dispatched, waiting for placement")
      log.reason("pp_sub", { transition = "dispatch->wait_place" })
    elseif now - (goal.dispatch_tick or now) > 50 then
      -- LGM didn't leave after 50 ticks — maybe can't build, retry
      goal.substate = "select_pill"
      print(TAG .. " [PP] dispatch: LGM didn't leave, re-selecting")
    end
    return
  end

  -- ── wait_place ──────────────────────────────────────────────────────
  if goal.substate == "wait_place" then
    -- Check if a friendly pill appeared at the placement position
    local placed = M.find_pill_at(world, goal.place_mx, goal.place_my)
    if placed and placed.owner == "friendly" and placed.health > 0 then
      goal.placed_mx = goal.place_mx
      goal.placed_my = goal.place_my
      goal.substate = "engage"
      goal.engage_tick = now
      goal.last_armour = info.armour
      print(string.format(TAG .. " [PP] wait_place: pill placed@(%d,%d) hp=%d, engaging target@(%d,%d)",
            goal.place_mx, goal.place_my, placed.health, goal.mx, goal.my))
      log.reason("pp_sub", {
        transition = "wait_place->engage",
        placed_mx = goal.place_mx, placed_my = goal.place_my,
        placed_hp = placed.health,
      })
      return
    end
    -- LGM returned without placing (died, blocked, etc.)
    if info.man_status == C.LGM_INTANK and now - (goal.wait_tick or now) > 50 then
      -- Check if we still have a pill
      if (info.carried_pills or 0) > 0 then
        goal.substate = "select_pill"  -- retry with different position
        print(TAG .. " [PP] wait_place: LGM returned, pill not placed, re-selecting")
      else
        -- LGM died and lost the pill
        goal.substate = "select_pill"
        print(TAG .. " [PP] wait_place: pill lost, re-selecting")
      end
      return
    end
    -- Timeout
    if now - (goal.wait_tick or now) > C.PILL_PLACE_TIMEOUT then
      goal.substate = "select_pill"
      print(TAG .. " [PP] wait_place: timeout, re-selecting")
      return
    end
    return
  end

  -- ── engage ──────────────────────────────────────────────────────────
  if goal.substate == "engage" then
    -- Armour critical → disengage
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " [PP] engage: armour critical (%d), disengaging", info.armour))
      log.reason("pp_sub", { transition = "engage->disengage", armour = info.armour })
      return
    end

    -- Time-under-fire disengage: bail if taking sustained hits
    if not goal.engage_first_hit then
      if info.armour < (goal.last_armour or info.armour) then
        goal.engage_first_hit = now
      end
    end
    goal.last_armour = info.armour

    if goal.engage_first_hit then
      local ticks_under_fire = now - goal.engage_first_hit
      if ticks_under_fire >= C.ENGAGE_MAX_INCOMING_TICKS then
        goal.substate = "disengage"
        print(string.format(TAG .. " [PP] engage: under fire %d ticks, disengaging", ticks_under_fire))
        log.reason("pp_sub", {
          transition = "engage->disengage", ticks_under_fire = ticks_under_fire,
        })
        return
      end
    end

    -- Check placed pill health
    local placed = nil
    if goal.placed_mx then
      placed = M.find_pill_at(world, goal.placed_mx, goal.placed_my)
    end
    if placed and placed.owner == "friendly" and placed.health > 0 then
      -- Placed pill still fighting, continue engagement
    else
      -- Placed pill died or gone — continue shooting with tank alone
      goal.substate = "finish"
      goal.finish_tick = now
      print(string.format(TAG .. " [PP] engage: placed pill died, finishing target@(%d,%d) with tank fire",
            goal.mx, goal.my))
      log.reason("pp_sub", { transition = "engage->finish" })
      return
    end
    return
  end

  -- ── finish ──────────────────────────────────────────────────────────
  -- Placed pill died but target is still alive; continue with tank-only fire.
  -- Behaves like a normal attack_pill engage — disengage if armour gets low.
  if goal.substate == "finish" then
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format(TAG .. " [PP] finish: armour critical (%d), disengaging", info.armour))
      return
    end

    -- Time-under-fire based disengage (reuse attack constants)
    if not goal.first_hit_tick then
      if info.armour < (goal.last_armour or info.armour) then
        goal.first_hit_tick = now
      end
    end
    goal.last_armour = info.armour

    if goal.first_hit_tick then
      local ticks_under_fire = now - goal.first_hit_tick
      if ticks_under_fire >= C.ENGAGE_MAX_INCOMING_TICKS then
        goal.substate = "disengage"
        print(string.format(TAG .. " [PP] finish: under fire %d ticks, disengaging", ticks_under_fire))
        log.reason("pp_sub", {
          transition = "finish->disengage", ticks_under_fire = ticks_under_fire,
        })
        return
      end
    end
    return
  end

  -- ── collect_target ──────────────────────────────────────────────────
  if goal.substate == "collect_target" then
    -- Drive onto dead target pill
    local pdist = U.mdist(tmx, tmy, goal.mx, goal.my)
    if pdist <= 1 then
      -- Arrived — pill auto-picked up by tank
      -- Chain: immediately re-select next target
      print(string.format(TAG .. " [PP] collect: arrived at target@(%d,%d), chaining to next",
            goal.mx, goal.my))
      log.reason("pp_sub", { transition = "collect_target->done" })
      -- Clear goal to let goal selection pick the next target
      state.pill_attack_plan = nil
      goal.kind = "none"
      goal.substate = nil
    end
    return
  end

  -- ── disengage ───────────────────────────────────────────────────────
  if goal.substate == "disengage" then
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    print(TAG .. " [PP] disengaged — will refuel and re-engage")
    return
  end
end
--]]
