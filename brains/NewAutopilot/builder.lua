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
local PF     = require("pathfinder")
local print2 = require("print2")
local viz    = require("viz")

local M = {}

-- Hoisted: was reallocated inside the wall-build threat-blocker
-- inner loop (per pill_threat × per direction = up to ~30 allocs/tick
-- when in build mode). Module-scope constant.
local BAD_TERRAIN_FOR_WALL = {
  [C.T_BUILDING]=true, [C.T_HALFBUILD]=true,
  [C.T_RIVER]=true, [C.T_DEEPSEA]=true,
  [C.T_PILLBOX]=true, [C.T_SWAMP]=true,
}

-- -------------------------------------------------------------------------
-- Mode mapping: tank goal kind → builder mode
-- -------------------------------------------------------------------------
local GOAL_TO_MODE = {
  flee_to_base   = "opportunistic",  -- danger check in lgm_path_safe handles safety
  rescue_lgm     = "suppressed",
  defend_pill    = "suppressed",
  attack_pill    = "suppressed",     -- overridden to "wall_shield" below when applicable
  -- bpc_pill removed: unified into attack_pill
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
  wait_for_lgm         = "suppressed",  -- whole point is to NOT dispatch the LGM
}

-- -------------------------------------------------------------------------
-- set_mode: call each tick after goal selection to populate state.builder
-- -------------------------------------------------------------------------
function M.set_mode(state, world, info, goal)
  local b    = state.builder
  local kind = goal.kind

  b.mode           = GOAL_TO_MODE[kind] or "opportunistic"
  b.target         = nil
  b.need_trees     = 0
  b.defensive_debug = nil

  -- Pre-flight tree gather for PPT pill takes. The plan_position substate
  -- transitions here when the bot is short on trees for the planned shield
  -- walls; we want the existing smart gather (Priority 3 in decide()) to
  -- pick a nearby reachable forest. need_trees drives the gate-out at
  -- decide():608.
  if kind == "attack_pill" and goal.substate == "gather_trees" then
    b.mode       = "gather"
    b.need_trees = goal._trees_for_walls or 0
  end

  -- Wall-shield attack: dispatch LGM to build/rebuild wall in specific substates
  if kind == "attack_pill" and goal.wall_shield and goal.wall_mx then
    local sub = goal.substate or ""
    if sub == "ws_prebuild" or sub == "ws_rebuild" or sub == "build_walls" then
      -- LGM should go build/rebuild the wall. "build_walls" is the
      -- shield-scan-driven multi-wall build phase from attack.lua —
      -- attack.lua advances goal.wall_mx/my to the next site itself,
      -- so the builder just dispatches whichever target is current.
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

  -- Defensive build during attack_tank: if carrying a pill and LGM is in
  -- the tank, send LGM to place a pill at ±45° from the enemy direction
  -- (2-5 tiles out).  Tank continues fighting; LGM runs out simultaneously.
  if kind == "attack_tank"
     and (info.carried_pills or 0) >= 1
     and info.man_status == C.LGM_INTANK
     and not info.inboat then
    local perc = state.perc
    if perc and perc.enemy_tanks and #perc.enemy_tanks > 0 then
      local tmx = info.tankx >> 8
      local tmy = info.tanky >> 8
      local closest_et, closest_dist = nil, math.huge
      for _, et in ipairs(perc.enemy_tanks) do
        if et.dist < closest_dist then closest_dist = et.dist; closest_et = et end
      end
      if closest_et then
        local aim = U.aim_at(info.tankx, info.tanky, U.m2w(closest_et.mx), U.m2w(closest_et.my))
        local found_mx, found_my, found_dist, found_aoff = nil, nil, nil, nil
        for dist = C.DEFENSIVE_BUILD_MAX_DIST, C.DEFENSIVE_BUILD_MIN_DIST, -1 do
          if found_mx then break end
          for _, aoff in ipairs({ C.DEFENSIVE_BUILD_ANGLE_OFFSET, -C.DEFENSIVE_BUILD_ANGLE_OFFSET }) do
            local angle = (aim + aoff) % 256
            local rad   = angle * (math.pi * 2 / 256)
            local dx    = math.sin(rad)
            local dy    = -math.cos(rad)
            local cx    = U.mclamp(math.floor(tmx + dx * dist + 0.5))
            local cy    = U.mclamp(math.floor(tmy + dy * dist + 0.5))
            if not U.is_placeable(cx, cy, world) then goto next_bdef_angle end
            if PF.wall_hp_between(tmx, tmy, cx, cy) ~= 0 then goto next_bdef_angle end
            do
              local water_blocked = false
              for step = 1, dist - 1 do
                local ix = U.mclamp(math.floor(tmx + dx * step + 0.5))
                local iy = U.mclamp(math.floor(tmy + dy * step + 0.5))
                if U.is_water(U.ttype(ix, iy)) then water_blocked = true; break end
              end
              if water_blocked then goto next_bdef_angle end
            end
            found_mx, found_my, found_dist, found_aoff = cx, cy, dist, aoff
            break
            ::next_bdef_angle::
          end
        end
        if found_mx then
          b.mode       = "place_pill"
          b.pill_target = { mx = found_mx, my = found_my }
          b.defensive_debug = {
            found    = true,
            mx       = found_mx, my    = found_my,
            aim      = aim,      dist  = found_dist, aoff = found_aoff,
            enemy_mx = closest_et.mx,  enemy_my = closest_et.my,
          }
        else
          b.defensive_debug = { found = false, reason = "no valid spot" }
        end
      else
        b.defensive_debug = { found = false, reason = "no enemy" }
      end
    else
      b.defensive_debug = { found = false, reason = "no enemy tanks" }
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
  if not danger.lgm_path_safe_enhanced(info, nmx, nmy, C.LGM_DANGER_LOW, now, world) then
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

  -- Compute LGM ETA when out on a mission (for base departure timing)
  if info.man_status == C.LGM_MOVING then
    local man_mx = info.man_x >> 8
    local man_my = info.man_y >> 8
    local tmx = info.tankx >> 8
    local tmy = info.tanky >> 8
    local ticks = cpf_lgm_travel_ticks_map(man_mx, man_my, tmx, tmy, 0, 0, 2000, 150)
    b.lgm_eta = ticks > 0 and (now + ticks) or nil
  else
    b.lgm_eta = nil
  end

  -- LGM must be in the tank and available
  if info.man_status ~= C.LGM_INTANK then return nil end

  -- Never dispatch the LGM while in a boat.  The pacing slowdown drops
  -- the tank below disembark speed, stranding it on water.
  if info.inboat then return nil end

  -- Don't dispatch when the next pathfinder step is water — tank is about
  -- to enter a boat and LGM would be stranded immediately.
  local pf = state.pf
  if pf and pf.next_mx and pf.next_mx >= 0 then
    local next_tt = U.ttype(pf.next_mx, pf.next_my)
    if next_tt == C.T_RIVER or next_tt == C.T_DEEPSEA or next_tt == C.T_BOAT then
      return nil
    end
  end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  -- Priority 0.4: repair_pill dispatch (FORCED mode — no danger gate).
  -- As soon as we're within 5 tiles of the target friendly damaged pill
  -- AND the LGM's tile-walk reachability sim says it can actually arrive,
  -- dispatch BUILDMODE_PBOX onto the pill tile. Engine-side (lgm.c:1060+)
  -- treats an empty-handed LGM walking onto a pill while carrying trees
  -- as a repair: pillsRepairPos consumes the trees, restores health.
  --
  -- After dispatch the brain is free to pick a new goal — the LGM
  -- runs the repair autonomously. state._repair_dispatched signals
  -- init.lua to clear state.goal back to "none".
  if state.goal and state.goal.kind == "repair_pill"
     and state.goal.mx and state.goal.my then
    local px, py = state.goal.mx, state.goal.my
    local dist  = U.mdist(tmx, tmy, px, py)
    -- Danger-blended distance cap. Insist on dist<=5 when we're not
    -- under fire; widen toward DIST_DANGEROUS as the tank's local
    -- danger climbs from DANGER_LOW to DANGER_HIGH. The tank still
    -- navigates toward the pill (goal.mx/my unchanged), so it keeps
    -- closing — we just stop EARLIER when staying close would cost
    -- armour. "Get as close as we can while it's safe" naturally
    -- emerges from re-evaluating the cap each tick.
    local DANGER_LOW, DANGER_HIGH = 50, 150
    local DIST_BASE, DIST_DANGEROUS = 5, 12
    local danger_at_tank = (state.perc and state.perc.threat_at_tank) or 0
    local t = (danger_at_tank - DANGER_LOW) / (DANGER_HIGH - DANGER_LOW)
    if t < 0 then t = 0 elseif t > 1 then t = 1 end
    local effective_max = DIST_BASE + (DIST_DANGEROUS - DIST_BASE) * t
    local in_range = dist <= effective_max
    local has_trees = info.trees > 0
    local ticks = (in_range and has_trees)
      and cpf_lgm_travel_ticks_map(tmx, tmy, px, py, 0, 0, 2000, 150)
      or -1
    local can_dispatch = in_range and has_trees and ticks > 0

    if BRAIN_DEBUG_MODE then
      -- Status circle on the pill: green = dispatch fires this tick,
      -- yellow = in goal but blocked on prerequisites, red = LGM can't
      -- reach. Status line text on the tank.
      local r, g, b
      if can_dispatch then          r, g, b =   0, 255,   0
      elseif ticks == 0 then        r, g, b = 255, 100, 100
      else                          r, g, b = 255, 220,   0 end
      viz.circle("repair_pill_viz", px + 0.5, py + 0.5, 0.55, r, g, b, 220)
      viz.circle("repair_pill_viz", px + 0.5, py + 0.5, 0.30, r, g, b, 180)
      local tank_fx = info.tankx / 256.0
      local tank_fy = info.tanky / 256.0
      viz.line("repair_pill_viz", tank_fx, tank_fy, px + 0.5, py + 0.5,
               r, g, b, 120)
      viz.text("repair_pill_viz", tank_fx + 0.6, tank_fy - 1.2,
               string.format("Repair d=%d/%.1f tr=%d dgr=%d lgm=%d",
                             dist, effective_max, info.trees,
                             danger_at_tank, ticks),
               "topleft", r, g, b, 240)
      print2(string.format(
        "REPAIR_DISPATCH_CHECK pill=(%d,%d) tank=(%d,%d) dist=%d eff_max=%.1f trees=%d danger=%d lgm_ticks=%d can=%s",
        px, py, tmx, tmy, dist, effective_max, info.trees,
        danger_at_tank, ticks, tostring(can_dispatch)))
    end

    if can_dispatch then
      state._repair_dispatched = true
      if BRAIN_DEBUG_MODE then
        print2(string.format(
          "REPAIR_DISPATCH_FIRED pill=(%d,%d) action=BUILDMODE_PBOX", px, py))
      end
      return { x = px, y = py, action = BUILDMODE_PBOX }
    end
  end

  -- Priority 0.5: base shield — build wall to block pill fire while on any base.
  -- Triggers ONLY on the tick we take damage (pill just fired → max window
  -- before next shot). Checks all 8 directions for the best blocking tile.
  -- Allow base shield when ON the base or within 1 tile of it.
  -- Wall must be placed on one of the 8 tiles adjacent to the base.
  local has_base = info.base and info.base.x
  local near_base = has_base and U.mdist(tmx, tmy, info.base.x, info.base.y) <= 1
  local has_trees = info.trees >= C.BASE_SHIELD_BUILD_COST
  local took_dmg = state.took_damage_this_tick
  local pill_threats_exist = state.perc and state.perc.pill_threats and #state.perc.pill_threats > 0

  -- Always show precondition status on the HUD when near a base
  if BRAIN_DEBUG_MODE and has_base then
    local parts = {}
    parts[#parts + 1] = near_base and "near_base:YES" or string.format("near_base:NO(dist=%d)", has_base and U.mdist(tmx, tmy, info.base.x, info.base.y) or -1)
    parts[#parts + 1] = has_trees and string.format("trees:YES(%d)", info.trees) or string.format("trees:NO(%d<%d)", info.trees, C.BASE_SHIELD_BUILD_COST)
    parts[#parts + 1] = took_dmg and "took_dmg:YES" or "took_dmg:NO"
    parts[#parts + 1] = pill_threats_exist and string.format("threats:%d", #state.perc.pill_threats) or "threats:0"
    local all_ok = near_base and has_trees and took_dmg and pill_threats_exist
    local cr, cg = all_ok and 0 or 200, all_ok and 200 or 100
    viz.hud_text("base_shield_viz", 10, 84, "BaseShield: " .. table.concat(parts, " | "), "topleft", cr, cg, 0)
  end

  if near_base and has_trees and took_dmg then
    local bmx, bmy = info.base.x, info.base.y
    local perc = state.perc
    local pill_threats = perc and perc.pill_threats or {}
    local DX8 = { 0, 1, 1, 1, 0, -1, -1, -1 }
    local DY8 = { -1, -1, 0, 1, 1, 1, 0, -1 }
    for _, pt in ipairs(pill_threats) do
      if pt.dist >= C.BASE_SHIELD_MIN_DIST then
        local pm = pt.pill
        -- Try all 8 tiles adjacent to the BASE, pick the one that best
        -- blocks the line from pill to base (smallest angle to pill direction)
        local pill_dir = math.atan(pm.mx - bmx, -(pm.my - bmy))
        local best_wx, best_wy, best_score = nil, nil, math.huge
        for d = 1, 8 do
          local wx, wy = bmx + DX8[d], bmy + DY8[d]
          if U.in_map(wx, wy) then
            local wtt = U.ttype(wx, wy)
            if not BAD_TERRAIN_FOR_WALL[wtt] then
              -- Score: how well does this tile block the pill's line to the base?
              -- Lower = better blocker (closer to the pill direction from base)
              local tile_dir = math.atan(wx - bmx, -(wy - bmy))
              local diff = math.abs(pill_dir - tile_dir)
              if diff > math.pi then diff = 2 * math.pi - diff end
              if diff < best_score then
                best_score = diff
                best_wx, best_wy = wx, wy
              end
            end
          end
        end
        if best_wx and lgm_can_reach(info, best_wx, best_wy) then
          b.mode = "base_shield"
          b.wall_target = { mx = best_wx, my = best_wy }
          -- Store for persistent visualization
          state._base_shield_viz = {
            wall_mx = best_wx, wall_my = best_wy,
            pill_mx = pm.mx, pill_my = pm.my,
            base_mx = bmx, base_my = bmy,
            anger = pt.anger, dist = pt.dist,
            tick = now,
          }
          if BRAIN_DEBUG_MODE then
            print2(string.format("base_shield: wall@(%d,%d) vs pill@(%d,%d) anger=%.2f dist=%d (just took damage)",
              best_wx, best_wy, pm.mx, pm.my, pt.anger, pt.dist))
          end
          break
        end
      end
    end
  end

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
       and danger.lgm_path_safe_enhanced(info, bx, by, C.LGM_DANGER_HIGH, now, world) then
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
    if danger.lgm_path_safe_enhanced(info, bx, by, C.LGM_DANGER_MED, now, world) then
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
    if gk ~= "attack_pill" and gk ~= "pill_place"
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
       and danger.lgm_path_safe_enhanced(info, px, py, C.LGM_DANGER_HIGH, now, world) then
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
      -- Exclude the TARGET pill (wall_shield) and the pill we're shielding
      -- against (base_shield) — those are the ones we're trying to block.
      local angry_pill_close = false
      local pill_threats = state.perc and state.perc.pill_threats or {}
      local tp = b.target_pill
      -- For base_shield, the shield pill's position is stored on the goal
      local sp_mx = state.goal and state.goal.shield_wall_mx and state.perc
                    and state.perc.fire_source_mx
      local sp_my = state.perc and state.perc.fire_source_my
      for _, pt in ipairs(pill_threats) do
        -- Skip the target pill (wall_shield) or shield source pill (base_shield)
        if tp and pt.pill.mx == tp.mx and pt.pill.my == tp.my then
          goto next_pill_threat
        end
        if sp_mx and pt.pill.mx == sp_mx and pt.pill.my == sp_my then
          goto next_pill_threat
        end
        if pt.anger > 0.3 and U.mdist(wx, wy, pt.pill.mx, pt.pill.my) <= C.PILL_RANGE_MAP then
          angry_pill_close = true
          break
        end
        ::next_pill_threat::
      end
      local has_trees   = info.trees >= cost
      local can_reach   = lgm_can_reach(info, wx, wy)
      -- Exclude the target pill's own per-tile contribution from the
      -- safety check — we're committed to killing it, so its danger
      -- footprint shouldn't bully our LGM dispatch within its own
      -- range. Mirrors goals.lua's self_dr trick. tp is the
      -- target_pill set above when mode == "wall_shield".
      local excl_mx = tp and tp.mx or nil
      local excl_my = tp and tp.my or nil
      local path_safe   = can_reach and
                          danger.lgm_path_safe_enhanced(info, wx, wy,
                              C.LGM_DANGER_HIGH, now, world,
                              excl_mx, excl_my)
      -- Wall-shield builds for an in-progress pill take are FORCED:
      -- the take strategy is already committed to walking into pill
      -- fire, the wall is what makes that survivable, and risking
      -- the LGM to get it up is part of the deal. Gates 1 (angry
      -- pill in range) and 4 (LGM path danger) are bypassed — only
      -- the hard-physical gates 2 (trees on hand) and 3 (LGM can
      -- physically reach the tile) still apply. base_shield (the
      -- refuel-defense variant) keeps full safety.
      local force_mode = (b.mode == "wall_shield")
      local safety_ok  = force_mode
        or (not angry_pill_close and path_safe)

      -- Pillbox-as-blocker: if the bot has carried pills on hand,
      -- spend up to PILLBOX_BLOCKERS_MAX of them on the closest shield
      -- slots instead of building walls. A pillbox is a much better
      -- blocker than a wall — it actively shoots back at enemies. Only
      -- applies to wall_shield (attack_pill); base_shield keeps its
      -- defensive-wall economy. PBOX placement doesn't need trees, but
      -- the engine refuses on FOREST tiles so we still farm those first.
      local PILLBOX_BLOCKERS_MAX = 2
      local pbox_used = (state.goal and state.goal._pillbox_blockers_used) or 0
      local can_pbox = b.mode == "wall_shield"
                       and (info.carried_pills or 0) > 0
                       and pbox_used < PILLBOX_BLOCKERS_MAX
                       and wtt ~= C.T_FOREST
                       and can_reach
                       and safety_ok
      if can_pbox then
        state.goal._pillbox_blockers_used = pbox_used + 1
        log.reason("build", { mode = b.mode,
                              why = "drop pillbox as wall blocker",
                              wall_mx = wx, wall_my = wy,
                              blockers_used = state.goal._pillbox_blockers_used })
        return { x = wx, y = wy, action = BUILDMODE_PBOX }
      end

      if has_trees and can_reach and safety_ok then
        -- Forest in the way? The engine can't drop a wall on T_FOREST;
        -- BUILDMODE_BUILD there just clears the trees, no wall goes up.
        -- Dispatch FARM first to harvest, then the next builder tick
        -- will see grass/road and dispatch the actual BUILD. Two
        -- separate LGM round-trips, but the wall_shield idx in attack.lua
        -- only advances on T_BUILDING/T_HALFBUILD so it'll keep
        -- targeting the same tile until the wall is genuinely up.
        if wtt == C.T_FOREST then
          log.reason("build", { mode = b.mode, why = "harvest forest before wall",
                                wall_mx = wx, wall_my = wy })
          return { x = wx, y = wy, action = BUILDMODE_FARM }
        end
        local why = b.mode == "base_shield" and "building wall to protect refuel"
                                              or "building wall for pill attack"
        log.reason("build", { mode = b.mode, why = why, wall_mx = wx, wall_my = wy })
        return { x = wx, y = wy, action = BUILDMODE_BUILD }
      end
      -- Stash gate failure on state so the brain can surface it (and
      -- the upcoming on-screen viz can read it). Also persist the most
      -- recent skip reason for trace logs / a future overlay.
      state._wall_shield_skip = {
        tick           = now,
        wx             = wx, wy = wy,
        angry_pill_close = angry_pill_close,
        has_trees      = has_trees,
        trees_have     = info.trees,
        trees_need     = cost,
        can_reach      = can_reach,
        path_safe      = path_safe,
        force_mode     = force_mode,
      }
      log.reason("build_skip", {
        mode = b.mode, wall_mx = wx, wall_my = wy,
        angry = angry_pill_close,
        trees = string.format("%d/%d", info.trees, cost),
        reach = can_reach, safe  = path_safe,
        force = force_mode,
      })
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
       and danger.lgm_path_safe_enhanced(info, fx, fy, C.LGM_DANGER_LOW, now, world) then
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
    -- Wider radius when stationary at refuel base — tank isn't moving so
    -- LGM has time to walk further without pacing issues.
    local is_refuel_stationary = state.goal and state.goal.kind == "refuel_at_base"
        and info.speed == 0
        and (info.tankx >> 8) == (state.goal.mx or -1)
        and (info.tanky >> 8) == (state.goal.my or -1)
    local farm_radius = is_refuel_stationary and C.FARM_REFUEL_RADIUS
                                              or C.FARM_OPPORTUNISTIC_RADIUS
    local deploy_dist = is_refuel_stationary and C.LGM_DEPLOY_DIST_REFUEL
                                              or C.LGM_DEPLOY_DIST
    local fx, fy, fd = nearest_onpath_forest(state, info, farm_radius)
    if fx and fd <= deploy_dist
       and lgm_can_reach(info, fx, fy)
       and danger.lgm_path_safe_enhanced(info, fx, fy, C.LGM_DANGER_LOW, now, world) then
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
