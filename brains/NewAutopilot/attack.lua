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

local M = {}

-- Toggle: set true to use behaviour-tree implementations
M.USE_WALLSHIELD_BT = true
M.USE_PILLPLACE_BT  = true
local wallshield_bt  -- lazy-loaded
local pillplace_bt   -- lazy-loaded

-- Find a pill at (mx, my) via spatial index. Returns the pill entry or nil.
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
local function water_corridor_to(x0, y0, x1, y1)
  local blocked = U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if not U.is_water(U.ttype(cx, cy)) then return true end
  end)
  return not blocked
end

-- Count forest tiles on the Bresenham line from (x0,y0) to (x1,y1), excluding
-- endpoints.  Each one would be destroyed by a shell fired along this path —
-- a resource cost since the brain farms trees for road building and pill repair.
local function forest_tiles_on_path(x0, y0, x1, y1)
  local count = 0
  U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if U.ttype(cx, cy) == C.T_FOREST then count = count + 1 end
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
  -- Use the tank's actual travel mode for approach cost.  Always using land mode
  -- made every northern/western standoff look prohibitively expensive when the
  -- tank was in deep sea (straight-line estimate crossed deepsea at 9999/tile),
  -- causing the planner to always pick the nearest accessible position regardless
  -- of proximity penalties.  Boat mode correctly reflects reachability.
  local tmx    = info.tankx >> 8
  local tmy    = info.tanky >> 8
  local ammo   = (info.shells or 0) + (info.mines or 0)
  local approach = cpf.estimate_cost(tmx, tmy, cx, cy, (info.inboat ~= 0) and 1 or 0)

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

  return approach + water_pen + pushback_pen + crossfire + escape_cost + tree_pen + approach_exposure + orbit_pen + threat_pen
end

-- Enumerate candidate standoff positions around `pill` and pick the best scored one.
-- Samples multiple radii (max range down to max-2) so that if the ring at exactly
-- shell range lands on walls, nearby passable tiles are still considered.
-- Falls back to the closest candidate if none have finite scores (e.g. pill in open water).
function M.pick_standoff(world, info, pill, state, standoff_override, orbit_radius)
  local R_MAX = standoff_override or C.ATTACK_PILL_STANDOFF   -- 7 (shell range)
  local R_MIN = math.max(4, R_MAX - 2)   -- 5 (don't get closer than this)
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
      end
    end
  end

  local smx = best_mx or fallback_mx
  local smy = best_my or fallback_my
  if smx then
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
  if info.inboat ~= 0 then return nil end

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

  -- Try wall-shield tactic first
  local wmx, wmy, smx, smy, pbmx, pbmy = pick_wall_shield(world, info, pill, state)
  if wmx then
    state.pill_attack_plan = {
      pill_key      = pill_key,
      standoff_mx   = smx,
      standoff_my   = smy,
      wall_mx       = wmx,
      wall_my       = wmy,
      prebuild_mx   = pbmx,
      prebuild_my   = pbmy,
      wall_shield   = true,
      replan_at     = now + C.PILL_ATTACK_REPLAN_TICKS,
    }
    return smx, smy
  end

  -- Fall back to normal standoff
  smx, smy = M.pick_standoff(world, info, pill, state)
  state.pill_attack_plan = {
    pill_key    = pill_key,
    standoff_mx = smx,
    standoff_my = smy,
    wall_shield = false,
    replan_at   = now + C.PILL_ATTACK_REPLAN_TICKS,
  }
  return smx, smy
end

-- =========================================================================
-- Attack pill substate machine (called every tick from init.lua)
-- =========================================================================
-- Substates: plan → approach → engage → reposition → approach ...
--
-- plan:       standoff not yet computed (transient — usually resolved same tick)
-- approach:   navigating to standoff position
-- engage:     at/near standoff, aiming and shooting
-- reposition: knocked too far from standoff, replanning

function M.update_attack_substate(goal, state, world, info)
  if goal.kind ~= "attack_pill" then return end

  if M.USE_WALLSHIELD_BT then
    if not wallshield_bt then wallshield_bt = require("wallshield_bt") end
    wallshield_bt.tick(goal, state, world, info)
    return
  end

  -- ── Legacy FSM (kept as fallback) ─────────────────────────────────
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  -- Backward compat: goals without substate default to old behavior
  if not goal.substate then return end

  if goal.substate == "plan" then
    -- Shouldn't persist; standoff is computed at goal creation.
    -- Safety fallback: recompute now.
    local pill = nil
    pill = M.find_pill_at(world, goal.mx, goal.my)
    if pill then
      local pk = U.mkey(goal.mx, goal.my)
      state.pill_attack_plan = nil  -- force fresh computation
      local smx, smy = M.get_standoff(world, info, pk, pill, state)
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
      if pbdist <= C.ATTACK_ENGAGE_RADIUS and info.inboat == 0 then
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
      -- Normal approach: navigate to standoff
      local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
      local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
      local in_range  = pdist_w <= C.ATTACK_PILL_RANGE * 256
      local clear_los = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
      if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range and clear_los
         and info.inboat == 0 then
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
    local outside_range = pdist_w > C.PILL_RANGE_MAP * 256
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
      local c = cpf.estimate_cost(tmx, tmy, p.mx, p.my, (info.inboat ~= 0) and 1 or 0)
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
       and (info.carried_pills or 0) > 0 and info.inboat == 0 then
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

return M
