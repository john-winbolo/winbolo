-- =========================================================================
-- NewAutopilot/steering.lua — translate goal + pathfinder into holdkeys/tapkeys
-- =========================================================================

local C   = require("constants")
local U   = require("util")
local PF  = require("pathfinder")
local cpf = require("cpathfinder")
local log = require("logger")
local bpc = require("bpc")

local M = {}

-- Local alias for the shared turn+speed helper in util.lua
local nav_turn_speed = U.nav_turn_speed

-- Wrapper: call C pathfinder and update state.pf for compatibility with
-- stuck detection, debug logging, and other consumers of state.pf.
local function cpf_path_to(state, info, dest_mx, dest_my)
  local pf  = state.pf
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local in_boat = (info.inboat ~= 0) and 1 or 0
  local shells  = info.shells or 0
  local trees   = info.trees or 0
  local mines   = info.mines or 0
  local armour  = info.armour or 40

  -- Save previous next step as fallback while new path computes
  local fallback_nx  = pf.next_mx
  local fallback_ny  = pf.next_my
  local tank_moved   = (pf.src_mx ~= tmx or pf.src_my ~= tmy)
  local dest_changed = (pf.dest_mx ~= dest_mx or pf.dest_my ~= dest_my)
  local use_fallback = tank_moved and not dest_changed and fallback_nx >= 0

  local status, nx, ny = cpf.path_to(tmx, tmy, dest_mx, dest_my, in_boat, shells, trees, mines, armour, C.ASTAR_BUDGET)

  -- Update state.pf tracking fields
  pf.src_mx  = tmx
  pf.src_my  = tmy
  pf.dest_mx = dest_mx
  pf.dest_my = dest_my

  if status == 1 then      -- done
    pf.status  = "done"
    pf.next_mx = nx
    pf.next_my = ny
    pf.age     = 0
    -- Capture full path chain for debug logging
    pf.path_chain = cpf.trace_path()
  elseif status == 0 then  -- running
    pf.status = "running"
    if nx >= 0 then
      pf.next_mx = nx
      pf.next_my = ny
    end
    pf.age = (pf.age or 0) + 1
  else                      -- failed (-1)
    pf.status  = "failed"
    pf.next_mx = -1
    pf.next_my = -1
  end

  if (pf.status == "done" or pf.status == "running") and pf.next_mx >= 0 then
    return pf.next_mx, pf.next_my
  end
  if use_fallback then
    return fallback_nx, fallback_ny
  end
  return nil, nil
end

-- Impassable terrain types for path lookahead line-of-sight checks.
local IMPASSABLE = {
  [C.T_BUILDING]  = true,
  [C.T_HALFBUILD] = true,
  [C.T_DEEPSEA]   = true,
}

-- Water terrain types: tiles where the tank rides the boat.
local WATER_TT = {
  [C.T_RIVER]   = true,
  [C.T_DEEPSEA] = true,
  [C.T_BOAT]    = true,  -- boat pickup tile is on water
}

-- Path lookahead: given the next A* step (nx, ny), walk pf.path_chain
-- forward and return the furthest waypoint reachable in a clear straight
-- line from the tank.  This eliminates per-tile wiggle on straight runs.
-- Returns the lookahead waypoint (lx, ly) or (nx, ny) if no skip is possible.
--
-- Boat-aware:
--   On foot: stop at BOAT tiles (must step on them to pick up).
--   In boat: stop at any water/land boundary.  Cutting diagonals through
--            a river corridor can clip a land tile and lose the boat.
--            Also stop at BOAT tiles on water (transition point).
local function path_lookahead(state, info, nx, ny)
  local pf = state.pf
  local chain = pf.path_chain
  if not chain or #chain < 2 then return nx, ny end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local in_boat = (info.inboat ~= 0)
  local on_foot = not in_boat

  -- When in a boat, disable lookahead entirely — follow the exact path.
  -- River channels are narrow and any diagonal cut can clip a land tile,
  -- causing the boat to be lost.
  if in_boat then return nx, ny end

  -- Find our current position in the chain
  local start_idx = nil
  for i = 1, #chain do
    if chain[i].x == nx and chain[i].y == ny then
      start_idx = i
      break
    end
  end
  if not start_idx then return nx, ny end

  -- Walk forward, checking line-of-sight to each candidate
  local best_x, best_y = nx, ny
  for i = start_idx + 1, #chain do
    local cx, cy = chain[i].x, chain[i].y
    -- Must-visit: BOAT tile when on foot (need to pick it up)
    if on_foot and U.in_map(cx, cy) and U.ttype(cx, cy) == C.T_BOAT then
      best_x, best_y = cx, cy
      break
    end
    -- Must-visit: water tile when on foot with a boat — this is the
    -- land-to-water entry point; don't skip past it or we may enter
    -- water at the wrong spot and lose the boat.
    if on_foot and U.in_map(cx, cy) and WATER_TT[U.ttype(cx, cy)] then
      best_x, best_y = cx, cy
      break
    end
    -- Check that the straight line from tank to this waypoint is clear
    local blocked = U.bresenham(tmx, tmy, cx, cy, function(bx, by)
      if U.in_map(bx, by) then
        local tt = U.ttype(bx, by)
        if IMPASSABLE[tt] then return true end
      else
        return true
      end
    end)
    if blocked then break end
    best_x, best_y = cx, cy
  end

  return best_x, best_y
