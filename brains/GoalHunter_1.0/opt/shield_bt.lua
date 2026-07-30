local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/shield_bt.lua — generic shield engage BT nodes
--
-- Shared engage logic for any "shield" attack (wall-shield or pill-place).
-- After a shield (wall or placed pill) is in position, handles:
--   prewait → advance → shield_engage → engage (unshielded) → reposition → disengage
--
-- Ported from the working wallshield_bt.lua ws_prewait / ws_advance /
-- ws_engage / engage / reposition logic.
--
-- The caller must set on goal before entering prewait:
--   goal.shield_mx, goal.shield_my  — map position of the shield
--   goal.shield_type                — "wall" or "pill"
--   goal.standoff_mx, goal.standoff_my — firing position
-- =========================================================================

local C      = require("constants")
local TAG    = "[" .. C.BRAIN_NAME .. "]"
local U      = require("util")
local PF     = require("pathfinder")
local cpf    = require("cpathfinder")
local log    = require("logger")
local attack = require("attack")

local M = {}

-- ── Shield status check ───────────────────────────────────────────────────

function M.shield_alive(goal, world)
  if not goal.shield_mx then return false end
  if goal.shield_type == "wall" then
    local wtt = U.ttype(goal.shield_mx, goal.shield_my)
    return wtt == C.T_BUILDING or wtt == C.T_HALFBUILD
  elseif goal.shield_type == "pill" then
    local placed = attack.find_pill_at(world, goal.shield_mx, goal.shield_my)
    return placed and placed.owner == "friendly" and placed.health > 0
  end
  return false
end

-- ── Prewait: hold position, wait for LGM to return safely ─────────────
-- Ported from wallshield_bt ws_prewait_tick.
-- The LGM just placed the shield; we must wait for it to return to the
-- tank before advancing into pill range (otherwise the LGM gets killed).

function M.prewait_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick

  if not goal.standoff_mx then
    goal.substate = "disengage"
    print(TAG .. " SHIELD: no standoff set in prewait, disengaging")
    return "running"
  end

  if info.man_status == C.LGM_INTANK then
    -- LGM is back in the tank — wait the safety margin then advance
    goal.lgm_return_tick = goal.lgm_return_tick or now
    local safe_ticks = now - goal.lgm_return_tick
    if safe_ticks >= C.WALL_SHIELD_LGM_SAFE_TICKS then
      goal.substate = "advance"
      goal.early_advance = nil
      print(string.format(TAG .. " SHIELD: LGM safe, advancing to standoff (%d,%d) with %s@(%d,%d)",
            goal.standoff_mx, goal.standoff_my,
            goal.shield_type or "?", goal.shield_mx, goal.shield_my))
      log.reason("shield_sub", {
        transition = "prewait->advance",
        standoff_mx = goal.standoff_mx, standoff_my = goal.standoff_my,
      })
      return "success"
    end
  else
    -- LGM still out — check if we can start advancing early
    goal.lgm_return_tick = nil

    if info.man_status == 2 then  -- LGM_MOVING
      local tmx = bit.rshift(info.tankx, 8)
      local tmy = bit.rshift(info.tanky, 8)
      local tank_ticks = cpf.estimate_tank_travel_ticks(
        tmx, tmy, goal.standoff_mx, goal.standoff_my, info.inboat)
      local lgm_return_ticks = cpf.lgm_travel_ticks(
        info.man_x, info.man_y, info.tankx, info.tanky,
        goal.shield_mx or 0, goal.shield_my or 0, 2000, 150)

      if lgm_return_ticks > 0 and tank_ticks > 0 then
        local safe_margin = C.WALL_SHIELD_LGM_SAFE_TICKS
        if tank_ticks + safe_margin <= lgm_return_ticks then
          goal.substate = "advance"
          goal.early_advance = true
          print(string.format(
            TAG .. " SHIELD: early advance — tank %d ticks, LGM return %d ticks, margin %d",
            tank_ticks, lgm_return_ticks, safe_margin))
          log.reason("shield_sub", {
            transition = "prewait->advance(early)",
            tank_ticks = tank_ticks, lgm_return_ticks = lgm_return_ticks,
            standoff_mx = goal.standoff_mx, standoff_my = goal.standoff_my,
          })
          return "success"
        end
      end
    end
  end
  return "running"
end

-- ── Advance: navigate to standoff, check shield intact ────────────────
-- Ported from wallshield_bt ws_advance_tick.
-- Drives from the deploy position (outside pill range) to the standoff
-- (inside pill range).  If the shield is destroyed during the advance,
-- fall to unshielded engage.

function M.advance_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick

  -- If early-advancing, clear the flag once LGM is back
  if goal.early_advance then
    if info.man_status == C.LGM_INTANK then
      goal.early_advance = nil
    end
  end

  local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
  local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
  local in_range = pdist_w <= C.ATTACK_PILL_RANGE * 256
  if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range then
    -- Don't start shooting until LGM is safely in tank
    if goal.early_advance then
      return "running"
    end
    goal.substate    = "shield_engage"
    goal.engage_tick = now
    goal.first_hit_tick = nil
    goal.last_armour = info.armour
    print(string.format(TAG .. " SHIELD: engaging pill@(%d,%d) with %s@(%d,%d)",
          goal.mx, goal.my,
          goal.shield_type or "?", goal.shield_mx, goal.shield_my))
    log.reason("shield_sub", {
      transition = "advance->shield_engage",
      shield_mx = goal.shield_mx, shield_my = goal.shield_my,
    })
    return "running"
  end

  -- If shield destroyed during advance, fall to unshielded engage
  if not M.shield_alive(goal, world) then
    goal.substate = "engage"
    goal.engage_tick = now
    goal.first_hit_tick = nil
    goal.last_armour = info.armour
    goal.early_advance = nil
    print(string.format(TAG .. " SHIELD: %s destroyed during advance, unshielded engage",
          goal.shield_type or "?"))
    log.reason("shield_sub", {
      transition = "advance->engage",
      shield_type = goal.shield_type,
    })
  end
  return "running"
