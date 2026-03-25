-- =========================================================================
-- NewAutopilot/goals.lua — strategic goal selection + exploration fallback
-- =========================================================================

local C      = require("constants")
local TAG    = "[" .. C.BRAIN_NAME .. "]"
local U      = require("util")
local heap   = require("heap")
local PF     = require("pathfinder")
local cpf    = require("cpathfinder")
local wsim   = require("cworldsim")
local W      = require("world")
local expl   = require("exploration")
local log    = require("logger")
local attack = require("attack")
local threat = require("threat")

local M = {}

local last_strategic_goal = nil  -- track to avoid spamming logs
-- Technique selection: which attack method to use against hostile pills.
-- Priority: pill placement > wall-shield > hardline (bpc)
-- Returns "pill_place", "wall_shield", or "hardline"
local function pick_attack_technique(world, info, state)
  -- Pill placement is always preferred when pills are available
  if (info.carried_pills or 0) > 0 then
    return "pill_place"
  end
  -- Check for dead friendly pills on the map we could pick up
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health == 0 then
      return "pill_place"
    end
  end
  -- Opening phase: prefer hardline for speed (skip wall-shield)
  if state.phase == "opening" then
    return "hardline"
  end
  -- No friendly pills: check for wall-shield (need trees and LGM)
  if C.WALL_SHIELD_ENABLED and (info.trees or 0) >= C.WALL_SHIELD_MIN_TREES
     and info.man_status == C.LGM_INTANK and not info.inboat then
    return "wall_shield"
  end
  -- Fallback: hardline (bpc)
  return "hardline"
end

-- Save/restore last_strategic_goal around lookahead calls so the nested
-- pick_goal doesn't contaminate the real goal-change log messages.
function M.save_goal_state()    return last_strategic_goal end
function M.restore_goal_state(s) last_strategic_goal = s end

-- Forward simulation: predict armor damage along a path to a goal.
-- Returns (extra_cost, killed, sim_desc) where extra_cost is added to
-- the goal's cost, killed=true means the path is lethal.
local function wsim_evaluate_goal(goal, world, info, attack_pill_idx)
  if not C.WSIM_ENABLED then return 0, false, "" end

  -- Build a straight-line path from tank to goal in map tiles
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local gmx, gmy = goal.mx, goal.my
  local dx = gmx - tmx
  local dy = gmy - tmy
  local steps = math.max(math.abs(dx), math.abs(dy))
  if steps == 0 then return 0, false, "" end
  if steps > 250 then steps = 250 end

  local path = {}
  for i = 1, steps do
    local t = i / steps
    path[i] = { x = math.floor(tmx + dx * t + 0.5), y = math.floor(tmy + dy * t + 0.5) }
  end

  wsim.snapshot(world, info, path, attack_pill_idx)
  local r = wsim.run(C.WSIM_MAX_TICKS)

  local extra_cost = r.damage * C.WSIM_DAMAGE_COST_WEIGHT
  local sim_desc = string.format(" wsim:dmg=%d/%dtk", r.damage, r.ticks)
  if r.killed then
    sim_desc = sim_desc .. " KILL"
  end

  log.reason("wsim", {
    goal_kind = goal.kind, dest_mx = gmx, dest_my = gmy,
    damage = r.damage, ticks = r.ticks, killed = r.killed,
    armour_remaining = r.armour,
  })

  return extra_cost, r.killed, sim_desc
end