end

-- =========================================================================
-- Pill placement steering
-- =========================================================================
local function pill_place_steer(state, world, info, goal)
  if goal.kind ~= "pill_place" then return nil end
  local keys = 0
  local taps = 0
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  -- Gunsight at max range for all substates
  if info.gunrange < C.GUNSIGHT_MAX then
    keys = keys | KEY_MORERANGE
  end

  -- ── pickup: navigate to dead friendly pill ────────────────────────
  if goal.substate == "pickup" then
    local nav_mx = goal.source_mx or goal.mx
    local nav_my = goal.source_my or goal.my
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  -- ── navigate: A* to deploy position (outside pill range) ───────────
  if goal.substate == "navigate" then
    local nav_mx = goal.deploy_mx or goal.place_mx or goal.mx
    local nav_my = goal.deploy_my or goal.place_my or goal.my
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  -- ── dispatch / wait_place: hold position or move to engage ────────
  if goal.substate == "dispatch" then
    -- Hold position while LGM goes to place pill
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  if goal.substate == "wait_place" or goal.substate == "prewait" then
    -- Hold position at deploy spot while LGM builds/returns.
    -- Moving toward engage now would put us inside pill range unshielded.
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  -- ── Post-placement states: delegate to general steer ──────────────
  -- These use the EXACT same steering as attack_pill (wall-shield).
  -- Returning nil makes the main steer() handle navigation + aim/fire.
  if goal.substate == "advance" or goal.substate == "shield_engage"
     or goal.substate == "engage" or goal.substate == "reposition" then
    return nil
  end

  -- ── collect_target: rush to dead pill ─────────────────────────────
  if goal.substate == "collect_target" then
    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      -- Rush: higher max speed, we want to get there fast
      local k, t = nav_turn_speed(corr, info.speed, 64)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  -- ── select_pill / disengage: brake ────────────────────────────────
  if info.speed > 0 then keys = keys | KEY_SLOWER end
  return keys, taps
end

-- Returns true if every intermediate map tile on the line from (x0,y0) to
-- (x1,y1) is water (river or deep sea).  When this holds, a shell fired from
-- a boat travels over those water tiles and strikes the first land square
-- (the target tile), so shooting is valid even from a boat.
local function water_corridor_to(x0, y0, x1, y1)
  local blocked = U.bresenham(x0, y0, x1, y1, function(cx, cy)
    if not U.is_water(U.ttype(cx, cy)) then return true end
  end)
  return not blocked
end

