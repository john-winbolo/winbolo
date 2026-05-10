-- =========================================================================
-- NewAutopilot/bpc.lua — basic pill capture, self-contained
--
-- "Hardline" technique from ErYan's replay:
--   1. Drive toward the pill
--   2. When in range, stop and shoot
--   3. After a few hits from return fire, hard-turn right to escape
--   4. Pill's predictive aim overshoots the curve, shots miss
--   5. Retreat, refuel, come back and finish
-- =========================================================================

local C   = require("constants")
local U   = require("util")
local PF  = require("pathfinder")
local cpf = require("cpathfinder")
local log = require("logger")

local M = {}

-- Toggle: set true to use behaviour-tree implementation instead of FSM
M.USE_BPC_BT = true
local bpc_bt -- lazy-loaded to avoid circular requires

-- C pathfinder wrapper (mirrors steering.lua cpf_path_to)
local function cpf_path_to(state, info, dest_mx, dest_my)
  local pf  = state.pf
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local in_boat = info.inboat and 1 or 0
  local shells  = info.shells or 0
  local trees   = info.trees or 0
  local mines   = info.mines or 0
  local armour  = info.armour or 40

  local fallback_nx  = pf.next_mx
  local fallback_ny  = pf.next_my
  local tank_moved   = (pf.src_mx ~= tmx or pf.src_my ~= tmy)
  local dest_changed = (pf.dest_mx ~= dest_mx or pf.dest_my ~= dest_my)
  local use_fallback = tank_moved and not dest_changed and fallback_nx >= 0

  local status, nx, ny = cpf.path_to(tmx, tmy, dest_mx, dest_my, in_boat, shells, trees, mines, armour, C.ASTAR_BUDGET)

  pf.src_mx  = tmx
  pf.src_my  = tmy
  pf.dest_mx = dest_mx
  pf.dest_my = dest_my

  if status == 1 then
    pf.status  = "done"
    pf.next_mx = nx
    pf.next_my = ny
    pf.age     = 0
  elseif status == 0 then
    pf.status = "running"
    if nx >= 0 then
      pf.next_mx = nx
      pf.next_my = ny
    end
    pf.age = (pf.age or 0) + 1
  else
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

-- =========================================================================
-- Steering — returns keys, taps.  Returns nil if goal is not bpc_pill.
-- =========================================================================

function M.steer(state, world, info, goal)
  if goal.kind ~= "bpc_pill" then return nil end

  local keys = 0
  local taps = 0
  local pwx, pwy = goal.wx, goal.wy
  local tmx, tmy = info.tankx >> 8, info.tanky >> 8
  local pdist_w = U.wdist(info.tankx, info.tanky, pwx, pwy)
  local aim_dir  = U.aim_at(info.tankx, info.tanky, pwx, pwy)
  local aim_corr = U.adiff(info.direction, aim_dir)

  -- Gunsight at max range
  if info.gunrange < C.GUNSIGHT_MAX then
    keys = keys | KEY_MORERANGE
  end

  -- ── approach: A* toward the pill, full speed ──────────────────────
  if goal.substate == "approach" then
    -- In a boat: fall through to M.steer() which has wall-clearing and
    -- boat_exit logic.  Without this, the boat gets stuck spinning when
    -- the water route is blocked by walls (HALFB) and can't disembark.
    if info.inboat then return nil end

    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      local k, t = U.nav_turn_speed(corr, info.speed)
      keys = keys | k
      taps = taps | t
    else
      -- Already on or adjacent to target tile
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps

  -- ── shoot: face the pill, pump shells, nudge to stay in range ─────
  elseif goal.substate == "stand_shoot" then
    local dist_tiles = pdist_w / 256.0
    local too_far  = dist_tiles > C.BPC_RANGE - 0.5
    local too_close = dist_tiles < C.BPC_RANGE - 2.0

    if too_far then
      -- Nudge toward the pill: turn to face it and creep forward
      if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
      elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
      elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
      elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
      end
      if math.abs(aim_corr) < 20 then
        if info.speed < 16 then keys = keys | KEY_FASTER end
      end
    elseif too_close then
      -- Drift backward: face pill but slow down / let pushback do the work
      if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
      elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
      elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
      elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
      end
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    else
      -- In the sweet spot: stop and aim
      if     aim_corr >  10 then keys = keys | KEY_TURNRIGHT
      elseif aim_corr < -10 then keys = keys | KEY_TURNLEFT
      elseif aim_corr >   2 then taps = taps | KEY_TURNRIGHT
      elseif aim_corr <  -2 then taps = taps | KEY_TURNLEFT
      end
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    -- Fire when aimed and in range
    local firing = false
    if math.abs(aim_corr) < 4 and info.shells > C.SHELL_RESERVE
       and dist_tiles <= C.BPC_RANGE then
      keys = keys | KEY_SHOOT
      firing = true
    end
    log.reason("steer", {
      mode = "bpc_shoot", dist = dist_tiles,
      aim_corr = aim_corr, firing = firing,
      too_far = too_far, too_close = too_close,
    })
    return keys, taps

  -- ── curve_away: hard turn + full speed to dodge pill fire ─────────
  elseif goal.substate == "curve_away" then
    if (goal.curve_dir or 1) > 0 then
      keys = keys | KEY_TURNRIGHT
    else
      keys = keys | KEY_TURNLEFT
    end
    keys = keys | KEY_FASTER
    -- Fire if we sweep past the pill during the curve
    local firing = false
    if math.abs(aim_corr) < 6 and info.shells > C.SHELL_RESERVE then
      keys = keys | KEY_SHOOT
      firing = true
    end
    log.reason("steer", {
      mode = "bpc_curve", dist = pdist_w / 256.0,
      aim_corr = aim_corr, firing = firing,
    })
    return keys, taps

  -- ── rush: pill dead, drive to pick it up ──────────────────────────
  elseif goal.substate == "rush" then
    -- In a boat: fall through to M.steer() for wall-clearing + boat_exit
    if info.inboat then return nil end

    local nx, ny = cpf_path_to(state, info, goal.mx, goal.my)
    if nx then
      local move_dir = U.aim_at(info.tankx, info.tanky, U.m2w(nx), U.m2w(ny))
      local corr = U.adiff(info.direction, move_dir)
      -- Rush: higher max speed to capture fast
      local k, t = U.nav_turn_speed(corr, info.speed, 64)
      keys = keys | k
      taps = taps | t
    else
      if info.speed > 0 then keys = keys | KEY_SLOWER end
    end
    return keys, taps
  end

  return 0, 0
