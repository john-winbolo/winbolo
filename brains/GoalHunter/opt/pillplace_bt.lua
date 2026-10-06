local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/pillplace_bt.lua — pill placement technique as a BT
--
-- Wraps attack.update_pill_place_substate() into a behaviour tree.
-- Action nodes read/write the same goal.* fields so steering/builder
-- are unchanged.
--
-- After the placed pill is in position, delegates to shield_bt for the
-- engage logic (same code as wall-shield: shield_engage → engage →
-- reposition → disengage).  A placed pill has 16 HP vs 5 for a wall,
-- so shield_engage lasts longer before falling to unshielded engage.
-- =========================================================================

local bt        = require("bt")
local C         = require("constants")
local U         = require("util")
local log       = require("logger")
local attack    = require("attack")
local shield_bt = require("shield_bt")

local TAG = "[" .. C.BRAIN_NAME .. "]"

-- ── Helpers ──────────────────────────────────────────────────────────────

local function find_pill(ctx)
  return attack.find_pill_at(ctx.world, ctx.goal.mx, ctx.goal.my)
end

local function sub_is(name)
  return function(ctx) return ctx.goal.substate == name end
end

-- ── Global priority checks (top of selector) ────────────────────────────

local function target_dead_not_collecting(ctx)
  local target = find_pill(ctx)
  local sub = ctx.goal.substate
  return target and target.health == 0
     and sub ~= "collect_target" and sub ~= "disengage"
end

local function transition_to_collect(ctx)
  local goal, state = ctx.goal, ctx.state
  goal.substate = "collect_target"
  goal.collect_tick = state.tick
  print(string.format(TAG .. " [PP] target@(%d,%d) dead, collecting", goal.mx, goal.my))
  log.reason("pp_sub", { transition = goal.substate .. "->collect_target" })
  return "success"
end

local function target_became_friendly(ctx)
  local target = find_pill(ctx)
  return target and target.owner == "friendly"
end

local function transition_to_done(ctx)
  local goal = ctx.goal
  print(string.format(TAG .. " [PP] target@(%d,%d) is now friendly, done", goal.mx, goal.my))
  goal.kind = "none"
  goal.substate = nil
  return "success"
end

-- ── Substate actions ─────────────────────────────────────────────────────

local function ensure_substate(ctx)
  if not ctx.goal.substate then
    ctx.goal.substate = "select_pill"
  end
  return "success"
end

local function select_pill_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local target = find_pill(ctx)

  -- Already carrying a pill?
  if (info.carried_pills or 0) > 0 then
    if not target then
      goal.kind = "none"; goal.substate = nil
      print(TAG .. " [PP] select_pill: target not found, aborting")
      return "success"
    end
    local pmx, pmy, dmx, dmy, smx, smy = attack.pick_pill_placement(world, info, target, state)
    if not pmx then
      goal.kind = "none"; goal.substate = nil
      print(TAG .. " [PP] select_pill: no valid placement position, aborting")
      return "success"
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
    return "success"
  end

  -- Find a dead friendly pill to pick up
  local src_pill, src_id = attack.pick_source_pill(world, info, state)
  if not src_pill then
    goal.kind = "none"; goal.substate = nil
    print(TAG .. " [PP] select_pill: no friendly pills to pick up, aborting")
    return "success"
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
  return "success"
end

local function pickup_tick(ctx)
  local goal, world, info = ctx.goal, ctx.world, ctx.info

  if (info.carried_pills or 0) > 0 then
    goal.substate = "select_pill"
    print(TAG .. " [PP] pickup: pill picked up, selecting placement")
    return "success"
  end
  -- Check source pill still available
  local src = attack.find_pill_at(world, goal.source_mx, goal.source_my)
  if not src or src.health > 0 or src.owner ~= "friendly" then
    goal.substate = "select_pill"
    print(TAG .. " [PP] pickup: source pill no longer available, re-selecting")
    return "success"
  end
  return "running"
end

local function navigate_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  if not goal.place_mx then
    goal.substate = "select_pill"
    return "success"
  end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick
  -- Navigate to deploy position (outside pill range), not the placement tile
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
    return "success"
  end
  return "running"
end

local function dispatch_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local now = state.tick
  if info.man_status ~= C.LGM_INTANK then
    goal.substate = "wait_place"
    goal.wait_tick = now
    print(TAG .. " [PP] dispatch: LGM dispatched, waiting for placement")
    log.reason("pp_sub", { transition = "dispatch->wait_place" })
  elseif now - (goal.dispatch_tick or now) > 50 then
    goal.substate = "select_pill"
    print(TAG .. " [PP] dispatch: LGM didn't leave, re-selecting")
  end
  return "running"