-- =========================================================================
-- Tank combat steering — chase, aim with lead prediction, shoot, jink
-- =========================================================================
local function tank_combat_steer(state, world, info, goal)
  if goal.kind ~= "attack_tank" then return nil end
  local keys = 0
  local taps = 0
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  -- Find the current target tank from perception (it moves every tick)
  local perc = state.perc or {}
  local target = nil
  local target_dist = math.huge

  -- Match by proximity to goal position (tank may have moved since goal was set)
  for _, et in ipairs(perc.enemy_tanks or {}) do
    local d = U.mdist(et.mx, et.my, goal.mx, goal.my)
    if d < target_dist then
      target_dist = d
      target = et
    end
  end

  -- If we can't see any enemy tank near the goal, find nearest visible one
  if not target or target_dist > 8 then
    local best_d = math.huge
    for _, et in ipairs(perc.enemy_tanks or {}) do
      if et.dist < best_d then
        best_d = et.dist
        target = et
      end
    end
  end

  -- Target lost — can't see any enemy tanks
  if not target then
    -- Navigate to last known position
    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    log.reason("steer", { mode = "tank_combat_lost", goal_mx = goal.mx, goal_my = goal.my })
    return keys, taps
  end

  -- Update goal position to track the moving target
  goal.mx = target.mx
  goal.my = target.my
  goal.wx = U.m2w(target.mx)
  goal.wy = U.m2w(target.my)

  local dist_tiles = target.dist
  local twx = U.m2w(target.mx)
  local twy = U.m2w(target.my)

  -- Gunsight at max range
  if info.gunrange < C.GUNSIGHT_MAX then
    keys = keys | KEY_MORERANGE
  end

  -- Disengage check: flee if outgunned
  if info.armour <= C.TANK_COMBAT_FLEE_ARMOUR
     or info.shells <= C.TANK_COMBAT_FLEE_SHELLS then
    goal.substate = "disengage"
    -- Will be invalidated next replan
    log.reason("steer", { mode = "tank_combat_disengage",
      arm = info.armour, sh = info.shells })
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    return keys, taps
  end

  if dist_tiles > C.TANK_COMBAT_ENGAGE_RANGE then
    -- ── CLOSE: navigate toward enemy tank ──
    goal.substate = "close"
    local nx, ny = cpf_path_to(state, info, target.mx, target.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end

    -- Opportunistic shot while closing: if already aimed, fire
    local aim_dir = U.aim_at(info.tankx, info.tanky, twx, twy)
    local aim_corr = U.adiff(info.direction, aim_dir)
    if math.abs(aim_corr) < C.TANK_COMBAT_OPPORTUNISTIC_AIM
       and info.shells > C.TANK_COMBAT_FLEE_SHELLS then
      keys = keys | KEY_SHOOT
    end

    log.reason("steer", { mode = "tank_combat_close",
      dist = dist_tiles, sub = "close" })
    return keys, taps
  end

  -- ── ENGAGE: in range, aim with lead prediction, shoot, and jink ──
  goal.substate = "engage"

  -- Lead-target prediction: where will the target be when our shell arrives?
  -- Shell travels at ~32 WU per step, distance in WU = dist_tiles * 256
  local wdist = U.wdist(info.tankx, info.tanky, twx, twy)
  local shell_travel_ticks = wdist / C.TANK_COMBAT_SHELL_SPEED  -- sim steps
  -- Predicted position (WU): use direction from animation frame (0-15) and wire speed.
  -- Wire speed is actual_speed * 4; actual_speed is already in WU/tick (16 on road).
  -- So WU/tick = speed / 4.
  local pred_wx, pred_wy = twx, twy
  if target.speed > 0 and target.obj then
    local frame = target.obj.direction or 0
    local angle_rad = frame * (2 * math.pi / 16)
    local spd_wu = target.speed / 4  -- WU per tick
    pred_wx = twx + math.sin(angle_rad) * spd_wu * shell_travel_ticks
    pred_wy = twy - math.cos(angle_rad) * spd_wu * shell_travel_ticks
  end

  local aim_dir = U.aim_at(info.tankx, info.tanky, pred_wx, pred_wy)
  local aim_corr = U.adiff(info.direction, aim_dir)

  -- Jink: periodic lateral offset to make us harder to hit
  -- Alternate direction every JINK_PERIOD ticks
  local jink_phase = math.floor(now / C.TANK_COMBAT_JINK_PERIOD) % 2
  local jink_offset = jink_phase == 0 and C.TANK_COMBAT_JINK_ANGLE
                                       or -C.TANK_COMBAT_JINK_ANGLE

  -- Turn toward predicted target position
  if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
  elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
  elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
  elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
  end

  -- Fire when aimed — commit fully once in combat (reserve is for non-combat use)
  if math.abs(aim_corr) < 5 and info.shells > C.TANK_COMBAT_FLEE_SHELLS then
    keys = keys | KEY_SHOOT
  end

  -- Distance control: maintain optimal range with jinking
  if dist_tiles < C.TANK_COMBAT_TOO_CLOSE then
    -- Too close: reverse away
    if info.speed > 0 then keys = keys | KEY_SLOWER end
    -- Jink by turning slightly off-axis
    if jink_offset > 0 then
      taps = taps | KEY_TURNRIGHT
    else
      taps = taps | KEY_TURNLEFT
    end
  elseif dist_tiles <= C.TANK_COMBAT_ENGAGE_RANGE then
    -- In range: hold moderate speed for evasion, use jink
    local desired_speed = 12  -- keep moving to dodge
    if info.speed > desired_speed + 4 then
      keys = keys | KEY_SLOWER
    elseif info.speed < desired_speed then
      keys = keys | KEY_FASTER
    end
  end

  log.reason("steer", {
    mode = "tank_combat_engage",
    dist = dist_tiles, aim_corr = aim_corr,
    lead_wx = pred_wx, lead_wy = pred_wy,
    speed = target.speed, dir = target.obj and target.obj.direction or 0,
    ob_speed = target.obj and target.obj.speed or -1,
    spd_wu = target.speed / 4,
    wdist = wdist, shell_t = shell_travel_ticks,
    jink = jink_offset,
    firing = math.abs(aim_corr) < 5 and info.shells > C.TANK_COMBAT_FLEE_SHELLS,
  })
  return keys, taps
end

function M.steer(state, world, info, goal)
  local keys = 0
  local taps = 0
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8

  -- Tank combat: self-contained steering for attack_tank goals
  if goal.kind == "attack_tank" then
    local k, t = tank_combat_steer(state, world, info, goal)
    if k then return k, t end
  end

  -- BPC: self-contained module handles all bpc_pill steering
  if goal.kind == "bpc_pill" then
    local k, t = bpc.steer(state, world, info, goal)
    if k then return k, t end
  end

  -- Pill placement: self-contained steering for all pill_place substates
  if goal.kind == "pill_place" then
    local k, t = pill_place_steer(state, world, info, goal)
    if k then return k, t end
  end

  -- Perception cache: read once at top of steer
  local perc = state.perc or {}
  local under_fire = perc.under_fire or false

  -- Determine desired movement direction
  local move_dir    = nil
  local target_dist = 0x7FFF
  local goal_dist   = 0x7FFF

  if goal.kind == "escape_water" then
    -- Bypass A*: steer directly at dry land
    move_dir    = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    target_dist = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    goal_dist   = target_dist

  -- BPC stand_shoot: park at standoff distance, face the pill, pump shells.
  -- ErYan's "hardline" technique: stand still and shoot until hit a few
  -- times, then curve away.  No forward movement — knockback from the pill
  -- nudges the tank slightly but it essentially stays put.
  elseif goal.kind == "bpc_pill" and goal.substate == "stand_shoot" then
    local pwx, pwy = goal.wx, goal.wy
    local wdist_pill = U.wdist(info.tankx, info.tanky, pwx, pwy)
    local dist_tiles = wdist_pill / 256.0

    local aim_dir  = U.aim_at(info.tankx, info.tanky, pwx, pwy)
    local aim_corr = U.adiff(info.direction, aim_dir)

    -- Face the pill
    if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
    elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
    elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
    elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
    end

    -- Brake to a stop — stand and shoot
    if info.speed > 0 then
      keys = keys | KEY_SLOWER
    end

    -- Gunsight at max range
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    -- Fire when aimed
    local firing = false
    if math.abs(aim_corr) < 4 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
      firing = true
    end

    log.reason("steer", {
      mode = "bpc_stand_shoot", dist_tiles = dist_tiles,
      aim_corr = aim_corr, firing = firing,
    })
    return keys, taps

  -- BPC curve_away: after taking enough hits, turn hard to one side.
  -- The pill's predictive aim overshoots the curve, so its shots miss.
  -- The tank curves around and drives away from the pill.
  elseif goal.kind == "bpc_pill" and goal.substate == "curve_away" then
    local pwx, pwy = goal.wx, goal.wy
    local wdist_pill = U.wdist(info.tankx, info.tanky, pwx, pwy)
    local dist_tiles = wdist_pill / 256.0

    local aim_dir  = U.aim_at(info.tankx, info.tanky, pwx, pwy)
    local aim_corr = U.adiff(info.direction, aim_dir)

    -- Constant turn in the chosen direction (default right)
    local turn_dir = goal.curve_dir or 1
    if turn_dir > 0 then
      keys = keys | KEY_TURNRIGHT
    else
      keys = keys | KEY_TURNLEFT
    end

    -- Full speed to escape
    keys = keys | KEY_FASTER

    -- Gunsight at max range
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    -- Fire if we happen to sweep past the pill during the curve
    local firing = false
    if math.abs(aim_corr) < 6 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
      firing = true
    end

    log.reason("steer", {
      mode = "bpc_curve_away", dist_tiles = dist_tiles,
      aim_corr = aim_corr, curve_dir = turn_dir, firing = firing,
    })
    return keys, taps

  -- BPC rush: pill is dead, drive straight to it
  -- In a boat: skip to general navigation for wall-clearing + boat_exit
  elseif goal.kind == "bpc_pill" and goal.substate == "rush" and info.inboat == 0 then
    -- Navigate to pill tile via A*
    local nav_mx, nav_my = goal.mx, goal.my
    local nav_wx, nav_wy = goal.wx, goal.wy
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)
    if nx then
      local step_wx = U.m2w(nx)
      local step_wy = U.m2w(ny)
      move_dir    = U.aim_at(info.tankx, info.tanky, step_wx, step_wy)
      target_dist = U.wdist(info.tankx, info.tanky, step_wx, step_wy)
    else
      local center_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
      if center_dist > 64 then
        move_dir    = U.aim_at(info.tankx, info.tanky, nav_wx, nav_wy)
        target_dist = center_dist
      end
    end
    goal_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)

    if move_dir then
      local corr = U.adiff(info.direction, move_dir)
      -- Rush: higher max speed, we want to capture fast
      local k, t = nav_turn_speed(corr, info.speed, 64)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end

    log.reason("steer", { mode = "bpc_rush", goal_dist = goal_dist })
    return keys, taps

  elseif goal.kind ~= "none" and goal.kind ~= "refuel_at_base" then
    -- For attack_pill: navigate to the planned standoff position (pre-scored by
    -- goals.lua for land quality, crossfire, pushback, and escape cost).
    -- Fall back to the old "approach from current direction" heuristic only if
    -- no standoff was planned (e.g. goal was created before the planner existed).
    local nav_mx, nav_my = goal.mx, goal.my
    local nav_wx, nav_wy = goal.wx, goal.wy
    if goal.kind == "attack_pill" or goal.kind == "pill_place" then
      if goal.wall_shield and goal.substate == "approach" and goal.prebuild_mx then
        -- Wall-shield: navigate to prebuild position (outside pill range) first
        nav_mx = goal.prebuild_mx
        nav_my = goal.prebuild_my
        nav_wx = U.m2w(nav_mx)
        nav_wy = U.m2w(nav_my)
      elseif goal.standoff_mx then
        nav_mx = goal.standoff_mx
        nav_my = goal.standoff_my
        nav_wx = U.m2w(nav_mx)
        nav_wy = U.m2w(nav_my)
      else
        -- Legacy fallback: stand off on the line pill→tank
        local dx  = tmx - goal.mx
        local dy  = tmy - goal.my
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 0.1 then
          nav_mx = U.mclamp(math.floor(goal.mx + dx / len * C.ATTACK_PILL_STANDOFF + 0.5))
          nav_my = U.mclamp(math.floor(goal.my + dy / len * C.ATTACK_PILL_STANDOFF + 0.5))
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      end
    elseif goal.kind == "bpc_pill" then
      if goal.standoff_mx then
        -- BPC approach: navigate to planned standoff position
        nav_mx = goal.standoff_mx
        nav_my = goal.standoff_my
        nav_wx = U.m2w(nav_mx)
        nav_wy = U.m2w(nav_my)
      else
        -- Fallback: stand off on the line pill→tank at BPC_STANDOFF
        local dx  = tmx - goal.mx
        local dy  = tmy - goal.my
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 0.1 then
          nav_mx = U.mclamp(math.floor(goal.mx + dx / len * C.BPC_STANDOFF + 0.5))
          nav_my = U.mclamp(math.floor(goal.my + dy / len * C.BPC_STANDOFF + 0.5))
          nav_wx = U.m2w(nav_mx)
          nav_wy = U.m2w(nav_my)
        end
      end
    end

    -- Follow the A* next-step waypoint, with path lookahead to reduce wiggle
    local nx, ny = cpf_path_to(state, info, nav_mx, nav_my)

    if nx then
      -- Skip ahead on the path when the straight line is clear
      local lx, ly = path_lookahead(state, info, nx, ny)
      local step_wx, step_wy = U.m2w(lx), U.m2w(ly)

      move_dir      = U.aim_at(info.tankx, info.tanky, step_wx, step_wy)
      target_dist   = U.wdist(info.tankx, info.tanky, step_wx, step_wy)
    else
      -- On the destination tile but not centered: steer to tile center
      local center_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
      if center_dist > 64 then
        move_dir    = U.aim_at(info.tankx, info.tanky, nav_wx, nav_wy)
        target_dist = center_dist
      end
    end
    goal_dist = U.wdist(info.tankx, info.tanky, nav_wx, nav_wy)
  end

  -- Attack_pill in range with clear LOS: stop navigating and stand to fight.
  -- Must be computed before the early return below, since that return fires
  -- exactly when move_dir is nil (i.e. we've reached the standoff tile).
  --
  -- On a boat: only engage if the pill is right at the water's edge (water
  -- corridor check).  Otherwise the shell won't reach, the boat blocks us
  -- from shooting, and — critically — setting move_dir=nil here prevents
  -- the boat_exit logic below from firing, stranding the tank at the water's
  -- edge indefinitely until the pill knocks the boat off.
  local attack_in_range = false
  if (goal.kind == "attack_pill" or goal.kind == "pill_place")
     and info.shells > C.SHELL_RESERVE then
    local wdist_pill = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local wall_hp = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my)
    -- Standoff is offset so shells have clear LOS past the wall/placed pill
    local los_ok = wall_hp == 0

    if wdist_pill <= C.ATTACK_PILL_RANGE * 256 and los_ok then
      local can_engage = info.inboat == 0
                         or water_corridor_to(tmx, tmy, goal.mx, goal.my)
      -- Allow engagement in engage substates only
      local engage_substates = { engage = true, ws_engage = true, shield_engage = true }
      if can_engage and goal.substate and not engage_substates[goal.substate] then
        can_engage = false
      end
      if can_engage then
        attack_in_range = true
      end
    end
  end

  -- Wall-shield: during prebuild/prewait/rebuild, hold position (brake to stop)
  local ws_holding = goal.kind == "attack_pill" and goal.wall_shield
                     and (goal.substate == "ws_prebuild" or goal.substate == "ws_prewait"
                          or goal.substate == "ws_rebuild")
  if ws_holding then
    if info.speed > 0 then
      return KEY_SLOWER, 0
    end
    return 0, 0
  end

  -- Wall-shield retreat: move directly AWAY from the pill to escape range
  -- as fast as possible, then return to prebuild distance.
  if goal.kind == "attack_pill" and goal.wall_shield
     and goal.substate == "ws_retreat" then
    local pmx, pmy = goal.mx, goal.my
    local pwx, pwy = U.m2w(pmx), U.m2w(pmy)
    -- Direction directly away from the pill
    local dx = info.tankx - pwx
    local dy = info.tanky - pwy
    local dist = math.sqrt(dx * dx + dy * dy)
    if dist > 1 then
      local ux, uy = dx / dist, dy / dist
      -- Target: prebuild distance from pill (outside range) straight back
      local omx = U.mclamp(math.floor(pmx + ux * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local omy = U.mclamp(math.floor(pmy + uy * C.WALL_SHIELD_PREBUILD_STANDOFF + 0.5))
      local owx, owy = U.m2w(omx), U.m2w(omy)
      local offset_dir = U.aim_at(info.tankx, info.tanky, owx, owy)
      local offset_dist = U.wdist(info.tankx, info.tanky, owx, owy)

      if offset_dist > 128 then
        local corr = U.adiff(info.direction, offset_dir)
        -- Retreat: high max speed, low min speed — escape ASAP
        local k, t = nav_turn_speed(corr, info.speed, 64, 2)
        keys = keys | k
        taps = taps | t
      else
        if info.speed > 0 then keys = keys | KEY_SLOWER end
      end

      log.reason("steer", {
        mode = "ws_retreat",
        offset_mx = omx, offset_my = omy,
        offset_dist = offset_dist,
      })
      return keys, taps
    end
  end

  -- No goal or idle: brake to a stop.
  -- When attack_in_range, skip the navigation block and fall through to
  -- the engage aim/shoot block below.
  if move_dir == nil and not attack_in_range then
    if info.speed > 0 then
      keys = keys | KEY_SLOWER
    end
    return keys, taps
  end

  if move_dir ~= nil and not attack_in_range then

    -- Wall-clear: if the A* next waypoint is a wall tile, stop and shoot it
    -- down before proceeding.  This is the primary wall-clearing mechanism;
    -- the opportunistic drive-by shooting further below is a bonus for walls
    -- we happen to be aimed at while moving.
    -- Skip when under fire — stopping to demolish a wall while being shot is
    -- too dangerous; fall through to normal navigation instead.
    -- In a boat: allowed for the immediate next A* tile — shells from a boat
    -- reach the first land square, which is exactly the water-edge wall we
    -- need to clear to disembark.
    local wall_clearing = false
    state.wall_clearing = false  -- reset each tick; stuck detection reads this
    if not under_fire and info.shells > C.SHELL_RESERVE then
      local pf = state.pf
      if pf.next_mx >= 0 then
        local next_tt = U.ttype(pf.next_mx, pf.next_my)
        if next_tt == C.T_BUILDING or next_tt == C.T_HALFBUILD then
          local wall_wx = U.m2w(pf.next_mx)
          local wall_wy = U.m2w(pf.next_my)
          local wall_dist = U.wdist(info.tankx, info.tanky, wall_wx, wall_wy)
          -- Only enter wall-clear when we're within 3 tiles (close enough
          -- that we should be dealing with it, not still far away navigating)
          if wall_dist < 768 then
            wall_clearing = true
            state.wall_clearing = true
            local aim_dir = U.aim_at(info.tankx, info.tanky, wall_wx, wall_wy)
            local corr    = U.adiff(info.direction, aim_dir)

            -- Turn to face the wall
            if     corr >  10 then keys = keys | KEY_TURNRIGHT
            elseif corr < -10 then keys = keys | KEY_TURNLEFT
            elseif corr >   2 then taps = taps | KEY_TURNRIGHT
            elseif corr <  -2 then taps = taps | KEY_TURNLEFT
            end

            -- Brake to a stop so we hold position while firing
            if info.speed > 0 then
              keys = keys | KEY_SLOWER
            end

            -- Fire when roughly aimed
            if math.abs(corr) < 8 then
              taps = taps | KEY_SHOOT
            end

            if state.tick % 10 == 0 then
              log.reason("steer", {
                mode = "wall_clear",
                wall_mx = pf.next_mx, wall_my = pf.next_my,
                wall_dist = wall_dist, aim_corr = corr,
                firing = math.abs(corr) < 8,
                wall_type = next_tt == C.T_BUILDING and "full" or "half",
              })
            end
          end
        end
      end
    end

    if wall_clearing then
      -- Wall-clear has set keys/taps; skip normal navigation but still run
      -- the logging at the end of this block.
      -- Fall through to the steer log + return below.

    else -- normal navigation

    -- Turn toward move_dir
    local correction = U.adiff(info.direction, move_dir)

    if     correction >  10 then keys = keys | KEY_TURNRIGHT
    elseif correction < -10 then keys = keys | KEY_TURNLEFT
    elseif correction >   2 then taps = taps | KEY_TURNRIGHT
    elseif correction <  -2 then taps = taps | KEY_TURNLEFT
    end

    local eff_dist = goal_dist
    local brake_dist = math.max(128, info.speed * 12)
    local facing_away = math.abs(correction) > 64

    -- Goal lookahead: if we have a next_goal, steer toward it instead of
    -- braking at the current destination.  Override move_dir and eff_dist
    -- so the tank drives through the capture point at speed.
    local lookahead_active = false
    if state.next_goal and eff_dist < brake_dist and info.inboat == 0 then
      local ng = state.next_goal
      move_dir   = U.aim_at(info.tankx, info.tanky, ng.wx, ng.wy)
      eff_dist   = U.wdist(info.tankx, info.tanky, ng.wx, ng.wy)
      brake_dist = math.max(128, info.speed * 12)
      correction = U.adiff(info.direction, move_dir)
      facing_away = math.abs(correction) > 64
      lookahead_active = true
    end

    -- Emergency stop: deep sea directly ahead while on land
    local ahead_mx = (info.tankx + U.bsin(info.direction) * 2) >> 8
    local ahead_my = (info.tanky - U.bcos(info.direction) * 2) >> 8
    local cliff    = U.ttype(ahead_mx, ahead_my) == C.T_DEEPSEA and info.inboat == 0

    -- Boat-to-land transition: must maintain high speed to disembark.
    -- Only applies when the tank is actually riding the boat ON water,
    -- not when merely carrying a boat on land.
    local boat_exit = false
    if info.inboat ~= 0 and move_dir ~= nil then
      local cur_tt = U.ttype(tmx, tmy)
      local on_water = WATER_TT[cur_tt]
      if on_water then
        local next_mx = (info.tankx + U.bsin(move_dir) * 2) >> 8
        local next_my = (info.tanky - U.bcos(move_dir) * 2) >> 8
        if U.in_map(next_mx, next_my) then
          local next_tt = U.ttype(next_mx, next_my)
          if next_tt ~= C.T_RIVER and next_tt ~= C.T_DEEPSEA then
            boat_exit = true
          end
        end
      end
    end

    -- Turn-sharpness speed limit — proportional ramp
    -- Under fire: allow higher speed through turns to escape faster
    local abs_corr = math.abs(correction)
    local turn_max_speed = 256
    if abs_corr > 10 then
      local base_cap = under_fire and 64 or 48
      -- Smooth ramp: full speed at 10°, floor at 80°+
      local factor = 1.0 - math.min((abs_corr - 10) / 70.0, 1.0)
      turn_max_speed = math.max(6, math.floor(factor * base_cap))
    end

    -- Pace the LGM: if the builder is out on a *build* mission, limit tank
    -- speed so we don't outrun him (important when building bridges).
    -- Farming is excluded: the LGM walks ahead to chop a tree and returns;
    -- the tank has no reason to slow down for that.
    local lgm_out = info.man_status == C.LGM_MOVING
    local lgm_is_farming = state.builder.last_action == BUILDMODE_FARM
    local lgm_speed_cap = nil
    if lgm_out and not lgm_is_farming and goal.kind ~= "escape_water"
       and goal.kind ~= "rescue_lgm" then
      -- Estimate LGM speed: use the terrain at the LGM's position
      local man_mx = info.man_x >> 8
      local man_my = info.man_y >> 8
      local man_tt = U.ttype(man_mx, man_my)
      local man_spd = C.MAN_SPEED[man_tt] or 0
      -- If LGM is on his blessed build square, he moves at full speed
      if man_spd == 0 then man_spd = C.MAN_SPEED_BLESSED end
      lgm_speed_cap = man_spd
    end

    -- Slightly slower than the LGM so he can catch up
    local tank_pace = lgm_speed_cap and math.max(1, math.floor(lgm_speed_cap * 0.7))

    if boat_exit and abs_corr < 24 then
      -- Boat-to-land transition needs high speed to disembark.
      -- Must take priority over LGM pacing or the tank gets stranded.
      -- Only boost when roughly facing the exit (< 24°); otherwise the
      -- tank overshoots the exit tile at speed and enters the wrong tile.
      keys = (keys & ~KEY_SLOWER) | KEY_FASTER
    elseif boat_exit then
      -- Facing away from exit — slow to turn, but keep above exit speed
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < 8 then
        keys = keys | KEY_FASTER
      end
    elseif tank_pace and info.speed > tank_pace then
      keys = keys | KEY_SLOWER
    elseif tank_pace and tank_pace > 0 and info.speed < tank_pace then
      keys = keys | KEY_FASTER
    elseif lgm_speed_cap and lgm_speed_cap == 0 then
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    elseif cliff and goal.kind ~= "escape_water" then
      keys = (keys & ~KEY_FASTER) | KEY_SLOWER
    elseif goal.kind == "escape_water" then
      keys = keys | KEY_FASTER
    elseif goal.kind == "bpc_pill" and goal.substate == "approach" then
      -- BPC approach: drive fast through the pill's firing zone.  The tank
      -- will stop when it transitions to stand_shoot; don't brake prematurely
      -- or pill knockback will prevent us from ever reaching the standoff.
      -- Higher max speed (64) to punch through the firing zone.
      if abs_corr < 16 then
        keys = keys | KEY_FASTER
      elseif abs_corr > 80 then
        if info.speed > 8 then keys = keys | KEY_SLOWER end
      else
        local factor = 1.0 - (abs_corr - 16) / 64.0
        local desired = math.max(8, math.floor(factor * 64))
        if info.speed > desired + 4 then
          keys = keys | KEY_SLOWER
        elseif info.speed < desired then
          keys = keys | KEY_FASTER
        end
      end
    elseif facing_away then
      -- Under fire: tolerate higher speed even when facing away — momentum
      -- helps escape the threat zone faster than braking and re-accelerating
      local facing_brake = under_fire and 16 or 8
      if info.speed > facing_brake then keys = keys | KEY_SLOWER end
    elseif eff_dist < brake_dist
           and goal.kind ~= "capture_base" and goal.kind ~= "capture_pill" then
      -- Approach braking: slow proportionally to remaining distance.
      -- capture_base/capture_pill are drive-through goals — captured on
      -- contact, no need to stop.  Keep cruising so we don't waste time
      -- decelerating then re-accelerating for the next objective.
      local desired_speed = math.max(6, math.floor(eff_dist * 0.08))
      -- Also respect turn_max_speed
      if desired_speed > turn_max_speed then desired_speed = turn_max_speed end
      if info.speed > desired_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < desired_speed and eff_dist > 128 then
        keys = keys | KEY_FASTER
      end
    else
      -- Cruise: target turn_max_speed with proportional control
      if info.speed > turn_max_speed + 4 then
        keys = keys | KEY_SLOWER
      elseif info.speed < turn_max_speed then
        keys = keys | KEY_FASTER
      end
    end

    -- Shoot walls on our planned path while driving by (opportunistic).
    -- Suppress on a boat: drive-by shots may destroy bridges we're sailing
    -- on or knock the tank into open water.  Water-edge walls are handled
    -- by the wall-clearing mode above which stops and aims deliberately.
    if info.inboat == 0 and info.shells > C.SHELL_RESERVE and math.abs(correction) < 16 then
      local pf = state.pf
      if pf.next_mx >= 0 then
        local next_tt = U.ttype(pf.next_mx, pf.next_my)
        if next_tt == C.T_BUILDING or next_tt == C.T_HALFBUILD then
          taps = taps | KEY_SHOOT
        end
      end
    end

    -- Shoot walls blocking our path when stuck (fallback).
    -- Allowed in a boat: shell reaches the water-edge wall blocking us.
    if info.tank_obstructed and math.abs(correction) < 10
       and state.stuck_for > 0 then
      local bx = (info.tankx + U.bsin(info.direction) * 1) >> 8
      local by = (info.tanky - U.bcos(info.direction) * 1) >> 8
      local bt = U.ttype(bx, by)
      if (bt == C.T_BUILDING or bt == C.T_HALFBUILD) and info.shells > 0 then
        taps = taps | KEY_SHOOT
      end
    end

    -- Log navigation steering reasoning (every 10 ticks to reduce volume)
    if state.tick % 10 == 0 then
      local steer_why = "navigate"
      if boat_exit then steer_why = "boat_exit_boost"
      elseif cliff then steer_why = "cliff_avoidance"
      elseif lgm_speed_cap then steer_why = "lgm_pacing"
      end
      if lookahead_active then steer_why = "lookahead" end
      log.reason("steer", {
        mode = steer_why,
        move_dir = move_dir,
        correction = correction,
        target_dist = target_dist,
        goal_dist = goal_dist,
        turn_max_spd = turn_max_speed,
        lgm_cap = lgm_speed_cap,
        lookahead = lookahead_active or nil,
        under_fire = under_fire or nil,
      })
    end

    end -- else (normal navigation vs wall_clearing)
  end

  -- Attack pill: aim and shoot when in range with clear LOS.
  -- The tank points at the pill and fires, but also creeps toward the
  -- standoff position to compensate for pill knockback.  This keeps the
  -- tank at optimal range rather than being slowly pushed out of position.
  local boat_can_hit = (info.inboat ~= 0)
                       and water_corridor_to(tmx, tmy, goal.mx, goal.my)
  if attack_in_range and (info.inboat == 0 or boat_can_hit) then
    local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    local corr    = U.adiff(info.direction, aim_dir)

    -- Gunsight at max range: shells travel further, hitting the pill from
    -- the greatest possible distance.
    if info.gunrange < C.GUNSIGHT_MAX then
      keys = keys | KEY_MORERANGE
    end

    -- Override navigation turn keys: point at the pill
    keys = keys & ~(KEY_TURNLEFT | KEY_TURNRIGHT | KEY_FASTER | KEY_SLOWER)
    taps = taps & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
    if     corr >  10 then keys = keys | KEY_TURNRIGHT
    elseif corr < -10 then keys = keys | KEY_TURNLEFT
    elseif corr >   2 then taps = taps | KEY_TURNRIGHT
    elseif corr <  -2 then taps = taps | KEY_TURNLEFT
    end
    local still_correcting = (taps & (KEY_TURNLEFT | KEY_TURNRIGHT)) ~= 0
    if math.abs(corr) < 3 and not still_correcting then
      keys = keys | KEY_SHOOT
    end

    -- Movement during engage: stay at max range where pill shots are hardest
    -- to land.  Knockback naturally pushes the tank outward — let it.
    -- Only nudge forward if knocked completely out of range.
    local wdist_pill = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)

    if wdist_pill > C.ATTACK_PILL_RANGE * 256 then
      -- Knocked out of range: gently push back in
      if info.speed < 4 and math.abs(corr) < 16 then
        keys = keys | KEY_FASTER
      end
    else
      -- In range: stop and shoot
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end

    log.reason("steer", {
      mode = "attack_pill", aim_corr = corr,
      firing = math.abs(corr) < 3 and not still_correcting,
      boat_can_hit = boat_can_hit,
      pill_dist = wdist_pill,
    })
  -- Attack base: aim and shoot when close enough to hit.
  -- Bases don't shoot back, so we just need to get within shell range and fire.
  elseif goal.kind == "attack_base" and info.shells > C.SHELL_RESERVE then
    local wdist_base = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    if wdist_base <= C.ATTACK_PILL_RANGE * 256 and info.inboat == 0 then
      local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
      local corr    = U.adiff(info.direction, aim_dir)

      if info.gunrange < C.GUNSIGHT_MAX then
        keys = keys | KEY_MORERANGE
      end

      -- Override turn keys: point at the base
      keys = keys & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
      taps = taps & ~(KEY_TURNLEFT | KEY_TURNRIGHT)
      if     corr >  10 then keys = keys | KEY_TURNRIGHT
      elseif corr < -10 then keys = keys | KEY_TURNLEFT
      elseif corr >   2 then taps = taps | KEY_TURNRIGHT
      elseif corr <  -2 then taps = taps | KEY_TURNLEFT
      end
      local still_correcting = (taps & (KEY_TURNLEFT | KEY_TURNRIGHT)) ~= 0
      if math.abs(corr) < 3 and not still_correcting then
        keys = keys | KEY_SHOOT
      end

      -- Slow down while shooting to maintain range
      if info.speed > 8 then
        keys = keys & ~KEY_FASTER
        keys = keys | KEY_SLOWER
      end

      log.reason("steer", {
        mode = "attack_base", aim_corr = corr,
        firing = math.abs(corr) < 3 and not still_correcting,
        base_dist = wdist_base,
      })
    end
  elseif not attack_in_range then
    if state.tick % 10 == 0 then
      log.reason("steer", { mode = "idle", why = "no goal or at destination" })
    end
  end

  return keys, taps
end

return M
