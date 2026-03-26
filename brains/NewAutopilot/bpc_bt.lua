-- =========================================================================
-- NewAutopilot/bpc_bt.lua — BPC (hardline) technique as a behaviour tree
--
-- Wraps the existing bpc.lua FSM logic into a BT.  Action nodes read/write
-- the same goal.* fields so steering and builder are unchanged.
-- =========================================================================

local bt  = require("bt")
local C   = require("constants")
local U   = require("util")
local PF  = require("pathfinder")
local log = require("logger")
local attack = require("attack")

local TAG = "[" .. C.BRAIN_NAME .. "]"

-- ── Helpers ──────────────────────────────────────────────────────────────

local function find_pill(ctx)
  return attack.find_pill_at(ctx.world, ctx.goal.mx, ctx.goal.my)
end

-- ── Conditions ───────────────────────────────────────────────────────────

local function is_bpc(ctx)
  return ctx.goal.kind == "bpc_pill"
end

local function pill_dead_and_not_rush(ctx)
  local pill = find_pill(ctx)
  return pill and pill.health == 0 and ctx.goal.substate ~= "rush"
end

local function sub_is(name)
  return function(ctx) return ctx.goal.substate == name end
end

-- ── Actions ──────────────────────────────────────────────────────────────

local function ensure_substate(ctx)
  if not ctx.goal.substate then
    ctx.goal.substate = "approach"
  end
  return "success"
end

local function transition_to_rush(ctx)
  local goal, state = ctx.goal, ctx.state
  goal.substate = "rush"
  goal.rush_tick = state.tick
  print(string.format(TAG .. " BPC: pill@(%d,%d) dead, rushing to pick up",
        goal.mx, goal.my))
  log.reason("bpc_sub", { transition = goal.substate .. "->rush" })
  return "success"
end

local function approach_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  if not goal.standoff_mx then return "running" end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local sdist   = U.mdist(tmx, tmy, goal.standoff_mx, goal.standoff_my)
  local pdist_w = U.wdist(info.tankx, info.tanky, goal.wx, goal.wy)
  local in_range  = pdist_w <= (C.BPC_RANGE + 1) * 256
  local clear_los = PF.wall_hp_between(tmx, tmy, goal.mx, goal.my) == 0
  local close_enough = sdist <= 2 or (in_range and clear_los)
  if close_enough and clear_los and info.inboat == 0 then
    goal.substate      = "stand_shoot"
    goal.engage_tick   = state.tick
    goal.engage_armour = info.armour
    goal.hits_taken    = 0
    print(string.format(TAG .. " BPC: stand_shoot pill@(%d,%d) from (%d,%d) dist=%.1f armour=%d",
          goal.mx, goal.my, tmx, tmy, pdist_w / 256.0, info.armour))
    log.reason("bpc_sub", {
      transition = "approach->stand_shoot", pdist = pdist_w, sdist = sdist,
      pill_x = goal.mx, pill_y = goal.my,
    })
    return "success"
  end
  return "running"
end