end

-- =========================================================================
-- Substate machine — call once per tick before steer
-- =========================================================================

function M.update(goal, state, world, info)
  if goal.kind ~= "bpc_pill" then return end

  if M.USE_BPC_BT then
    if not bpc_bt then bpc_bt = require("bpc_bt") end
    bpc_bt.tick(goal, state, world, info)
    return
  end

  -- ── Legacy FSM (kept as fallback) ─────────────────────────────────
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  if not goal.substate then
    goal.substate = "approach"
  end

  -- Look up pill health
  local pill = nil
  for _, pm in pairs(world.pills) do
    if pm.mx == goal.mx and pm.my == goal.my then
      pill = pm
      break
    end
  end

  -- Pill dead in any substate → rush to capture
  if pill and pill.health == 0 and goal.substate ~= "rush" then
    goal.substate = "rush"
    print(string.format("[BPC] pill@(%d,%d) dead, rushing", goal.mx, goal.my))
    log.event("bpc", "rush")
    return
  end

  -- ── approach: navigate to standoff position, stop there ──────────
  if goal.substate == "approach" then
    -- Check if we've arrived at the standoff position
    local smx = goal.standoff_mx or goal.mx
    local smy = goal.standoff_my or goal.my
    local sdist = U.mdist(tmx, tmy, smx, smy)
    if sdist <= 1 and info.speed <= 4 and not info.inboat then
      -- Arrived at standoff — now aim at the pill before shooting
      goal.substate = "aim"
      print(string.format("[BPC] arrived at standoff (%d,%d), aiming at pill@(%d,%d)",
            smx, smy, goal.mx, goal.my))
      log.event("bpc", "aim")
    end
    return
  end

  -- ── aim: turn to face the pill without moving, then shoot ────────
  if goal.substate == "aim" then
    local aim_dir = U.aim_at(info.tankx, info.tanky, goal.wx, goal.wy)
    local corr = math.abs(U.adiff(info.direction, aim_dir))
    local clear = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
    if corr < 4 and clear then
      goal.substate     = "stand_shoot"
      goal.shoot_armour = info.armour
      print(string.format("[BPC] aimed at pill@(%d,%d) corr=%d, firing",
            goal.mx, goal.my, corr))
      log.event("bpc", "stand_shoot")
    end
    return
  end

  -- ── stand_shoot ───────────────────────────────────────────────────
  if goal.substate == "stand_shoot" then
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      print(string.format("[BPC] armour critical (%d)", info.armour))
      return
    end
    -- Feature 3: armour drain projection
    if not goal.bpc_shoot_tick then goal.bpc_shoot_tick = now end
    local ticks_shooting = now - goal.bpc_shoot_tick
    local armour_lost = (goal.shoot_armour or info.armour) - info.armour
    if ticks_shooting >= C.DRAIN_PROJECTION_MIN_TICKS and armour_lost > 0 then
      local drain_rate = armour_lost / ticks_shooting
      local pill_hp = pill and pill.health or 0
      local remaining_ttk = pill_hp * C.TTK_TICKS_PER_HIT
      local projected = info.armour - drain_rate * remaining_ttk
      if projected < C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN then
        goal.substate = "disengage"
        print(string.format("[BPC] drain disengage pill@(%d,%d) arm=%d proj=%.1f",
              goal.mx, goal.my, info.armour, projected))
        return
      end
    end
    local hits = (goal.shoot_armour or info.armour) - info.armour
    if hits >= C.BPC_CURVE_AFTER_HITS then
      goal.substate  = "curve_away"
      goal.curve_tick = now
      goal.curve_dir  = 1  -- right
      print(string.format("[BPC] curve_away after %d hits, arm=%d", hits, info.armour))
      log.event("bpc", "curve_away")
    end
    return
  end

  -- ── curve_away ────────────────────────────────────────────────────
  if goal.substate == "curve_away" then
    if info.armour <= C.ARMOUR_CRITICAL then
      goal.substate = "disengage"
      return
    end
    if now - (goal.curve_tick or now) >= C.BPC_CURVE_TICKS then
      goal.substate = "disengage"
      print("[BPC] curve complete, disengaging")
      log.event("bpc", "disengage")
    end
    return
  end

  -- ── rush ──────────────────────────────────────────────────────────
  if goal.substate == "rush" then
    -- Nothing to do — steering handles navigation
    return
  end

  -- ── disengage ─────────────────────────────────────────────────────
  if goal.substate == "disengage" then
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    print("[BPC] disengaged")
    return
  end
end

return M
