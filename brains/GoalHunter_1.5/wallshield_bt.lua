-- =========================================================================
-- GoalHunter/wallshield_bt.lua — wall-shield + normal attack as a BT
--
-- Wraps the attack_pill FSM from attack.update_attack_substate() into a
-- behaviour tree.  Action nodes read/write the same goal.* fields so
-- steering and builder are unchanged.
-- =========================================================================

local bt     = require("bt")
local C      = require("constants")
local U      = require("util")
local PF     = require("pathfinder")
local cpf    = require("cpathfinder")
local log    = require("logger")
local attack = require("attack")

local TAG = "[" .. C.BRAIN_NAME .. "]"

-- ── Helpers ──────────────────────────────────────────────────────────────

local function find_pill(ctx)
  return attack.find_pill_at(ctx.world, ctx.goal.mx, ctx.goal.my)
end

local function sub_is(name)
  return function(ctx) return ctx.goal.substate == name end
end

-- ── Actions ──────────────────────────────────────────────────────────────

local function plan_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local pill = find_pill(ctx)
  if pill then
    local pk = U.mkey(goal.mx, goal.my)
    state.pill_attack_plan = nil
    local smx, smy = attack.get_standoff(world, info, pk, pill, state)
    goal.standoff_mx = smx; goal.standoff_my = smy
    goal.substate = smx and "approach" or "plan"
  end
  return "success"
end

local function approach_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  if not goal.standoff_mx then return "running" end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  if goal.wall_shield and goal.wall_mx then
    -- Wall-shield approach: navigate to PREBUILD position
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
      return "success"
    end
  else
    -- Normal approach: navigate to standoff
    local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
    local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
    local in_range  = pdist_w <= C.ATTACK_PILL_RANGE * 256
    local clear_los = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
    if sdist <= C.ATTACK_ENGAGE_RADIUS and in_range and clear_los
       and not info.inboat then
      goal.substate    = "engage"
      goal.engage_tick = now
      print(string.format(TAG .. " ATTACK: engage pill@(%d,%d) from (%d,%d)",
            goal.mx, goal.my, tmx, tmy))
      log.reason("attack_sub", {
        transition = "approach->engage", sdist = sdist,
        pill_x = goal.mx, pill_y = goal.my,
      })
      return "success"
    end
  end
  return "running"
end

-- ── Wall-shield substates ────────────────────────────────────────────────

local function ws_prebuild_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick
  local wtt = U.ttype(goal.wall_mx, goal.wall_my)
  if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
    goal.substate = "ws_prewait"
    goal.ws_wait_tick = now
    goal.lgm_return_tick = nil
    print(string.format(TAG .. " WALL-SHIELD: wall built@(%d,%d), waiting for LGM return (outside range)",
          goal.wall_mx, goal.wall_my))
    log.reason("attack_sub", {
      transition = "ws_prebuild->ws_prewait",
      wall_mx = goal.wall_mx, wall_my = goal.wall_my,
    })
    return "success"
  elseif now - (goal.ws_build_tick or now) > 400 then
    goal.substate = "engage"
    goal.engage_tick = now
    goal.wall_shield = false
    goal.first_hit_tick = nil  -- BUG FIX: clear stale first_hit_tick on timeout
    print(TAG .. " WALL-SHIELD: prebuild timeout, falling back to normal attack")
    return "success"
  end
  return "running"
end