end

-- ── Shield engage: fire while shield is intact ────────────────────────────
-- Ported from wallshield_bt ws_engage_tick.
-- Only checks shield status and critical armour — NO time-under-fire flee,
-- because the shield absorbs return fire.

function M.shield_engage_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local now = state.tick

  if not M.shield_alive(goal, world) then
    -- Shield destroyed → fall to unshielded engage
    goal.substate = "engage"
    goal.engage_tick = now
    goal.first_hit_tick = nil
    goal.last_armour = info.armour
    print(string.format(TAG .. " SHIELD: %s@(%d,%d) destroyed, switching to unshielded engage",
          goal.shield_type or "?", goal.shield_mx, goal.shield_my))
    log.reason("shield_sub", {
      transition = "shield_engage->engage",
      shield_type = goal.shield_type,
      shield_mx = goal.shield_mx, shield_my = goal.shield_my,
    })
    return "running"
  end

  if info.armour <= C.ARMOUR_CRITICAL then
    goal.substate = "disengage"
    print(string.format(TAG .. " SHIELD: armour critical (%d), disengaging", info.armour))
    log.reason("shield_sub", {
      transition = "shield_engage->disengage",
      armour = info.armour,
    })
  end
  return "running"
end

-- ── Unshielded engage: full flee logic ────────────────────────────────────
-- Ported from wallshield_bt engage_tick.
-- Time-under-fire with anger scaling and escape terrain analysis.

function M.engage_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  if not goal.standoff_mx then return "running" end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick

  local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
  local tank_tt = U.ttype(tmx, tmy)
  local hazard  = (tank_tt == C.T_RIVER or tank_tt == C.T_DEEPSEA
                   or tank_tt == C.T_BUILDING or tank_tt == C.T_HALFBUILD)
  local min_time_met = (now - (goal.engage_tick or 0)) >= C.ATTACK_MIN_ENGAGE_TICKS

  -- Knocked too far or hazardous terrain → reposition
  if hazard or (sdist > C.ATTACK_REPOSITION_RADIUS and min_time_met) then
    goal.substate        = "reposition"
    goal.reposition_tick = now
    print(string.format(TAG .. " SHIELD: reposition pill@(%d,%d) sdist=%d hazard=%s",
          goal.mx, goal.my, sdist, tostring(hazard)))
    log.reason("shield_sub", {
      transition = "engage->reposition", sdist = sdist,
      hazard = hazard, tank_tt = tank_tt,
    })
    return "running"
  end

  -- Time-under-fire based disengage
  local pill = attack.find_pill_at(world, goal.mx, goal.my)
  local pill_anger = pill and (pill.anger or 0) or 0

  if not goal.first_hit_tick then
    if info.armour < (goal.last_armour or info.armour) then
      goal.first_hit_tick = now
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
        TAG .. " SHIELD: disengage pill@(%d,%d) fire=%d/%.0f anger=%.2f esc_slow=%s",
        goal.mx, goal.my, ticks_under_fire, max_ticks, pill_anger,
        tostring(escape_slow)))
      log.reason("shield_sub", {
        transition = "engage->disengage",
        ticks_under_fire = ticks_under_fire, max_ticks = max_ticks,
        pill_anger = pill_anger, escape_slow = escape_slow,
      })
      return "running"
    end
  end
  return "running"
end

-- ── Reposition ────────────────────────────────────────────────────────────
-- Ported from wallshield_bt reposition_tick.
-- Re-computes standoff and re-enters engage with fresh tracking.

function M.reposition_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local pill = attack.find_pill_at(world, goal.mx, goal.my)
  if pill then
    local pk = U.mkey(goal.mx, goal.my)
    state.pill_attack_plan = nil
    local smx, smy = attack.get_standoff(world, info, pk, pill, state)
    goal.standoff_mx = smx; goal.standoff_my = smy
    if smx then
      goal.substate = "engage"
      goal.engage_tick = state.tick
      goal.first_hit_tick = nil
      goal.last_armour = info.armour
      print(string.format(TAG .. " SHIELD: re-engage pill@(%d,%d) new standoff=(%s,%s)",
            goal.mx, goal.my, tostring(smx), tostring(smy)))
      log.reason("shield_sub", {
        transition = "reposition->engage",
        new_standoff_mx = smx, new_standoff_my = smy,
      })
    else
      goal.substate = "disengage"
      print(TAG .. " SHIELD: no standoff after reposition, disengaging")
    end
  else
    goal.substate = "disengage"
    print(TAG .. " SHIELD: target pill gone after reposition, disengaging")
  end
  return "success"
end

-- ── Disengage ─────────────────────────────────────────────────────────────

function M.disengage_action(ctx)
  local goal, state = ctx.goal, ctx.state
  state.pill_attack_plan = nil
  goal.kind = "none"
  goal.substate = nil
  print(TAG .. " SHIELD: disengaged — will refuel and re-engage")
  return "success"
end

return M
