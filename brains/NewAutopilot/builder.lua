-- =========================================================================
-- NewAutopilot/builder.lua — LGM / build-action decision layer
--
-- Separates builder logic from tank goal logic.  Each tick the tank goal
-- sets a builder mode (set_mode), then decide() resolves what build command
-- (if any) to issue.
--
-- Builder modes:
--   "suppressed"    — LGM stays in tank (combat, emergency flee, rescue LGM)
--   "gather"        — proactively farm trees before executing a plan that
--                     needs them (e.g. repair/capture pill)
--   "opportunistic" — grab forest tiles that are right on our path, road
--                     ahead when trees allow (refuel, capture base, travel)
--   "infrastructure"— road ahead of current path (explore)
--   "repair_nearby" — dispatch LGM to nearest repairable friendly pill, then
--                     road ahead (holding position)
--
-- Priority within decide():
--   1. Water emergency (build road under self when drowning) — always, mode-bypasses
--   2. suppressed → nil
--   3. gather + trees < need_trees → farm on-path (FARM_GATHER_RADIUS), else nil
--      (block road building until stocked: don't burn LGM time on roads mid-gather)
--   4. gather (stocked) + opportunistic → on-path farm (FARM_OPPORTUNISTIC_RADIUS,
--      trees < TREE_OPPORTUNISTIC_MAX only)
--   5. infrastructure / repair_nearby → road ahead
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local log    = require("logger")

local M = {}

-- -------------------------------------------------------------------------
-- Mode mapping: tank goal kind → builder mode
-- -------------------------------------------------------------------------
local GOAL_TO_MODE = {
  flee_to_base   = "opportunistic",  -- danger check in lgm_path_safe handles safety
  rescue_lgm     = "suppressed",
  defend_pill    = "suppressed",
  attack_pill    = "suppressed",     -- overridden to "wall_shield" below when applicable
  bpc_pill       = "suppressed",     -- circle-strafe attack: no building during combat
  pill_place     = "suppressed",     -- overridden to "place_pill" below when dispatching
  attack_tank    = "suppressed",     -- no building during tank combat
  attack_base    = "suppressed",
  repair_pill    = "gather",
  capture_pill   = "gather",
  refuel_at_base = "opportunistic",
  capture_base   = "opportunistic",
  explore              = "infrastructure",
  place_pill_strategic = "place_pill_strategic",
  none                 = "repair_nearby",
}

-- -------------------------------------------------------------------------
-- set_mode: call each tick after goal selection to populate state.builder
-- -------------------------------------------------------------------------
function M.set_mode(state, world, info, goal)
  local b    = state.builder
  local kind = goal.kind

  b.mode       = GOAL_TO_MODE[kind] or "opportunistic"
  b.target     = nil
  b.need_trees = 0

  -- Wall-shield attack: dispatch LGM to build/rebuild wall in specific substates
  if kind == "attack_pill" and goal.wall_shield and goal.wall_mx then
    local sub = goal.substate or ""
    if sub == "ws_prebuild" or sub == "ws_rebuild" then
      -- LGM should go build/rebuild the wall
      b.mode = "wall_shield"
      b.wall_target = { mx = goal.wall_mx, my = goal.wall_my }
      b.target_pill = { mx = goal.mx, my = goal.my }  -- exclude from angry check
    elseif sub == "ws_prewait" or sub == "ws_advance" or sub == "ws_engage"
           or sub == "ws_retreat" then
      -- Wall is up / LGM returning / retreating — suppress building
      b.mode = "suppressed"
    end
  end

  -- Pill placement: dispatch LGM to place pill during dispatch substate
  if kind == "pill_place" and goal.place_mx then
    local sub = goal.substate or ""
    if sub == "dispatch" then
      b.mode = "place_pill"
      b.pill_target = { mx = goal.place_mx, my = goal.place_my }
    end
  end

  -- Base shield: build a wall to block a calm hostile pill while refueling
  if kind == "none" and goal.base_shield and goal.shield_wall_mx then
    b.mode = "base_shield"
    b.wall_target = { mx = goal.shield_wall_mx, my = goal.shield_wall_my }
  end

  -- Strategic pill placement: dispatch LGM to place pill when within 1 tile of target
  if kind == "place_pill_strategic" then
    local tmx = info.tankx >> 8
    local tmy = info.tanky >> 8
    local pdist = U.mdist(tmx, tmy, goal.mx, goal.my)
    if pdist <= 1 and (info.carried_pills or 0) > 0
       and info.man_status == C.LGM_INTANK and not info.inboat then
      b.mode = "place_pill"
      b.pill_target = { mx = goal.mx, my = goal.my }
    end
  end

  if kind == "repair_pill" then
    -- Find the pill at this destination to calculate how many trees we need
    local entries = world.pill_at[goal.my * 256 + goal.mx]
    if entries then
      for _, e in ipairs(entries) do
        b.need_trees = math.max(0, C.PILLS_MAX_HEALTH - e.pill.health) * C.PILL_REPAIR_COST
        break
      end
    end
    b.target = { mx = goal.mx, my = goal.my }

  elseif kind == "capture_pill" then
    -- Dead pill: no trees needed to pick it up; may need some to repair after placing
    b.target = { mx = goal.mx, my = goal.my }
  end
end

-- -------------------------------------------------------------------------
-- nearest_forest_near: closest forest tile within 'radius' of (cx, cy)
-- -------------------------------------------------------------------------
local function nearest_forest_near(cx, cy, radius)
  local best_d, best_x, best_y = math.huge, nil, nil
  for dy = -radius, radius do
    for dx = -radius, radius do
      local fx, fy = cx + dx, cy + dy
      if U.in_map(fx, fy) and U.ttype(fx, fy) == C.T_FOREST then
        local d = U.mdist(cx, cy, fx, fy)
        if d < best_d then
          best_d = d; best_x = fx; best_y = fy
        end
      end
    end
  end
  return best_x, best_y, best_d
end

-- path_checkpoints: collect N evenly-spaced positions along the A* path ahead
-- of the tank by tracing the parent chain.  Cheaper than full path traces
-- because we stop after n entries.  Returns a list of {mx, my} pairs ordered
-- from tank outward (closest first, furthest last).
local function path_checkpoints(state, n)
  local pf = state.pf
  if pf.status ~= "done" or pf.next_mx < 0 then return {} end

  -- C pathfinder doesn't expose its parent chain.  Walk parent if available
  -- (legacy Lua A*), otherwise approximate with straight-line samples from
  -- current position toward destination.
  if pf.parent then
    local dest_key = U.mkey(pf.dest_mx, pf.dest_my)
    local chain = {}
    local cur = dest_key
    local limit = 300
    while cur ~= nil and cur ~= -1 and limit > 0 do
      table.insert(chain, cur)
      cur = pf.parent[cur]
      limit = limit - 1
      if cur == -1 then break end
    end
    local pts = {}
    for i = #chain - 1, 1, -1 do
      table.insert(pts, { U.mkey_x(chain[i]), U.mkey_y(chain[i]) })
      if #pts >= n then break end
    end
    return pts
  end

  -- Straight-line fallback for C pathfinder
  local sx, sy = pf.src_mx, pf.src_my
  local dx, dy = pf.dest_mx, pf.dest_my
  local dist = math.abs(dx - sx) + math.abs(dy - sy)
  if dist == 0 then return {} end
  local pts = {}
  local steps = math.min(dist, n)
  for i = 1, steps do
    local t = i / steps
    local mx = U.mclamp(math.floor(sx + (dx - sx) * t + 0.5))
    local my = U.mclamp(math.floor(sy + (dy - sy) * t + 0.5))
    pts[#pts + 1] = { mx, my }
  end
  return pts
end

-- lgm_can_reach: check if the LGM can walk from the tank to (dmx, dmy).
-- Uses the C tick-by-tick LGM walk simulation. Returns true if reachable.
-- Skips the check for adjacent tiles (distance <= 1) since those are always fine.
local function lgm_can_reach(info, dmx, dmy)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  if math.abs(dmx - tmx) + math.abs(dmy - tmy) <= 1 then return true end
  local ticks = cpf_lgm_travel_ticks_map(tmx, tmy, dmx, dmy, 0, 0, 2000, 150)
  return ticks ~= -1
end

-- nearest_onpath_forest: search for forest tiles within 'radius' of checkpoints
-- along the path ahead.  Looks at tank, next waypoint, AND several steps further
-- ahead so the LGM farms terrain the tank will traverse, not terrain it just left.
-- Returns the forest tile closest to the TANK (minimises LGM travel time).
local function nearest_onpath_forest(state, info, radius)
  local cx = info.tankx >> 8
  local cy = info.tanky >> 8
  local best_d, best_x, best_y = math.huge, nil, nil

  local function check(ox, oy)
    local x, y, _ = nearest_forest_near(ox, oy, radius)
    if x then
      local dtank = U.mdist(cx, cy, x, y)
      if dtank < best_d then
        best_d = dtank; best_x = x; best_y = y
      end
    end
  end

  -- Check tank position and up to 5 steps ahead on the planned path
  check(cx, cy)
  local pts = path_checkpoints(state, 5)
  for _, pt in ipairs(pts) do
    check(pt[1], pt[2])
  end

  return best_x, best_y, best_d
end

-- -------------------------------------------------------------------------
-- road_ahead: pave the next A* waypoint if it's slow terrain and we have
-- enough trees.  Extracted from the old init.lua inline block.
-- -------------------------------------------------------------------------
local function road_ahead(state, info, now, world)
  if state.pf.next_mx < 0 then return nil end
  if info.inboat then return nil end  -- boat doesn't need roads; LGM dispatch triggers pacing slowdown
  local cur_mx = info.tankx >> 8
  local cur_my = info.tanky >> 8
  local nmx, nmy = state.pf.next_mx, state.pf.next_my
  if U.mdist(cur_mx, cur_my, nmx, nmy) > 1 then return nil end
  local next_tt  = U.ttype(nmx, nmy)
  local tree_cost = C.ROAD_BUILD_TERRAIN[next_tt]
  if not tree_cost then return nil end
  if info.trees < tree_cost + C.TREE_RESERVE then return nil end
  -- Quick reject: if threat_at_tank > 0, the LGM path starting at the tank
  -- tile already exceeds LGM_DANGER_LOW (0), so lgm_path_safe will fail.
  if state.perc and state.perc.threat_at_tank > C.LGM_DANGER_LOW then return nil end
  if not danger.lgm_path_safe(info, nmx, nmy, C.LGM_DANGER_LOW, now, world) then
    return nil
  end
  return { x = nmx, y = nmy, action = BUILDMODE_ROAD }
end

-- -------------------------------------------------------------------------
-- decide: returns a build table {x, y, action} or nil
-- Call once per tick from init.lua after set_mode().
-- -------------------------------------------------------------------------
function M.decide(state, world, info, now)
  local b = state.builder

  -- LGM must be in the tank and available
  if info.man_status ~= C.LGM_INTANK then return nil end

  -- Never dispatch the LGM while in a boat.  The pacing slowdown drops
  -- the tank below disembark speed, stranding it on water.
  if info.inboat then return nil end

  -- Priority 1: emergency road under self when drowning in river
  if state.water_build then
    local bx, by = state.water_build.x, state.water_build.y
    -- Never send the LGM out if an actively-firing (angry) hostile pill is
    -- close by.  Uses state.perc.pill_threats (already filtered to pills
    -- within PILL_RANGE_MAP) instead of scanning world.pills.
    local angry_pill_close = false
    local pill_threats = state.perc and state.perc.pill_threats or {}
    for _, pt in ipairs(pill_threats) do
      if pt.anger > 0.3 and U.mdist(bx, by, pt.pill.mx, pt.pill.my) <= 4 then
        angry_pill_close = true
        break
      end
    end
    if not angry_pill_close
       and danger.lgm_path_safe(info, bx, by, C.LGM_DANGER_HIGH, now, world) then
      return { x = bx, y = by, action = BUILDMODE_ROAD }
    end
    -- Danger too high even for emergency; nothing else is safe to do either
    return nil
  end

  -- Priority 1b: build road under self on slow terrain (swamp/rubble/crater).
  -- LGM stays on the current tile (~44 ticks away), saves ~40 ticks per tile vs raw traversal.
  -- Uses LGM_DANGER_MED: the LGM barely leaves the tank so exposure is short, but being
  -- stuck at speed 3 in swamp under pill fire is worse than the brief LGM risk.  MED (20)
  -- allows builds with a calm pill at 4+ tiles but aborts under heavy/angry fire.
  if state.slow_build and b.mode ~= "suppressed" then
    local bx, by = state.slow_build.x, state.slow_build.y
    if danger.lgm_path_safe(info, bx, by, C.LGM_DANGER_MED, now, world) then
      return { x = bx, y = by, action = BUILDMODE_ROAD }
    end
  end

  -- Priority 2: suppressed — no builder activity
  if b.mode == "suppressed" then
    if state.tick % 50 == 0 then
      log.reason("build", { mode = "suppressed", why = "combat/emergency goal" })
    end
    return nil
  end

  -- Priority 2.3: defensive trail dropping — drop a pill behind the tank
  -- while moving through open territory with surplus pills.
  if C.TRAIL_DROP_ENABLED
     and b.mode ~= "suppressed"
     and (info.carried_pills or 0) >= C.TRAIL_DROP_MIN_PILLS
     and info.speed >= C.TRAIL_DROP_MIN_SPEED
     and not info.inboat
     and (not state.trail_drop_cooldown or now >= state.trail_drop_cooldown) then
    local gk = state.goal and state.goal.kind or "none"
    if gk ~= "attack_pill" and gk ~= "pill_place" and gk ~= "bpc_pill"
       and gk ~= "place_pill_strategic" then
      -- Check no friendly pill within radius
      local tmx = info.tankx >> 8
      local tmy = info.tanky >> 8
      local friendly_nearby = false
      for _, p in pairs(world.pills) do
        if p.owner == "friendly" and p.health > 0 then
          if U.mdist(tmx, tmy, p.mx, p.my) <= C.TRAIL_DROP_NO_PILL_RADIUS then
            friendly_nearby = true
            break
          end
        end
      end
      if not friendly_nearby then
        -- Position 2 tiles behind current heading
        local behind_dir = (info.direction + 128) & 0xFF
        local bmx = U.mclamp(tmx + math.floor(U.bsin(behind_dir) * C.TRAIL_DROP_BEHIND_DIST / 128 + 0.5))
        local bmy = U.mclamp(tmy - math.floor(U.bcos(behind_dir) * C.TRAIL_DROP_BEHIND_DIST / 128 + 0.5))
        if (bmx ~= tmx or bmy ~= tmy) and U.is_placeable(bmx, bmy, world)
           and lgm_can_reach(info, bmx, bmy) then
          state.trail_drop_cooldown = now + C.TRAIL_DROP_COOLDOWN
          log.reason("build", { mode = "trail_drop", behind_mx = bmx, behind_my = bmy })
          return { x = bmx, y = bmy, action = BUILDMODE_PBOX }
        end
      end
    end
  end
  -- Expire trail drop cooldown
  if state.trail_drop_cooldown and now >= state.trail_drop_cooldown then
    state.trail_drop_cooldown = nil
  end

  -- Priority 2.4: pill placement — dispatch LGM to place pill at target
  if b.mode == "place_pill" and b.pill_target then
    local px, py = b.pill_target.mx, b.pill_target.my
    if (info.carried_pills or 0) > 0
       and lgm_can_reach(info, px, py)
       and danger.lgm_path_safe(info, px, py, C.LGM_DANGER_HIGH, now, world) then
      log.reason("build", { mode = "place_pill", why = "placing pill",
                             pill_mx = px, pill_my = py })
      return { x = px, y = py, action = BUILDMODE_PBOX }
    end
    return nil
  end

  -- Priority 2.5: wall-shield / base-shield — build wall at target location
  if (b.mode == "wall_shield" or b.mode == "base_shield") and b.wall_target then
    local wx, wy = b.wall_target.mx, b.wall_target.my
    -- Only build if the tile doesn't already have a wall
    local wtt = U.ttype(wx, wy)
    if wtt ~= C.T_BUILDING and wtt ~= C.T_HALFBUILD then
      local cost = b.mode == "base_shield" and C.BASE_SHIELD_BUILD_COST
                                             or C.WALL_SHIELD_BUILD_COST
      -- Check for angry pills close to the wall tile — if a pill has woken up
      -- (e.g., another player provoked it) the LGM will die in transit.
      -- Exclude the TARGET pill for wall_shield mode: we know the wall is near
      -- it, and the pill is calm at prebuild distance (9 tiles, outside range).
      local angry_pill_close = false
      local pill_threats = state.perc and state.perc.pill_threats or {}
      local tp = b.target_pill
      for _, pt in ipairs(pill_threats) do
        -- Skip the target pill in wall_shield mode
        if tp and pt.pill.mx == tp.mx and pt.pill.my == tp.my then
          goto next_pill_threat
        end
        if pt.anger > 0.3 and U.mdist(wx, wy, pt.pill.mx, pt.pill.my) <= C.PILL_RANGE_MAP then
          angry_pill_close = true
          break
        end
        ::next_pill_threat::
      end
      if not angry_pill_close and info.trees >= cost
         and lgm_can_reach(info, wx, wy)
         and danger.lgm_path_safe(info, wx, wy, C.LGM_DANGER_HIGH, now, world) then
        local why = b.mode == "base_shield" and "building wall to protect refuel"
                                              or "building wall for pill attack"
        log.reason("build", { mode = b.mode, why = why, wall_mx = wx, wall_my = wy })
        return { x = wx, y = wy, action = BUILDMODE_BUILD }
      end
    end
    -- Wall already exists or can't build safely — fall through to default
    if b.mode == "wall_shield" then return nil end
    -- base_shield: wall built or unsafe; fall through to repair_nearby / road-ahead
  end

  -- Priority 3: gather — need trees before we can execute the plan
  if b.mode == "gather" and info.trees < b.need_trees then
    -- Quick reject: any threat at tank tile means lgm_path_safe(LOW) will fail
    if state.perc and state.perc.threat_at_tank > C.LGM_DANGER_LOW then return nil end
    local fx, fy, fd = nearest_onpath_forest(state, info, C.FARM_GATHER_RADIUS)
    if fx and fd <= C.LGM_DEPLOY_DIST
       and lgm_can_reach(info, fx, fy)
       and danger.lgm_path_safe(info, fx, fy, C.LGM_DANGER_LOW, now, world) then
      return { x = fx, y = fy, action = BUILDMODE_FARM }
    end
    -- Forest not reachable or unsafe; don't fall through to road building
    -- (don't burn trees on roads while we still need them for the mission)
    return nil
  end

  -- Priority 4: opportunistic on-path farm
  -- Applies in "gather" (already stocked) and "opportunistic" modes.
  -- Only fills to TREE_OPPORTUNISTIC_MAX so we don't over-farm.
  -- Radius is kept small (FARM_OPPORTUNISTIC_RADIUS) to limit pacing slowdown:
  -- the LGM must be able to farm and catch up before the tank moves far.
  if (b.mode == "gather" or b.mode == "opportunistic")
     and info.trees < C.TREE_OPPORTUNISTIC_MAX
     and not info.inboat then
    -- Quick reject: any threat at tank tile means lgm_path_safe(LOW) will fail
    if state.perc and state.perc.threat_at_tank > C.LGM_DANGER_LOW then
      return road_ahead(state, info, now, world)
    end
    local fx, fy, fd = nearest_onpath_forest(state, info, C.FARM_OPPORTUNISTIC_RADIUS)
    if fx and fd <= C.LGM_DEPLOY_DIST
       and lgm_can_reach(info, fx, fy)
       and danger.lgm_path_safe(info, fx, fy, C.LGM_DANGER_LOW, now, world) then
      return { x = fx, y = fy, action = BUILDMODE_FARM }
    end
  end

  -- Priority 5: repair nearby friendly pill (repair_nearby mode)
  -- TODO: scan world.pills for nearest friendly pill with health < PILLS_MAX_HEALTH
  -- within LGM_DEPLOY_DIST, dispatch LGM to repair it.

  -- Default: road ahead of current path
  return road_ahead(state, info, now, world)
end

return M