local function ws_prewait_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick

  if info.man_status == C.LGM_INTANK then
    -- LGM is back in the tank — wait the safety margin then advance
    goal.lgm_return_tick = goal.lgm_return_tick or now
    local safe_ticks = now - goal.lgm_return_tick
    if safe_ticks >= C.WALL_SHIELD_LGM_SAFE_TICKS then
      goal.substate = "ws_advance"
      goal.early_advance = nil
      print(string.format(TAG .. " WALL-SHIELD: LGM safe, advancing to standoff (%d,%d) with wall@(%d,%d)",
            goal.standoff_mx, goal.standoff_my, goal.wall_mx, goal.wall_my))
      log.reason("attack_sub", {
        transition = "ws_prewait->ws_advance",
        standoff_mx = goal.standoff_mx, standoff_my = goal.standoff_my,
      })
      return "success"
    end
  else
    -- LGM still out — check if we can start advancing early
    goal.lgm_return_tick = nil

    if info.man_status == 2 then  -- LGM_MOVING
      local tmx = info.tankx >> 8
      local tmy = info.tanky >> 8
      local tank_ticks = cpf.estimate_tank_travel_ticks(
        tmx, tmy, goal.standoff_mx, goal.standoff_my, info.inboat)
      local lgm_return_ticks = cpf.lgm_travel_ticks(
        info.man_x, info.man_y, info.tankx, info.tanky,
        goal.wall_mx or 0, goal.wall_my or 0, 2000, 150)

      if lgm_return_ticks > 0 and tank_ticks > 0 then
        local safe_margin = C.WALL_SHIELD_LGM_SAFE_TICKS
        if tank_ticks + safe_margin <= lgm_return_ticks then
          goal.substate = "ws_advance"
          goal.early_advance = true
          print(string.format(
            TAG .. " WALL-SHIELD: early advance — tank %d ticks, LGM return %d ticks, margin %d",
            tank_ticks, lgm_return_ticks, safe_margin))
          log.reason("attack_sub", {
            transition = "ws_prewait->ws_advance(early)",
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

local function ws_advance_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
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
    goal.early_advance = nil
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
  return "running"
end

local function ws_engage_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick

  local wtt = U.ttype(goal.wall_mx, goal.wall_my)
  local wall_intact = (wtt == C.T_BUILDING or wtt == C.T_HALFBUILD)

  if not wall_intact then
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
    return "running"
  end

  if info.armour <= C.ARMOUR_CRITICAL then
    goal.substate = "disengage"
    print(string.format(TAG .. " WALL-SHIELD: armour critical (%d), disengaging", info.armour))
    log.reason("attack_sub", {
      transition = "ws_engage->disengage",
      armour = info.armour,
    })
  end
  return "running"
end

local function ws_retreat_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick
  local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
  local outside_range = pdist_w > C.PILL_RANGE_MAP * 256
  local retreat_time = now - (goal.ws_retreat_tick or now)
  if outside_range or retreat_time > 150 then
    -- BUG FIX: check pill anger before sending LGM to rebuild
    local pill = find_pill(ctx)
    local pill_anger = pill and (pill.anger or 0) or 0
    if pill_anger > 0.8 then
      -- Pill is very angry; wait for it to calm down before rebuilding
      return "running"
    end
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
  return "running"
end

local function ws_rebuild_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick
  local wtt = U.ttype(goal.wall_mx, goal.wall_my)
  if wtt == C.T_BUILDING or wtt == C.T_HALFBUILD then
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
    goal.first_hit_tick = nil  -- BUG FIX: clear stale first_hit_tick on timeout
    print(TAG .. " WALL-SHIELD: rebuild timeout, falling back to normal attack")
  end
  return "running"
end

-- ── Normal (non-wall-shield) engage ──────────────────────────────────────

local function engage_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  if not goal.standoff_mx then return "running" end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
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
    print(string.format(TAG .. " ATTACK: reposition pill@(%d,%d) sdist=%d hazard=%s",
          goal.mx, goal.my, sdist, tostring(hazard)))
    log.reason("attack_sub", {
      transition = "engage->reposition", sdist = sdist,
      hazard = hazard, tank_tt = tank_tt,
    })
    return "running"
  end

  -- Time-under-fire based disengage
  local pill = find_pill(ctx)
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
        TAG .. " ATTACK: disengage pill@(%d,%d) fire=%d/%.0f anger=%.2f esc_slow=%s",
        goal.mx, goal.my, ticks_under_fire, max_ticks, pill_anger,
        tostring(escape_slow)))
      log.reason("attack_sub", {
        transition = "engage->disengage",
        ticks_under_fire = ticks_under_fire, max_ticks = max_ticks,
        pill_anger = pill_anger, escape_slow = escape_slow,
      })
      return "running"
    end
  end
  return "running"
end

local function disengage_action(ctx)
  local goal, state = ctx.goal, ctx.state
  state.pill_attack_plan = nil
  goal.kind = "none"
  goal.substate = nil
  print(TAG .. " ATTACK: disengaged — will refuel and re-engage")
  return "success"
end

local function reposition_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local pill = find_pill(ctx)
  if pill then
    local pk = U.mkey(goal.mx, goal.my)
    state.pill_attack_plan = nil
    local smx, smy = attack.get_standoff(world, info, pk, pill, state)
    goal.standoff_mx = smx; goal.standoff_my = smy
    goal.substate = smx and "approach" or "plan"
    print(string.format(TAG .. " ATTACK: re-approach pill@(%d,%d) new standoff=(%s,%s)",
          goal.mx, goal.my, tostring(smx), tostring(smy)))
    log.reason("attack_sub", {
      transition = "reposition->approach",
      new_standoff_mx = smx, new_standoff_my = smy,
    })
  end
  return "success"
end

-- ── Tree ─────────────────────────────────────────────────────────────────

local tree = bt.selector(
  bt.guard(bt.condition(sub_is("plan")),         bt.action(plan_tick)),
  bt.guard(bt.condition(sub_is("approach")),      bt.action(approach_tick)),
  bt.guard(bt.condition(sub_is("ws_prebuild")),   bt.action(ws_prebuild_tick)),
  bt.guard(bt.condition(sub_is("ws_prewait")),    bt.action(ws_prewait_tick)),
  bt.guard(bt.condition(sub_is("ws_advance")),    bt.action(ws_advance_tick)),
  bt.guard(bt.condition(sub_is("ws_engage")),     bt.action(ws_engage_tick)),
  bt.guard(bt.condition(sub_is("ws_retreat")),     bt.action(ws_retreat_tick)),
  bt.guard(bt.condition(sub_is("ws_rebuild")),     bt.action(ws_rebuild_tick)),
  bt.guard(bt.condition(sub_is("engage")),         bt.action(engage_tick)),
  bt.guard(bt.condition(sub_is("disengage")),      bt.action(disengage_action)),
  bt.guard(bt.condition(sub_is("reposition")),     bt.action(reposition_tick))
)

-- ── Public API ───────────────────────────────────────────────────────────

local M = {}

function M.tick(goal, state, world, info)
  if goal.kind ~= "attack_pill" then return end
  if not goal.substate then return end
  bt.tick(tree, { goal = goal, state = state, world = world, info = info })
end

return M