end

local function wait_place_tick(ctx)
  local goal, state, world, info = ctx.goal, ctx.state, ctx.world, ctx.info
  local now = state.tick

  -- Check if a friendly pill appeared at placement position
  local placed = attack.find_pill_at(world, goal.place_mx, goal.place_my)
  if placed and placed.owner == "friendly" and placed.health > 0 then
    -- Set up shield fields for generic shield engage (same as wall-shield)
    goal.placed_mx    = goal.place_mx
    goal.placed_my    = goal.place_my
    goal.shield_mx    = goal.place_mx
    goal.shield_my    = goal.place_my
    goal.shield_type  = "pill"
    goal.substate     = "prewait"
    goal.lgm_return_tick = nil
    print(string.format(TAG .. " [PP] wait_place: pill placed@(%d,%d) hp=%d, waiting for LGM before advance to target@(%d,%d)",
          goal.place_mx, goal.place_my, placed.health, goal.mx, goal.my))
    log.reason("pp_sub", {
      transition = "wait_place->prewait",
      placed_mx = goal.place_mx, placed_my = goal.place_my,
      placed_hp = placed.health,
    })
    return "success"
  end
  -- LGM returned without placing
  if info.man_status == C.LGM_INTANK and now - (goal.wait_tick or now) > 50 then
    if (info.carried_pills or 0) > 0 then
      goal.substate = "select_pill"
      print(TAG .. " [PP] wait_place: LGM returned, pill not placed, re-selecting")
    else
      goal.substate = "select_pill"
      print(TAG .. " [PP] wait_place: pill lost, re-selecting")
    end
    return "success"
  end
  -- Timeout
  if now - (goal.wait_tick or now) > C.PILL_PLACE_TIMEOUT then
    goal.substate = "select_pill"
    print(TAG .. " [PP] wait_place: timeout, re-selecting")
    return "success"
  end
  return "running"
end

local function collect_target_tick(ctx)
  local goal, state, info = ctx.goal, ctx.state, ctx.info
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local pdist = U.mdist(tmx, tmy, goal.mx, goal.my)
  if pdist <= 1 then
    print(string.format(TAG .. " [PP] collect: arrived at target@(%d,%d), chaining to next",
          goal.mx, goal.my))
    log.reason("pp_sub", { transition = "collect_target->done" })
    state.pill_attack_plan = nil
    goal.kind = "none"
    goal.substate = nil
    return "success"
  end
  return "running"
end

-- ── Tree ─────────────────────────────────────────────────────────────────

local tree = bt.sequence(
  -- Ensure substate is initialised
  bt.action(ensure_substate),
  -- Priority selector
  bt.selector(
    -- Global: target dead → collect
    bt.guard(bt.condition(target_dead_not_collecting), bt.action(transition_to_collect)),
    -- Global: target friendly → done
    bt.guard(bt.condition(target_became_friendly),     bt.action(transition_to_done)),
    -- Per-substate dispatch (placement pipeline)
    bt.guard(bt.condition(sub_is("select_pill")),    bt.action(select_pill_tick)),
    bt.guard(bt.condition(sub_is("pickup")),          bt.action(pickup_tick)),
    bt.guard(bt.condition(sub_is("navigate")),        bt.action(navigate_tick)),
    bt.guard(bt.condition(sub_is("dispatch")),        bt.action(dispatch_tick)),
    bt.guard(bt.condition(sub_is("wait_place")),      bt.action(wait_place_tick)),
    -- Generic shield states (same logic as wall-shield: prewait → advance → shield_engage → engage)
    bt.guard(bt.condition(sub_is("prewait")),          bt.action(shield_bt.prewait_tick)),
    bt.guard(bt.condition(sub_is("advance")),          bt.action(shield_bt.advance_tick)),
    bt.guard(bt.condition(sub_is("shield_engage")),    bt.action(shield_bt.shield_engage_tick)),
    bt.guard(bt.condition(sub_is("engage")),            bt.action(shield_bt.engage_tick)),
    bt.guard(bt.condition(sub_is("reposition")),        bt.action(shield_bt.reposition_tick)),
    -- Collect and disengage
    bt.guard(bt.condition(sub_is("collect_target")),   bt.action(collect_target_tick)),
    bt.guard(bt.condition(sub_is("disengage")),        bt.action(shield_bt.disengage_action))
  )
)

-- ── Public API ───────────────────────────────────────────────────────────

local M = {}

function M.tick(goal, state, world, info)
  if goal.kind ~= "pill_place" then return end
  bt.tick(tree, { goal = goal, state = state, world = world, info = info })
end

return M