local function stand_shoot_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick

  -- Armour critical → disengage
  if info.armour <= C.ARMOUR_CRITICAL then
    goal.substate = "disengage"
    print(string.format(TAG .. " BPC: armour critical (%d), disengaging", info.armour))
    log.reason("bpc_sub", { transition = "stand_shoot->disengage", armour = info.armour })
    return "success"
  end

  -- Track hits
  local armour_lost = (goal.engage_armour or info.armour) - info.armour
  if armour_lost > (goal.hits_taken or 0) then
    goal.hits_taken = armour_lost
  end

  -- Feature 3: armour drain projection — disengage early if we'll run out
  if not goal.bpc_engage_tick then goal.bpc_engage_tick = now end
  local ticks_engaged = now - goal.bpc_engage_tick
  if ticks_engaged >= C.DRAIN_PROJECTION_MIN_TICKS and armour_lost > 0 then
    local drain_rate = armour_lost / ticks_engaged
    -- Look up remaining pill HP
    local pill_hp = 0
    for _, pm in pairs(ctx.world.pills) do
      if pm.mx == goal.mx and pm.my == goal.my and pm.health > 0 then
        pill_hp = pm.health; break
      end
    end
    local remaining_ttk = pill_hp * C.TTK_TICKS_PER_HIT
    local projected = info.armour - drain_rate * remaining_ttk
    if projected < C.ARMOUR_CRITICAL + C.DRAIN_ARMOUR_MARGIN then
      goal.substate = "disengage"
      print(string.format(TAG .. " BPC: drain disengage pill@(%d,%d) arm=%d proj=%.1f drain=%.3f",
            goal.mx, goal.my, info.armour, projected, drain_rate))
      log.reason("bpc_sub", {
        transition = "stand_shoot->disengage(drain)",
        armour = info.armour, projected = projected,
        drain_rate = drain_rate, pill_hp = pill_hp,
      })
      return "success"
    end
  end

  -- After enough hits, curve away
  if (goal.hits_taken or 0) >= C.BPC_CURVE_AFTER_HITS then
    goal.substate   = "curve_away"
    goal.curve_tick = now
    goal.curve_dir  = 1
    print(string.format(TAG .. " BPC: curve_away pill@(%d,%d) after %d hits, armour=%d, dir=%s",
          goal.mx, goal.my, goal.hits_taken, info.armour,
          goal.curve_dir > 0 and "right" or "left"))
    log.reason("bpc_sub", {
      transition = "stand_shoot->curve_away",
      hits_taken = goal.hits_taken, armour = info.armour,
    })
    return "success"
  end
  return "running"
end

local function curve_away_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick

  if info.armour <= C.ARMOUR_CRITICAL then
    goal.substate = "disengage"
    print(string.format(TAG .. " BPC: armour critical (%d) during curve, disengaging", info.armour))
    log.reason("bpc_sub", { transition = "curve_away->disengage", armour = info.armour })
    return "success"
  end

  local curve_ticks = now - (goal.curve_tick or now)
  if curve_ticks >= C.BPC_CURVE_TICKS then
    goal.substate = "disengage"
    print(string.format(TAG .. " BPC: curve complete after %d ticks, disengaging", curve_ticks))
    log.reason("bpc_sub", { transition = "curve_away->disengage", curve_ticks = curve_ticks })
    return "success"
  end
  return "running"
end

local function rush_tick(ctx)
  local goal, info = ctx.goal, ctx.info
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local pdist = U.mdist(tmx, tmy, goal.mx, goal.my)
  if pdist <= C.BPC_RUSH_ARRIVE then
    print(string.format(TAG .. " BPC: arrived at pill@(%d,%d), pickup", goal.mx, goal.my))
    log.reason("bpc_sub", { transition = "rush->done" })
    return "success"
  end
  return "running"
end

local function disengage_action(ctx)
  local goal, state = ctx.goal, ctx.state
  state.pill_attack_plan = nil
  goal.kind = "none"
  goal.substate = nil
  print(TAG .. " BPC: disengaged — will refuel and re-engage")
  return "success"
end

-- ── Tree ─────────────────────────────────────────────────────────────────

local tree = bt.sequence(
  -- Gate: only run for bpc_pill goals
  bt.condition(is_bpc),
  -- Ensure substate is initialised
  bt.action(ensure_substate),
  -- Priority selector over substates
  bt.selector(
    -- Pill dead in any substate → rush
    bt.guard(bt.condition(pill_dead_and_not_rush), bt.action(transition_to_rush)),
    -- approach
    bt.guard(bt.condition(sub_is("approach")),    bt.action(approach_tick)),
    -- stand_shoot
    bt.guard(bt.condition(sub_is("stand_shoot")), bt.action(stand_shoot_tick)),
    -- curve_away
    bt.guard(bt.condition(sub_is("curve_away")),  bt.action(curve_away_tick)),
    -- rush
    bt.guard(bt.condition(sub_is("rush")),        bt.action(rush_tick)),
    -- disengage
    bt.guard(bt.condition(sub_is("disengage")),   bt.action(disengage_action))
  )
)

-- ── Public API ───────────────────────────────────────────────────────────

local M = {}

function M.tick(goal, state, world, info)
  return bt.tick(tree, { goal = goal, state = state, world = world, info = info })
end

return M