-- Helper: find the cheapest-to-reach object matching a filter
-- Uses full A* cost_to for accurate ranking (replaces estimate_cost).
-- Returns best, best_id, best_cost, candidates (array of all evaluated)
local function nearest_where(collection, world, tmx, tmy, filter, in_boat, ammo, state, info)
  local best_cost = math.huge
  local best_id, best = nil, nil
  local now = state and state.tick or 0
  local candidates = {}
  local boat_flag = in_boat and 1 or 0
  local shells = info and info.shells or 32
  local trees  = info and info.trees or 0
  local mines  = info and info.mines or 0
  local armour = info and info.armour or 40
  for id, obj in pairs(collection) do
    if filter(obj) then
      -- Skip blocked destinations
      if state and state.blocked then
        local bk = U.mkey(obj.mx, obj.my)
        if state.blocked[bk] then
          if now < state.blocked[bk] then
            candidates[#candidates + 1] = {
              id = id, mx = obj.mx, my = obj.my, cost = -1,
              reject = string.format("blocked until t=%d", state.blocked[bk]),
            }
            goto skip
          end
        end
      end
      -- Skip objects unseen for too long (stale data — likely captured/changed)
      if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then
        candidates[#candidates + 1] = {
          id = id, mx = obj.mx, my = obj.my, cost = -1,
          own = obj.owner or "?", hp = obj.health or 0,
          reject = string.format("stale (unseen %d ticks)", now - obj.last_seen),
        }
        goto skip
      end
      do
        local c = cpf.cost_to(tmx, tmy, obj.mx, obj.my, boat_flag,
                               shells, trees, mines, armour)
        -- Add staleness penalty for objects not seen recently
        if obj.last_seen and now > 0 then
          local age = now - obj.last_seen
          if age > C.STALE_PENALTY_START then
            local penalty = (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
            c = c + penalty
          end
        end
        candidates[#candidates + 1] = {
          id = id, mx = obj.mx, my = obj.my, cost = c,
          own = obj.owner or "?", hp = obj.health or 0,
          stale = obj.last_seen and (now - obj.last_seen) or 0,
        }
        if c < best_cost then
          best_cost = c; best_id = id; best = obj
        end
      end
      ::skip::
    end
  end
  return best, best_id, best_cost, candidates
end

-- Helper: find the best friendly or neutral base for resupply.
-- danger_weight: REFUEL_DANGER_WEIGHT for normal top-up, FLEE_DANGER_WEIGHT
--               when health is critical (scales pill-danger penalty steeply).
-- cur_mx/cur_my: if the tank is already heading to a base, give that base a
--               REFUEL_SWITCH_THRESHOLD score bonus so we only switch when
--               there is a genuinely better option (prevents oscillation).
-- danger_reject: if non-nil, skip bases with pill danger above this value.
-- Used when fleeing at critical armour — sitting at a dangerous base = death.
-- Returns best, best_id, best_score, candidates
local function nearest_resupply_base(world, tmx, tmy, in_boat, ammo, state, info,
                                     danger_weight, cur_mx, cur_my, danger_reject)
  danger_weight = danger_weight or C.REFUEL_DANGER_WEIGHT
  local best_score = math.huge
  local best_id, best = nil, nil
  local now = state and state.tick or 0
  local candidates = {}
  for id, b in pairs(world.bases) do
    if b.owner == "friendly" or b.owner == "neutral" then
      -- Skip blocked destinations
      if state and state.blocked then
        local bk = U.mkey(b.mx, b.my)
        if state.blocked[bk] and now < state.blocked[bk] then
          candidates[#candidates + 1] = {
            id = id, mx = b.mx, my = b.my, score = -1,
            reject = string.format("blocked until t=%d", state.blocked[bk]),
          }
          goto skip
        end
      end
      -- Skip bases with recently observed low stock (not worth the trip).
      -- Observation expires after REFUEL_OBS_STALE ticks (base regenerates).
      if b.obs_tick and now > 0 and (now - b.obs_tick) < C.REFUEL_OBS_STALE then
        local obs_low = true
        if info.armour < C.TANK_FULL_ARMOUR and (b.obs_armour or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
        if info.shells < C.TANK_FULL_SHELLS and (b.obs_shells or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
        if obs_low then
          candidates[#candidates + 1] = {
            id = id, mx = b.mx, my = b.my, own = b.owner, score = -1,
            reject = string.format("low stock (sh=%d arm=%d obs %dt ago)",
                     b.obs_shells or 0, b.obs_armour or 0, now - b.obs_tick),
          }
          goto skip
        end
      end
      -- Skip neutral bases unseen for too long (likely captured by someone else)
      -- Friendly bases are less likely to change so only penalise neutrals
      if b.owner == "neutral" and b.last_seen and now > 0
         and (now - b.last_seen) > C.STALE_SKIP_TICKS then
        candidates[#candidates + 1] = {
          id = id, mx = b.mx, my = b.my, own = b.owner, score = -1,
          reject = string.format("stale neutral (unseen %d ticks)", now - b.last_seen),
        }
        goto skip
      end
      do
        -- Manhattan early-exit: mdist is a lower bound on travel cost
        if best_score ~= math.huge and U.mdist(tmx, tmy, b.mx, b.my) > best_score then
          goto skip
        end
        local travel = cpf.cost_to(tmx, tmy, b.mx, b.my, in_boat and 1 or 0,
                                    info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
        local danger = threat.at(b.mx, b.my)
        -- When fleeing at critical armour, hard-reject bases that are too
        -- dangerous to sit at.  No point driving to a base where you'll die
        -- before the refuel completes.
        if danger_reject and danger > danger_reject then
          candidates[#candidates + 1] = {
            id = id, mx = b.mx, my = b.my, own = b.owner,
            travel = travel, danger = danger, dw = danger_weight,
            score = -1,
            reject = string.format("danger %.1f > reject %.1f", danger, danger_reject),
          }
          goto skip
        end
        local score  = travel + danger * danger_weight
        -- Staleness penalty for neutral bases (might have been captured)
        if b.owner == "neutral" and b.last_seen and now > 0 then
          local age = now - b.last_seen
          if age > C.STALE_PENALTY_START then
            score = score + (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
          end
        end
        -- Feature 4: contested base avoidance — penalise bases with an
        -- enemy tank nearby and heading roughly toward the base
        local contested = false
        local enemy_tanks = state and state.perc and state.perc.enemy_tanks or {}
        for _, et in ipairs(enemy_tanks) do
          local et_base_dist = U.mdist(et.mx, et.my, b.mx, b.my)
          if et_base_dist <= C.CONTESTED_BASE_RANGE and et.speed > 0 then
            -- Enemy tank is near our base and moving — treat as contested.
            -- (Speed comes directly from TankSnapshot, > 0 means moving.)
            score = score + C.CONTESTED_BASE_PENALTY
            contested = true
          end
        end

        local hysteresis = false
        if cur_mx and b.mx == cur_mx and b.my == cur_my then
          score = score - C.REFUEL_SWITCH_THRESHOLD
          hysteresis = true
        end
        candidates[#candidates + 1] = {
          id = id, mx = b.mx, my = b.my, own = b.owner,
          travel = travel, danger = danger, dw = danger_weight,
          score = score, hyst = hysteresis, contested = contested,
          stale = b.last_seen and (now - b.last_seen) or 0,
        }
        if score < best_score then
          best_score = score; best_id = id; best = b
        end
      end
      ::skip::
    end
  end
  return best, best_id, best_score, candidates
end

-- =========================================================================
-- Goal group mapping for hysteresis — switching between groups costs more
-- than switching targets within the same group.
-- =========================================================================
local GOAL_GROUPS = {
  attack_pill = "attack", bpc_pill = "attack", pill_place = "attack",
  attack_tank = "attack_tank",
  capture_base = "capture_base", capture_pill = "capture_pill",
  attack_base = "attack_base",
  refuel_at_base = "refuel", flee_to_base = "refuel",
  defend_pill = "defend",
  repair_pill = "repair", explore = "explore",
  place_pill_strategic = "place_pill",
  none = "none",
}

local function goal_group(kind)
  return GOAL_GROUPS[kind] or kind
end

-- =========================================================================
-- Resolve an attack pill winner into a concrete goal with technique,
-- standoff positions, and wall-shield planning.
-- Reuses the existing pick_attack_technique, attack.get_standoff,
-- attack.pick_standoff infrastructure.
-- =========================================================================
local function resolve_attack_goal(pill, pid, world, info, state)
  -- If already attacking this exact pill, keep the current technique
  -- to avoid flip-flopping (e.g. wall_shield→hardline when LGM is out)
  local active_kinds = { attack_pill=true, bpc_pill=true, pill_place=true }
  local cur = state.goal
  local already_attacking = active_kinds[cur.kind]
                            and cur.mx == pill.mx and cur.my == pill.my
  local technique
  if already_attacking
     and not ((info.carried_pills or 0) > 0 and cur.kind ~= "pill_place") then
    if cur.kind == "pill_place" then technique = "pill_place"
    elseif cur.kind == "bpc_pill" then technique = "hardline"
    else technique = "wall_shield"
    end
  else
    technique = pick_attack_technique(world, info, state)
  end

  if technique == "pill_place" then
    return {
      kind = "pill_place", mx = pill.mx, my = pill.my,
      wx = U.m2w(pill.mx), wy = U.m2w(pill.my),
      target_id = pid, substate = "select_pill",
    }, technique
  elseif technique == "hardline" then
    local smx, smy = attack.pick_standoff(world, info, pill, state, C.BPC_STANDOFF, C.BPC_STANDOFF)
    return {
      kind = "bpc_pill", mx = pill.mx, my = pill.my,
      wx = U.m2w(pill.mx), wy = U.m2w(pill.my),
      target_id = pid,
      standoff_mx = smx, standoff_my = smy,
      substate = smx and "approach" or "approach",
    }, technique
  else
    local pk = U.mkey(pill.mx, pill.my)
    local smx, smy = attack.get_standoff(world, info, pk, pill, state)
    local plan = state.pill_attack_plan
    return {
      kind = "attack_pill", mx = pill.mx, my = pill.my,
      wx = U.m2w(pill.mx), wy = U.m2w(pill.my),
      target_id = pid,
      standoff_mx = smx, standoff_my = smy,
      substate = smx and "approach" or "plan",
      wall_shield = plan and plan.wall_shield or false,
      wall_mx = plan and plan.wall_mx or nil,
      wall_my = plan and plan.wall_my or nil,
      prebuild_mx = plan and plan.prebuild_mx or nil,
      prebuild_my = plan and plan.prebuild_my or nil,
    }, technique
  end
end

-- =========================================================================
-- Pool evaluators — each evaluates one category of goal candidates.
-- Called one-per-tick by update_pool_cache() to spread the cost.
-- Each returns a pool entry table or nil.
-- =========================================================================

local function eval_refuel(state, world, info, tmx, tmy, boat, ammo)
  local needs_resupply = (info.armour <= C.ARMOUR_LOW or info.shells <= C.SHELLS_LOW)
  if not needs_resupply then return nil end
  -- Block depleted bases
  if info.base then
    local base_useless = true
    if info.armour < C.TANK_FULL_ARMOUR and (info.base.armour or 0) > 0 then base_useless = false end
    if info.shells < C.TANK_FULL_SHELLS and (info.base.shells or 0) > 0 then base_useless = false end
    if base_useless then
      state.blocked[U.mkey(tmx, tmy)] = state.tick + 200
      log.reason("goal", {
        pick = "base_depleted", why = "current base has no supplies we need",
        base_arm = info.base.armour, base_sh = info.base.shells,
        arm = info.armour, sh = info.shells,
      })
    end
  end
  local cur_mx, cur_my = nil, nil
  if state.goal.kind == "flee_to_base" or state.goal.kind == "refuel_at_base" then
    cur_mx = state.goal.mx; cur_my = state.goal.my
  end
  local base, bid, bscore, bcands = nearest_resupply_base(world, tmx, tmy, boat, ammo, state, info,
                                                C.REFUEL_DANGER_WEIGHT, cur_mx, cur_my,
                                                C.REFUEL_DANGER_REJECT)
  if not base then return nil end
  local arm_u = math.min(1.0, info.armour / C.ARMOUR_LOW)
  local sh_u  = math.min(1.0, info.shells / C.SHELLS_LOW)
  local urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(arm_u, sh_u))
  local cost = bscore * urgency
  return {
    cost = cost,
    goal = { kind = "refuel_at_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
    desc = string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d sh=%d",
           bid, base.mx, base.my, bscore, urgency, cost, info.armour, info.shells),
    cands = bcands,
  }
end

local function eval_capture_base(state, world, info, tmx, tmy, boat, ammo)
  local has_capturable = not state.perc
        or (state.perc.neutral_base_count > 0)
        or ((state.perc.capturable_hostile_base_count or 0) > 0)
  if not has_capturable then return nil end
  -- Capturable: neutral OR hostile with health=0 (armour beaten below capture threshold)
  local base, bid, bcost, bcands = nearest_where(world.bases, world, tmx, tmy,
    function(b) return b.owner == "neutral" or (b.owner == "hostile" and b.health == 0) end,
    boat, ammo, state, info)
  if not base then return nil end
  return {
    cost = bcost,
    goal = { kind = "capture_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
    desc = string.format("capture_base#%d@(%d,%d) cost=%.0f", bid, base.mx, base.my, bcost),
    cands = bcands,
  }
end

local function eval_capture_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_dead = not state.perc or (state.perc.dead_neutral_pill_count > 0)
  if not has_dead then return nil end
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p) return p.owner == "neutral" and p.health == 0 end, boat, ammo, state, info)
  if not pill then return nil end
  return {
    cost = pcost,
    goal = { kind = "capture_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
    desc = string.format("capture_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, pcost),
    cands = pcands,
  }
end

local function eval_repair_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_damaged = not state.perc or (state.perc.friendly_pills_damaged > 0)
  if not (has_damaged and info.man_status == C.LGM_INTANK and info.trees > 0) then return nil end
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p)
      return p.owner == "friendly" and p.health > 0
             and p.health < C.PILLS_MAX_HEALTH
    end, boat, ammo, state, info)
  if not pill then return nil end
  local damage = C.PILLS_MAX_HEALTH - pill.health
  local adj_cost = math.max(0, pcost - damage * C.REPAIR_DAMAGE_BONUS)
  return {
    cost = adj_cost,
    goal = { kind = "repair_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
    desc = string.format("repair_pill#%d@(%d,%d) cost=%.0f (path=%.0f -dam=%d×%d)",
           pid, pill.mx, pill.my, adj_cost, pcost, damage, C.REPAIR_DAMAGE_BONUS),
    cands = pcands,
  }
end

-- Compute attack_pill cost adjustments for a given pill/path-cost.
-- Returns adjusted_cost, description_suffix.
local function attack_pill_adjustments(pill, pcost, state, world)
  local adj_cost = pcost + pill.health * C.PILL_HEALTH_WEIGHT
  local antic_desc = ""

  -- Pill anger cooldown
  local pill_anger = pill.anger or 0
  if pill_anger > C.ANGER_ATTACK_THRESHOLD then
    local ticks_to_calm = (pill_anger - C.ANGER_ATTACK_THRESHOLD) * C.PILL_ANGER_DECAY
    if ticks_to_calm < C.ANGER_WAIT_MAX then
      local anger_cost = ticks_to_calm * C.ANGER_COST_PER_TICK
      adj_cost = adj_cost + anger_cost
      antic_desc = antic_desc .. string.format(" +anger=%.0f", anger_cost)
    end
  end

  -- Enemy tank intercept (TTK vs TTI)
  local ttk = pill.health * C.TTK_TICKS_PER_HIT
  local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
  local worst_intercept = 0
  for _, et in ipairs(enemy_tanks) do
    local et_dist = U.mdist(et.mx, et.my, pill.mx, pill.my)
    if et_dist <= C.INTERCEPT_MAX_RANGE then
      local espeed = math.max(et.speed, 0.5)
      local tti = et_dist / espeed
      if ttk > tti * C.INTERCEPT_SAFETY_MARGIN then
        local ratio = ttk / math.max(1, tti)
        local pen = C.INTERCEPT_PENALTY * math.min(2.0, ratio)
        if pen > worst_intercept then worst_intercept = pen end
      end
    end
  end
  if worst_intercept > 0 then
    adj_cost = adj_cost + worst_intercept
    antic_desc = antic_desc .. string.format(" +intercept=%.0f", worst_intercept)
  end

  -- Crossfire penalty
  local crossfire_pen = 0
  for _, pm in pairs(world.pills) do
    if (pm.mx ~= pill.mx or pm.my ~= pill.my)
       and (pm.owner == "hostile" or pm.owner == "neutral")
       and pm.health > 0 then
      local d = U.mdist(pill.mx, pill.my, pm.mx, pm.my)
      if d <= C.PILL_RANGE_MAP + C.ATTACK_PILL_STANDOFF then
        local max_d = C.PILL_RANGE_MAP + C.ATTACK_PILL_STANDOFF
        local proximity = 1.0 - d / (max_d + 1)
        crossfire_pen = crossfire_pen + C.GOAL_CROSSFIRE_PENALTY * proximity
      end
    end
  end
  if crossfire_pen > 0 then
    adj_cost = adj_cost + crossfire_pen
    antic_desc = antic_desc .. string.format(" +xfire=%.0f", crossfire_pen)
  end

  return adj_cost, antic_desc
end

local function eval_attack_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_hostile = not state.perc or (state.perc.attackable_pill_count > 0)
  if not (has_hostile and info.shells > C.SHELLS_LOW) then return nil end
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p) return (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 end,
    boat, ammo, state, info)
  if not pill then return nil end
  local adj_cost, antic_desc = attack_pill_adjustments(pill, pcost, state, world)

  return {
    cost = adj_cost,
    _pill = pill, _pill_id = pid,
    goal = { kind = "attack_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my) },
    desc = string.format("attack_pill#%d@(%d,%d) cost=%.0f (path=%.0f +hp=%d×%d%s)",
           pid, pill.mx, pill.my, adj_cost, pcost, pill.health, C.PILL_HEALTH_WEIGHT, antic_desc),
    cands = pcands,
  }
end

-- If we're currently attacking a pill and nearest_where picked a different one,
-- re-evaluate the current target so hysteresis can properly compare them.
local function eval_current_attack_pill(state, world, info, tmx, tmy, boat, ammo)
  local goal = state.goal
  if not goal then return nil end
  -- Only applies when we're actively attacking a pill
  if goal.kind ~= "attack_pill" and goal.kind ~= "bpc_pill" then return nil end
  if not (info.shells > C.SHELLS_LOW) then return nil end

  -- Find the pill at our current goal target
  local cur_pill, cur_pid = nil, nil
  for id, p in pairs(world.pills) do
    if p.mx == goal.mx and p.my == goal.my then
      cur_pill = p; cur_pid = id; break
    end
  end
  if not cur_pill then return nil end
  -- Must still be attackable
  if not ((cur_pill.owner == "hostile" or cur_pill.owner == "neutral") and cur_pill.health > 0) then
    return nil
  end

  -- Check if eval_attack_pill (slot 5) already picked this pill (avoid duplicate).
  -- Only check slot 5 specifically — checking all slots would match our own stale
  -- entry from the previous replan cycle.
  local pc = state.pool_cache or {}
  local attack_pill_slot = pc[5]
  if attack_pill_slot and attack_pill_slot._pill_id == cur_pid then return nil end

  local pcost = cpf.cost_to(tmx, tmy, cur_pill.mx, cur_pill.my, boat and 1 or 0,
                             info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
  local adj_cost, antic_desc = attack_pill_adjustments(cur_pill, pcost, state, world)

  return {
    cost = adj_cost,
    _pill = cur_pill, _pill_id = cur_pid,
    goal = { kind = "attack_pill", mx = cur_pill.mx, my = cur_pill.my,
             wx = U.m2w(cur_pill.mx), wy = U.m2w(cur_pill.my) },
    desc = string.format("attack_pill#%d@(%d,%d) cost=%.0f (path=%.0f +hp=%d×%d%s)",
           cur_pid, cur_pill.mx, cur_pill.my, adj_cost, pcost, cur_pill.health,
           C.PILL_HEALTH_WEIGHT, antic_desc),
  }
end

local function eval_attack_base(state, world, info, tmx, tmy, boat, ammo)
  local has_hbases = not state.perc or (state.perc.hostile_base_count > 0)
  if not (has_hbases and info.shells > C.SHELLS_LOW) then return nil end
  -- Only attack hostile bases that are still alive (health > 0).
  -- health=0 means capturable — eval_capture_base handles those.
  local base, bid, bcost, bcands = nearest_where(world.bases, world, tmx, tmy,
    function(b) return b.owner == "hostile" and b.health > 0 end, boat, ammo, state, info)
  if not base then return nil end
  -- Penalise bases covered by enemy pills/tanks (like aIndy's cover penalty).
  local threat_at_base = threat.at(base.mx, base.my)
  local adj_cost = bcost + C.ATTACK_BASE_EXTRA_COST
                 + threat_at_base * C.ATTACK_BASE_THREAT_WEIGHT
  return {
    cost = adj_cost,
    goal = { kind = "attack_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
    desc = string.format("attack_base#%d@(%d,%d) cost=%.0f (path=%.0f +base=%d +threat=%.0f×%d)",
           bid, base.mx, base.my, adj_cost, bcost, C.ATTACK_BASE_EXTRA_COST,
           threat_at_base, C.ATTACK_BASE_THREAT_WEIGHT),
    cands = bcands,
  }
end

-- =========================================================================
-- Strategic pill placement: place carried pills near the front line / bases
-- when no attack targets are available.
-- =========================================================================

-- Compute average position of all hostile/neutral live pills (enemy center of gravity)
local function enemy_center_of_gravity(world)
  local sx, sy, n = 0, 0, 0
  for _, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 then
      sx = sx + p.mx; sy = sy + p.my; n = n + 1
    end
  end
  for _, b in pairs(world.bases) do
    if b.owner == "hostile" then
      sx = sx + b.mx; sy = sy + b.my; n = n + 1
    end
  end
  if n == 0 then return nil, nil end
  return math.floor(sx / n + 0.5), math.floor(sy / n + 0.5)
end

local function nearest_friendly_base_pos(world, mx, my)
  local best_d = math.huge
  local bx, by = nil, nil
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      local d = U.mdist(mx, my, b.mx, b.my)
      if d < best_d then best_d = d; bx = b.mx; by = b.my end
    end
  end
  return bx, by, best_d
end

local function nearest_friendly_pill_dist(world, mx, my)
  local best_d = math.huge
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health > 0 then
      local d = U.mdist(mx, my, p.mx, p.my)
      if d < best_d then best_d = d end
    end
  end
  return best_d
end

-- Count pills within radius of a map position matching an owner filter
local function count_pills_near(world, mx, my, radius, owner_filter)
  local count = 0
  for _, p in pairs(world.pills) do
    if p.health > 0 and p.owner == owner_filter then
      if U.mdist(mx, my, p.mx, p.my) <= radius then
        count = count + 1
      end
    end
  end
  return count
end

-- Find nearest hostile base to a map position
local function nearest_hostile_base(world, mx, my)
  local best_d = math.huge
  local best = nil
  for _, b in pairs(world.bases) do
    if b.owner == "hostile" then
      local d = U.mdist(mx, my, b.mx, b.my)
      if d < best_d then best_d = d; best = b end
    end
  end
  return best, best_d
end

local function eval_attack_tank(state, world, info, tmx, tmy, boat, ammo)
  if not C.TANK_COMBAT_ENABLED then return nil end
  if info.shells < C.TANK_COMBAT_MIN_SHELLS then return nil end
  if info.armour < C.TANK_COMBAT_MIN_ARMOUR then return nil end
  if info.inboat then return nil end  -- can't fight from a boat effectively

  local perc = state.perc
  if not perc or not perc.enemy_tanks or #perc.enemy_tanks == 0 then return nil end

  local best_cost = math.huge
  local best_tank = nil

  for _, et in ipairs(perc.enemy_tanks) do
    if et.dist <= C.TANK_COMBAT_MAX_RANGE then
      -- Base cost: distance (using A* for terrain awareness)
      local path_cost = cpf.cost_to(tmx, tmy, et.mx, et.my, 0,
                                     info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
      local cost = path_cost + C.TANK_COMBAT_BASE_COST

      -- Aim bonus: if we're already pointed roughly at this tank, cheaper to engage
      local aim_dir = U.aim_at(info.tankx, info.tanky, U.m2w(et.mx), U.m2w(et.my))
      local aim_diff = math.abs(U.adiff(info.direction, aim_dir))
      if aim_diff < C.TANK_COMBAT_AIM_THRESHOLD then
        cost = cost - C.TANK_COMBAT_AIM_BONUS
      end

      -- Crossfire penalty: if enemy tank is near a hostile pill, we'll take pill fire too
      for _, pt in ipairs(perc.pill_threats or {}) do
        if U.mdist(et.mx, et.my, pt.pill.mx, pt.pill.my) <= C.TANK_COMBAT_NEAR_PILL_RANGE then
          cost = cost + C.TANK_COMBAT_NEAR_PILL_PENALTY
          break
        end
      end

      -- Future enhancement: if we could detect carried pills, increase priority here

      if cost < best_cost then
        best_cost = cost
        best_tank = et
      end
    end
  end

  if not best_tank then return nil end

  return {
    cost = best_cost,
    goal = { kind = "attack_tank", mx = best_tank.mx, my = best_tank.my,
             wx = U.m2w(best_tank.mx), wy = U.m2w(best_tank.my),
             target_obj = best_tank.obj,
             substate = "close" },
    desc = string.format("attack_tank@(%d,%d) cost=%.0f dist=%d spd=%.1f",
           best_tank.mx, best_tank.my, best_cost, best_tank.dist, best_tank.speed),
  }
end

local function eval_place_pill_strategic(state, world, info, tmx, tmy, boat, ammo)
  if not C.STRATEGIC_PLACE_ENABLED then return nil end
  if (info.carried_pills or 0) < 1 then return nil end
  if info.man_status ~= C.LGM_INTANK then return nil end
  if info.inboat then return nil end

  -- Don't interrupt active combat goals
  local gk = state.goal and state.goal.kind or "none"
  if gk == "attack_pill" or gk == "bpc_pill" or gk == "pill_place" then return nil end

  local fbx, fby, fb_dist = nearest_friendly_base_pos(world, tmx, tmy)
  if not fbx then return nil end  -- no friendly base

  -- Offensive mode: when dominating and all bases adequately defended,
  -- target hostile bases with pill spikes.
  local offensive = false
  if state.strength and state.strength > C.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD then
    local all_defended = true
    for _, b in pairs(world.bases) do
      if b.owner == "friendly" then
        if count_pills_near(world, b.mx, b.my, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly") < 2 then
          all_defended = false
          break
        end
      end
    end
    if all_defended then offensive = true end
  end

  -- Search center: front line if available, else midpoint heuristic
  local search_mx, search_my
  local spike_base = nil  -- hostile base to spike (offensive mode)

  if offensive then
    local hb, hb_dist = nearest_hostile_base(world, tmx, tmy)
    if hb then
      search_mx, search_my = hb.mx, hb.my
      spike_base = hb
    end
  end

  if not search_mx then
    if state.front_center_mx then
      search_mx, search_my = state.front_center_mx, state.front_center_my
    else
      local ecx, ecy = enemy_center_of_gravity(world)
      if not ecx then ecx, ecy = fbx, fby end
      search_mx = math.floor((fbx + ecx) / 2 + 0.5)
      search_my = math.floor((fby + ecy) / 2 + 0.5)
    end
  end

  local R = C.STRATEGIC_PLACE_SEARCH_RADIUS
  local best_score = -math.huge
  local best_mx, best_my = nil, nil

  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(search_mx + dx)
      local cy = U.mclamp(search_my + dy)
      if U.is_placeable(cx, cy, world) then
        local score = 0

        -- 1. Base proximity (pills should be near friendly bases)
        local _, _, base_dist = nearest_friendly_base_pos(world, cx, cy)
        if base_dist > C.STRATEGIC_PLACE_MAX_BASE_DIST then goto skip_cell end
        score = score + (C.STRATEGIC_PLACE_MAX_BASE_DIST - base_dist) * C.STRATEGIC_PLACE_BASE_WEIGHT

        -- 2. Base defense need (bases with < 2 nearby pills get a bonus)
        do
          local base_pill_count = count_pills_near(world, cx, cy, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly")
          if base_pill_count < 2 then
            score = score + C.STRATEGIC_PLACE_UNDERDEFENDED_BONUS * (2 - base_pill_count)
          end
        end

        -- 3. Influence-aware front line proximity
        do
          local influence = cpf.influence_at(cx, cy)
          if influence < 0 then
            -- Beyond front line into enemy territory: heavy penalty
            score = score - C.STRATEGIC_PLACE_BEYOND_FRONT_PENALTY
          elseif influence > 0 then
            -- In friendly territory: closer to 0 (front) is more useful
            score = score + math.max(0, C.STRATEGIC_PLACE_FRONT_PROX_CAP - influence)
                   * C.STRATEGIC_PLACE_FRONT_PROX_WEIGHT
          end
        end

        -- 4. Pill spacing (avoid clustering, prefer 2-3 tile gaps)
        do
          local pill_dist = nearest_friendly_pill_dist(world, cx, cy)
          if pill_dist < C.STRATEGIC_PLACE_PILL_SPACING then
            score = score - C.STRATEGIC_PLACE_PILL_PENALTY
          elseif pill_dist >= 2 and pill_dist <= 4 then
            score = score + C.STRATEGIC_PLACE_SPACING_BONUS
          end
        end

        -- 5. LOS coverage
        do
          local los = U.los_coverage(cx, cy, C.STRATEGIC_PLACE_LOS_DIRS, C.STRATEGIC_PLACE_LOS_MAX_RANGE)
          score = score + los * C.STRATEGIC_PLACE_LOS_WEIGHT
        end

        -- 6. Threat penalty
        do
          local thr = threat.at(cx, cy)
          score = score - thr * C.STRATEGIC_PLACE_THREAT_WEIGHT
        end

        -- 7. Distance from tank
        score = score - U.mdist(tmx, tmy, cx, cy) * 0.5

        -- 8. Offensive spike bonus (placing adjacent to hostile base)
        if spike_base then
          local hb_dist = U.mdist(cx, cy, spike_base.mx, spike_base.my)
          if hb_dist <= 2 then
            score = score + C.STRATEGIC_PLACE_SPIKE_BONUS
          end
        end

        if score > best_score then
          best_score = score; best_mx = cx; best_my = cy
        end
      end
      ::skip_cell::
    end
  end

  if not best_mx then return nil end

  local path_cost = cpf.cost_to(tmx, tmy, best_mx, best_my, boat and 1 or 0,
                                info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
  local cost = path_cost + C.STRATEGIC_PLACE_BASE_COST

  return {
    cost = cost,
    goal = { kind = "place_pill_strategic", mx = best_mx, my = best_my,
             wx = U.m2w(best_mx), wy = U.m2w(best_my) },
    desc = string.format("place_pill_strategic@(%d,%d) cost=%.0f (path=%.0f +base=%d) score=%.1f%s",
           best_mx, best_my, cost, path_cost, C.STRATEGIC_PLACE_BASE_COST, best_score,
           offensive and " [offensive]" or ""),
  }
end

-- =========================================================================
-- Defend pill: respond to sustained attacks on friendly pills
-- =========================================================================
local function eval_defend_pill(state, world, info, tmx, tmy, boat, ammo)
  local target = state.perc and state.perc.pill_under_attack
  if not target then return nil end

  -- Don't defend if we're critically low on health ourselves
  if info.armour < C.ARMOUR_CRITICAL then return nil end

  -- Only respond to sustained attacks (>= threshold damage)
  if target.damage < C.DEFEND_PILL_MIN_DAMAGE then return nil end

  -- Don't defend if already attacking near this pill
  local gk = state.goal and state.goal.kind or "none"
  if gk == "attack_pill" or gk == "bpc_pill" or gk == "pill_place" then
    if state.goal.mx and U.mdist(state.goal.mx, state.goal.my, target.mx, target.my) <= 5 then
      return nil
    end
  end

  local travel = cpf.cost_to(tmx, tmy, target.mx, target.my, boat and 1 or 0,
                              info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)

  -- Don't go if it's too far (pill will be dead before we arrive)
  if travel > C.DEFEND_PILL_MAX_TRAVEL then return nil end

  -- Urgency bonus: more damage = lower cost (more urgent)
  local urgency = math.max(0, target.damage - C.DEFEND_PILL_MIN_DAMAGE) * C.DEFEND_PILL_URGENCY_WEIGHT

  local cost = travel + C.DEFEND_PILL_BASE_COST - urgency

  return {
    cost = cost,
    goal = { kind = "defend_pill", mx = target.mx, my = target.my,
             wx = U.m2w(target.mx), wy = U.m2w(target.my),
             pill_id = target.id },
    desc = string.format("defend_pill#%d@(%d,%d) cost=%.0f (travel=%.0f urgency=%.0f dmg=%d)",
           target.id, target.mx, target.my, cost, travel, urgency, target.damage),
  }
end

-- Ordered list of pool evaluators — index 1..GOAL_POOL_COUNT
-- "cheap" evaluators run at finalize time (0-1 A* calls each).
-- "nearest_where" pools are split: filter at queue-build, cost_to incremental.
local POOL_EVALUATORS = {
  eval_refuel,
  eval_defend_pill,
  eval_capture_base,
  eval_capture_pill,
  eval_repair_pill,
  eval_attack_pill,
  eval_attack_base,
  eval_place_pill_strategic,
  eval_attack_tank,
  eval_current_attack_pill,
}

-- Phase weight keys for each pool evaluator (maps into C.PHASE_WEIGHTS[phase]).
-- nil = no phase weighting (always critical, e.g. refuel/attack_tank).
local POOL_NAMES = {
  "refuel", "defend_pill", "capture_base", "capture_pill", "repair_pill",
  "attack_pill", "attack_base", "place_strategic", "attack_tank", "attack_pill",
}

-- =========================================================================
-- Incremental candidate queue system
--
-- Instead of evaluating one pool-category per tick (which spikes when a
-- category has many candidates), we flatten ALL candidates from all
-- nearest_where-based pools into a single queue and process 2 per tick.
--
-- Pools that don't iterate candidates (defend_pill, attack_tank,
-- place_pill_strategic, current_attack_pill) run at finalize time since
-- they each do at most 1 A* call.
--
-- Lifecycle per replan cycle:
--   tick 0:  build_eval_queue  — filter candidates, build work queue
--   tick 1..N: step_eval_queue — pop 2 items, run cost_to
--   decision tick: finalize_pools — pick best per pool, run cheap pools
-- =========================================================================

-- Pools that use nearest_where and need incremental evaluation.
-- Maps pool_idx -> { collection_key, filter_fn }
-- collection_key: "bases" or "pills" (keys in world table)
local INCREMENTAL_POOLS = {
  -- 1: eval_refuel  (uses nearest_resupply_base — custom filter, handled specially)
  [1] = { collection = "bases", kind = "refuel" },
  -- 3: eval_capture_base
  [3] = { collection = "bases", kind = "capture_base" },
  -- 4: eval_capture_pill
  [4] = { collection = "pills", kind = "capture_pill" },
  -- 5: eval_repair_pill
  [5] = { collection = "pills", kind = "repair_pill" },
  -- 6: eval_attack_pill
  [6] = { collection = "pills", kind = "attack_pill" },
  -- 7: eval_attack_base
  [7] = { collection = "bases", kind = "attack_base" },
}

-- Pools evaluated at finalize time (cheap, 0-1 A* calls)
local FINALIZE_POOLS = { 2, 8, 9, 10 }  -- defend_pill, place_pill, attack_tank, current_attack_pill

-- Filter functions for each incremental pool.
-- Return true if the object is a valid candidate.
local function filter_refuel(obj, state, info)
  if not (obj.owner == "friendly" or obj.owner == "neutral") then return false end
  -- Skip blocked
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  -- Skip neutral bases unseen too long
  local now = state and state.tick or 0
  if obj.owner == "neutral" and obj.last_seen and now > 0
     and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  -- Skip bases with recently observed low stock
  if obj.obs_tick and now > 0 and (now - obj.obs_tick) < C.REFUEL_OBS_STALE then
    local obs_low = true
    if info.armour < C.TANK_FULL_ARMOUR and (obj.obs_armour or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
    if info.shells < C.TANK_FULL_SHELLS and (obj.obs_shells or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
    if obs_low then return false end
  end
  -- Reject bases too dangerous (pill fire)
  local danger = threat.at(obj.mx, obj.my)
  if danger > C.REFUEL_DANGER_REJECT then return false end
  return true
end

local function filter_capture_base(obj, state)
  if not (obj.owner == "neutral" or (obj.owner == "hostile" and obj.health == 0)) then return false end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  return true
end

local function filter_capture_pill(obj, state)
  if not (obj.owner == "neutral" and obj.health == 0) then return false end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  return true
end

local function filter_repair_pill(obj, state, info)
  if not (obj.owner == "friendly" and obj.health > 0
          and obj.health < C.PILLS_MAX_HEALTH) then return false end
  if not (info.man_status == C.LGM_INTANK and info.trees > 0) then return false end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  return true
end

local function filter_attack_pill(obj, state)
  if not ((obj.owner == "hostile" or obj.owner == "neutral") and obj.health > 0) then return false end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  return true
end

local function filter_attack_base(obj, state)
  if not (obj.owner == "hostile" and obj.health > 0) then return false end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 and (now - obj.last_seen) > C.STALE_SKIP_TICKS then return false end
  return true
end

local POOL_FILTERS = {
  [1] = filter_refuel,
  [3] = filter_capture_base,
  [4] = filter_capture_pill,
  [5] = filter_repair_pill,
  [6] = filter_attack_pill,
  [7] = filter_attack_base,
}

-- =========================================================================
-- build_eval_queue — called at the start of each replan cycle.
-- Iterates all incremental pools, applies filters, and builds a flat
-- work queue of candidates to evaluate (2 per tick).
-- =========================================================================
function M.build_eval_queue(state, world, info)
  local queue = {}
  local now = state.tick or 0
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  -- Check preconditions that would skip entire pools
  local needs_resupply = (info.armour <= C.ARMOUR_LOW or info.shells <= C.SHELLS_LOW)
  local has_shells = info.shells > C.SHELLS_LOW
  local perc = state.perc

  -- Block depleted bases (side effect from eval_refuel)
  if needs_resupply and info.base then
    local base_useless = true
    if info.armour < C.TANK_FULL_ARMOUR and (info.base.armour or 0) > 0 then base_useless = false end
    if info.shells < C.TANK_FULL_SHELLS and (info.base.shells or 0) > 0 then base_useless = false end
    if base_useless then
      state.blocked[U.mkey(tmx, tmy)] = now + 200
      log.reason("goal", {
        pick = "base_depleted", why = "current base has no supplies we need",
        base_arm = info.base.armour, base_sh = info.base.shells,
        arm = info.armour, sh = info.shells,
      })
    end
  end

  -- Pool 1: refuel (only if needs resupply)
  if needs_resupply then
    for id, obj in pairs(world.bases) do
      if filter_refuel(obj, state, info) then
        queue[#queue + 1] = { pool = 1, id = id, obj = obj }
      end
    end
  end

  -- Pool 3: capture_base
  local has_capturable = not perc
        or (perc.neutral_base_count > 0)
        or ((perc.capturable_hostile_base_count or 0) > 0)
  if has_capturable then
    for id, obj in pairs(world.bases) do
      if filter_capture_base(obj, state) then
        queue[#queue + 1] = { pool = 3, id = id, obj = obj }
      end
    end
  end

  -- Pool 4: capture_pill
  local has_dead = not perc or (perc.dead_neutral_pill_count > 0)
  if has_dead then
    for id, obj in pairs(world.pills) do
      if filter_capture_pill(obj, state) then
        queue[#queue + 1] = { pool = 4, id = id, obj = obj }
      end
    end
  end

  -- Pool 5: repair_pill
  local has_damaged = not perc or (perc.friendly_pills_damaged > 0)
  if has_damaged and info.man_status == C.LGM_INTANK and info.trees > 0 then
    for id, obj in pairs(world.pills) do
      if filter_repair_pill(obj, state, info) then
        queue[#queue + 1] = { pool = 5, id = id, obj = obj }
      end
    end
  end

  -- Pool 6: attack_pill (only if enough shells)
  local has_hostile_pills = not perc or (perc.attackable_pill_count > 0)
  if has_hostile_pills and has_shells then
    for id, obj in pairs(world.pills) do
      if filter_attack_pill(obj, state) then
        queue[#queue + 1] = { pool = 6, id = id, obj = obj }
      end
    end
  end

  -- Pool 7: attack_base (only if enough shells)
  local has_hbases = not perc or (perc.hostile_base_count > 0)
  if has_hbases and has_shells then
    for id, obj in pairs(world.bases) do
      if filter_attack_base(obj, state) then
        queue[#queue + 1] = { pool = 7, id = id, obj = obj }
      end
    end
  end

  state.eval_queue = queue
  state.eval_queue_pos = 1
  -- Partial results: pool_idx -> { candidates = {}, best_cost, best_id, best_obj }
  state.pool_partial = {}
end

-- =========================================================================
-- step_eval_queue — called every tick.  Pops up to 2 candidates from the
-- queue, runs cost_to for each, and accumulates partial results.
-- =========================================================================
function M.step_eval_queue(state, world, info)
  local queue = state.eval_queue
  if not queue then return end
  local pos = state.eval_queue_pos or 1
  if pos > #queue then return end  -- queue exhausted

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local boat_flag = info.inboat and 1 or 0
  local shells = info.shells or 32
  local trees  = info.trees or 0
  local mines  = info.mines or 0
  local armour = info.armour or 40
  local now = state.tick or 0

  local partial = state.pool_partial
  if not partial then partial = {}; state.pool_partial = partial end

  local count = 0
  while pos <= #queue and count < C.GOAL_CANDS_PER_TICK do
    local item = queue[pos]
    pos = pos + 1
    count = count + 1

    local pool_idx = item.pool
    local obj = item.obj
    local id = item.id

    -- Initialize partial result for this pool if needed
    if not partial[pool_idx] then
      partial[pool_idx] = { candidates = {}, best_cost = math.huge, best_id = nil, best_obj = nil }
    end
    local pr = partial[pool_idx]

    -- Run cost_to (raw A* path cost)
    local raw_cost = cpf.cost_to(tmx, tmy, obj.mx, obj.my, boat_flag, shells, trees, mines, armour)

    -- Pool-specific adjustments applied inline
    if pool_idx == 1 then
      -- Refuel: uses its own scoring (danger + staleness + contested + hysteresis)
      local danger = threat.at(obj.mx, obj.my)
      local score = raw_cost + danger * C.REFUEL_DANGER_WEIGHT
      -- Staleness penalty for neutral bases
      if obj.owner == "neutral" and obj.last_seen and now > 0 then
        local age = now - obj.last_seen
        if age > C.STALE_PENALTY_START then
          score = score + (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
        end
      end
      -- Contested base avoidance
      local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
      for _, et in ipairs(enemy_tanks) do
        if U.mdist(et.mx, et.my, obj.mx, obj.my) <= C.CONTESTED_BASE_RANGE and et.speed > 0 then
          score = score + C.CONTESTED_BASE_PENALTY
          break
        end
      end
      -- Hysteresis for current refuel target
      local gk = state.goal and state.goal.kind
      if (gk == "flee_to_base" or gk == "refuel_at_base")
         and obj.mx == state.goal.mx and obj.my == state.goal.my then
        score = score - C.REFUEL_SWITCH_THRESHOLD
      end
      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, own = obj.owner,
        travel = raw_cost, danger = danger, score = score,
      }
      if score < pr.best_cost then
        pr.best_cost = score; pr.best_id = id; pr.best_obj = obj
      end
    else
      -- Generic pools: raw cost + staleness penalty
      local c = raw_cost
      if obj.last_seen and now > 0 then
        local age = now - obj.last_seen
        if age > C.STALE_PENALTY_START then
          c = c + (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
        end
      end
      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, cost = c,
        own = obj.owner or "?", hp = obj.health or 0,
        stale = obj.last_seen and (now - obj.last_seen) or 0,
      }
      if c < pr.best_cost then
        pr.best_cost = c; pr.best_id = id; pr.best_obj = obj
      end
    end
  end

  state.eval_queue_pos = pos
end

-- =========================================================================
-- finalize_pools — called at decision tick.  Converts partial results
-- into pool_cache entries (same format as the old evaluators returned).
-- Also runs cheap evaluators that don't need incremental evaluation.
-- =========================================================================
function M.finalize_pools(state, world, info)
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8
  local boat = info.inboat
  local ammo = (info.shells or 0) + (info.mines or 0)
  local partial = state.pool_partial or {}

  -- Fresh cache each cycle (don't carry stale entries from last cycle)
  state.pool_cache = {}
  local pc = state.pool_cache

  -- Pool 1: refuel — finalize from partial
  local pr1 = partial[1]
  if pr1 and pr1.best_obj then
    local base = pr1.best_obj
    local bid = pr1.best_id
    local bscore = pr1.best_cost
    local arm_u = math.min(1.0, info.armour / C.ARMOUR_LOW)
    local sh_u  = math.min(1.0, info.shells / C.SHELLS_LOW)
    local urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(arm_u, sh_u))
    local cost = bscore * urgency
    pc[1] = {
      cost = cost,
      goal = { kind = "refuel_at_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d sh=%d",
             bid, base.mx, base.my, bscore, urgency, cost, info.armour, info.shells),
      cands = pr1.candidates,
    }
  else
    pc[1] = nil
  end

  -- Pool 3: capture_base
  local pr3 = partial[3]
  if pr3 and pr3.best_obj then
    local base = pr3.best_obj
    local bid = pr3.best_id
    pc[3] = {
      cost = pr3.best_cost,
      goal = { kind = "capture_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = string.format("capture_base#%d@(%d,%d) cost=%.0f", bid, base.mx, base.my, pr3.best_cost),
      cands = pr3.candidates,
    }
  else
    pc[3] = nil
  end

  -- Pool 4: capture_pill
  local pr4 = partial[4]
  if pr4 and pr4.best_obj then
    local pill = pr4.best_obj
    local pid = pr4.best_id
    pc[4] = {
      cost = pr4.best_cost,
      goal = { kind = "capture_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
      desc = string.format("capture_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, pr4.best_cost),
      cands = pr4.candidates,
    }
  else
    pc[4] = nil
  end

  -- Pool 5: repair_pill
  local pr5 = partial[5]
  if pr5 and pr5.best_obj then
    local pill = pr5.best_obj
    local pid = pr5.best_id
    local pcost = pr5.best_cost
    local damage = C.PILLS_MAX_HEALTH - pill.health
    local adj_cost = math.max(0, pcost - damage * C.REPAIR_DAMAGE_BONUS)
    pc[5] = {
      cost = adj_cost,
      goal = { kind = "repair_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
      desc = string.format("repair_pill#%d@(%d,%d) cost=%.0f (path=%.0f -dam=%d×%d)",
             pid, pill.mx, pill.my, adj_cost, pcost, damage, C.REPAIR_DAMAGE_BONUS),
      cands = pr5.candidates,
    }
  else
    pc[5] = nil
  end

  -- Pool 6: attack_pill
  local pr6 = partial[6]
  if pr6 and pr6.best_obj then
    local pill = pr6.best_obj
    local pid = pr6.best_id
    local pcost = pr6.best_cost
    local adj_cost, antic_desc = attack_pill_adjustments(pill, pcost, state, world)
    pc[6] = {
      cost = adj_cost,
      _pill = pill, _pill_id = pid,
      goal = { kind = "attack_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my) },
      desc = string.format("attack_pill#%d@(%d,%d) cost=%.0f (path=%.0f +hp=%d×%d%s)",
             pid, pill.mx, pill.my, adj_cost, pcost, pill.health, C.PILL_HEALTH_WEIGHT, antic_desc),
      cands = pr6.candidates,
    }
  else
    pc[6] = nil
  end

  -- Pool 7: attack_base
  local pr7 = partial[7]
  if pr7 and pr7.best_obj then
    local base = pr7.best_obj
    local bid = pr7.best_id
    local bcost = pr7.best_cost
    local threat_at_base = threat.at(base.mx, base.my)
    local adj_cost = bcost + C.ATTACK_BASE_EXTRA_COST
                   + threat_at_base * C.ATTACK_BASE_THREAT_WEIGHT
    pc[7] = {
      cost = adj_cost,
      goal = { kind = "attack_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = string.format("attack_base#%d@(%d,%d) cost=%.0f (path=%.0f +base=%d +threat=%.0f×%d)",
             bid, base.mx, base.my, adj_cost, bcost, C.ATTACK_BASE_EXTRA_COST,
             threat_at_base, C.ATTACK_BASE_THREAT_WEIGHT),
      cands = pr7.candidates,
    }
  else
    pc[7] = nil
  end

  -- Run cheap evaluators directly
  for _, idx in ipairs(FINALIZE_POOLS) do
    local evaluator = POOL_EVALUATORS[idx]
    if evaluator then
      pc[idx] = evaluator(state, world, info, tmx, tmy, boat, ammo)
    end
  end
end

-- =========================================================================
-- update_pool_cache — called every tick.
-- Tick 0 of cycle: build the eval queue (filter candidates).
-- Subsequent ticks: process 2 candidates from the queue.
-- =========================================================================
function M.update_pool_cache(state, world, info)
  -- Process candidates from the eval queue (2 per tick).
  -- The queue is built by init.lua after each replan decision,
  -- giving ~49 ticks to process before the next decision.
  M.step_eval_queue(state, world, info)
end

-- =========================================================================
-- fill_pool_cache — evaluate ALL pools at once (used for urgent replans
-- when goal=none and the rolling cache may be stale/empty).
-- =========================================================================
function M.fill_pool_cache(state, world, info)
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8
  local boat = info.inboat
  local ammo = (info.shells or 0) + (info.mines or 0)
  state.pool_cache = {}
  for i, evaluator in ipairs(POOL_EVALUATORS) do
    state.pool_cache[i] = evaluator(state, world, info, tmx, tmy, boat, ammo)
  end
end

-- =========================================================================
-- Strategic goal selection (called on decision ticks)
-- Returns a goal table or nil to defer to exploration.
--
-- Architecture:
--   1. Emergency overrides (critical flee, at-base refuel, commands)
--   2. Cost-based competition — reads pre-evaluated pool cache,
--      applies hysteresis, picks lowest cost winner.
-- =========================================================================
local function goal_selection(state, world, info)
  local tmx    = info.tankx >> 8
  local tmy    = info.tanky >> 8
  local boat   = info.inboat
  local ammo   = (info.shells or 0) + (info.mines or 0)
  local result = nil
  local desc   = nil

  -- Clear stranded LGM state when LGM is back in tank
  if info.man_status == C.LGM_INTANK then
    state.lgm_stranded = nil
    state.lgm_stranded_check_tick = nil
  end

  -- Helper: are we on a friendly base that can actually resupply us?
  -- Uses state.perc.base_supply (computed once per tick by perception.lua)
  -- instead of re-reading info.base directly.
  -- IMPORTANT: info.base reports the nearest friendly base within 7 tiles,
  -- but the engine only refuels when the tank is ON the base tile.  We must
  -- check that the tank map position matches the base map position.
  local at_resupply_base = false
  local bs = state.perc and state.perc.base_supply
  if bs and info.base then
    local on_base = (tmx == info.base.x and tmy == info.base.y)
    if on_base then
      local need_arm = (info.armour < C.TANK_FULL_ARMOUR) and bs.armour > 0
      local need_sh  = (info.shells < C.TANK_FULL_SHELLS) and bs.shells > 0
      if need_arm or need_sh then
        at_resupply_base = true
      end
    end
  end

  -- Dynamic flee threshold: when attacking a pill, account for escape cost.
  -- On slow terrain or far from a base the tank needs more armour buffer
  -- to survive the retreat.
  -- Only computed when in combat to avoid expensive per-base pathfinding every tick.
  local flee_threshold = C.ARMOUR_CRITICAL
  local in_combat = (state.goal and state.goal.kind == "attack_pill" and state.goal.substate == "engage")
                 or (state.goal and state.goal.kind == "pill_place"
                     and (state.goal.substate == "engage" or state.goal.substate == "finish"))
  if in_combat and info.armour <= C.ARMOUR_LOW then
    local best_esc = math.huge
    for _, b in pairs(world.bases) do
      if b.owner == "friendly" or b.owner == "neutral" then
        local ec = cpf.cost_to(tmx, tmy, b.mx, b.my, 0,
                                info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
        if ec < best_esc then best_esc = ec end
      end
    end
    local tank_tt = U.ttype(tmx, tmy)
    local slow_start = (tank_tt == C.T_SWAMP or tank_tt == C.T_RUBBLE
                        or tank_tt == C.T_CRATER or tank_tt == C.T_RIVER)
    if best_esc < math.huge then
      local extra = math.floor(best_esc / C.FLEE_ESCAPE_COST_DIVISOR)
      if slow_start then extra = extra + C.FLEE_SLOW_TERRAIN_BONUS end
      flee_threshold = math.min(C.ARMOUR_LOW, C.ARMOUR_CRITICAL + extra)
      if flee_threshold > C.ARMOUR_CRITICAL then
        log.reason("goal", {
          pick = "dynamic_flee", base_threshold = C.ARMOUR_CRITICAL,
          adjusted = flee_threshold, escape_cost = best_esc,
          slow_start = slow_start, extra = extra,
        })
      end
    end
  end

  local needs_resupply = (info.armour <= C.ARMOUR_LOW or info.shells <= C.SHELLS_LOW)
  local critical       = (info.armour <= flee_threshold)

  -- Held attack exception: don't abort an attack just because shells crossed
  -- the SHELLS_LOW watermark, if we have enough shells to finish the target
  -- and keep the reserve.
  if needs_resupply and not critical
     and info.armour > C.ARMOUR_LOW then
    local held = false
    if state.capture_objective then
      local p = world.pills[state.capture_objective.id]
      if p and p.health > 0 and info.shells > p.health + C.SHELL_RESERVE then
        print(string.format(
          TAG .. " GOAL: holding attack — can finish pill#%d (hp=%d) with %d shells (reserve %d)",
          state.capture_objective.id, p.health, info.shells, C.SHELL_RESERVE))
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "pill_place" then
      local p = W.pill_at(world, state.goal.mx, state.goal.my)
      if p and p.health > 0 and p.health <= 8 then
        print(string.format(
          TAG .. " GOAL: holding pill_place — target at (%d,%d) hp=%d, placed pill fighting",
          state.goal.mx, state.goal.my, p.health))
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "attack_pill" then
      local p = W.pill_at(world, state.goal.mx, state.goal.my)
      if p and p.health > 0 and info.shells > p.health + C.SHELL_RESERVE then
        print(string.format(
          TAG .. " GOAL: holding attack_pill — can finish pill at (%d,%d) (hp=%d) with %d shells (reserve %d)",
          state.goal.mx, state.goal.my, p.health, info.shells, C.SHELL_RESERVE))
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "attack_base" then
      local b = W.base_at(world, state.goal.mx, state.goal.my)
      -- Hold attack as long as base is still alive (health > 0) and we have
      -- shells to spare.  health=0 means the base is capturable — let the
      -- cost competition switch to capture_base.
      if b and b.owner == "hostile" and b.health > 0 and info.shells > C.SHELL_RESERVE then
        print(string.format(
          TAG .. " GOAL: holding attack_base at (%d,%d) with %d shells (reserve %d)",
          state.goal.mx, state.goal.my, info.shells, C.SHELL_RESERVE))
        held = true
      end
    end
    if held then needs_resupply = false end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Override 0: Rescue stranded LGM — highest priority
  -- (LGM_DEAD means parachuting/dead — unreachable, ignore it)
  -- ════════════════════════════════════════════════════════════════════
  if not result then
    local lgm_mx = info.man_x >> 8
    local lgm_my = info.man_y >> 8

    if info.man_status == C.LGM_MOVING then
      -- Alive but possibly stranded: check every 50 ticks
      local now = state.tick or 0
      if not state.lgm_stranded_check_tick
         or now - state.lgm_stranded_check_tick >= 50 then
        state.lgm_stranded_check_tick = now
        local ticks = cpf_lgm_travel_ticks(
          info.man_x, info.man_y, info.tankx, info.tanky,
          0, 0, 2000, 150)
        state.lgm_stranded = (ticks == -1)
      end

      if state.lgm_stranded and (lgm_mx > 0 or lgm_my > 0) then
        result = {
          kind = "rescue_lgm", mx = lgm_mx, my = lgm_my,
          wx = U.m2w(lgm_mx), wy = U.m2w(lgm_my),
        }
        desc = string.format("rescue_lgm@(%d,%d) [stranded]", lgm_mx, lgm_my)
        log.reason("goal", {
          pick = "rescue_lgm", why = "LGM stranded (cannot reach tank)",
          mx = lgm_mx, my = lgm_my,
        })
      end
    end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Override 1: Critical armour → flee immediately (not contestable)
  -- ════════════════════════════════════════════════════════════════════
  if critical then
    -- Block depleted bases
    if info.base then
      local base_useless = true
      if info.armour < C.TANK_FULL_ARMOUR and (info.base.armour or 0) > 0 then base_useless = false end
      if info.shells < C.TANK_FULL_SHELLS and (info.base.shells or 0) > 0 then base_useless = false end
      if base_useless then
        state.blocked[U.mkey(tmx, tmy)] = state.tick + 200
      end
    end
    local cur_mx, cur_my = nil, nil
    if state.goal.kind == "flee_to_base" or state.goal.kind == "refuel_at_base" then
      cur_mx = state.goal.mx; cur_my = state.goal.my
    end
    local base, bid, bdist, base_cands = nearest_resupply_base(world, tmx, tmy, boat, ammo, state, info,
                                                    C.FLEE_DANGER_WEIGHT, cur_mx, cur_my, C.FLEE_DANGER_REJECT)
    if base then
      result = {
        kind = "flee_to_base", mx = base.mx, my = base.my,
        wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid,
      }
      desc = string.format("flee_to_base#%.0f@(%.0f,%.0f)d=%.0f arm=%.0f",
             bid, base.mx, base.my, bdist, info.armour)
      log.reason("goal", {
        pick = "flee_to_base", why = "armour critical",
        arm = info.armour, sh = info.shells, critical = true,
        chosen_id = bid, chosen_score = bdist,
        candidates = base_cands,
      })
    end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Override 2: Already at resupply base → stay and refuel
  -- ════════════════════════════════════════════════════════════════════
  if not result and needs_resupply and at_resupply_base
     and (state.goal.kind == "none"
          or state.goal.kind == "refuel_at_base"
          or state.goal.kind == "flee_to_base"
          or (tmx == state.goal.mx and tmy == state.goal.my)) then
    -- Feature 5: angry pill at refuel base — don't stay if a nearby pill
    -- just went angry and is actively shooting.  Fall through to cost
    -- competition which will pick a safer base or a different goal.
    local angry_threat = false
    local pill_threats_o2 = state.perc and state.perc.pill_threats or {}
    for _, pt in ipairs(pill_threats_o2) do
      if pt.anger > C.ANGRY_REFUEL_THRESHOLD then
        angry_threat = true
        log.reason("goal", {
          pick = "angry_refuel_flee", why = "angry pill near base while refueling",
          pill_mx = pt.pill.mx, pill_my = pt.pill.my,
          pill_anger = pt.anger, threshold = C.ANGRY_REFUEL_THRESHOLD,
        })
        break
      end
    end
    if angry_threat then goto skip_override2 end
    result = { kind = "refuel_at_base", mx = tmx, my = tmy, wx = U.m2w(tmx), wy = U.m2w(tmy) }
    desc = string.format("refueling_at_base arm=%.0f sh=%.0f", info.armour, info.shells)

    -- Base shield: if a calm hostile pill is in range, build a wall to block fire.
    local shield_pill, shield_dist = nil, math.huge
    local pill_threats = state.perc and state.perc.pill_threats or {}
    for _, pt in ipairs(pill_threats) do
      if pt.anger < C.BASE_SHIELD_MAX_ANGER
         and pt.dist >= C.BASE_SHIELD_MIN_DIST
         and pt.dist < shield_dist then
        shield_pill = pt.pill
        shield_dist = pt.dist
      end
    end
    if shield_pill then
      local dx = shield_pill.mx - tmx
      local dy = shield_pill.my - tmy
      local wx, wy
      if math.abs(dx) >= math.abs(dy) then
        wx = tmx + (dx > 0 and 1 or -1)
        wy = tmy
      else
        wx = tmx
        wy = tmy + (dy > 0 and 1 or -1)
      end
      local wtt = U.ttype(wx, wy)
      if wtt ~= C.T_BUILDING and wtt ~= C.T_HALFBUILD
         and wtt ~= C.T_RIVER and wtt ~= C.T_DEEPSEA then
        result.base_shield = true
        result.shield_wall_mx = wx
        result.shield_wall_my = wy
        log.reason("goal", {
          pick = "base_shield", why = "calm pill in range while refueling",
          pill_mx = shield_pill.mx, pill_my = shield_pill.my,
          pill_anger = shield_pill.anger or 0, pill_dist = shield_dist,
          wall_mx = wx, wall_my = wy,
        })
      end
    end

    log.reason("goal", {
      pick = "refuel_here", why = "already at resupply base",
      arm = info.armour, sh = info.shells,
    })
  end
  ::skip_override2::

  -- ════════════════════════════════════════════════════════════════════
  -- Override 3: Capture objective (cp command)
  -- ════════════════════════════════════════════════════════════════════
  if not result and state.capture_objective then
    local co = state.capture_objective
    local p  = world.pills[co.id]
    if not p or p.owner == "friendly" then
      print(string.format(TAG .. " CAPTURE: pill #%d captured!", co.id))
      state.command_reply     = string.format(C.BRAIN_NAME .. ": pill #%d captured!", co.id)
      state.capture_objective = nil
    elseif p.health == 0 then
      if not co.kill_tick then
        co.kill_tick = state.tick
        print(string.format(TAG .. " CAPTURE: pill#%d killed at t=%d — holding %d ticks for shots to clear",
              co.id, co.kill_tick, C.POST_KILL_WAIT_TICKS))
      end
      local ticks_waited = state.tick - co.kill_tick
      if ticks_waited < C.POST_KILL_WAIT_TICKS then
        result = { kind = "none", mx = tmx, my = tmy,
                   wx = U.m2w(tmx), wy = U.m2w(tmy) }
        desc = string.format("cp#%d@(%d,%d) [killed, clearing shots %d/%d]",
               co.id, p.mx, p.my, ticks_waited, C.POST_KILL_WAIT_TICKS)
      else
        result = {
          kind = "capture_pill", mx = p.mx, my = p.my,
          wx = U.m2w(p.mx), wy = U.m2w(p.my), target_id = co.id,
        }
        desc = string.format("cp#%d@(%d,%d) [pickup]", co.id, p.mx, p.my)
      end
    else
      result, _ = resolve_attack_goal(p, co.id, world, info, state)
      desc = string.format("cp#%d@(%d,%d) [%s hp=%d own=%s]",
             co.id, p.mx, p.my, result.kind, p.health, p.owner)
    end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Override 4: Base capture objective (cb command)
  -- ════════════════════════════════════════════════════════════════════
  if not result and state.base_capture_objective then
    local bco = state.base_capture_objective

    if bco.all then
      local base, bid, bdist, bcands = nearest_where(world.bases, world, tmx, tmy,
        function(b) return b.owner ~= "friendly" end, boat, ammo, state, info)
      if not base then
        print(TAG .. " CAPTURE: all bases captured!")
        state.command_reply = C.BRAIN_NAME .. ": all bases captured!"
        state.base_capture_objective = nil
      else
        bco.id = bid
        bco.mx = base.mx; bco.my = base.my
        bco.wx = U.m2w(base.mx); bco.wy = U.m2w(base.my)
      end
    end

    if state.base_capture_objective then
      local b = world.bases[bco.id]
      if not b or b.owner == "friendly" then
        if bco.all then
          print(string.format(TAG .. " CAPTURE: base #%d captured, continuing cb:all", bco.id))
          bco.id = nil; bco.mx = 0; bco.my = 0
          state.pf.status = "idle"
          state.stuck_for = 0
        else
          print(string.format(TAG .. " CAPTURE: base #%d captured!", bco.id))
          state.command_reply = string.format(C.BRAIN_NAME .. ": base #%d captured!", bco.id)
          state.base_capture_objective = nil
        end
      elseif b.owner == "neutral" then
        result = {
          kind = "capture_base", mx = b.mx, my = b.my,
          wx = U.m2w(b.mx), wy = U.m2w(b.my), target_id = bco.id,
        }
        desc = string.format("cb#%d@(%d,%d) [neutral, drive over]", bco.id, b.mx, b.my)
        log.reason("goal", {
          pick = "capture_base", why = "cb command, neutral base",
          chosen_id = bco.id,
        })
      else
        result = {
          kind = "attack_base", mx = b.mx, my = b.my,
          wx = U.m2w(b.mx), wy = U.m2w(b.my), target_id = bco.id,
        }
        desc = string.format("cb#%d@(%d,%d) [hostile, attack then capture]", bco.id, b.mx, b.my)
        log.reason("goal", {
          pick = "attack_base", why = "cb command, hostile base",
          chosen_id = bco.id,
        })
      end
    end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Cost-based goal competition
  -- Pool results are pre-evaluated by update_pool_cache() one per tick
  -- in the ticks leading up to the decision.  Here we just assemble
  -- the cached results, apply hysteresis, and pick the winner.
  -- ════════════════════════════════════════════════════════════════════
  if not result then
    local pc = state.pool_cache or {}
    -- Build pool with copies so hysteresis doesn't mutate cached costs
    local phase_weights = C.PHASE_WEIGHTS[state.phase]
    local pool = {}
    local now = state.tick or 0
    for idx, entry in pairs(pc) do
      if entry then
        -- Skip entries whose destination is blocked (e.g. by goal lookahead)
        local gmx = entry.goal and entry.goal.mx
        local gmy = entry.goal and entry.goal.my
        if gmx and gmy and state.blocked then
          local bk = U.mkey(gmx, gmy)
          if state.blocked[bk] and now < state.blocked[bk] then
            goto continue_pool
          end
        end
        local cost = entry.cost
        -- Apply phase-dependent weight multiplier
        local pool_name = POOL_NAMES[idx]
        local pw = phase_weights and pool_name and phase_weights[pool_name]
        if pw and cost and cost > 0 then
          cost = cost * pw
        end
        pool[#pool + 1] = {
          cost = cost, goal = entry.goal, desc = entry.desc,
          cands = entry.cands, _pill = entry._pill, _pill_id = entry._pill_id,
          phase_weight = pw,
        }
        ::continue_pool::
      end
    end

    -- ── Apply hysteresis to discourage thrashing ──
    local cur_group = goal_group(state.goal.kind)
    local ticks_on_goal = (state.tick or 0) - (state.goal_set_tick or 0)
    local commitment = math.min(ticks_on_goal * C.GOAL_COMMITMENT_PER_TICK, C.GOAL_COMMITMENT_CAP)
    -- Wall-shield attacks are a big investment (trees, LGM time, positioning);
    -- add extra penalty to discourage abandoning them mid-attack.
    local ws_subs = { ws_prebuild=true, ws_prewait=true, ws_advance=true, ws_engage=true, ws_retreat=true, ws_rebuild=true }
    local cur_sub = state.goal and state.goal.substate
    if cur_sub and ws_subs[cur_sub] then
      commitment = commitment + C.WALL_SHIELD_COMMITMENT
    end
    for _, c in ipairs(pool) do
      local cg = goal_group(c.goal.kind)
      if cg ~= cur_group then
        c.cost = c.cost + C.GOAL_SWITCH_PENALTY + commitment
        c.hysteresis = "type"
      elseif c.goal.mx ~= state.goal.mx or c.goal.my ~= state.goal.my then
        c.cost = c.cost + C.GOAL_TARGET_SWITCH_PENALTY + commitment
        c.hysteresis = "target"
      end
    end

    -- ── Sort by cost, pick winner ──
    table.sort(pool, function(a, b) return a.cost < b.cost end)

    -- ── Forward simulation: adjust costs and reject lethal paths ──
    -- Skip wsim for attack_pill when we're already in an active attack substate —
    -- the sim uses a straight-line path and doesn't know about wall shields,
    -- standoff positions, etc. The commitment hysteresis handles these cases.
    local cur_attack_active = state.goal
      and (state.goal.kind == "attack_pill" or state.goal.kind == "bpc_pill"
           or state.goal.kind == "pill_place")
      and state.goal.substate ~= nil

    if C.WSIM_ENABLED then
      for _, c in ipairs(pool) do
        -- Only sim goals that travel through danger (skip refuel/explore)
        local sim_kinds = { capture_base=true, capture_pill=true,
                            attack_pill=true, attack_base=true,
                            place_pill_strategic=true }
        -- Don't sim our current attack target if we're mid-attack
        local skip = cur_attack_active
          and c.goal.kind == state.goal.kind
          and c.goal.mx == state.goal.mx and c.goal.my == state.goal.my

        if sim_kinds[c.goal.kind] and not skip then
          local attack_id = nil
          if c.goal.kind == "attack_pill" and c._pill_id then
            attack_id = c._pill_id
          end
          local extra, killed, sdesc = wsim_evaluate_goal(c.goal, world, info, attack_id)
          c.cost = c.cost + extra
          c.desc = c.desc .. sdesc
          if killed and C.WSIM_KILL_REJECT then
            c.cost = c.cost + 99999  -- effectively reject
            c.wsim_killed = true
          end
        end
      end
      -- Re-sort after sim adjustments
      table.sort(pool, function(a, b) return a.cost < b.cost end)
    end

    if #pool > 0 then
      local winner = pool[1]

      -- Log all competing candidates for debugging
      local pool_log = {}
      for i, c in ipairs(pool) do
        local d = c.desc
        if c.hysteresis then d = d .. " [" .. c.hysteresis .. "]" end
        if c.wsim_killed then d = d .. " [KILL]" end
        pool_log[i] = { desc = d, cost = c.cost, hysteresis = c.hysteresis,
                        winner = (i == 1), wsim_killed = c.wsim_killed,
                        phase_weight = c.phase_weight }
      end
      -- Persist for C-side debug viewer (BrainTest overlay)
      state.last_goal_pool = pool_log

      -- Include candidates from the winning pool entry so we can see
      -- why a particular base/pill was chosen over alternatives.
      local winner_cands = nil
      if winner.cands then
        winner_cands = {}
        for _, cd in ipairs(winner.cands) do
          winner_cands[#winner_cands + 1] = {
            id = cd.id, mx = cd.mx, my = cd.my, cost = cd.cost,
            own = cd.own, hp = cd.hp, stale = cd.stale,
            reject = cd.reject,
          }
        end
      end

      log.reason("goal", {
        pick = "cost_competition",
        winner = winner.desc,
        winner_cost = winner.cost,
        pool = pool_log,
        cands = winner_cands,
      })

      -- If attack pill won, resolve technique (standoff, wall-shield, etc.)
      if winner._pill then
        local resolved, technique = resolve_attack_goal(winner._pill, winner._pill_id, world, info, state)
        result = resolved
        desc = winner.desc .. " tech=" .. (technique or "?")
      else
        result = winner.goal
        desc = winner.desc
      end
    end
  end

  -- ── Log strategic goal changes ──
  -- Compare on stable key (kind+target) so wsim tick changes don't spam
  local goal_key = result and string.format("%s@%d,%d", result.kind, result.mx or 0, result.my or 0) or nil
  if goal_key ~= last_strategic_goal then
    if not quiet then
      if desc then
        print(TAG .. " GOAL: " .. desc)
      elseif last_strategic_goal then
        print(TAG .. " GOAL: strategic goals clear")
      end
    end
    last_strategic_goal = goal_key
  end

  -- Log why no strategic goal was found
  if not result then
    local skip_reasons = {}
    if not (needs_resupply or critical) then
      skip_reasons[#skip_reasons + 1] = string.format("no resupply needed (arm=%d sh=%d)", info.armour, info.shells)
    end
    if not state.capture_objective then
      skip_reasons[#skip_reasons + 1] = "no capture objective"
    end
    if info.shells <= C.SHELLS_LOW then
      skip_reasons[#skip_reasons + 1] = string.format("shells too low for attack (%d<=%d)", info.shells, C.SHELLS_LOW)
    end
    log.reason("goal", {
      pick = "none", why = "no strategic goal (pool empty)",
      skips = table.concat(skip_reasons, "; "),
    })
  end

  return result
end

-- Main goal picker: commands > strategic > exploration
function M.pick_goal(state, world, info)
  -- Command goal overrides everything
  if state.command_goal then
    local cg  = state.command_goal
    local tmx = info.tankx >> 8
    local tmy = info.tanky >> 8

    -- Arrival condition depends on goal kind
    local arrived = false
    if cg.kind == "attack_pill" then
      -- Done when pill is dead (health == 0) or no longer visible
      local p = world.pills[cg.id]
      arrived = (not p) or (p.health == 0)
    elseif cg.kind == "bpc_pill" then
      -- Done when pill is picked up (at pill tile and pill is dead)
      local p = world.pills[cg.id]
      local pdist = U.mdist(tmx, tmy, cg.mx, cg.my)
      arrived = (not p) or (p.health == 0 and pdist <= C.BPC_RUSH_ARRIVE)
    elseif cg.kind == "pill_place" then
      -- Done when pill is dead or friendly
      local p = world.pills[cg.id]
      arrived = (not p) or (p.owner == "friendly") or (p.health == 0)
    else
      arrived = U.mdist(tmx, tmy, cg.mx, cg.my) <= 1
    end

    if arrived then
      print(string.format(TAG .. " CMD: ARRIVED at %s #%d (%d,%d)",
            cg.kind, cg.id, cg.mx, cg.my))
      state.command_reply = string.format(C.BRAIN_NAME .. ": arrived at %s #%d (%d,%d)",
        cg.kind, cg.id, cg.mx, cg.my)
      state.command_goal = nil
      -- Fall through to exploration (if enabled)
    else
      local g = { kind = cg.kind, mx = cg.mx, my = cg.my, wx = cg.wx, wy = cg.wy }
      -- For pill attacks via command, plan the best approach angle
      if cg.kind == "attack_pill" then
        local pk   = U.mkey(cg.mx, cg.my)
        local pill = nil
        pill = attack.find_pill_at(world, cg.mx, cg.my)
        if pill then
          local smx, smy = attack.get_standoff(world, info, pk, pill, state)
          local plan = state.pill_attack_plan
          g.standoff_mx = smx; g.standoff_my = smy
          g.substate = smx and "approach" or "plan"
          g.wall_shield = plan and plan.wall_shield or false
          g.wall_mx = plan and plan.wall_mx or nil
          g.wall_my = plan and plan.wall_my or nil
          g.prebuild_mx = plan and plan.prebuild_mx or nil
          g.prebuild_my = plan and plan.prebuild_my or nil
        end
      elseif cg.kind == "pill_place" then
        -- Pill placement command: set substate if not already set
        if not g.substate then
          g.substate = "select_pill"
        end
      elseif cg.kind == "bpc_pill" then
        local pill = nil
        pill = attack.find_pill_at(world, cg.mx, cg.my)
        if pill then
          -- Use normal standoff planner (no wall-shield) at BPC_STANDOFF distance
          -- Pass orbit_radius so the planner scores terrain on the orbit arc
          local smx, smy = attack.pick_standoff(world, info, pill, state, C.BPC_STANDOFF, C.BPC_STANDOFF)
          g.standoff_mx = smx; g.standoff_my = smy
          g.substate = smx and "approach" or "approach"
        end
      end
      return g
    end
  end

  -- Strategic goal selection
  local strategic = goal_selection(state, world, info)
  if strategic then return strategic end

  -- If exploration is disabled, just idle
  if not state.auto_explore then
    return { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
  end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  -- Pop frontier entries that are too close
  local fx, fy = expl.best_frontier(state)
  while fx and U.mdist(tmx, tmy, fx, fy) < C.MIN_EXPLORE_DIST do
    local nk = U.mkey(fx, fy)
    state.visited[nk]      = true
    state.frontier_set[nk] = nil
    heap.pop(state.frontier)
    fx, fy = expl.best_frontier(state)
  end

  if fx then
    return { kind = "explore", mx = fx, my = fy,
             wx = U.m2w(fx), wy = U.m2w(fy) }
  end

  -- Frontier empty: scan nearby for any unvisited passable square
  local scan_radius = 10
  local best_dist, best_fx, best_fy = math.huge, nil, nil
  for dr = 1, scan_radius do
    for dy = -dr, dr do
      for dx = -dr, dr do
        if math.abs(dx) == dr or math.abs(dy) == dr then
          local cx, cy = tmx + dx, tmy + dy
          if U.in_map(cx, cy) then
            local ck = U.mkey(cx, cy)
            if not state.visited[ck] then
              local tt = U.ttype(cx, cy)
              local cost_table = info.inboat and C.TERRAIN_COST_BOAT
                                              or C.TERRAIN_COST_LAND
              local tc = cost_table[tt] or 9999
              if tc < 100 then
                local d = U.mdist(tmx, tmy, cx, cy)
                if d < best_dist then
                  best_dist = d; best_fx = cx; best_fy = cy
                end
              end
            end
          end
        end
      end
    end
    if best_fx then break end
  end

  if best_fx then
    local nk = U.mkey(best_fx, best_fy)
    if not state.frontier_set[nk] then
      state.frontier_set[nk] = true
      heap.push(state.frontier, {
        cost = best_dist, mx = best_fx, my = best_fy
      })
    end
    return { kind = "explore", mx = best_fx, my = best_fy,
             wx = U.m2w(best_fx), wy = U.m2w(best_fy) }
  end

  return { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
end

return M
