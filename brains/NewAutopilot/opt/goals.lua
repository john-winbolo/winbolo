-- =========================================================================
-- NewAutopilot/goals.lua — strategic goal selection + exploration fallback
-- =========================================================================

local C      = require("constants")
local opt    = require("optimize")
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
local print2 = require("print2")
local threat = require("threat")
local vizmod = require("viz")
local json   = require("json")
local ally_state = require("ally_state")
local _SELF_PN = -1   -- updated each tick by step_eval_queue / get_pool_breakdown_json

local M = {}

-- Local aliases for the smart_cost helper / kind constants in cpathfinder.
local smart_cost = cpf.smart_cost
local KIND_NORMAL = cpf.KIND_NORMAL
local KIND_PILL   = cpf.KIND_PILL

local last_strategic_goal = nil  -- track to avoid spamming logs
-- Technique selection: which attack method to use against hostile pills.
-- Priority: pill placement > wall-shield > hardline (bpc)
-- Returns "pill_place", "wall_shield", or "hardline"
-- TTK-vs-TTI intercept penalty: looks at every enemy tank, computes
-- their time-to-arrive at (target_mx, target_my) and compares to our
-- time-to-kill (= target_hp * TTK_TICKS_PER_HIT). If they can intercept
-- with safety margin to spare, returns the largest penalty across all
-- threats; otherwise 0. Centralises a loop that was duplicated in
-- attack_pill_adjustments and compute_pool*_cost — drift risk was high
-- since the same penalty curve was in two places.
local function intercept_penalty_ttk(target_mx, target_my, target_hp, enemy_tanks)
  if not enemy_tanks or #enemy_tanks == 0 then return 0 end
  local ttk = target_hp * C.TTK_TICKS_PER_HIT
  local worst = 0
  for _, et in ipairs(enemy_tanks) do
    local et_dist = U.mdist(et.mx, et.my, target_mx, target_my)
    if et_dist <= C.INTERCEPT_MAX_RANGE then
      local espeed = math.max(et.speed, 0.5)
      local tti = et_dist / espeed
      if ttk > tti * C.INTERCEPT_SAFETY_MARGIN then
        local ratio = ttk / math.max(1, tti)
        local pen = C.INTERCEPT_PENALTY * math.min(2.0, ratio)
        if pen > worst then worst = pen end
      end
    end
  end
  return worst
end

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
local function wsim_evaluate_goal(goal, world, info, attack_pill_idx, spot_mx, spot_my)
  if not C.WSIM_ENABLED then return 0, false, "" end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local gmx, gmy = goal.mx, goal.my
  if tmx == gmx and tmy == gmy then return 0, false, "" end

  -- Route to the ENGAGE point, not the target itself. The tank shoots
  -- from standoff/range, never drives onto the target tile.
  -- Always use KIND_NORMAL (full danger) for wsim — we want the safest
  -- route to evaluate survivability, not the aggressive low-danger path.
  local wsim_kind = cpf.KIND_NORMAL
  local wsim_dx, wsim_dy = gmx, gmy
  if attack_pill_idx then
    if spot_mx and spot_my then
      wsim_dx, wsim_dy = spot_mx, spot_my
    else
      local _, ax, ay = cpf.cheapest_adjacent_dij(cpf.KIND_NORMAL, gmx, gmy, 0)
      if ax then wsim_dx, wsim_dy = ax, ay end
    end
  elseif goal.kind == "attack_tank" or goal.kind == "attack_base" then
    -- Route to the standoff/adjacent tile, not the target itself
    local _, ax, ay = cpf.cheapest_adjacent_dij(cpf.KIND_NORMAL, gmx, gmy, 0)
    if ax then wsim_dx, wsim_dy = ax, ay end
  elseif goal.kind == "kill_lgm" then
    -- Route to the shoot_from position if available
    if goal.shoot_mx and goal.shoot_my then
      wsim_dx, wsim_dy = goal.shoot_mx, goal.shoot_my
    end
  end

  -- Try Dijkstra path first (matches actual navigation route).
  -- Fall back to straight line if Dijkstra hasn't reached the destination.
  local path = cpf.dijkstra_trace_path(wsim_kind, wsim_dx, wsim_dy)
  if path then
    -- Dijkstra path starts at the Dijkstra source, which may not be
    -- our exact current position. Trim leading waypoints we've already
    -- passed (tiles before or at our current position).
    local nwp = #path // 2
    local start_idx = 1
    for i = 1, nwp do
      if path[2*i-1] == tmx and path[2*i] == tmy then
        start_idx = i + 1
        break
      end
    end
    if start_idx > 1 and start_idx <= nwp then
      local trimmed = {}
      for i = start_idx, math.min(nwp, start_idx + 249) do
        trimmed[#trimmed + 1] = path[2*i-1]
        trimmed[#trimmed + 1] = path[2*i]
      end
      path = trimmed
    elseif nwp > 250 then
      local trimmed = {}
      for i = 1, 500 do trimmed[i] = path[i] end
      path = trimmed
    end
  end

  -- Fallback: straight line to adjusted destination
  if not path or #path == 0 then
    local dx = wsim_dx - tmx
    local dy = wsim_dy - tmy
    local steps = math.max(math.abs(dx), math.abs(dy))
    if steps == 0 then return 0, false, "" end
    if steps > 250 then steps = 250 end
    path = {}
    for i = 1, steps do
      local t = i / steps
      path[2*i-1] = math.floor(tmx + dx * t + 0.5)
      path[2*i]   = math.floor(tmy + dy * t + 0.5)
    end
  end

  -- Don't set attack_target — wsim evaluates travel survivability only,
  -- not prolonged combat. Setting it makes the bot shoot at the pill
  -- during travel, angering it and unrealistically accelerating fire rate.
  local _tw0 = clock_us()
  wsim.snapshot(world, info, path, nil)
  local _tw1 = clock_us()
  local r = wsim.run(C.WSIM_MAX_TICKS)
  local _tw2 = clock_us()
  if BRAIN_PROFILE_LOG and (_tw2 - _tw0) > 200 then
    opt.append("optimize.log", string.format(
      "  [wsim] goal=%s(%d,%d) snap=%.3fms run=%.3fms npath=%d",
      goal.kind, gmx, gmy, (_tw1-_tw0)/1000, (_tw2-_tw1)/1000, #path//2))
  end

  local extra_cost = r.damage * C.WSIM_DAMAGE_COST_WEIGHT
  local sim_desc = string.format(" wsim:%ddmg %.1fs arm=%d->%d",
    r.damage, r.ticks / 50.0, info.armour, r.armour)
  if r.killed then
    sim_desc = sim_desc .. " KILL"
  end
  -- Per-pill shot breakdown
  if r.pills then
    local shot_parts = {}
    for _, ps in ipairs(r.pills) do
      if ps.shots > 0 then
        shot_parts[#shot_parts + 1] = string.format("p#%d:%d", ps.id, ps.shots)
      end
    end
    if #shot_parts > 0 then
      sim_desc = sim_desc .. " [" .. table.concat(shot_parts, ",") .. "]"
    end
  end

  log.reason("wsim", {
    goal_kind = goal.kind, dest_mx = gmx, dest_my = gmy,
    damage = r.damage, ticks = r.ticks, killed = r.killed,
    armour_remaining = r.armour,
  })

  return extra_cost, r.killed, sim_desc, path, r
end

-- Helper: find the cheapest-to-reach object matching a filter.
-- Uses smart_cost (dijkstra fast-path with cost_to fallback).
-- `kind` selects the dijkstra slate (KIND_NORMAL or KIND_PILL).
-- Returns best, best_id, best_cost, candidates (array of all evaluated)
local function nearest_where(collection, world, tmx, tmy, filter, in_boat, ammo, state, info, kind, danger_scale_override)
  kind = kind or KIND_NORMAL

  local best_cost = math.huge
  local best_id, best = nil, nil
  local now = state and state.tick or 0
  local candidates = {}
  local boat_flag = in_boat and 1 or 0
  local shells = info and info.shells or 32
  local trees  = info and info.trees or 0
  local mines  = info and info.mines or 0
  local armour = info and info.armour or 40
  -- Per-call danger_scale override (affects smart_cost A* fallback; the
  -- dijkstra fast-path uses each slate's baked-in scale). Restore to 1.0 after.
  if danger_scale_override then cpf.set_config("danger_scale", danger_scale_override) end
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
        -- For live pills/bases with high overlay cost, use the cheapest
        -- adjacent tile instead of the object tile itself (can't drive
        -- onto a live pill; bases are expensive to stand on).
        local dx, dy = obj.mx, obj.my
        if obj.health and obj.health > 0 then
          local _, ax, ay = cpf.cheapest_adjacent(kind, tmx, tmy,
            obj.mx, obj.my, boat_flag, shells, trees, mines, armour)
          if ax then dx, dy = ax, ay end
        end
        local c = smart_cost(kind, tmx, tmy, dx, dy, boat_flag,
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
  -- Always restore even if no override was set — defends against any
  -- earlier code path leaving danger_scale in a non-default state.
  cpf.set_config("danger_scale", 1.0)
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
      -- Hard-skip bases that have zero of EVERYTHING we need, regardless
      -- of observation age. This is the "isn't empty" filter — even a
      -- stale "fully empty" observation is more reliable than driving
      -- across the map and finding an empty base.
      if b.obs_tick then
        local need_a = info.armour < state.armour_target
        local need_s = info.shells < state.shell_target
        local has_a  = (b.obs_armour or 0) > 0
        local has_s  = (b.obs_shells or 0) > 0
        local helps  = (need_a and has_a) or (need_s and has_s)
        if not helps then
          candidates[#candidates + 1] = {
            id = id, mx = b.mx, my = b.my, own = b.owner, score = -1,
            reject = string.format("empty (sh=%d arm=%d)",
                     b.obs_shells or 0, b.obs_armour or 0),
          }
          goto skip
        end
      end
      -- Skip bases with recently observed low stock (not worth the trip).
      -- Observation expires after REFUEL_OBS_STALE ticks (base regenerates).
      if b.obs_tick and now > 0 and (now - b.obs_tick) < C.REFUEL_OBS_STALE then
        local obs_low = true
        if info.armour < state.armour_target and (b.obs_armour or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
        if info.shells < state.shell_target and (b.obs_shells or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
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
        local travel = smart_cost(KIND_NORMAL, tmx, tmy, b.mx, b.my, in_boat and 1 or 0,
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
        -- Oscillation penalty: if this base has appeared recently in the
        -- goal history, penalize it exponentially to break flee cycles.
        -- Same formula as goal_selection's oscillation detection.
        local hist = state and state.goal_history or {}
        local hist_count = 0
        for _, h in ipairs(hist) do
          if (h.kind == "flee_to_base" or h.kind == "refuel_at_base")
             and h.mx == b.mx and h.my == b.my then
            hist_count = hist_count + 1
          end
        end
        if hist_count > 0 then
          local hist_pen = C.GOAL_HISTORY_TARGET_BASE * (C.GOAL_HISTORY_EXP ^ hist_count - 1)
          score = score + hist_pen
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
  attack_pill = "attack", pill_place = "attack",
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
  local pk = U.mkey(pill.mx, pill.my)
  local smx, smy = attack.get_standoff(world, info, pk, pill, state)
  return {
    kind = "attack_pill", mx = pill.mx, my = pill.my,
    wx = U.m2w(pill.mx), wy = U.m2w(pill.my),
    target_id = pid,
    standoff_mx = smx, standoff_my = smy,
    substate = "plan_position",
  }, "attack"
end

-- =========================================================================
-- Phase 2: strategic location multiplier.
-- Biases goal cost by territorial context using the influence map +
-- nearest-friendly-asset distance. Result is stored on the pool entry as
-- (loc_mult, loc_reason); goal_selection applies it under a clamp against
-- the phase_weight so compounds can't blow up.
-- =========================================================================
local function nearest_friendly_asset_dist(world, mx, my)
  local best = math.huge
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      local d = U.mdist(mx, my, b.mx, b.my)
      if d < best then best = d end
    end
  end
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health > 0 then
      local d = U.mdist(mx, my, p.mx, p.my)
      if d < best then best = d end
    end
  end
  return best
end

local function nearest_hostile_asset_dist(world, mx, my)
  local best = math.huge
  for _, p in pairs(world.pills) do
    if p.owner == "hostile" and p.health > 0 then
      local d = U.mdist(mx, my, p.mx, p.my)
      if d < best then best = d end
    end
  end
  for _, b in pairs(world.bases) do
    if b.owner == "hostile" then
      local d = U.mdist(mx, my, b.mx, b.my)
      if d < best then best = d end
    end
  end
  return best
end

local function strategic_location_mult(mx, my, state, world, info, goal_kind, target_pill)
  local inf = cpf.influence_at(mx, my) or 0
  local support_dist = nearest_friendly_asset_dist(world, mx, my)
  local enemy_dist = nearest_hostile_asset_dist(world, mx, my)
  if support_dist == math.huge then support_dist = 999 end
  if enemy_dist == math.huge then enemy_dist = 999 end

  if inf >= C.STRATEGIC_CONSOLIDATE_INF
     and support_dist <= C.STRATEGIC_SUPPORT_RADIUS then
    -- A nominally consolidated tile may still host a hostile pill polluting
    -- our backyard. Scan within HOME_SWEEP_RADIUS for live hostile pills;
    -- early-exit on the first hit (squared distance — no sqrt).
    local r2 = C.STRATEGIC_HOME_SWEEP_RADIUS * C.STRATEGIC_HOME_SWEEP_RADIUS
    local has_hostile = false
    for _, p in pairs(world.pills) do
      if p.owner == "hostile" and p.health > 0 then
        local dx = p.mx - mx
        local dy = p.my - my
        if dx * dx + dy * dy <= r2 then
          has_hostile = true
          break
        end
      end
    end
    if has_hostile then
      if (goal_kind == "capture_pill" or goal_kind == "attack_pill")
         and target_pill
         and (target_pill.owner == "hostile" or target_pill.owner == "neutral") then
        return C.STRATEGIC_HOME_SWEEP_MULT,
          string.format("home_sweep inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
      end
      return 1.0,
        string.format("contested_home inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
    end
    return C.STRATEGIC_CONSOLIDATE_MULT,
      string.format("consolidate inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
  end

  if math.abs(inf) < C.STRATEGIC_FRONTIER_INF
     and support_dist <= C.STRATEGIC_SUPPORT_RADIUS * 1.5 then
    if C.STRATEGIC_USE_FRONT_DIR and state.front_dir_x and state.front_dir_y
       and state.front_center_mx and state.front_center_my then
      local dx = mx - state.front_center_mx
      local dy = my - state.front_center_my
      local len = math.sqrt(dx * dx + dy * dy)
      if len > 0.1 then
        local dot = (dx / len) * state.front_dir_x + (dy / len) * state.front_dir_y
        if dot > 0.3 then
          return C.STRATEGIC_FRONTIER_MULT,
            string.format("frontier+dir inf=%.0f ally=%.1f enemy=%.1f dot=%.2f", inf, support_dist, enemy_dist, dot)
        end
      end
    end
    return C.STRATEGIC_FRONTIER_MULT * 1.1,
      string.format("frontier inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
  end

  if inf <= -C.STRATEGIC_DEEP_HOSTILE
     and support_dist > C.STRATEGIC_SUPPORT_RADIUS * 2 then
    return C.STRATEGIC_DEEP_ISOLATED_MULT,
      string.format("deep_hostile inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
  end
  if inf <= -C.STRATEGIC_DEEP_HOSTILE * 0.5
     and support_dist > C.STRATEGIC_SUPPORT_RADIUS then
    return C.STRATEGIC_ISOLATED_MULT,
      string.format("isolated inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
  end

  return 1.0, string.format("neutral inf=%.0f ally=%.1f enemy=%.1f", inf, support_dist, enemy_dist)
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
  -- Urgency = (deficit/threshold)^2 so low resources discount more
  -- aggressively. Linear gave armour=10/15 → 0.67 (33% cheaper);
  -- squared gives 0.44 at the same point (~56% cheaper). Curve
  -- still ends at 1.0 when armour >= ARMOUR_LOW and shells >= SHELLS_LOW.
  local arm_u = math.min(1.0, info.armour / C.ARMOUR_LOW)
  local sh_u  = math.min(1.0, info.shells / C.SHELLS_LOW)
  arm_u = arm_u * arm_u
  sh_u  = sh_u  * sh_u
  local urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(arm_u, sh_u))
  local cost = bscore * urgency
  -- Surface the hysteresis state in the desc so the user can see when
  -- the current refuel target's score is being discounted to keep us
  -- committed to it. Find the chosen candidate's hyst flag.
  local hyst_str = ""
  if BRAIN_POOL_VIZ and cur_mx and base.mx == cur_mx and base.my == cur_my then
    hyst_str = string.format(" hyst{-%d}", C.REFUEL_SWITCH_THRESHOLD)
  end
  return {
    cost = cost,
    goal = { kind = "refuel_at_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
    desc = BRAIN_POOL_VIZ and string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d sh=%d%s",
           bid, base.mx, base.my, bscore, urgency, cost, info.armour, info.shells, hyst_str) or "",
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
    boat, ammo, state, info, KIND_NORMAL, C.CAPTURE_THREAT_WEIGHT)
  if not base then return nil end
  local lm, lr = strategic_location_mult(base.mx, base.my, state, world, info, "capture_base", nil)

  local raw_cost = bcost
  local imminent = false
  if base.health == 0
     and raw_cost <= C.IMMINENT_CAPTURE_PATH_COST
     and info.armour >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
    raw_cost = math.min(raw_cost, C.IMMINENT_CAPTURE_FLOOR)
    imminent = true
  end
  -- Urgent capture: we just killed this base via attack_base. Heavily
  -- discount the cost so the bot commits to capturing before the base
  -- recharges for the enemy. Decays after 500 ticks (~10s).
  local urgent = state.urgent_capture_base
  if urgent and urgent.mx == base.mx and urgent.my == base.my
     and (state.tick or 0) - urgent.tick < 500 then
    raw_cost = math.min(raw_cost, C.IMMINENT_CAPTURE_FLOOR)
    imminent = true
  end

  local desc = ""  -- pool viz string; populated only when BRAIN_POOL_VIZ
  if BRAIN_POOL_VIZ then
    desc = string.format("capture_base#%d@(%d,%d) cost=%.0f", bid, base.mx, base.my, raw_cost)
    if imminent then desc = desc .. " IMMINENT" end
  end
  -- TODO: Phase 6 race-loss should clear race_mode on captures we've decided not to win.
  -- race_mode is currently a single bool. The "or imminent" branch
  -- is dead because CAPTURE_RACE_MODE_CAPTURE is always true; left
  -- here as a single assignment from the global constant. If we ever
  -- want to distinguish "always race" from "only race when imminent",
  -- this needs to become two flags or a string.
  local race_mode = C.CAPTURE_RACE_MODE_CAPTURE
  return {
    cost = raw_cost,
    loc_mult = lm, loc_reason = BRAIN_POOL_VIZ and lr or nil,
    imminent = imminent,
    goal = { kind = "capture_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid,
             race_mode = race_mode },
    desc = desc,
    cands = bcands,
  }
end

local function eval_capture_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_dead = not state.perc or (state.perc.dead_neutral_pill_count > 0)
  if not has_dead then return nil end
  -- Don't try to capture a pill that's already in someone's tank — even
  -- if it's still on the world.pills list, in_tank means it's been
  -- picked up and the (mx,my) is stale.
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p) return (p.owner == "neutral" or p.owner == "friendly")
                       and p.health == 0 and not p.in_tank end,
    boat, ammo, state, info, KIND_NORMAL, C.CAPTURE_THREAT_WEIGHT)
  if not pill then return nil end
  local lm, lr = strategic_location_mult(pill.mx, pill.my, state, world, info, "capture_pill", pill)

  local raw_cost = pcost
  local imminent = false
  if pill.health == 0
     and raw_cost <= C.IMMINENT_CAPTURE_PATH_COST
     and info.armour >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
    raw_cost = math.min(raw_cost, C.IMMINENT_CAPTURE_FLOOR)
    imminent = true
  end

  local desc = ""  -- pool viz string; populated only when BRAIN_POOL_VIZ
  if BRAIN_POOL_VIZ then
    desc = string.format("capture_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, raw_cost)
    if imminent then desc = desc .. " IMMINENT" end
  end
  -- TODO: Phase 6 race-loss should clear race_mode on captures we've decided not to win.
  -- race_mode is currently a single bool. The "or imminent" branch
  -- is dead because CAPTURE_RACE_MODE_CAPTURE is always true; left
  -- here as a single assignment from the global constant. If we ever
  -- want to distinguish "always race" from "only race when imminent",
  -- this needs to become two flags or a string.
  local race_mode = C.CAPTURE_RACE_MODE_CAPTURE
  return {
    cost = raw_cost,
    loc_mult = lm, loc_reason = BRAIN_POOL_VIZ and lr or nil,
    imminent = imminent,
    goal = { kind = "capture_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid,
             race_mode = race_mode },
    desc = desc,
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
    end, boat, ammo, state, info, KIND_NORMAL)
  if not pill then return nil end
  local damage = C.PILLS_MAX_HEALTH - pill.health
  local adj_cost = math.max(0, pcost - damage * C.REPAIR_DAMAGE_BONUS)
  return {
    cost = adj_cost,
    goal = { kind = "repair_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
    desc = BRAIN_POOL_VIZ and string.format("repair_pill#%d@(%d,%d) cost=%.0f (path=%.0f -dam=%d×%d)",
           pid, pill.mx, pill.my, adj_cost, pcost, damage, C.REPAIR_DAMAGE_BONUS) or "",
    cands = pcands,
  }
end

-- Compute attack_pill cost adjustments for a given pill/path-cost.
-- Returns adjusted_cost, description_suffix.
local function attack_pill_adjustments(pill, pcost, state, world)
  -- Separate path cost from combat cost. Only the combat portion gets
  -- scaled by health-based multipliers (wounded, eLGMdead, bkiller).
  -- Path cost stays fixed — a nearly-dead pill across the map shouldn't
  -- have its travel cost slashed.
  local combat_cost = pill.health * C.PILL_HEALTH_WEIGHT
  local antic_desc = BRAIN_POOL_VIZ and "" or nil

  -- Pill anger cooldown
  local pill_anger = pill.anger or 0
  if pill_anger > C.ANGER_ATTACK_THRESHOLD then
    local ticks_to_calm = (pill_anger - C.ANGER_ATTACK_THRESHOLD) * C.PILL_ANGER_DECAY
    if ticks_to_calm < C.ANGER_WAIT_MAX then
      local anger_cost = ticks_to_calm * C.ANGER_COST_PER_TICK
      combat_cost = combat_cost + anger_cost
      if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" +anger=%.0f", anger_cost) end
    end
  end

  -- Enemy tank intercept (TTK vs TTI)
  local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
  local worst_intercept = intercept_penalty_ttk(pill.mx, pill.my, pill.health, enemy_tanks)
  if worst_intercept > 0 then
    combat_cost = combat_cost + worst_intercept
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" +intercept=%.0f", worst_intercept) end
  end

  -- Crossfire penalty
  local crossfire_pen = 0
  for _, pm in pairs(world.pills) do
    if (pm.mx ~= pill.mx or pm.my ~= pill.my)
       and (pm.owner == "hostile" or pm.owner == "neutral")
       and pm.health > 0 then
      local d = U.mdist(pill.mx, pill.my, pm.mx, pm.my)
      if d <= C.PILL_FIRE_RANGE + C.ATTACK_PILL_STANDOFF then
        local max_d = C.PILL_FIRE_RANGE + C.ATTACK_PILL_STANDOFF
        local proximity = 1.0 - d / (max_d + 1)
        crossfire_pen = crossfire_pen + C.GOAL_CROSSFIRE_PENALTY * proximity
      end
    end
  end
  if crossfire_pen > 0 then
    combat_cost = combat_cost + crossfire_pen
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" +xfire=%.0f", crossfire_pen) end
  end

  -- Enemy LGM dead: pill can't be repaired, attack is more valuable
  if pill.owner == "hostile" and state.perc and state.perc.enemy_lgm_dead then
    combat_cost = combat_cost * C.ENEMY_LGM_DEAD_ATTACK_DISCOUNT
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. " *eLGMdead" end
  end

  -- Wounded pill: we already damaged it, finish the job. 0.3x is the
  -- in-pool discount (vs sibling pills). The cross-goal commit
  -- discount layered on top scales by the same time_factor as the
  -- finish_other penalty so all three wounded-pill effects expire
  -- together.
  if state.wounded_pill and state.wounded_pill.mx == pill.mx and state.wounded_pill.my == pill.my then
    combat_cost = combat_cost * 0.3
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" *wounded(hp=%d)", state.wounded_pill.hp) end
    local wp = state.wounded_pill
    local age         = (state.tick or 0) - (wp.tick or 0)
    local time_factor = math.max(0, 1.0 - age / (C.WOUNDED_FINISH_DECAY_TICKS or 500))
    if time_factor > 0 then
      local commit = 1.0 - (1.0 - (C.WOUNDED_COMMIT_DISCOUNT or 0.5)) * time_factor
      combat_cost = combat_cost * commit
      if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" *commit(x%.2f)", commit) end
    end
  end

  -- "Finish what you started" penalty: every OTHER pill take gets
  -- more expensive while we have an in-progress wounded pill at low
  -- HP. Encourages the bot to come back and close out a kill instead
  -- of starting a fresh take elsewhere. Scales with (1 - hp/thresh)
  -- so a 1-HP wounded pill gets the full penalty, a 10-HP one gets
  -- none. Decays linearly to 0 over WOUNDED_FINISH_DECAY_TICKS so it
  -- doesn't trap the bot if it can't actually get back to that pill.
  -- Self-defense (attack_tank) is in a different evaluator — this
  -- never bumps an enemy-tank response off the top.
  if state.wounded_pill
     and (state.wounded_pill.mx ~= pill.mx or state.wounded_pill.my ~= pill.my) then
    local wp = state.wounded_pill
    local wp_now = wp.id and world.pills and world.pills[wp.id] or nil
    local wp_hp  = wp_now and wp_now.health or wp.hp or 0
    local thresh = C.WOUNDED_FINISH_THRESHOLD or 10
    if wp_hp > 0 and wp_hp <= thresh then
      local hp_factor   = (thresh - wp_hp) / thresh        -- 0..1, low HP = stronger
      local age         = (state.tick or 0) - (wp.tick or 0)
      local time_factor = math.max(0, 1.0 - age / (C.WOUNDED_FINISH_DECAY_TICKS or 500))
      local peak_mult   = C.WOUNDED_FINISH_OTHER_PENALTY or 3.0
      local mult        = 1.0 + (peak_mult - 1.0) * hp_factor * time_factor
      if mult > 1.001 then
        combat_cost = combat_cost * mult
        if BRAIN_POOL_VIZ then
          antic_desc  = antic_desc .. string.format(
            " *finish_other(x%.2f hp=%d age=%d)", mult, wp_hp, age)
        end
      end
    end
  end

  -- Base Killer Mode: deprioritize pill attacks in favor of bases
  if state.perc and state.perc.base_killer_mode then
    combat_cost = combat_cost * C.BASE_KILLER_PILL_PENALTY
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. " *bkiller" end
  end

  -- Final cost = fixed path cost + scaled combat cost
  return pcost + combat_cost, antic_desc
end

local function eval_attack_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_hostile = not state.perc or (state.perc.attackable_pill_count > 0)
  if not (has_hostile and info.shells > C.SHELLS_LOW) then return nil end
  -- Lower armour threshold for wounded pills (few shots needed)
  local min_armour = C.ATTACK_PILL_MIN_ARMOUR
  if state.wounded_pill then min_armour = 15 end
  if info.armour < min_armour then return nil end
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p) return (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 end,
    boat, ammo, state, info, KIND_NORMAL)
  if not pill then return nil end
  local shells_on_arrival = cpf.dijkstra_shells_at(KIND_NORMAL, pill.mx, pill.my)
                         or cpf.astar_shells_at(pill.mx, pill.my)
  local adj_cost, antic_desc = attack_pill_adjustments(pill, pcost, state, world)
  -- Wounded-tank discouragement: starting a take on a healthy pill
  -- (HP >= 12, ~4+ shots needed) with our own armour at/below
  -- ARMOUR_LOW means we'll likely die to return fire before finishing.
  -- Flat +200 surcharge nudges the bot toward refuel first without
  -- hard-blocking the take.
  if pill.health >= 12 and info.armour <= (C.ARMOUR_LOW or 15) then
    adj_cost = adj_cost + 200
    if BRAIN_POOL_VIZ and antic_desc then antic_desc = antic_desc .. " +loArm200" end
  end
  local lm, lr = strategic_location_mult(pill.mx, pill.my, state, world, info, "attack_pill", pill)

  return {
    cost = adj_cost,
    loc_mult = lm, loc_reason = BRAIN_POOL_VIZ and lr or nil,
    _pill = pill, _pill_id = pid,
    _shells_on_arrival = shells_on_arrival,
    goal = { kind = "attack_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my) },
    desc = BRAIN_POOL_VIZ and string.format("attack_pill#%d@(%d,%d) cost=%.0f (path=%.0f +hp=%d×%d%s)",
           pid, pill.mx, pill.my, adj_cost, pcost, pill.health, C.PILL_HEALTH_WEIGHT, antic_desc or "") or "",
    cands = pcands,
  }
end

local function eval_attack_base(state, world, info, tmx, tmy, boat, ammo)
  local has_hbases = not state.perc or (state.perc.hostile_base_count > 0)
  if not (has_hbases and info.shells > C.SHELLS_LOW) then return nil end
  -- Only attack hostile bases that are still alive (health > 0).
  -- health=0 means capturable — eval_capture_base handles those.
  local base, bid, bcost, bcands = nearest_where(world.bases, world, tmx, tmy,
    function(b) return b.owner == "hostile" and b.health > 0 end, boat, ammo, state, info, KIND_NORMAL)
  if not base then return nil end
  local shells_on_arrival = cpf.dijkstra_shells_at(KIND_NORMAL, base.mx, base.my)
                         or cpf.astar_shells_at(base.mx, base.my)
  -- Penalise bases covered by enemy pills/tanks (like aIndy's cover penalty).
  local threat_at_base = threat.at(base.mx, base.my)
  local adj_cost = bcost + C.ATTACK_BASE_EXTRA_COST
                 + threat_at_base * C.ATTACK_BASE_THREAT_WEIGHT
  -- Base Killer Mode: heavily discount base attacks when we have numbers advantage
  if state.perc and state.perc.base_killer_mode then
    adj_cost = adj_cost * C.BASE_KILLER_ATTACK_DISCOUNT
  end
  local lm, lr = strategic_location_mult(base.mx, base.my, state, world, info, "attack_base", nil)
  return {
    cost = adj_cost,
    loc_mult = lm, loc_reason = BRAIN_POOL_VIZ and lr or nil,
    _shells_on_arrival = shells_on_arrival,
    goal = { kind = "attack_base", mx = base.mx, my = base.my,
             wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
    desc = BRAIN_POOL_VIZ and string.format("attack_base#%d@(%d,%d) cost=%.0f (path=%.0f +base=%d +threat=%.0f×%d)",
           bid, base.mx, base.my, adj_cost, bcost, C.ATTACK_BASE_EXTRA_COST,
           threat_at_base, C.ATTACK_BASE_THREAT_WEIGHT) or "",
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

-- Find nearest hostile pill to a map position
local function nearest_hostile_pill_pos(world, mx, my)
  local best_d = math.huge
  local best_mx, best_my = nil, nil
  for _, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 then
      local d = U.mdist(mx, my, p.mx, p.my)
      if d < best_d then best_d = d; best_mx = p.mx; best_my = p.my end
    end
  end
  return best_mx, best_my, best_d
end

-- Detect pill war zone: area where both sides have pills within range of each other
local function detect_pill_war_zone(world)
  for _, fp in pairs(world.pills) do
    if fp.owner == "friendly" and fp.health > 0 then
      for _, hp in pairs(world.pills) do
        if (hp.owner == "hostile" or hp.owner == "neutral") and hp.health > 0 then
          if U.mdist(fp.mx, fp.my, hp.mx, hp.my) <= C.PILL_FIRE_RANGE * 2 then
            -- Midpoint of the two pills
            local mx = math.floor((fp.mx + hp.mx) / 2 + 0.5)
            local my = math.floor((fp.my + hp.my) / 2 + 0.5)
            return mx, my, fp, hp
          end
        end
      end
    end
  end
  return nil, nil, nil, nil
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

-- Substates during which a fresh attack_pill goal selection should
-- LOCK ONTO the current pill instead of re-picking from the pool —
-- protects in-progress takes from being yanked off-target. Hoisted
-- above eval_attack_tank so the engage-break logic there can read it.
local LOCK_SUBS = {
  gather_trees=true, approach=true, build_walls=true,
  aim=true, detree=true, charge=true, engage=true, rush=true,
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true,
  ws_prebuild=true, ws_prewait=true, ws_advance=true,
  ws_engage=true, ws_retreat=true, ws_rebuild=true,
  swerve=true, post_engage=true, loiter=true,
}

local function eval_attack_tank(state, world, info, tmx, tmy, boat, ammo)
  state.attack_tank_breakdown = nil

  -- Check gates but don't return early — always populate breakdown so
  -- every visible tank shows in the pool window with its rejection reason.
  -- NOTE: the low_shells gate is evaluated per-candidate below, not globally,
  -- because a target in deep sea is a boat (1 shot to sink) and we only need
  -- 1 shell to engage it. Other gates (DISABLED, low_armour, in_boat) still
  -- apply globally.
  local gate_reason = nil
  if not C.TANK_COMBAT_ENABLED then gate_reason = "DISABLED"
  elseif info.armour < C.TANK_COMBAT_MIN_ARMOUR then
    gate_reason = string.format("low_armour(%d<%d)", info.armour, C.TANK_COMBAT_MIN_ARMOUR)
  elseif info.inboat then gate_reason = "in_boat"
  end
  local low_shells_global = info.shells < C.TANK_COMBAT_MIN_SHELLS
  if gate_reason and BRAIN_DEBUG_MODE then print2("eval_attack_tank: " .. gate_reason) end

  local perc = state.perc
  local enemy_tanks = (perc and perc.enemy_tanks) or {}
  if #enemy_tanks == 0 then
    -- no return here: the pool panel still shows not_visible player rows.
    if false then
    return nil  -- no tanks visible at all — nothing to show
  end

  end

  local best_cost = math.huge
  local best_tank = nil
  -- Per-candidate breakdown for the pool window: every tank we
  -- considered with the sub-costs that make up its score.
  local breakdown = {}
  local visible_enemy_ids = {}

  for _, et in ipairs(enemy_tanks) do
    if et.id ~= nil then visible_enemy_ids[et.id] = true end
    -- Target in deep sea = enemy is in a boat = 1 shot to sink. Skip the
    -- low_shells gate so we can always harass a boat even with 1 shell.
    local target_tt = U.ttype(et.mx, et.my)
    local boat_sink = (target_tt == C.T_DEEPSEA) and info.shells >= 1
    -- Per-candidate low_shells gate: bypassed for boat_sink targets.
    local local_gate = gate_reason
    if not local_gate and low_shells_global and not boat_sink then
      local_gate = string.format("low_shells(%d<%d)", info.shells, C.TANK_COMBAT_MIN_SHELLS)
    end
    if local_gate then
      breakdown[#breakdown + 1] = {
        id = et.id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
        path_cost = 0, base = 0, aim_bonus = 0, aim_diff = 0,
        crossfire = 0, wall_penalty = 0, low_shells_penalty = 0,
        tank_shells = info.shells,
        cost = 1e30, shells_on_arrival = 0, skipped = local_gate,
      }
      goto continue_tanks
    end
    do
      -- LOS fast-engage: if enemy is within shoot range + LOS_EXTRA_RANGE and
      -- there are no walls between us, skip the expensive path evaluation and
      -- assign a very cheap cost so this fires over almost everything else.
      local los_range = C.TANK_COMBAT_ENGAGE_RANGE + C.TANK_COMBAT_LOS_EXTRA_RANGE
      local los_engage = et.dist <= los_range
                     and PF.wall_hp_between(tmx, tmy, et.mx, et.my) == 0

      local path_cost, shells_on_arrival
      local aim_bonus, aim_diff, crossfire, wall_penalty = 0, 0, 0, 0
      local cost

      -- Wall obstruction: count HP of blocks on the direct line.
      -- 0-1 full block (wall_hp <= WALL_HP_FULL=5): no penalty.
      -- Each HP above that costs TANK_COMBAT_WALL_PENALTY_PER_HP (20).
      -- 2 blocks → +100, 5 blocks → +400, discouraging wasted-ammo fights.
      local wall_hp = PF.wall_hp_between(tmx, tmy, et.mx, et.my)
      if wall_hp > C.WALL_HP_FULL then
        wall_penalty = (wall_hp - C.WALL_HP_FULL) * C.TANK_COMBAT_WALL_PENALTY_PER_HP
      end

      -- Low-shells penalty: discourage starting fights when ammo is low.
      -- Scales linearly: 0 at threshold, ~30 at 0 shells.
      local low_shells_penalty = 0
      if info.shells < C.TANK_COMBAT_LOW_SHELLS_THRESHOLD then
        low_shells_penalty = (C.TANK_COMBAT_LOW_SHELLS_THRESHOLD - info.shells)
                           * C.TANK_COMBAT_LOW_SHELLS_COST_PER
      end

      -- Boat vulnerability: enemy on water is an easy target.
      -- Deep sea = one-shot kill (very attractive), river = exposed on boat.
      local boat_mult = 1.0
      local et_terrain = U.ttype(et.mx, et.my)
      if et_terrain == C.T_DEEPSEA then
        boat_mult = C.TANK_COMBAT_DEEPSEA_MULT
      elseif et_terrain == C.T_RIVER or et_terrain == C.T_BOAT then
        boat_mult = C.TANK_COMBAT_BOAT_MULT
      end

      if los_engage then
        -- Clear LOS in range: very cheap cost, scaled by distance so closer = better.
        path_cost = 0  -- no wall-clearing needed
        shells_on_arrival = info.shells  -- full shells available (no walls to shoot)
        cost = (C.TANK_COMBAT_LOS_BASE_COST + et.dist * C.TANK_COMBAT_LOS_COST_PER_TILE
               + low_shells_penalty) * boat_mult
        -- wall_penalty is always 0 for los_engage (wall_hp_between == 0 is gated above)
      else
        -- Standoff evaluation: find the best engagement position at shooting
        -- range around the enemy tank (8 positions at 45° intervals).
        -- A* routes to the standoff, not the enemy's exact tile.
        local so_mx, so_my, so_score, so_path, so_shells, so_spots, so_deg =
            attack.evaluate_tank_standoff(et, tmx, tmy, info, world, state)

        if not so_mx then
          -- No valid standoff position (enemy surrounded by water/walls)
          breakdown[#breakdown + 1] = {
            id = et.id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
            path_cost = 0, base = C.TANK_COMBAT_BASE_COST,
            aim_bonus = 0, aim_diff = 0, crossfire = 0, wall_penalty = wall_penalty,
            low_shells_penalty = low_shells_penalty,
            tank_shells = info.shells,
            cost = 1e30, shells_on_arrival = 0, skipped = "no_standoff",
          }
          goto continue_tanks
        end

        path_cost = so_path
        shells_on_arrival = so_shells
        if shells_on_arrival and shells_on_arrival < C.TANK_COMBAT_MIN_SHELLS then
          -- Won't have enough shells left after clearing walls to fight effectively.
          breakdown[#breakdown + 1] = {
            id = et.id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
            path_cost = path_cost, base = C.TANK_COMBAT_BASE_COST,
            aim_bonus = 0, aim_diff = 0, crossfire = 0, wall_penalty = wall_penalty,
            low_shells_penalty = low_shells_penalty,
            tank_shells = info.shells,
            cost = 1e30, shells_on_arrival = shells_on_arrival, skipped = "low_shells",
          }
          goto continue_tanks
        end

        -- Standoff score is used internally to pick the best position —
        -- it doesn't inflate the final cost. The A* path_cost to the
        -- winning standoff already reflects the real travel expense.
        cost = path_cost + C.TANK_COMBAT_BASE_COST + wall_penalty + low_shells_penalty

        -- Aim bonus: if we're already pointed roughly at this tank, cheaper to engage.
        -- Capped at 10 if out of shooting range, 25 if in range.
        local aim_dir = U.aim_at(info.tankx, info.tanky, U.m2w(et.mx), U.m2w(et.my))
        aim_diff = math.abs(U.adiff(info.direction, aim_dir))
        if aim_diff < C.TANK_COMBAT_AIM_THRESHOLD then
          local aim_cap = et.dist <= C.TANK_COMBAT_ENGAGE_RANGE and 25 or 10
          aim_bonus = math.min(C.TANK_COMBAT_AIM_BONUS, aim_cap)
          cost = cost - aim_bonus
        end

        -- Crossfire penalty: hostile pills near the enemy tank. Accumulates
        -- across all pills in range and scales by anger — heated pills
        -- fire instantly and are far deadlier than calm ones.
        for _, pt in ipairs(perc.pill_threats or {}) do
          local pd = U.mdist(et.mx, et.my, pt.pill.mx, pt.pill.my)
          if pd <= C.TANK_COMBAT_NEAR_PILL_RANGE then
            local anger_scale = math.max(0.5, (pt.anger or 0) + 0.5)
            crossfire = crossfire + C.TANK_COMBAT_NEAR_PILL_PENALTY * anger_scale
          end
        end
        cost = cost + crossfire

        -- Apply boat vulnerability multiplier (computed above both branches)
        if boat_mult < 1.0 then
          cost = cost * boat_mult
        end
      end

      -- Penalize fights when our CURRENT tile is already under threat —
      -- starting a tank duel from inside a hot zone gets us killed by
      -- the surrounding pill fire while we're aiming at the enemy. Read
      -- once per candidate (cheap O(1) lookup) outside any branch so it
      -- applies to both LOS and standoff engages.
      local tank_tile_threat = threat.at(tmx, tmy) or 0
      cost = cost + tank_tile_threat

      breakdown[#breakdown + 1] = {
        id = et.id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
        path_cost = path_cost, base = los_engage and 0 or C.TANK_COMBAT_BASE_COST,
        aim_bonus = aim_bonus, aim_diff = aim_diff, crossfire = crossfire,
        wall_hp = wall_hp, wall_penalty = wall_penalty,
        low_shells_penalty = low_shells_penalty, boat_mult = boat_mult,
        tank_tile_threat = tank_tile_threat,
        tank_shells = info.shells,  -- stash for get_pool_breakdown detail
        cost = cost, shells_on_arrival = shells_on_arrival, los_engage = los_engage,
        standoff_mx = so_mx, standoff_my = so_my, standoff_score = so_score,
        standoff_deg = so_deg, scan_spots = so_spots,
      }

      if cost < best_cost then
        best_cost = cost
        best_tank = et
      end
    end  -- do block
    ::continue_tanks::
  end

  -- Add inactive rows for enemy player slots whose tank is not currently
  -- visible. We know the player exists, but not its tank position, so these
  -- rows explain why the tank is not an actionable attack_tank candidate.
  if info.player_names then
    local allies = info.allies or 0
    for pn = 0, (info.max_players or 0) - 1 do
      if pn ~= info.player_number and not visible_enemy_ids[pn] then
        local name = info.player_names[pn + 1]
        local active = (type(name) == "string" and name ~= "")
                    or pn < (info.num_players or 0)
        local allied = (allies & (1 << pn)) ~= 0
        if active and not allied then
          if type(name) ~= "string" or name == "" then name = "player " .. tostring(pn) end
          breakdown[#breakdown + 1] = {
            id = pn, mx = -1, my = -1, dist = 0, speed = 0,
            path_cost = 0, base = 0, aim_bonus = 0, aim_diff = 0,
            crossfire = 0, wall_penalty = 0, low_shells_penalty = 0,
            tank_shells = info.shells,
            cost = 1e30, shells_on_arrival = 0,
            skipped = "not_visible", player_name = name,
          }
        end
      end
    end
  end

  state.attack_tank_breakdown = breakdown
  if not best_tank then
    return nil
  end

  -- Mark the winning entry for the pool window display.
  for _, b in ipairs(breakdown) do
    if b.mx == best_tank.mx and b.my == best_tank.my then b.winner = true end
  end

  -- Store scan spots from the winning tank's standoff evaluation on the goal
  -- so steering can draw them persistently during the attack.
  local win_entry = nil
  for _, b in ipairs(breakdown) do
    if b.mx == best_tank.mx and b.my == best_tank.my then win_entry = b; break end
  end

  -- Mid-take engage break: when we're committed to an attack_pill take
  -- (in any LOCK_SUB substate) and an enemy tank is BOTH (a) within
  -- shooting range and (b) further from our target pill than we are,
  -- the situation is unfavourable — the target pill is shooting US, not
  -- the tank — and we should turn and engage. Drop cost under pool 6's
  -- mid-take floor (10) and flag the entry so hysteresis (additive +
  -- multiplicative) is skipped for it. Without the flag, mid-take SW+CM
  -- penalties would push the cost back over the lock floor.
  local engage_break_lock = false
  if state.goal and state.goal.kind == "attack_pill"
     and LOCK_SUBS[state.goal.substate or ""]
     and best_tank.dist <= C.TANK_COMBAT_ENGAGE_RANGE then
    local tank_to_pill = U.mdist(best_tank.mx, best_tank.my,
                                 state.goal.mx, state.goal.my)
    local our_to_pill  = U.mdist(tmx, tmy, state.goal.mx, state.goal.my)
    if tank_to_pill > our_to_pill then
      best_cost = math.min(best_cost, 9)
      engage_break_lock = true
    end
  end

  return {
    cost = best_cost,
    _engage_break_lock = engage_break_lock or nil,
    goal = { kind = "attack_tank", mx = best_tank.mx, my = best_tank.my,
             wx = U.m2w(best_tank.mx), wy = U.m2w(best_tank.my),
             target_id = best_tank.id,
             target_obj = best_tank.obj,
             substate = "close",
             tank_scan_spots = win_entry and win_entry.scan_spots or nil,
             tank_standoff_deg = win_entry and win_entry.standoff_deg or nil,
             tank_standoff_mx = win_entry and win_entry.standoff_mx or nil,
             tank_standoff_my = win_entry and win_entry.standoff_my or nil, },
    desc = BRAIN_POOL_VIZ and string.format("attack_tank@(%d,%d) cost=%.0f dist=%d spd=%.1f%s",
           best_tank.mx, best_tank.my, best_cost, best_tank.dist, best_tank.speed,
           engage_break_lock and " [engage-break]" or "") or "",
  }
end

-- Relaxed fallback for place_pill: scan a small box around the tank, accept
-- any drivable land that's placeable, score by simple criteria (LOS, threat,
-- distance from tank, mild front-line awareness). Used when the strategic
-- search can't find a candidate (no friendly base, all cells too far, etc).
local function eval_place_pill_fallback(state, world, info, tmx, tmy, boat, ammo, carry_discount)
  carry_discount = carry_discount or 0
  local R = C.STRATEGIC_PLACE_FALLBACK_RADIUS
  local best_score = -math.huge
  local best_mx, best_my = nil, nil
  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(tmx + dx)
      local cy = U.mclamp(tmy + dy)
      if U.is_placeable(cx, cy, world) then
        local score = 0
        -- Mild front-line bias: don't place in enemy territory.
        local influence = cpf.influence_at(cx, cy)
        if influence < 0 then score = score - 50 end
        -- LOS coverage
        score = score + U.los_coverage(cx, cy, C.STRATEGIC_PLACE_LOS_DIRS,
                                       C.STRATEGIC_PLACE_LOS_MAX_RANGE)
                       * C.STRATEGIC_PLACE_LOS_WEIGHT
        -- Threat penalty
        score = score - threat.at(cx, cy) * C.STRATEGIC_PLACE_THREAT_WEIGHT
        -- Spacing from existing friendly pills
        local pill_dist = nearest_friendly_pill_dist(world, cx, cy)
        if pill_dist < C.STRATEGIC_PLACE_PILL_SPACING then
          score = score - C.STRATEGIC_PLACE_PILL_PENALTY
        end
        -- Tank distance: closer = cheaper to reach, slight preference
        score = score - U.mdist(tmx, tmy, cx, cy) * 0.5
        if score > best_score then
          best_score = score; best_mx = cx; best_my = cy
        end
      end
    end
  end
  if not best_mx then return nil end
  local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_mx, best_my, boat and 1 or 0,
                                info.shells or 32, info.trees or 0,
                                info.mines or 0, info.armour or 40)
  local raw_cost = path_cost + C.STRATEGIC_PLACE_FALLBACK_COST - carry_discount
  local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT)
  return {
    cost = cost,
    goal = { kind = "place_pill_strategic", mx = best_mx, my = best_my,
             wx = U.m2w(best_mx), wy = U.m2w(best_my), fallback = true },
    desc = BRAIN_POOL_VIZ and string.format("(A*{%.0f}+base{%.0f}-carry{%.0f})*mult{%.2f} fbk",
           path_cost, C.STRATEGIC_PLACE_FALLBACK_COST, carry_discount,
           C.STRATEGIC_PLACE_COST_MULT) or "",
  }
end

local function eval_place_pill_strategic(state, world, info, tmx, tmy, boat, ammo)
  if not C.STRATEGIC_PLACE_ENABLED then return nil end
  if (info.carried_pills or 0) < 1 then return nil end
  if info.man_status ~= C.LGM_INTANK then return nil end
  if info.inboat then return nil end

  -- ── Carry value penalty ─────────────────────────────────────────────────
  -- Increase placement cost when carrying is more useful than placing.
  -- Offset by urgency (base undefended + enemy nearby, or critical health).
  local carry_value_penalty = 0
  local phase = state.phase or "opening"
  if phase == "opening" or phase == "early_expansion" then
    carry_value_penalty = carry_value_penalty + C.STRATEGIC_PLACE_CARRY_EARLY_PENALTY
  end
  -- Check for dead pill nearby we'd want to capture → keep carrying
  for _, p in pairs(world.pills) do
    if p.health == 0 and U.mdist(tmx, tmy, p.mx, p.my) <= C.STRATEGIC_PLACE_CARRY_CAPTURE_RANGE then
      carry_value_penalty = carry_value_penalty + C.STRATEGIC_PLACE_CARRY_CAPTURE_PENALTY
      break
    end
  end
  local gk = state.goal and state.goal.kind or "none"
  if gk == "attack_pill" then
    carry_value_penalty = carry_value_penalty + C.STRATEGIC_PLACE_CARRY_ATTACK_PENALTY
  end
  -- Urgency overrides: cancel carry penalty when placement is critical
  local fbx, fby, fb_dist = nearest_friendly_base_pos(world, tmx, tmy)
  if fbx then
    local base_pills = count_pills_near(world, fbx, fby, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly")
    local enemy_near = state.perc and state.perc.enemy_tanks and #state.perc.enemy_tanks > 0
    if base_pills == 0 and enemy_near then
      carry_value_penalty = 0  -- base naked + enemy visible: place NOW
    end
  end
  if info.armour <= C.ARMOUR_CRITICAL then
    carry_value_penalty = 0  -- drop before we die
  end

  -- Carry-time urgency discount (pill gets cheaper to place the longer held).
  local carry_discount = 0
  if state.carrying_pill_since then
    local ticks_carried = (state.tick or 0) - state.carrying_pill_since
    if ticks_carried > 0 then
      carry_discount = math.min(C.STRATEGIC_PLACE_CARRY_DISCOUNT_MAX,
                                ticks_carried * C.STRATEGIC_PLACE_CARRY_DISCOUNT_PER_TICK)
    end
  end

  -- ── Defensive build ──────────────────────────────────────────────────────
  -- Enemy tank visible + we're carrying: drop a pill at ±45° from the threat
  -- direction, 2–5 tiles out.
  if state.perc and state.perc.enemy_tanks and #state.perc.enemy_tanks > 0 then
    local closest_et, closest_dist = nil, math.huge
    for _, et in ipairs(state.perc.enemy_tanks) do
      if et.dist < closest_dist then closest_dist = et.dist; closest_et = et end
    end
    if closest_et then
      local aim = U.aim_at(info.tankx, info.tanky, U.m2w(closest_et.mx), U.m2w(closest_et.my))
      local best_cx, best_cy = nil, nil
      for dist = C.DEFENSIVE_BUILD_MAX_DIST, C.DEFENSIVE_BUILD_MIN_DIST, -1 do
        for _, aoff in ipairs({ C.DEFENSIVE_BUILD_ANGLE_OFFSET, -C.DEFENSIVE_BUILD_ANGLE_OFFSET }) do
          local angle = (aim + aoff) % 256
          local rad   = angle * (math.pi * 2 / 256)
          local dx    = math.sin(rad)
          local dy    = -math.cos(rad)
          local cx    = U.mclamp(math.floor(tmx + dx * dist + 0.5))
          local cy    = U.mclamp(math.floor(tmy + dy * dist + 0.5))
          if not U.is_placeable(cx, cy, world) then goto next_def_angle end
          if PF.wall_hp_between(tmx, tmy, cx, cy) ~= 0 then goto next_def_angle end
          do
            local water_blocked = false
            for step = 1, dist - 1 do
              local ix = U.mclamp(math.floor(tmx + dx * step + 0.5))
              local iy = U.mclamp(math.floor(tmy + dy * step + 0.5))
              if U.is_water(U.ttype(ix, iy)) then water_blocked = true; break end
            end
            if water_blocked then goto next_def_angle end
          end
          best_cx, best_cy = cx, cy
          ::next_def_angle::
          if best_cx then break end
        end
        if best_cx then break end
      end
      if best_cx then
        local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_cx, best_cy, 0,
                           info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
        local raw_cost = path_cost + C.STRATEGIC_PLACE_BASE_COST - carry_discount
        local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT)
        return {
          cost = cost,
          goal = { kind = "place_pill_strategic", mx = best_cx, my = best_cy,
                   wx = U.m2w(best_cx), wy = U.m2w(best_cy) },
          desc = BRAIN_POOL_VIZ and string.format("def_build@(%d,%d) cost=%.0f tank@(%d,%d) (A*{%.0f}+base{%.0f}-carry{%.0f})*%.2f",
                 best_cx, best_cy, cost, closest_et.mx, closest_et.my,
                 path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_discount, C.STRATEGIC_PLACE_COST_MULT) or "",
        }
      end
    end
  end

  -- Don't interrupt active combat goals (normal strategic only — defensive
  -- build above is allowed to preempt since it's directly threat-reactive).
  if gk == "attack_pill" or gk == "pill_place" then return nil end

  if not fbx then
    return eval_place_pill_fallback(state, world, info, tmx, tmy, boat, ammo, carry_discount)
  end

  -- ── Search center selection (priority chain) ────────────────────────────
  -- 1. Pill war zone: both sides have pills facing each other
  -- 2. Hostile pill anchoring: place between our base and the threat
  -- 3. Offensive spike: target hostile base (when dominating)
  -- 4. Pill under attack: reinforce near the contested pill
  -- 5. Fallback: nearest friendly base (pure defense)
  local search_mx, search_my
  local spike_base = nil
  local search_reason = "base_defense"

  -- 1. Pill war zone
  local wz_mx, wz_my = detect_pill_war_zone(world)
  if wz_mx then
    search_mx, search_my = wz_mx, wz_my
    search_reason = "pill_war"
  end

  -- 2. Hostile pill anchoring (if no pill war)
  if not search_mx then
    local hp_mx, hp_my, hp_dist = nearest_hostile_pill_pos(world, tmx, tmy)
    if hp_mx then
      search_mx = math.floor((fbx + hp_mx) / 2 + 0.5)
      search_my = math.floor((fby + hp_my) / 2 + 0.5)
      search_reason = "hostile_pill"
    end
  end

  -- 3. Offensive spike
  if not search_mx then
    local offensive = false
    if state.strength and state.strength > C.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD then
      local all_defended = true
      for _, b in pairs(world.bases) do
        if b.owner == "friendly" then
          if count_pills_near(world, b.mx, b.my, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly") < 2 then
            all_defended = false; break
          end
        end
      end
      if all_defended then offensive = true end
    end
    if offensive then
      local hb = nearest_hostile_base(world, tmx, tmy)
      if hb then
        search_mx, search_my = hb.mx, hb.my
        spike_base = hb
        search_reason = "offensive_spike"
      end
    end
  end

  -- 4. Pill under attack: reinforce near the contested pill
  if not search_mx then
    local pua = state.perc and state.perc.pill_under_attack
    if pua then
      search_mx = math.floor((fbx + pua.mx) / 2 + 0.5)
      search_my = math.floor((fby + pua.my) / 2 + 0.5)
      search_reason = "reinforce"
    end
  end

  -- 5. Fallback: nearest friendly base (pure defense, esp. early game)
  if not search_mx then
    search_mx, search_my = fbx, fby
    search_reason = "base_defense"
  end

  -- ── Scoring grid ────────────────────────────────────────────────────────
  -- Capacity tier place_r: cap the heatmap search radius. (2R+1)² tiles
  -- get scored, so halving R quarters the work.
  local R = C.STRATEGIC_PLACE_SEARCH_RADIUS
  if state._capacity and state._capacity.place_r and state._capacity.place_r < R then
    R = state._capacity.place_r
  end
  local best_score = -math.huge
  local best_mx, best_my = nil, nil
  local all_cands = {}

  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(search_mx + dx)
      local cy = U.mclamp(search_my + dy)
      if U.is_placeable(cx, cy, world) then
        local score = 0
        local sc1, sc2, sc3, sc4, sc5, sc6, sc7, sc8, sc9, sc10 = 0,0,0,0,0,0,0,0,0,0

        -- 1. Base proximity
        local _, _, base_dist = nearest_friendly_base_pos(world, cx, cy)
        if base_dist > C.STRATEGIC_PLACE_MAX_BASE_DIST then goto skip_cell end
        sc1 = (C.STRATEGIC_PLACE_MAX_BASE_DIST - base_dist) * C.STRATEGIC_PLACE_BASE_WEIGHT
        score = score + sc1

        -- 2. Base defense need
        do
          local base_pill_count = count_pills_near(world, cx, cy, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly")
          if base_pill_count < 2 then
            sc2 = C.STRATEGIC_PLACE_UNDERDEFENDED_BONUS * (2 - base_pill_count)
            score = score + sc2
          end
        end

        -- 3. Influence-aware front line proximity
        do
          local s0 = score
          local influence = cpf.influence_at(cx, cy)
          if influence < 0 then
            score = score - C.STRATEGIC_PLACE_BEYOND_FRONT_PENALTY
          elseif influence > 0 then
            score = score + math.max(0, C.STRATEGIC_PLACE_FRONT_PROX_CAP - influence)
                   * C.STRATEGIC_PLACE_FRONT_PROX_WEIGHT
          end
          sc3 = score - s0
        end

        -- 4. Pill spacing
        do
          local s0 = score
          local pill_dist = nearest_friendly_pill_dist(world, cx, cy)
          if pill_dist < C.STRATEGIC_PLACE_PILL_SPACING then
            score = score - C.STRATEGIC_PLACE_PILL_PENALTY
          elseif pill_dist >= 2 and pill_dist <= 4 then
            score = score + C.STRATEGIC_PLACE_SPACING_BONUS
          end
          sc4 = score - s0
        end

        -- 5. LOS coverage
        do
          local los = U.los_coverage(cx, cy, C.STRATEGIC_PLACE_LOS_DIRS, C.STRATEGIC_PLACE_LOS_MAX_RANGE)
          sc5 = los * C.STRATEGIC_PLACE_LOS_WEIGHT
          score = score + sc5
        end

        -- 6. Threat penalty
        do
          local thr = threat.at(cx, cy)
          sc6 = -thr * C.STRATEGIC_PLACE_THREAT_WEIGHT
          score = score + sc6
        end

        -- 7. Distance from tank
        sc7 = -U.mdist(tmx, tmy, cx, cy) * 0.5
        score = score + sc7

        -- 8. Offensive spike bonus
        if spike_base then
          local hb_dist = U.mdist(cx, cy, spike_base.mx, spike_base.my)
          if hb_dist <= 2 then
            sc8 = C.STRATEGIC_PLACE_SPIKE_BONUS
            score = score + sc8
          end
        end

        -- 9. Enemy pill proximity (NEW)
        do
          local s0 = score
          local ep_mx, ep_my, ep_dist = nearest_hostile_pill_pos(world, cx, cy)
          if ep_mx then
            if ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_DANGER_RANGE then
              score = score - C.STRATEGIC_PLACE_ENEMY_PILL_DANGER_PEN
            elseif ep_dist >= C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MIN
               and ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MAX then
              score = score + C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_BONUS
            elseif ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_FAR_RANGE then
              score = score + C.STRATEGIC_PLACE_ENEMY_PILL_FAR_BONUS
            end
          end
          sc9 = score - s0
        end

        -- 10. Pill war zone reinforcement (NEW)
        if wz_mx then
          local wz_dist = U.mdist(cx, cy, wz_mx, wz_my)
          if wz_dist <= 5 then
            sc10 = C.STRATEGIC_PLACE_WAR_ZONE_BONUS * (1.0 - wz_dist / 6.0)
            score = score + sc10
          end
        end

        all_cands[#all_cands + 1] = {
          mx = cx, my = cy, score = score,
          sc1=sc1, sc2=sc2, sc3=sc3, sc4=sc4, sc5=sc5,
          sc6=sc6, sc7=sc7, sc8=sc8, sc9=sc9, sc10=sc10,
        }
        if score > best_score then
          best_score = score; best_mx = cx; best_my = cy
        end
      end
      ::skip_cell::
    end
  end

  if not best_mx then
    return eval_place_pill_fallback(state, world, info, tmx, tmy, boat, ammo, carry_discount)
  end

  local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_mx, best_my, boat and 1 or 0,
                                info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
  local raw_cost = path_cost + C.STRATEGIC_PLACE_BASE_COST + carry_value_penalty - carry_discount
  -- Last pill: 1.5× cost so the bot holds on to its only pill unless
  -- placement is clearly worthwhile.
  local last_pill_mult = 1.0
  if (info.carried_pills or 0) == 1 then last_pill_mult = 1.5 end
  local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT * last_pill_mult)

  -- Build pool-grid candidate list: winner gets actual cost, others get cost + score delta.
  -- Pool-grid panel data only — wrapped so lua_strip removes it from opt/.
  local cands = {}

  return {
    cost = cost,
    goal = { kind = "place_pill_strategic", mx = best_mx, my = best_my,
             wx = U.m2w(best_mx), wy = U.m2w(best_my) },
    desc = BRAIN_POOL_VIZ and string.format("(A*{%.0f}+base{%.0f}+carry_pen{%.0f}-carry{%.0f})*mult{%.2f} center=%s score=%.0f",
           path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_value_penalty, carry_discount,
           C.STRATEGIC_PLACE_COST_MULT, search_reason, best_score) or "",
    cands = cands,
  }
end

-- =========================================================================
-- Strategic placement heatmap (for BrainTest visualization key 8)
-- Returns a string: "CENTER\tmx\tmy\tR\treason\n" then "mx\tmy\tscore\n" per tile.
-- Uses the same search center + scoring as eval_place_pill_strategic.
-- =========================================================================
function M.get_strategic_place_heatmap(state, world, info)
  if not info then return nil end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8

  local fbx, fby = nearest_friendly_base_pos(world, tmx, tmy)
  if not fbx then fbx, fby = tmx, tmy end

  -- Same search center priority as eval_place_pill_strategic
  local search_mx, search_my
  local spike_base = nil
  local search_reason = "base_defense"

  local wz_mx, wz_my = detect_pill_war_zone(world)
  if wz_mx then
    search_mx, search_my = wz_mx, wz_my
    search_reason = "pill_war"
  end
  if not search_mx then
    local hp_mx, hp_my = nearest_hostile_pill_pos(world, tmx, tmy)
    if hp_mx then
      search_mx = math.floor((fbx + hp_mx) / 2 + 0.5)
      search_my = math.floor((fby + hp_my) / 2 + 0.5)
      search_reason = "hostile_pill"
    end
  end
  if not search_mx then
    local offensive = state.strength and state.strength > C.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD
    if offensive then
      local hb = nearest_hostile_base(world, tmx, tmy)
      if hb then
        search_mx, search_my = hb.mx, hb.my
        spike_base = hb
        search_reason = "offensive_spike"
      end
    end
  end
  if not search_mx then
    local pua = state.perc and state.perc.pill_under_attack
    if pua then
      search_mx = math.floor((fbx + pua.mx) / 2 + 0.5)
      search_my = math.floor((fby + pua.my) / 2 + 0.5)
      search_reason = "reinforce"
    end
  end
  if not search_mx then
    search_mx, search_my = fbx, fby
    search_reason = "base_defense"
  end

  local R = C.STRATEGIC_PLACE_SEARCH_RADIUS
  local lines = {}
  lines[#lines + 1] = string.format("CENTER\t%d\t%d\t%d\t%s", search_mx, search_my, R, search_reason)

  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(search_mx + dx)
      local cy = U.mclamp(search_my + dy)
      if U.is_placeable(cx, cy, world) then
        local score = 0

        local _, _, base_dist = nearest_friendly_base_pos(world, cx, cy)
        if base_dist and base_dist > C.STRATEGIC_PLACE_MAX_BASE_DIST then goto skip_hm end
        if base_dist then
          score = score + (C.STRATEGIC_PLACE_MAX_BASE_DIST - base_dist) * C.STRATEGIC_PLACE_BASE_WEIGHT
        end

        do
          local bpc = count_pills_near(world, cx, cy, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly")
          if bpc < 2 then score = score + C.STRATEGIC_PLACE_UNDERDEFENDED_BONUS * (2 - bpc) end
        end

        do
          local influence = cpf.influence_at(cx, cy)
          if influence < 0 then
            score = score - C.STRATEGIC_PLACE_BEYOND_FRONT_PENALTY
          elseif influence > 0 then
            score = score + math.max(0, C.STRATEGIC_PLACE_FRONT_PROX_CAP - influence)
                   * C.STRATEGIC_PLACE_FRONT_PROX_WEIGHT
          end
        end

        do
          local pd = nearest_friendly_pill_dist(world, cx, cy)
          if pd < C.STRATEGIC_PLACE_PILL_SPACING then score = score - C.STRATEGIC_PLACE_PILL_PENALTY
          elseif pd >= 2 and pd <= 4 then score = score + C.STRATEGIC_PLACE_SPACING_BONUS end
        end

        do
          local los = U.los_coverage(cx, cy, C.STRATEGIC_PLACE_LOS_DIRS, C.STRATEGIC_PLACE_LOS_MAX_RANGE)
          score = score + los * C.STRATEGIC_PLACE_LOS_WEIGHT
        end

        score = score - threat.at(cx, cy) * C.STRATEGIC_PLACE_THREAT_WEIGHT
        score = score - U.mdist(tmx, tmy, cx, cy) * 0.5

        if spike_base then
          if U.mdist(cx, cy, spike_base.mx, spike_base.my) <= 2 then
            score = score + C.STRATEGIC_PLACE_SPIKE_BONUS
          end
        end

        -- 9. Enemy pill proximity
        do
          local ep_mx, ep_my, ep_dist = nearest_hostile_pill_pos(world, cx, cy)
          if ep_mx then
            if ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_DANGER_RANGE then
              score = score - C.STRATEGIC_PLACE_ENEMY_PILL_DANGER_PEN
            elseif ep_dist >= C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MIN
               and ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MAX then
              score = score + C.STRATEGIC_PLACE_ENEMY_PILL_SWEET_BONUS
            elseif ep_dist <= C.STRATEGIC_PLACE_ENEMY_PILL_FAR_RANGE then
              score = score + C.STRATEGIC_PLACE_ENEMY_PILL_FAR_BONUS
            end
          end
        end

        -- 10. Pill war zone reinforcement
        if wz_mx then
          local wz_dist = U.mdist(cx, cy, wz_mx, wz_my)
          if wz_dist <= 5 then
            score = score + C.STRATEGIC_PLACE_WAR_ZONE_BONUS * (1.0 - wz_dist / 6.0)
          end
        end

        lines[#lines + 1] = string.format("%d\t%d\t%.1f", cx, cy, score)
      end
      ::skip_hm::
    end
  end
  return table.concat(lines, "\n")
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
  if gk == "attack_pill" or gk == "attack_tank" or gk == "pill_place" then
    if state.goal.mx and U.mdist(state.goal.mx, state.goal.my, target.mx, target.my) <= 5 then
      return nil
    end
  end

  local travel = smart_cost(KIND_NORMAL, tmx, tmy, target.mx, target.my, boat and 1 or 0,
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
    desc = BRAIN_POOL_VIZ and string.format("A*{%.0f}+base{%.0f}-urgency{%.0f} dmg=%d",
           travel, C.DEFEND_PILL_BASE_COST, urgency, target.damage) or "",
  }
end

-- =========================================================================
-- Pill repositioning (aIndy: "pissing" — move badly-positioned friendly pills)
-- Evaluates friendly pills for bad positioning: too far from any base,
-- exposed to many enemy pills, on bad terrain. If a pill scores badly,
-- create a goal to pick it up (drive over dead/pissed pill). The existing
-- place_pill_strategic system handles replanting once we carry it.
-- =========================================================================
local function eval_reposition_pill(state, world, info, tmx, tmy, boat, ammo)
  if not C.PILL_REPOSITION_ENABLED then return nil end
  if (info.carried_pills or 0) >= 1 then return nil end  -- already carrying
  if info.man_status ~= C.LGM_INTANK then return nil end
  if info.inboat then return nil end

  -- Don't reposition during opening (need pills in place)
  if state.phase == "opening" then return nil end

  local best_pill, best_pid, best_badness = nil, nil, 0
  for pid, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health > 0 then
      -- Score how "bad" this pill's position is
      local badness = 0

      -- Distance from nearest friendly base (pills far from bases are less useful)
      local nearest_base_dist = math.huge
      for _, b in pairs(world.bases) do
        if b.owner == "friendly" then
          local bd = U.mdist(p.mx, p.my, b.mx, b.my)
          if bd < nearest_base_dist then nearest_base_dist = bd end
        end
      end
      if nearest_base_dist > C.PILL_REPOSITION_ORPHAN_DIST then
        badness = badness + (nearest_base_dist - C.PILL_REPOSITION_ORPHAN_DIST) * 5
      end

      -- Exposure: count hostile pills within range (crossfire)
      local hostile_cover = 0
      for _, op in pairs(world.pills) do
        if (op.owner == "hostile" or op.owner == "neutral") and op.health > 0 then
          if U.mdist(p.mx, p.my, op.mx, op.my) <= C.PILL_FIRE_RANGE then
            hostile_cover = hostile_cover + 1
          end
        end
      end
      badness = badness + hostile_cover * 30

      -- Bad terrain (pill on swamp/rubble/crater is hard to reach for repair)
      local ptt = U.ttype(p.mx, p.my)
      if ptt == C.T_SWAMP or ptt == C.T_RUBBLE or ptt == C.T_CRATER then
        badness = badness + 20
      end

      -- Only reposition if badness exceeds threshold
      if badness > C.PILL_REPOSITION_THRESHOLD and badness > best_badness then
        best_badness = badness
        best_pill = p
        best_pid = pid
      end
    end
  end

  if not best_pill then return nil end

  -- Cost: distance to the pill + inverse badness (worse position = cheaper to fix)
  local pcost = U.estimate_cost(tmx, tmy, best_pill.mx, best_pill.my, boat, ammo)
  local cost = pcost + 200 - best_badness  -- 200 base cost, reduced by badness

  return {
    cost = math.max(1, cost),
    goal = { kind = "capture_pill", mx = best_pill.mx, my = best_pill.my,
             wx = U.m2w(best_pill.mx), wy = U.m2w(best_pill.my),
             target_id = best_pid, reposition = true },
    desc = BRAIN_POOL_VIZ and string.format("reposition_pill#%d@(%d,%d) cost=%.0f badness=%.0f",
           best_pid, best_pill.mx, best_pill.my, cost, best_badness) or "",
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
  eval_reposition_pill,
}

-- Phase weight keys for each pool evaluator (maps into C.PHASE_WEIGHTS[phase]).
-- nil = no phase weighting (always critical, e.g. refuel/attack_tank).
local POOL_NAMES = {
  "refuel", "defend_pill", "capture_base", "capture_pill", "repair_pill",
  "attack_pill", "attack_base", "place_strategic", "attack_tank",
  [12] = "wait_for_lgm",
  [13] = "kill_lgm",
}

-- Reverse map: actual goal.kind → pool index, for looking up cost_cache
-- entries by candidate.  Note pool 1 (refuel) and pool 8 (place_strategic)
-- have different UI labels than their goal.kind values.
-- Pool 10 in the JSON is the WINNERS section, 11 is def_build, 12 is
-- wait_for_lgm, 13 is kill_lgm — those four render as strips below the
-- main 2x5 grid.
local KIND_TO_POOL = {
  refuel_at_base = 1, defend_pill = 2, capture_base = 3, capture_pill = 4,
  repair_pill = 5, attack_pill = 6, attack_base = 7,
  place_pill_strategic = 8, attack_tank = 9, wait_for_lgm = 12,
  kill_lgm = 13,
}

-- Reverse map: actual goal.kind → pool index, for looking up cost_cache
-- entries by candidate.  Note pool 1 (refuel) and pool 8 (place_strategic)
-- have different UI labels than their goal.kind values.
local KIND_TO_POOL = {
  refuel_at_base = 1, defend_pill = 2, capture_base = 3, capture_pill = 4,
  repair_pill = 5, attack_pill = 6, attack_base = 7,
  place_pill_strategic = 8, attack_tank = 9, wait_for_lgm = 12,
}

-- (LOCK_SUBS defined above eval_attack_tank.)

-- Wall-shield investment substates; gain extra commitment penalty
-- in goal_selection's hysteresis so we don't abandon a half-built
-- shield setup just because another pill briefly looks cheaper.
local WS_SUBS = {
  ws_prebuild=true, ws_prewait=true, ws_advance=true,
  ws_engage=true,   ws_retreat=true, ws_rebuild=true,
  gather_trees=true,
  -- build_walls intentionally NOT here: no shells committed yet, the
  -- LGM-time + trees investment is small and recoverable, so a
  -- high-priority preempt (kill_lgm, attack_tank, capture_pill) should
  -- win mid-build without paying the WALL_SHIELD_COMMITMENT surcharge.
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true,
}

-- HP multiplier lookup for attack_pill cost shaping.  Indexed by remaining
-- pill HP (1..15); applied to the entire combat_cost block, so wounded
-- pills get scaled down.  Hand-tuned: aggressive discount on near-dead
-- pills (HP 1-3) to make them snap-pickups, a knee at HP=4 (28%) to keep
-- non-snap-pickup wounded pills attractive but not free, then linear
-- 40% -> 100% from HP=5 to HP=15.  Replaces the previous (hp/15)^2 curve
-- which was too aggressive in the mid-range (10HP came out at 0.44).
local ATTACK_PILL_HP_MULT = {
  0.05, 0.10, 0.18, 0.28,
  0.40, 0.46, 0.52, 0.58, 0.64, 0.70,
  0.76, 0.82, 0.88, 0.94, 1.00,
}

-- Inject a low-cost wait_for_lgm candidate so the bot prefers to wait
-- when the LGM is out (e.g. farming) and we'd otherwise wander off.
-- Skipped during goals that ARE actively driving the LGM to do
-- something (build_walls, ws_*, pill_place, repair_pill, capture_pill,
-- rescue_lgm) so we don't preempt a real LGM-using mission.
local function eval_wait_for_lgm(state, info)
  if not C.WAIT_FOR_LGM_ENABLED then return nil end
  if not info or info.man_status ~= C.LGM_MOVING then return nil end
  if state.lgm_stranded then return nil end
  local g = state.goal
  if g then
    if g.kind == "rescue_lgm"   then return nil end
    if g.kind == "pill_place"   then return nil end
    if g.kind == "repair_pill"  then return nil end
    if g.kind == "capture_pill" then return nil end
    if g.kind == "attack_pill" and (
         g.substate == "build_walls"
      or g.substate == "ws_prebuild"
      or g.substate == "ws_rebuild") then
      return nil
    end
  end
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local cost = C.WAIT_FOR_LGM_COST or 50
  return {
    cost = cost,
    goal = { kind = "wait_for_lgm", mx = tmx, my = tmy,
             wx = info.tankx, wy = info.tanky },
    desc = BRAIN_POOL_VIZ and string.format("wait_for_lgm@(%d,%d) lgm=(%d,%d) cost=%d",
                         tmx, tmy,
                         (info.man_x or 0) >> 8, (info.man_y or 0) >> 8, cost) or "",
    cands = {
      { id = 0, mx = tmx, my = tmy, cost = cost,
        own = "self", hp = 0, stale = 0 },
    },
  }
end

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
-- Maps pool_idx → { collection_key, filter_fn }
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
local FINALIZE_POOLS = { 2, 8, 9 }  -- defend_pill, place_pill, attack_tank

-- Filter functions for each incremental pool.
-- Return true if the object is a valid candidate.
-- filter_refuel: returns nil if the base qualifies for scoring, or a
-- reject descriptor table { reason = "<short>", remaining = <ticks> }
-- when it should be SHOWN in the pool grid but greyed out (so the
-- user can see "this base exists, here's why we're not picking it").
-- The only HARD reject (drop from queue entirely) is "hostile" — we
-- never refuel at an enemy base.
local function filter_refuel(obj, state, info)
  if not (obj.owner == "friendly" or obj.owner == "neutral") then
    return { reason = "hostile" }
  end
  -- Blocked tile
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    local until_tick = state.blocked[bk]
    if until_tick and (state.tick or 0) < until_tick then
      return { reason = "blocked", remaining = until_tick - (state.tick or 0) }
    end
  end
  -- Neutral and unseen too long
  local now = state and state.tick or 0
  if obj.owner == "neutral" and obj.last_seen and now > 0
     and (now - obj.last_seen) > C.STALE_SKIP_TICKS then
    return { reason = "stale", remaining = (now - obj.last_seen) - C.STALE_SKIP_TICKS }
  end
  -- Recently observed depleted of what we actually need
  if obj.obs_tick and now > 0 and (now - obj.obs_tick) < C.REFUEL_OBS_STALE then
    local obs_low = true
    if info.armour < C.TANK_FULL_ARMOUR and (obj.obs_armour or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
    if info.shells < C.TANK_FULL_SHELLS and (obj.obs_shells or 0) >= C.REFUEL_MIN_STOCK then obs_low = false end
    if obs_low then
      return { reason = "depleted",
               remaining = C.REFUEL_OBS_STALE - (now - obj.obs_tick) }
    end
  end
  -- Too dangerous (pill fire)
  local danger = threat.at(obj.mx, obj.my)
  if danger > C.REFUEL_DANGER_REJECT then
    return { reason = "danger" }
  end
  return nil
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

-- filter_capture_pill: returns nil if the pill qualifies for scoring,
-- or a reject descriptor table { reason = "<short>", remaining = <ticks> }
-- when it should be SHOWN in the pool grid but greyed out (so the user can
-- see "this pill exists, here's why we're not picking it"). The only
-- truly-hard reject is "alive" (that's attack_pill's job, not ours).
local function filter_capture_pill(obj, state)
  -- Alive pills aren't capturable — they belong in attack_pill. Hide
  -- entirely (we don't want every alive pill cluttering pool 4).
  if (obj.health or 0) > 0 then return { reason = "alive" } end
  -- in_tank: someone picked it up. Show as rejected so the user can see
  -- "yes the dead pill exists, but it's currently in flight."
  if obj.in_tank then return { reason = "in_tank" } end
  -- blocked: stuck/no-build cooldown on this tile.
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    local until_tick = state.blocked[bk]
    if until_tick and (state.tick or 0) < until_tick then
      return { reason = "blocked", remaining = until_tick - (state.tick or 0) }
    end
  end
  -- stale: bot hasn't observed this pill recently. Likely fog of war.
  local now = state and state.tick or 0
  if obj.last_seen and now > 0 then
    local age = now - obj.last_seen
    if age > C.STALE_SKIP_TICKS then
      return { reason = "stale", remaining = age - C.STALE_SKIP_TICKS }
    end
  end
  return nil
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
-- compute_pool4_cost — the capture_pill cost formula extracted so it
-- can be evaluated synchronously at queue-add time (high-priority
-- "grab the pill we just killed" responsiveness) AND at the normal
-- per-tick step_eval_queue cadence (refresh as conditions change).
-- Returns (cost, dist_score, danger_val, intercept) so the caller
-- can stash the components for the breakdown formula.
local function compute_pool4_cost(state, world, info, obj, tmx, tmy)
  -- Distance to cheapest reachable adjacent tile (pill tile itself
  -- carries an impassable overlay so we route to a neighbor).
  --
  -- After a threat rebuild (pill died), the danger grid is updated
  -- immediately but the Dijkstra slates still carry stale costs from
  -- the old danger values until they complete their next run (~2 ticks).
  -- During that window, fall back to A* (smart_cost) which reads the
  -- live danger grid directly — avoids inflated costs from phantom
  -- fire of dead pills baked into the old Dijkstra.
  local dij_stale = false
  if state.dij and threat.last_rebuild_tick > 0 then
    local slates = state.dij.slates
    local short_ok = slates[0] and slates[0].started_tick >= threat.last_rebuild_tick
    local long_ok  = slates[2] and slates[2].started_tick >= threat.last_rebuild_tick
    if not (short_ok or long_ok) then
      dij_stale = true
    end
  end
  local dist_raw
  local dist_method = "dij"
  -- Try dijkstra first using ONLY fresh slates. The C-side
  -- dijkstra_lookup_by_kind (and thus cheapest_adjacent_dij) walks slates
  -- newest-started-first and falls through to older ones when the newer
  -- slate hasn't expanded the target yet. Backup slates can predate the
  -- last threat rebuild — their g_cost still carries the dead pill's
  -- danger, producing inflated costs. Enforce the invariant: trust a
  -- dijkstra cost only if it came from a slate started AT OR AFTER the
  -- last threat rebuild.
  if not dij_stale and state.dij then
    local slates = state.dij.slates
    local min_tick = threat.last_rebuild_tick
    local best_adj, best_ax, best_ay = math.huge, nil, nil
    local NDX = { 0,  1,  1,  1,  0, -1, -1, -1 }
    local NDY = {-1, -1,  0,  1,  1,  1,  0, -1 }
    for d = 1, 8 do
      local ax = obj.mx + NDX[d]
      local ay = obj.my + NDY[d]
      if ax >= 0 and ax <= 255 and ay >= 0 and ay <= 255 then
        local ac = math.huge
        for idx = 0, 3 do
          local s = slates[idx]
          if s and s.started_tick >= min_tick then
            local c = cpf.dijkstra_cost_at(idx, ax, ay, 0)
            if c and c < ac then ac = c end
          end
        end
        if ac < best_adj then
          best_adj = ac; best_ax = ax; best_ay = ay
        end
      end
    end
    if best_adj < math.huge then
      local pcontrib = threat.pill_contrib
                       and threat.pill_contrib[obj.my * 256 + obj.mx]
      if best_ax and pcontrib then
        -- NOTE: smart_cost_minus_pill_danger_dij_only routes through C's
        -- lookup_by_kind, which can still hit stale backups. For dead
        -- pills (capture_pill's domain) pcontrib is nil so this branch
        -- is unreached; left in place for the alive-pill code paths.
        dist_raw = cpf.smart_cost_minus_pill_danger_dij_only(
                      cpf.KIND_NORMAL, best_ax, best_ay,
                      pcontrib, obj.mx, obj.my, 0)
      else
        dist_raw = best_adj
      end
    else
      -- No fresh slate has expanded any neighbour yet — fall through to A*.
      dij_stale = true
    end
  end

  if dij_stale or not dist_raw then
    dist_method = "astar"
    local boat = info.inboat and 1 or 0
    -- Strict A* — `smart_cost` queries dijkstra_lookup_by_kind first,
    -- which is exactly the stale-backup leak we are trying to avoid in
    -- this branch. Go straight to A* against the live danger grid.
    dist_raw = cpf.cost_to_astar(tmx, tmy, obj.mx, obj.my, boat,
                                  info.shells or 32, info.trees or 0,
                                  info.mines or 0, info.armour or 40,
                                  4000, false)
    if dist_raw >= 1e29 then
      dist_method = "astar_boat"
      dist_raw = cpf.cost_to_astar(tmx, tmy, obj.mx, obj.my, boat,
                                    info.shells or 32, info.trees or 0,
                                    info.mines or 0, info.armour or 40,
                                    16000, true)
    end
    if dist_raw >= 1e29 then
      return 1e30, 1e30, 1e30, 0
    end
  end
  local dist_score = (dist_raw ^ 1.5) * C.CAPTURE_PILL_DIST_SCALE
  local danger_val = threat.at(obj.mx, obj.my)
  -- Cautious-mode danger multiplier (see init.lua state.cautious_mode
  -- and constants.lua CAUTIOUS_MODE_MULT).  Capture path that
  -- runs through hostile territory costs more when we're in cautious
  -- mode (e.g. LGM dead + carrying pills).
  local _lgm_mult = state.cautious_mode and C.CAUTIOUS_MODE_MULT or 1
  -- Intercept: an enemy tank close enough to beat us to the pill
  -- (Manhattan dist ratio scaled by safety margin) bumps the cost.
  local our_dist = U.mdist(tmx, tmy, obj.mx, obj.my)
  local intercept = 0
  local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
  for _, et in ipairs(enemy_tanks) do
    local et_dist = U.mdist(et.mx, et.my, obj.mx, obj.my)
    if et_dist <= C.INTERCEPT_MAX_RANGE
       and our_dist > et_dist * C.INTERCEPT_SAFETY_MARGIN then
      local ratio = our_dist / math.max(1, et_dist)
      local pen = C.INTERCEPT_PENALTY * math.min(2.0, ratio)
      if pen > intercept then intercept = pen end
    end
  end
  local c = C.CAPTURE_PILL_BASE_COST + dist_score
          + danger_val * C.CAPTURE_PILL_DANGER_SCALE * _lgm_mult
          + intercept
  return c, dist_raw, dist_score, danger_val, intercept, _lgm_mult, dist_method
end

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

  -- Pool 1: refuel
  -- Check if we'll need combat supplies: scan for hostile pills we might attack
  local combat_ahead = false
  local worst_pill_hp = 0
  if has_shells then
    for _, obj in pairs(world.pills) do
      if (obj.owner == "hostile" or obj.owner == "neutral") and (obj.health or 0) > 0 then
        if obj.health > worst_pill_hp then worst_pill_hp = obj.health end
        combat_ahead = true
      end
    end
  end

  -- Expand refuel threshold if combat is likely
  local needs_refuel = needs_resupply
  if not needs_refuel and combat_ahead then
    needs_refuel = (info.armour <= C.ARMOUR_COMBAT or info.shells <= C.SHELLS_COMBAT)
  end

  -- Diagnostic: log the first time each base shows up in a pool. Goes
  -- to optimize.log directly so we don't need print2 enabled.
  if not state._base_in_pool then state._base_in_pool = {} end
  local function _diag_log_first_pool_add(pool_idx, pool_name, id, obj)
    if not BRAIN_PROFILE_LOG then return end
    local key = pool_idx .. ":" .. id
    if state._base_in_pool[key] then return end
    state._base_in_pool[key] = state.tick or 0
    opt.append("optimize.log", string.format(
      "  [diag] base #%d at (%d,%d) owner=%s ADDED to pool %d (%s) tick=%d",
      id, obj.mx or -1, obj.my or -1, tostring(obj.owner),
      pool_idx, pool_name, state.tick or 0))
  end

  -- Pool 1: refuel. Friendly/neutral bases that pass filter_refuel get
  -- scored normally. Bases that fail soft (blocked / stale / depleted /
  -- danger) ride along with cost = INF and a _reject tag so the pool
  -- grid can show them dimmed with the reason — same pattern as
  -- pool 4 (capture_pill). Hostile bases are HARD-rejected (not queued).
  if needs_refuel then
    for id, obj in pairs(world.bases) do
      local reject = filter_refuel(obj, state, info)
      if not reject or reject.reason ~= "hostile" then
        queue[#queue + 1] = { pool = 1, id = id, obj = obj, reject = reject }
        _diag_log_first_pool_add(1, "refuel", id, obj)
        if reject then
          local ck = "1:" .. id
          if not state.cost_cache then state.cost_cache = {} end
          if not state.cost_cache[ck] or state.cost_cache[ck]._reject ~= reject.reason then
            state.cost_cache[ck] = {
              cost = 1e30, raw = 1e30, tick = now, _p = 1,
              _mx = obj.mx, _my = obj.my,
              _dv = 0, _dang = 0, _age = 0,
              _stale = 0, _contest = 0, _hyst = 0,
              _ratio = 0, _dep = 0,
              _reject = reject.reason,
              _reject_remaining = reject.remaining or 0,
            }
          end
        end
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
        _diag_log_first_pool_add(3, "capture_base", id, obj)
      end
    end
  end

  -- Pool 4: capture_pill. Score brand-new candidates synchronously
  -- so a pill we just killed shows up in the pool grid with a real
  -- cost the same tick it appears (instead of "—" for the ticks
  -- step_eval_queue takes to pop it). The eval is cheap (8 dijkstra
  -- lookups + a threat lookup + an enemy-tank loop), so doing it at
  -- add time has negligible cost. step_eval_queue still re-evaluates
  -- on its normal cadence so the score stays current.
  -- Pool 4 (capture_pill): no perception gate — every dead pill on the
  -- map gets a row in the queue. Pills that can't actually be picked
  -- (in_tank / blocked / stale) ride along with cost = INF and a
  -- _reject tag so the pool grid can show them dimmed with the reason.
  -- Hard reject only "alive" (that's attack_pill's territory).
  if not state.cost_cache then state.cost_cache = {} end
  for id, obj in pairs(world.pills) do
    local reject = filter_capture_pill(obj, state)
    if not reject or reject.reason ~= "alive" then
      queue[#queue + 1] = { pool = 4, id = id, obj = obj, reject = reject }
      local ck = "4:" .. id
      if not state.cost_cache[ck] or (state.cost_cache[ck]._reject ~= nil) ~= (reject ~= nil) then
        if reject then
          -- Skip the cost compute — entry just exists so the row shows.
          state.cost_cache[ck] = {
            cost = 1e30, raw = 1e30, tick = now, _p = 4, _id = id,
            _mx = obj.mx, _my = obj.my,
            _ds = 0, _dv = 0, _intcpt = 0,
            _reject = reject.reason,
            _reject_remaining = reject.remaining or 0,
          }
        else
          local c, _draw, dscore, dval, intcpt, _lm4, _dm4 =
            compute_pool4_cost(state, world, info, obj, tmx, tmy)
          state.cost_cache[ck] = {
            cost = c, raw = _draw, tick = now, _p = 4, _id = id,
            _mx = obj.mx, _my = obj.my,
            _ds = dscore, _dv = dval, _intcpt = intcpt,
            _dist_method = _dm4,
          }
        end
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

  -- Pool 6: attack_pill. Always populate when hostile pills exist —
  -- the per-candidate cost handles the "is it sane to attack with this
  -- many shells?" question via penalties (COST_INF if shells < pill HP,
  -- escalating ending-shells penalty once we'd dip below SHELLS_LOW).
  -- The old has_shells gate cleared the entire pool the moment shells
  -- crossed SHELLS_LOW, even mid-take.
  local has_hostile_pills = not perc or (perc.attackable_pill_count > 0)
  if has_hostile_pills then
    for id, obj in pairs(world.pills) do
      if filter_attack_pill(obj, state) then
        queue[#queue + 1] = { pool = 6, id = id, obj = obj }
      end
    end
  end

  -- Pool 7: attack_base (only if enough shells, but always keep
  -- the current target so a mid-attack base doesn't vanish from the
  -- eval queue just because shells dipped to SHELLS_LOW)
  local has_hbases = not perc or (perc.hostile_base_count > 0)
  if has_hbases then
    local cur_is_attack_base = state.goal and state.goal.kind == "attack_base"
    local cur_mx = cur_is_attack_base and state.goal.mx or nil
    local cur_my = cur_is_attack_base and state.goal.my or nil
    for id, obj in pairs(world.bases) do
      if filter_attack_base(obj, state) then
        local is_current = (cur_mx and obj.mx == cur_mx and obj.my == cur_my)
        if has_shells or is_current then
          queue[#queue + 1] = { pool = 7, id = id, obj = obj }
        end
      end
    end
  end

  -- Sort queue by euclidean distance (closest evaluated first).
  -- tmx/tmy already in scope from line 2110-2111.
  table.sort(queue, function(a, b)
    local da = (a.obj.mx - tmx)^2 + (a.obj.my - tmy)^2
    local db = (b.obj.mx - tmx)^2 + (b.obj.my - tmy)^2
    return da < db
  end)

  -- Prioritize the current goal's target: move it to the front of the queue
  -- so step_eval_queue always refreshes its score first next cycle. This
  -- replaces the old pool 10 eval_current_attack_pill behavior.
  if state.goal and state.goal.kind == "attack_pill" then
    local gmx, gmy = state.goal.mx, state.goal.my
    for i = 1, #queue do
      local item = queue[i]
      if item.pool == 6 and item.obj.mx == gmx and item.obj.my == gmy then
        if i ~= 1 then
          table.remove(queue, i)
          table.insert(queue, 1, item)
        end
        break
      end
    end
  end

  -- Count candidates per pool for diagnostics

  state.eval_queue = queue
  state.eval_queue_pos = 1
  -- Partial results: pool_idx → { candidates = {}, best_cost, best_id, best_obj }
  state.pool_partial = {}
  -- Cost cache persists across cycles (initialized once)
  if not state.cost_cache then state.cost_cache = {} end

  -- Synchronously evaluate attack_tank at the START of the cycle so a
  -- newly-spotted enemy tank shows up in the pool grid with a real
  -- cost on the same tick it appears, instead of waiting for finalize
  -- (which can be many ticks later for a long eval cycle). Same
  -- spirit as the pool-4 capture_pill seeding above. attack_tank
  -- doesn't go through the per-tick eval queue (it's in
  -- FINALIZE_POOLS), so this is its analog of "evaluate at add time".
  -- finalize_pools will re-run it again at decision tick to pick up
  -- any updates from the intervening ticks; the redundant eval is
  -- cheap (1 cost-scan over enemy_tanks) and the final value wins.
  if not state.pool_cache then state.pool_cache = {} end
  do
    local ammo = (info.shells or 0) + (info.mines or 0)
    state.pool_cache[9] = eval_attack_tank(state, world, info, tmx, tmy,
                                            info.inboat, ammo)
  end

  -- Incremental cost_to disabled (A* heuristic bias causes missing targets)
  -- TODO: switch to Dijkstra (heuristic=0) for incremental to work correctly
end

-- =========================================================================
-- step_eval_queue — called every tick.  Pops up to 2 candidates from the
-- queue, runs cost_to for each, and accumulates partial results.
-- =========================================================================
-- Shared detail-string helpers (appended after || in formulas for the popup).
local function fmt_stale_detail(age, stale_cost)
  age = age or 0
  if age <= C.STALE_PENALTY_START then
    return string.format("age=%.0f < %.0f[STALE_PENALTY_START] → 0",
      age, C.STALE_PENALTY_START)
  end
  local raw = (age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
  return string.format(
    "max((%.0f[age] - %.0f[STALE_PENALTY_START]) x %.2f[STALE_PENALTY_PER_TICK], 0)"..
    " → max(%.0f x %.2f, 0) → max(%.0f, 0) → %.0f",
    age, C.STALE_PENALTY_START, C.STALE_PENALTY_PER_TICK,
    age - C.STALE_PENALTY_START, C.STALE_PENALTY_PER_TICK,
    raw, stale_cost)
end

-- Lazy formula builder for cost_cache entries.
-- step_eval_queue stores raw component numbers (_p pool-tag + _dc table)
-- instead of building formula strings on every tick. This function builds
-- the display string on first access (cold path: BrainTest UI only) and
-- caches the result in e.formula so subsequent calls are free.
local function get_formula_inner(e)
  local p = e._p
  local raw = e.raw
  local f
  -- Generic ally-claimed REJECT row.  Pools 2/3/4/5/6/7 use the per-tick
  -- sync to set _reject="ally_claimed" against fresh ally_state.  We
  -- render a uniform row so the pool grid shows who out-bid us and by
  -- how much.
  if e._reject == "ally_claimed" then
    local rem = e._reject_remaining or 0
    local their = e._ally_score
    local by    = e._ally_by
    local our   = e.cost or 0
    local their_str = their and string.format("%.0f", their) or "?"
    local diff_str  = (their and string.format(" (we_more_by=%.0f)", our - their)) or ""
    return string.format(
      "REJECT ally_claimed %dt @(%d,%d)||reject:ally_claimed %dt — p%s bid %s < ours %.0f%s",
      rem, e._mx or 0, e._my or 0,
      rem, tostring(by or "?"), their_str, our, diff_str)
  end
  if e._reject == "armour_too_low" then
    return string.format(
      "REJECT armour_too_low @(%d,%d)||reject:armour_too_low — arm=%d (need >= %d) vs pillHP=%d (>= %d)",
      e._mx or 0, e._my or 0,
      e._armour_at_reject or 0, C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR,
      e._pillhp_at_reject or 0, C.ATTACK_PILL_UNSAFE_HP_THRESHOLD)
  end
  if e._reject == "ally_pill_take_priority" then
    local rem = e._reject_remaining or 0
    local by  = e._priority_by
    return string.format(
      "REJECT ally_pill_take_priority %dt @(%d,%d)||reject:ally_pill_take_priority %dt — p%s is doing the take, holding off ~%.1fs",
      rem, e._mx or 0, e._my or 0,
      rem, tostring(by or "?"), rem / 50.0)
  end
  if p == 1 then
    -- Rejected refuel base: short-circuit with REJECT formula so the
    -- breakdown panel makes clear why the row exists with INF cost.
    -- Cost compute was skipped at queue-build time.
    if e._reject then
      local rem = e._reject_remaining or 0
      local rem_tok = (e._reject == "blocked" or e._reject == "stale"
                       or e._reject == "depleted")
                      and string.format(" %dt", rem) or ""
      local desc = ({
        blocked  = "tile blocked (cooldown)",
        stale    = "neutral, unseen too long",
        depleted = "recently observed low on stock we need",
        danger   = "pill fire on base — too dangerous to refuel",
      })[e._reject] or e._reject
      e.formula = string.format(
        "REJECT %s%s @(%d,%d)||reject:%s%s — %s",
        e._reject, rem_tok, e._mx or 0, e._my or 0,
        e._reject, rem_tok, desc)
      return e.formula
    end
    local _lgm_mult_d = e._lgm_mult or 1
    local _d_danger
    if _lgm_mult_d ~= 1 then
      _d_danger = string.format(
        "%.1f[danger_val] x %.1f[REFUEL_DANGER_WEIGHT] x %d (cautious mode) = %.0f",
        e._dv, C.REFUEL_DANGER_WEIGHT, _lgm_mult_d, e._dang)
    else
      _d_danger = string.format("%.1f[danger_val] x %.1f[REFUEL_DANGER_WEIGHT] = %.0f",
        e._dv, C.REFUEL_DANGER_WEIGHT, e._dang)
    end
    local _d_stale   = fmt_stale_detail(e._age, e._stale)
    local _d_contest = e._contest > 0
      and string.format("enemy tank within %.0f[CONTESTED_BASE_RANGE] tiles → %.0f[CONTESTED_BASE_PENALTY]",
            C.CONTESTED_BASE_RANGE, C.CONTESTED_BASE_PENALTY)
      or  string.format("no enemy tank within %.0f[CONTESTED_BASE_RANGE] tiles → 0",
            C.CONTESTED_BASE_RANGE)
    local _d_hyst = e._hyst ~= 0
      and string.format("already targeting this base → -%.0f[REFUEL_SWITCH_THRESHOLD]",
            C.REFUEL_SWITCH_THRESHOLD)
      or  "not currently targeting this base → 0"
    local _d_deplete = e._ratio >= 1.0
      and string.format("supply_ratio=%.2f (fully stocked) → 0", e._ratio)
      or  string.format("(1 - %.2f[supply_ratio]) x %.0f[REFUEL_DEPLETION_PENALTY] = %.0f",
            e._ratio, C.REFUEL_DEPLETION_PENALTY, e._dep)

    -- Live cost shape stashed by goal_selection's pool-1 cost competition.
    -- All five fields are populated together; if _urgency is nil the stash
    -- hasn't run yet (first tick / pool-1 cache built but not yet shaped).
    local _has_shape = e._urgency ~= nil
    local _shape_head = ""
    local _shape_detail = ""
    if _has_shape then
      local _u = e._urgency or 1
      local _bf = e._base_floor or 0
      local _db = e._defic_bonus or 0
      local _fm = e._fill_mult or 1
      local _lgm = e._lgm_wait_floor
      _shape_head = string.format(
        " × ur{%.2f} - def{%.0f} × fill{%.2f}%s",
        _u, _db, _fm,
        _lgm and string.format(" → lgm_wait_floor{%.0f}", _lgm) or "")
      local _arm     = e._arm or 0
      local _sh      = e._sh or 0
      local _arm_def = e._arm_def or 0
      local _sh_def  = e._sh_def or 0
      local _fill    = e._fill or 0
      local _arm_lin = math.min(1.0, _arm / C.ARMOUR_LOW)
      local _sh_lin  = math.min(1.0, _sh  / C.SHELLS_LOW)
      local _arm_sq  = _arm_lin * _arm_lin
      local _sh_sq   = _sh_lin  * _sh_lin
      local _d_urgency = string.format(
        "armour=%d/%d→%.2f², shells=%d/%d→%.2f² → min=%.2f, clamped(min=%.2f)=%.2f",
        _arm, C.ARMOUR_LOW, _arm_sq,
        _sh,  C.SHELLS_LOW, _sh_sq,
        math.min(_arm_sq, _sh_sq),
        C.REFUEL_URGENCY_MIN, _u)
      local _d_def   = string.format(
        "arm_def=%.2f, sh_def=%.2f → max=%.2f × %.0f[REFUEL_DEFICIT_BONUS] = %.0f",
        _arm_def, _sh_def, math.max(_arm_def, _sh_def), C.REFUEL_DEFICIT_BONUS, _db)
      local _d_fill  = (_fm > 1.0)
        and string.format(
          "fill=%.2f (above LOW) → 1 + %.2f x (%.2f[FULL_MULT]-1) = %.2f",
          _fill, _fill, C.REFUEL_FULL_COST_MULT, _fm)
        or  "fill=0 (at/below LOW thresholds) → 1.00"
      local _d_lgm   = _lgm
        and string.format("LGM returning, at this base → floor=%.0f (cost capped)", _lgm)
        or  "no LGM-wait active → no floor"
      _shape_detail = string.format("|urgency:%s|deficit:%s|fill:%s|lgm:%s",
        _d_urgency, _d_def, _d_fill, _d_lgm)
    end

    local _safe_token = e._safe_refuel
      and string.format(" × safe{%.2f}", C.REFUEL_NO_DANGER_DISCOUNT) or ""
    local _safe_detail = e._safe_refuel
      and string.format(
        "|safe:danger_val=0 → multiply final cost by %.2f[REFUEL_NO_DANGER_DISCOUNT]",
        C.REFUEL_NO_DANGER_DISCOUNT)
      or ""
    f = string.format(
      "A*{%.0f}@(%d,%d) + base{%.0f} + danger{%.0f} + stale{%.0f} + contest{%.0f} + hyst{%.0f} + deplete{%.0f}%s%s"..
      "||base:%.0f[REFUEL_BASE_COST]|danger:%s|stale:%s|contest:%s|hyst:%s|deplete:%s%s%s",
      raw, e._mx or 0, e._my or 0, C.REFUEL_BASE_COST, e._dang, e._stale, e._contest, e._hyst, e._dep, _shape_head, _safe_token,
      C.REFUEL_BASE_COST, _d_danger, _d_stale, _d_contest, _d_hyst, _d_deplete, _shape_detail, _safe_detail)
  elseif p == 6 then
    local _d_hp = string.format(
      "ATTACK_PILL_HP_MULT[%d] = %.2f (hand-tuned table: 5/10/18/28%% for hp 1-4, then linear 40%%→100%% over hp 5-15)",
      e._hpv, e._hp)
    local _d_anger = e._ttc > 0
      and string.format(
        "anger=%.0f > %.0f[ANGER_ATTACK_THRESHOLD]; ticks_to_calm=(%.0f-%.0f)x%.2f[PILL_ANGER_DECAY]=%.2f;"
        .." %.2f[ticks] x %.1f[ANGER_COST_PER_TICK] = %.0f",
        e._pa, C.ANGER_ATTACK_THRESHOLD, e._pa, C.ANGER_ATTACK_THRESHOLD,
        C.PILL_ANGER_DECAY, e._ttc, e._ttc, C.ANGER_COST_PER_TICK, e._anger)
      or string.format("anger=%.0f <= %.0f[ANGER_ATTACK_THRESHOLD] → 0",
        e._pa, C.ANGER_ATTACK_THRESHOLD)
    local _d_stale = fmt_stale_detail(e._age, e._stale)
    -- _wound shows as *wound{0.30} on the wounded pill (discount); on
    -- every OTHER pill it's the finish_other multiplier (>1, penalty),
    -- because step_eval_queue folds finish_other into the same wound_mult
    -- variable. Display them with distinct labels so it's obvious
    -- which kind of adjustment is in play.
    local _fin_mult = e._fin_mult or 1.0
    local _wound_detail
    if e._wound < 1 then
      local _cm = e._commit_mult or 1.0
      if _cm < 0.999 then
        _wound_detail = string.format("*wound{%.2f}*commit{%.2f}", e._wound / _cm, _cm)
      else
        _wound_detail = string.format("*wound{%.2f}", e._wound)
      end
    elseif _fin_mult > 1.001 then
      _wound_detail = string.format("*finish_other{%.2f}", _fin_mult)
    else
      _wound_detail = ""
    end
    local _d_finish_other = _fin_mult > 1.001
      and string.format(
        "wounded pill (id=%d, hp=%d, age=%d ticks) is at or below WOUNDED_FINISH_THRESHOLD=%d;"..
        " mult = 1 + (%.1f-1) × (%d-%d)/%d × max(0, 1 - %d/%d) = %.2f",
        e._fin_wpid or -1,
        e._fin_wphp or 0, e._fin_age or 0,
        C.WOUNDED_FINISH_THRESHOLD or 10,
        C.WOUNDED_FINISH_OTHER_PENALTY or 3.0,
        C.WOUNDED_FINISH_THRESHOLD or 10, e._fin_wphp or 0,
        C.WOUNDED_FINISH_THRESHOLD or 10,
        e._fin_age or 0, C.WOUNDED_FINISH_DECAY_TICKS or 500,
        _fin_mult)
      or  "1.00 (no wounded pill, or this IS the wounded pill, or HP > threshold, or decayed out)"
    local _ammo = e._ammo or 0
    local _sh_now = e._sh_now or 0
    local _sh_end = e._sh_end or 0
    local _ammo_str = (_ammo >= 1e29) and "INF" or string.format("%.0f", _ammo)
    local _d_ammo
    if _ammo >= 1e29 then
      _d_ammo = string.format(
        "shells=%d < pill_hp=%d → cannot finish; cost = INF (filtered)",
        _sh_now, e._hpv)
    elseif _ammo > 0 then
      _d_ammo = string.format(
        "shells=%d, pill_hp=%d, end=%d < SHELLS_LOW=%d → (%d-%d) × 5 = %d",
        _sh_now, e._hpv, _sh_end, C.SHELLS_LOW,
        C.SHELLS_LOW, _sh_end, _ammo)
    else
      _d_ammo = string.format(
        "shells=%d - pill_hp=%d = %d ≥ SHELLS_LOW=%d → 0",
        _sh_now, e._hpv, _sh_end, C.SHELLS_LOW)
    end
    -- "How spot was computed" detail. Method tag (dijkstra/astar/none),
    -- slate index when dijkstra was used, the tick smart_cost ran on,
    -- and the realized path tiles (truncated at 24 by the producer to
    -- fit the 400-char per-segment cap in pool_grid.cpp's parser).
    local _spot_method = e._spot_method or "?"
    local _spot_slate  = e._spot_slate
    local _spot_tick   = e._spot_tick
    local _spot_path   = e._spot_path or "(no path captured)"
    local _spot_pathlen= e._spot_path_len or 0
    local _d_spot
    if _spot_method == "dijkstra" then
      _d_spot = string.format(
        "method=dijkstra slate=%s tick=%s path_len=%d path=%s",
        tostring(_spot_slate), tostring(_spot_tick), _spot_pathlen, _spot_path)
    elseif _spot_method == "astar" then
      _d_spot = string.format(
        "method=astar (dijkstra slate hadn't reached spot) tick=%s path_len=%d path=%s",
        tostring(_spot_tick), _spot_pathlen, _spot_path)
    else
      _d_spot = string.format(
        "method=%s tick=%s — no path produced",
        tostring(_spot_method), tostring(_spot_tick))
    end
    local _tw = e._travel_wound or 1.0
    local _d_pickup = e._pickup_detail or "(no path captured)"
    local _danger_term = e._danger_mult
      and string.format(" * danger_nearby{%.2fx %dt}", e._danger_mult, e._danger_left or 0)
      or ""
    local _d_danger = e._danger_mult
      and string.format("recent abort due to enemy LGM near pill — %.2fx multiplier, %dt (~%.1fs) remaining",
            e._danger_mult, e._danger_left or 0, (e._danger_left or 0) / 50.0)
      or "no recent LGM-near-pill abort on this target"
    f = string.format(
      "(spot{%.0f}@(%d,%d) + pickup{%.0f}@(%d,%d)→(%d,%d)*wound_x2{%.2f} + (stale{%.0f} + diff{%.0f} + anger{%.0f} + xfire{%.0f} + intcpt{%.0f}) * hp{%.2f}%s + ammo{%s})%s"..
      "||spot cost is offset-aware (target pill's danger contribution subtracted via load_danger_offset before A*); NOT scaled by hp or wound"..
      "|pickup:%s|hp:%s|anger:%s|stale:%s|finish_other:%s|ammo:%s|spot:%s|danger_nearby:%s",
      e._spot, e._spot_mx or 0, e._spot_my or 0,
      e._travel,
      e._spot_mx or 0, e._spot_my or 0, e._mx or 0, e._my or 0,
      _tw,
      e._stale, e._diff, e._anger, e._xfire, e._intcpt,
      e._hp, _wound_detail, _ammo_str, _danger_term,
      _d_pickup, _d_hp, _d_anger, _d_stale, _d_finish_other, _d_ammo, _d_spot, _d_danger)
  elseif p == 7 then
    local _lgm_mult_b = e._lgm_mult or 1
    local _d_threat
    if _lgm_mult_b ~= 1 then
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT] x %d (cautious mode) = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, _lgm_mult_b, e._thr)
    else
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT] = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, e._thr)
    end
    local _d_stale = fmt_stale_detail(e._age, e._stale)
    f = string.format(
      "A*{%.0f}@(%d,%d) + base{%.0f} + threat{%.0f} + stale{%.0f}||base:%.0f[ATTACK_BASE_EXTRA_COST]|threat:%s|stale:%s",
      raw, e._mx or 0, e._my or 0, e._base, e._thr, e._stale, C.ATTACK_BASE_EXTRA_COST, _d_threat, _d_stale)
  elseif p == 4 then
    -- Rejected dead-pill rows: short-circuit with a "REJECT: <reason>"
    -- formula so the breakdown panel makes clear why the row exists
    -- with INF cost. Cost compute was skipped at queue-build time.
    if e._reject then
      local rem = e._reject_remaining or 0
      local rem_tok = (e._reject == "blocked" or e._reject == "stale")
                      and string.format(" %dt", rem) or ""
      local desc = ({
        in_tank = "pill is in flight (picked up by a tank)",
        blocked = "tile on retry cooldown (recent stuck-escape or pathfinder failed to reach it)",
        stale   = "not seen recently — fog of war",
      })[e._reject] or "pill exists on the map but cannot be picked this tick"
      f = string.format(
        "REJECT %s%s @(%d,%d)||reject:%s%s — %s",
        e._reject, rem_tok, e._mx or 0, e._my or 0,
        e._reject, rem_tok, desc)
    else
      local _lgm_mult_c = e._lgm_mult or 1
      local _cpill_danger_score = e._dv * C.CAPTURE_PILL_DANGER_SCALE * _lgm_mult_c
      local _intcpt = e._intcpt or 0
      -- Always-on intcpt term (matches attack_pill's formula display
      -- pattern at line 2795).  Empty when 0 was confusing — looked
      -- like the cost didn't include it at all, when really it just
      -- evaluated to zero.
      local intcpt_det = _intcpt > 0
        and string.format("|intcpt:%.0f (enemy can beat us to pill)", _intcpt)
        or  "|intcpt:0 (no enemy tank within INTERCEPT_MAX_RANGE that can beat us)"
      local _lgm_mult_str = (_lgm_mult_c ~= 1)
        and string.format(" × %d (cautious mode)", _lgm_mult_c) or ""
      local _lgm_mult_det = ""
      local _dm_str = e._dist_method or "dij"
      f = string.format(
        "base{%d} + dist{%.1f}[%s]@(%d,%d) + danger{%.1f} + intcpt{%.0f}||dist:%.0f^1.5 × %.3f[DIST_SCALE] = %.1f [%s]|danger:%.1f × %.3f[DANGER_SCALE]%s = %.1f%s%s",
        C.CAPTURE_PILL_BASE_COST, e._ds, _dm_str, e._mx or 0, e._my or 0, _cpill_danger_score, _intcpt,
        raw, C.CAPTURE_PILL_DIST_SCALE, e._ds, _dm_str,
        e._dv, C.CAPTURE_PILL_DANGER_SCALE, _lgm_mult_str, _cpill_danger_score, _lgm_mult_det, intcpt_det)
    end
  else
    f = string.format("A*{%.0f}@(%d,%d) + stale{%.0f}||stale:%s",
      raw, e._mx or 0, e._my or 0, e._stale, fmt_stale_detail(e._age, e._stale))
  end
  e.formula = f
  return f
end

local function get_formula(e)
  local f = e.formula
  if f == nil then
    if not e._p or not e.raw then return "" end
    local ok, ff = pcall(get_formula_inner, e)
    if ok and ff then
      f = ff
    else
      e.formula = ""
      return ""
    end
  end
  -- REJECT row already formatted by inner — no trailing term to append.
  if e._reject == "ally_claimed"
     or e._reject == "armour_too_low"
     or e._reject == "ally_pill_take_priority" then
    return f
  end
  -- Cost-baked ally penalty (pool 1 soft +100, pool 8 hard +10000).  Only
  -- emit the term when the penalty actually exists on this entry.
  local pen = e.ally_claimed_pen or 0
  local term_disp, term_map
  if pen > 0 then
    term_disp = string.format(" + ally_claimed{%d}", pen)
    term_map  = string.format("|ally_claimed:yielding to ally (p%s)",
                              tostring(e.ally_claimed_by or "-"))
  elseif e._ally_score then
    -- Hard-REJECT pool, ally is bidding, but we kept the candidate
    -- (either we're cheaper or we're within the steal band).  Display
    -- the bid + relationship so the band is visible without polluting
    -- the cost equation.
    local our   = e.cost or 0
    local their = e._ally_score
    local thr   = C.ALLY_CLAIMED_STEAL_THRESHOLD or 0
    local rel
    if our + thr < their then
      rel = string.format("we cheaper by %.0f", their - our)
    elseif their + thr < our then
      rel = string.format("they cheaper by %.0f (would REJECT)", our - their)
    else
      rel = string.format("within steal band +/-%d", thr)
    end
    term_disp = ""  -- no cost contribution
    term_map  = string.format("|ally_claimed:p%s bid %.0f, ours %.0f — %s",
                              tostring(e._ally_by or "?"), their, our, rel)
  else
    -- No penalty, no ally claim — suppress the term entirely.
    return f
  end
  local sep_start = f:find("||", 1, true)
  if sep_start then
    return f:sub(1, sep_start - 1) .. term_disp .. " "
           .. f:sub(sep_start) .. term_map
  else
    return f .. term_disp .. " ||" .. term_map:sub(2)
  end
end

function M.step_eval_queue(state, world, info)
  _SELF_PN = info.player_number or -1
  -- Skip if pathfinder is actively running — cost_to would destroy its state
  local pf = state.pf
  if pf and pf.status == "running" then return end

  -- Capacity tier eval_iv: pop a candidate only every Nth tick when
  -- throttled. Tier 10 → every tick. Tier 1 → every 5 ticks.
  local _eval_iv = (state._capacity and state._capacity.eval_iv) or 1
  if _eval_iv > 1 and ((state.tick or 0) % _eval_iv) ~= 0 then return end

  local queue = state.eval_queue
  if not queue then return end
  local pos = state.eval_queue_pos or 1
  if pos > #queue then
    -- Queue exhausted — rebuild only when sitting on a base refueling
    -- and only if the freshest cache entry is stale enough to be worth it
    if state.goal and state.goal.kind == "refuel_at_base" and info.base then
      local now = state.tick or 0
      local cache = state.cost_cache or {}
      local freshest_age = math.huge
      for _, v in pairs(cache) do
        local age = now - (v.tick or 0)
        if age < freshest_age then freshest_age = age end
      end
      if freshest_age < 100 then return end  -- everything is fresh enough, skip
      M.build_eval_queue(state, world, info)
      queue = state.eval_queue
      if not queue then return end
      pos = state.eval_queue_pos or 1
      if pos > #queue then return end
    else
      return
    end
  end

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

  -- Chunked pill-eval pre-probe (BRAIN_DEBUG_MODE only — the C fast path
  -- is plenty fast outside debug). Peek the next GOAL_CANDS_PER_TICK
  -- pool-6 candidates and advance one chunk on each whose diff_cache
  -- entry is stale.  If any is still in_progress, bail without draining
  -- the queue this tick so the bar can fill across ticks and the work
  -- is actually spread over time at lower capacity tiers.

  local count = 0
  while pos <= #queue and count < C.GOAL_CANDS_PER_TICK do
    local item = queue[pos]
    pos = pos + 1
    count = count + 1

    local _t0 = clock_us()
    local pool_idx = item.pool
    local obj = item.obj
    local id = item.id
    local _diff_us = 0  -- pool-6 eval_pill_difficulty timing
    local _spot_us = 0  -- pool-6 spot+travel timing

    -- Initialize partial result for this pool if needed
    if not partial[pool_idx] then
      partial[pool_idx] = { candidates = {}, best_cost = math.huge, best_id = nil, best_obj = nil }
    end
    local pr = partial[pool_idx]

    -- Rejected candidates (in_tank / blocked / stale dead pills): the
    -- entry already exists in cost_cache with cost = INF and a _reject
    -- tag. Skip the expensive cost compute — they're displayed as a
    -- greyed-out row, not a real choice. Also recompute remaining-
    -- ticks each tick so countdowns tick down in the panel.
    if item.reject then
      local ck = pool_idx .. ":" .. id
      local entry = state.cost_cache[ck]
      if entry then
        if item.reject.reason == "blocked" and state.blocked then
          local bk = U.mkey(obj.mx, obj.my)
          local until_tick = state.blocked[bk]
          entry._reject_remaining = (until_tick and (until_tick - now)) or 0
          if entry._reject_remaining < 0 then entry._reject_remaining = 0 end
        elseif item.reject.reason == "stale" and obj.last_seen then
          entry._reject_remaining = (now - obj.last_seen) - C.STALE_SKIP_TICKS
          if entry._reject_remaining < 0 then entry._reject_remaining = 0 end
        elseif item.reject.reason == "depleted" and obj.obs_tick then
          entry._reject_remaining = C.REFUEL_OBS_STALE - (now - obj.obs_tick)
          if entry._reject_remaining < 0 then entry._reject_remaining = 0 end
        end
        entry.tick = now
      end
      goto continue
    end

    -- All pools use KIND_NORMAL. Pool 6 (attack_pill) computes spot_cost
    -- with an offset-aware A* below — the target pill's contribution to
    -- the danger field is temporarily subtracted via load_danger_offset
    -- so the spot cost reflects an "as-if-the-pill-were-dead" approach.
    --
    -- Note: capture_pool / CAPTURE_THREAT_WEIGHT used to scale the A*
    -- fallback's danger weighting. With dij-only there's no fallback,
    -- and Dijkstra weights are baked into the slate at start time —
    -- so the capture-pool override is no longer applicable here.
    -- Pool 4 (capture_pill) does its own distance calculation via
    -- compute_pool4_cost (8-neighbor sweep with KIND_NORMAL — the
    -- pill is dead). Skip the smart_cost block entirely for pool 4 to avoid
    -- a wasted A*/Dijkstra call per candidate per tick. raw_cost is
    -- backfilled from compute_pool4_cost's return below.
    local cost_dx, cost_dy = obj.mx, obj.my
    local raw_cost = 0
    local _t_adj = 0
    local _t_smart = 0
    if pool_idx ~= 4 then
      -- Dijkstra-only path: no A* fallback. The cold-start window
      -- (~14 ticks until long Dijkstra completes) used to pay 7-10 ms
      -- per A*-fallback candidate; deferring is free since
      -- STARTUP_HOLD_TICKS prevents the bot from moving anyway. After
      -- Dijkstra is done, every reachable tile resolves in O(1).
      -- Truly unreachable tiles (small islands without a boat) keep
      -- returning math.huge — the pool selector treats them as
      -- non-selectable, same as before.
      --
      -- ds_override (capture pools) used to scale the A* fallback's
      -- danger weighting. Dijkstra weights are baked into the slate
      -- at start time, so the override is a no-op here — left out.
      if obj.health and obj.health > 0 then
        if pool_idx == 6 then
          -- attack_pill: route to cheapest adjacent tile (pill is still an
          -- obstacle) but subtract the target pill's own danger contribution
          -- from the path — the bot will neutralise it en-route so its fire
          -- field shouldn't inflate the approach cost.
          local _ta = clock_us()
          local _, ax, ay = cpf.cheapest_adjacent_dij(KIND_NORMAL,
            obj.mx, obj.my, boat_flag)
          _t_adj = clock_us() - _ta
          if ax then cost_dx, cost_dy = ax, ay end
          local pcontrib = threat.pill_contrib and
                           threat.pill_contrib[obj.my * 256 + obj.mx]
          local _ts = clock_us()
          raw_cost = cpf.smart_cost_minus_pill_danger_dij_only(
            KIND_NORMAL, cost_dx, cost_dy,
            pcontrib, obj.mx, obj.my, boat_flag)
          _t_smart = clock_us() - _ts
        else
          -- For other live pills/bases, route to cheapest adjacent tile.
          local _ta = clock_us()
          local _, ax, ay = cpf.cheapest_adjacent_dij(KIND_NORMAL,
            obj.mx, obj.my, boat_flag)
          _t_adj = clock_us() - _ta
          if ax then cost_dx, cost_dy = ax, ay end
          local _ts = clock_us()
          raw_cost = cpf.smart_cost_dij_only(KIND_NORMAL, cost_dx, cost_dy, boat_flag)
          _t_smart = clock_us() - _ts
          -- Diag: log pool 3 (capture_base) cost lookups so we can see
          -- whether Dijkstra is returning finite values yet.
          if BRAIN_PROFILE_LOG and pool_idx == 3 then
            local rc = (raw_cost == math.huge) and "INF" or string.format("%.1f", raw_cost)
            opt.append("optimize.log", string.format(
              "  [diag] update_pool_cache pool=3 id=%s obj=(%d,%d) cheapest_adj=(%s,%s) raw_cost=%s tick=%d",
              tostring(id), obj.mx, obj.my,
              tostring(ax), tostring(ay), rc, now))
          end
        end
      else
        local _ts = clock_us()
        raw_cost = cpf.smart_cost_dij_only(KIND_NORMAL, cost_dx, cost_dy, boat_flag)
        _t_smart = clock_us() - _ts
      end
    end

    -- For attack_pill / attack_base candidates, capture shells-on-arrival so
    -- strategy.compute_refuel_targets can account for wall-shoot consumption
    -- when estimating mission shell needs.
    local cand_shells_on_arrival = nil
    if pool_idx == 6 or pool_idx == 7 then
      cand_shells_on_arrival = cpf.dijkstra_shells_at(KIND_NORMAL, obj.mx, obj.my)
                            or cpf.astar_shells_at(obj.mx, obj.my)
    end
    local _used_dij_for_raw = (raw_cost < 1e29) and C.DIJKSTRA_USE_FOR_GOALS
    local _t_raw = clock_us() - _t0
    local cache_key = pool_idx .. ":" .. id
    if not state.cost_cache then state.cost_cache = {} end

    -- Pool-specific adjustments applied inline
    if pool_idx == 1 then
      -- Refuel: uses its own scoring (danger + staleness + contested + hysteresis + depletion)
      local danger_val = threat.at(obj.mx, obj.my)
      -- Cautious-mode danger multiplier (see init.lua state.cautious_mode
      -- and constants.lua CAUTIOUS_MODE_MULT).  When the bot is
      -- in cautious mode, danger terms get pumped so exposure costs
      -- much more — biases hard toward safer refuel candidates.
      local _lgm_mult = state.cautious_mode and C.CAUTIOUS_MODE_MULT or 1
      local danger_cost = danger_val * C.REFUEL_DANGER_WEIGHT * _lgm_mult

      -- Depletion penalty: penalize bases that can't get us above LOW thresholds.
      -- need_* anchored at LOW (not TANK_FULL) so supply_ratio=1 once the base
      -- can cover the minimum needed to escape the danger zone.
      local depletion_cost = 0
      local obs_arm = obj.obs_armour or 90
      local obs_sh  = obj.obs_shells or 90
      -- How much we need to reach the LOW threshold (not to top off)
      local need_arm = math.max(0, C.ARMOUR_LOW - info.armour)
      local need_sh  = math.max(0, C.SHELLS_LOW  - info.shells)
      -- Ratio of what the base can provide vs what we need (0=empty, 1=covers LOW)
      local supply_ratio = 1.0
      if need_arm > 0 then supply_ratio = math.min(supply_ratio, obs_arm / need_arm) end
      if need_sh  > 0 then supply_ratio = math.min(supply_ratio, obs_sh  / need_sh)  end
      supply_ratio = math.min(1.0, supply_ratio)
      -- Penalty: inverse of supply ratio (empty base = big penalty)
      if supply_ratio < 1.0 then
        depletion_cost = (1.0 - supply_ratio) * C.REFUEL_DEPLETION_PENALTY
      end

      local stale_cost = 0
      local _p1_age = (obj.owner == "neutral" and obj.last_seen and now > 0)
                      and (now - obj.last_seen) or 0
      if _p1_age > C.STALE_PENALTY_START then
        stale_cost = (_p1_age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
      end
      local contested_cost = 0
      local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
      for _, et in ipairs(enemy_tanks) do
        if U.mdist(et.mx, et.my, obj.mx, obj.my) <= C.CONTESTED_BASE_RANGE and et.speed > 0 then
          contested_cost = C.CONTESTED_BASE_PENALTY
          break
        end
      end
      -- Angry-pill-near-base: don't refuel here if a hostile pill is mad and
      -- in range to shoot us while we sit (replaces the old Override 2 skip).
      local pill_threats_p1 = state.perc and state.perc.pill_threats or {}
      for _, pt in ipairs(pill_threats_p1) do
        if pt.anger and pt.anger > C.ANGRY_REFUEL_THRESHOLD
           and pt.pill and U.mdist(pt.pill.mx, pt.pill.my, obj.mx, obj.my) <= C.PILL_FIRE_RANGE then
          contested_cost = math.max(contested_cost, C.ANGRY_PILL_AT_BASE_PENALTY)
          break
        end
      end
      local hysteresis_cost = 0
      local gk = state.goal and state.goal.kind
      if (gk == "flee_to_base" or gk == "refuel_at_base")
         and obj.mx == state.goal.mx and obj.my == state.goal.my then
        hysteresis_cost = -C.REFUEL_SWITCH_THRESHOLD
      end
      -- Clamp hysteresis to non-negative so the current target gets stickier
      -- (when same goal) but never goes BELOW zero — avoids refuel cost
      -- being driven negative and dominating dead pills / other cheap goals.
      local score = raw_cost + C.REFUEL_BASE_COST + danger_cost + stale_cost + contested_cost + math.max(hysteresis_cost, 0) + depletion_cost
      -- Safe-refuel discount: when this base sits in zero-danger territory
      -- (no pill / no tank threat), trim the cost so it wins ties against
      -- bases with even mild exposure.  Compounds with everything above.
      local _safe_refuel = (danger_val == 0)
      if _safe_refuel then
        score = score * C.REFUEL_NO_DANGER_DISCOUNT
      end

      -- Ally-claimed penalty (refuel-specific small value).  Live look
      -- up so the per-target cost reflects ally claims regardless of
      -- whether goal_selection has run yet.  Cost-comparison yield
      -- rule (same as pool 6+ below): cheaper bot keeps it, tiebreak
      -- on player_number.  Old "we were here first" exemption is gone
      -- because it broke the simultaneous-commit case (both bots had
      -- the same state.goal so both skipped the check and ran to the
      -- same base).
      local _p1_ac_pen, _p1_ac_by = 0, nil
      do
        for ally_pn, slot in ally_state.iter_active(now, 1750) do
          if ally_pn ~= info.player_number then
            local info_h = slot.info
            if info_h.goal == "refuel_at_base" then
              local aid = tonumber(info_h.target)
              if (aid and id and aid == id)
                 or (tonumber(info_h.mx) == obj.mx and tonumber(info_h.my) == obj.my) then
                local their_cost = tonumber(info_h.cost)
                local we_keep
                if their_cost == nil then
                  we_keep = (info.player_number < ally_pn)
                elseif score < their_cost then
                  we_keep = true
                elseif score > their_cost then
                  we_keep = false
                else
                  we_keep = (info.player_number < ally_pn)
                end
                if we_keep then break end
                _p1_ac_pen = C.ALLY_CLAIMED_REFUEL_PENALTY
                _p1_ac_by  = ally_pn
                score = score + _p1_ac_pen
                break
              end
            end
          end
        end
      end

      state.cost_cache[cache_key] = {
        cost = score, raw = raw_cost, tick = now, _p = 1,
        _dv=danger_val, _dang=danger_cost, _age=_p1_age,
        _stale=stale_cost, _contest=contested_cost,
        _hyst=hysteresis_cost, _ratio=supply_ratio, _dep=depletion_cost,
        _lgm_mult = _lgm_mult,
        _safe_refuel = _safe_refuel or nil,
        ally_claimed_pen = _p1_ac_pen > 0 and _p1_ac_pen or nil,
        ally_claimed_by  = _p1_ac_by,
      }

      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, own = obj.owner,
        travel = raw_cost, danger = danger_val, score = score,
      }
      if score < pr.best_cost then
        pr.best_cost = score; pr.best_id = id; pr.best_obj = obj
      end
    else
      -- Generic pools: raw cost + staleness penalty + pool-specific extras
      local stale_cost = 0
      local _gen_age = (obj.last_seen and now > 0) and (now - obj.last_seen) or 0
      if _gen_age > C.STALE_PENALTY_START then
        stale_cost = (_gen_age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
      end
      -- Attack_base extra costs (same as finalization path)
      local base_extra, threat_cost, _threat_val = 0, 0, 0
      local _p7_lgm_mult = 1
      if pool_idx == 7 then
        base_extra  = C.ATTACK_BASE_EXTRA_COST
        _threat_val = threat.at(obj.mx, obj.my)
        _p7_lgm_mult = state.cautious_mode and C.CAUTIOUS_MODE_MULT or 1
        threat_cost = _threat_val * C.ATTACK_BASE_THREAT_WEIGHT * _p7_lgm_mult
      end
      -- HP multiplier for attack_pill: weaker pills scale the entire cost
      -- down.  Lookup table (see ATTACK_PILL_HP_MULT at module top) — knee
      -- at HP 1-4 keeps near-dead pills cheap; linear 40-100% over 5-15.
      local hp_mult = 1.0
      if pool_idx == 6 then
        local hp = obj.health or C.PILLS_MAX_HEALTH
        if hp < 1 then hp = 1 elseif hp > 15 then hp = 15 end
        hp_mult = ATTACK_PILL_HP_MULT[hp]
      end
      local travel = raw_cost
      local capture_mult = 1.0  -- pool 4 uses its own formula below

      -- Extra costs for attack_pill (pool 6)
      local anger_cost, xfire_cost, intcpt_cost, wound_mult = 0, 0, 0, 1.0
      local diff_cost, spot_cost = 0, 0
      local ammo_cost = 0  -- shells gate: COST_INF if we can't finish the
                           -- pill, otherwise 25 per shell under SHELLS_LOW
                           -- the take would leave us at.
      -- "Finish what you started" multiplier on this candidate (>1 only
      -- when this is NOT the wounded pill AND the wounded pill is at
      -- low HP within the decay window). 1.0 = no penalty, surfaced in
      -- the per-pool detail formula via entry._fin_mult below.
      local _finish_other_mult  = 1.0
      local _finish_other_wp_hp = 0
      local _finish_other_age   = 0
      local _finish_other_wp_id = -1
      local spot_found_mx, spot_found_my = 0, 0  -- hoisted for formula
      local pill_anger, _ticks_to_calm = 0, 0  -- hoisted for formula detail
      -- Spot-cost provenance for the detail panel (pool 6 only).
      local goal_spot_method   = nil
      local goal_spot_slate    = nil
      local goal_spot_tick     = nil
      local goal_spot_path_str = nil
      local goal_spot_path_len = 0
      local goal_pickup_detail = nil
      local goal_pickup_path   = nil  -- list of {x,y} for viz
      if pool_idx == 6 then
        -- Quick difficulty scan + best spot cost.
        -- Cache the scan per pill — result only changes when pill HP or
        -- nearby walls change, which is rare between queue cycles.
        local _t_diff = clock_us()
        local diff_cache = state._pill_diff_cache
        if not diff_cache then diff_cache = {}; state._pill_diff_cache = diff_cache end
        local dck = obj.mx .. ":" .. obj.my .. ":" .. (state.phase or "")
        local dc = diff_cache[dck]
        local diff_score, best_spot, _spots
        -- Never request detailed spots here — the C fast path requires
        -- detailed=false, and spots are only used for viz overlays.
        -- The attack_scan_spots_all_pills viz reads from plan_position's
        -- own detailed evaluation instead.
        local force_detailed = BRAIN_DEBUG_MODE
        local just_evaluated = false

        -- ── Distance-tiered TTL ──
        --
        -- evaluate_pill_difficulty is the dominant pool-6 cost (~1.5–3 ms
        -- per cache miss) and runs once per pill per TTL. Tank-relative
        -- distance determines how stale a re-evaluation can be:
        --
        --   Tier 1 (close, sqdist <  100 ≈ 10 tiles): TTL  50 ticks
        --   Tier 2 (mid,   sqdist <  900 ≈ 30 tiles): TTL 150 ticks
        --   Tier 3 (far,   sqdist >= 900            ): TTL 500 ticks
        --
        -- Distant pills aren't viable attack targets in the next second
        -- anyway; their score doesn't need to be fresh. When the tank
        -- closes in, the tier shrinks and we force an immediate re-eval
        -- (covered below).
        --
        -- FIRST evaluations always run — the queue already rate-limits
        -- to GOAL_CANDS_PER_TICK candidates/tick, so all 16 pills get
        -- their first score in roughly 16 ticks before any one re-evals.
        -- This stops the bot from picking a "strange" first attack_pill
        -- target based on partial-tier data while distant candidates
        -- haven't been scored yet. Earlier we deferred first eval of
        -- distant pills to smooth startup CPU; that's now handled by
        -- startup_mode (no pool eval runs at all in ticks 1-10).
        local _dx_t = obj.mx - tmx
        local _dy_t = obj.my - tmy
        local _sqdist = _dx_t * _dx_t + _dy_t * _dy_t
        local tier_ttl, tier_idx
        if     _sqdist < 100 then tier_ttl, tier_idx = 50,  1
        elseif _sqdist < 900 then tier_ttl, tier_idx = 150, 2
        else                       tier_ttl, tier_idx = 500, 3
        end
        local _ttl_mult = (state._capacity and state._capacity.ttl_mult) or 1.0
        if _ttl_mult ~= 1.0 then tier_ttl = math.floor(tier_ttl * _ttl_mult) end

        local needs_eval
        if not dc then
          needs_eval = true                              -- first eval, no defer
        elseif dc.hp ~= (obj.health or 0) then
          needs_eval = true                              -- HP changed
        elseif dc.tier and tier_idx < dc.tier then
          needs_eval = true                              -- moved closer; refresh
        elseif force_detailed and not dc.spots then
          needs_eval = true                              -- viz wants spots
        elseif (now - dc.tick) >= tier_ttl then
          needs_eval = true                              -- TTL expired
        else
          needs_eval = false
        end

        if not needs_eval then
          if dc then
            diff_score  = dc.score
            best_spot   = dc.spot
            _spots      = dc.spots
            if dc.pickup_travel ~= nil then
              travel = dc.pickup_travel  -- nil = not yet computed; A* runs on first eval below
              if BRAIN_POOL_VIZ and dc.spot then
                goal_pickup_detail = string.format("cached=%.0f spot(%d,%d)→pill(%d,%d)",
                  travel, dc.spot.mx, dc.spot.my, obj.mx, obj.my)
              end
            end
          else
            -- First-eval deferred. Stub keeps the candidate parked
            -- without paying for a scan.
            diff_score = 999
            best_spot  = nil
            _spots     = nil
          end
        else
          local _scan_step = (state._capacity and state._capacity.scan_step) or 5
          -- In debug mode the chunked pre-probe at the top of this fn
          -- has already produced a fresh result in state._pill_eval_cache
          -- before we ever get here (the probe bails the whole tick when
          -- any in-flight sweep is still running). Read from the cache
          -- instead of re-doing the 72-angle sweep inline.
          local _pe = state._pill_eval_cache and state._pill_eval_cache[id]
          diff_cache[dck] = { score = diff_score, spot = best_spot,
                              spots = _spots,  -- nil unless force_detailed
                              mx = obj.mx, my = obj.my,
                              hp = obj.health or 0, tick = now,
                              tier = tier_idx }
          just_evaluated = true
        end
        -- Per-pill candidate overlays now emit every tick from the
        -- staged-reveal pass below (driven by state._pill_diff_cache),
        -- not from the just_evaluated branch — so the all → bucket →
        -- winner progression plays out across consecutive frames for
        -- every cached pill.
        diff_cost = diff_score or 999
        local _diff_us = clock_us() - _t_diff
        local _spot_us = 0
        if best_spot then
          spot_found_mx = best_spot.mx
          spot_found_my = best_spot.my
          local _t_spot = clock_us()
          -- KIND_NORMAL Dijkstra lookup with target pill subtraction.
          -- C side walks the slate's parent chain back to source and
          -- subtracts pcontrib[tile] * danger_scale * inv_speed at each
          -- non-source tile, returning the exact "as-if-this-pill-were-
          -- dead" cost. Replaces smart_cost + self_dr walk-and-subtract.
          local _pck = obj.my * 256 + obj.mx
          local _pc  = threat.pill_contrib and threat.pill_contrib[_pck]
          spot_cost = cpf.dijkstra_lookup_subtract_by_kind(
                        cpf.KIND_NORMAL, best_spot.mx, best_spot.my, boat_flag, _pc)
          if spot_cost >= 1e9 then spot_cost = 500 end
          -- Travel = spot → dead pill (pill will be dead by the time we
          -- reach the spot, so this is a short capture walk).
          -- Estimate is accurate here: spot has LOS to the pill and the
          -- distance is within PILL_FIRE_RANGE tiles.
          -- pickup: walk from spot to dead pill.
          -- Delegate to the helper which picks the best available slate
          -- One-shot A* from spot → pill so the cost reflects the actual
          -- "walk after the pill is dead" leg (Dijkstra slates are tank-
          -- rooted and would give tank→pill instead). Then walk the
          -- returned path and subtract this pill's own danger contrib +
          -- the 32767 overlay on its tile, matching what the planner
          -- would experience post-kill.
          if dc == nil or dc.pickup_travel == nil or just_evaluated then  -- not cached or spot changed
          do
            local pck = obj.my * 256 + obj.mx
            local pc  = threat.pill_contrib and threat.pill_contrib[pck]
            cpf.set_overlay(obj.mx, obj.my, 0)
            if pc then cpf.load_danger_offset(pc, -1) end
            local _stuck_bl = state.stuck_blacklist
            if _stuck_bl then
              for k in pairs(_stuck_bl) do
                cpf.set_overlay(U.mkey_x(k), U.mkey_y(k), 0)
              end
            end
            cpf.set_config("armour_drain_rate", 0)
            local raw = cpf.cost_to(best_spot.mx, best_spot.my, obj.mx, obj.my,
                                    boat_flag, shells, trees, mines, armour, 4096)
            cpf.set_config("armour_drain_rate", 0.02)
            cpf.clear_danger_offset()
            -- Only restamp the impassable overlay if the pill is still
            -- alive. A dead pill's tile is walkable for capture; leaving
            -- 32767 here would make subsequent A* treat it as a wall.
            if (obj.health or 0) > 0 then
              cpf.set_overlay(obj.mx, obj.my, 32767)
            else
              cpf.set_overlay(obj.mx, obj.my, 0)
            end
            if _stuck_bl then
              for k in pairs(_stuck_bl) do
                cpf.set_overlay(U.mkey_x(k), U.mkey_y(k), 1500)
              end
            end
            travel = math.max(0, raw)
            -- Cache pickup travel so subsequent queue pops skip the A*.
            -- Same TTL as diff_cache (spot only changes on re-eval).
            if diff_cache[dck] then diff_cache[dck].pickup_travel = travel end
          end  -- end pickup A* do-block
          end  -- end pickup cache check
          if BRAIN_POOL_VIZ then
            local pck2 = obj.my * 256 + obj.mx
            local has_contrib = threat.pill_contrib and threat.pill_contrib[pck2] ~= nil
            goal_pickup_detail = string.format("A*=%.0f spot(%d,%d)→pill(%d,%d)%s",
              travel, best_spot.mx, best_spot.my, obj.mx, obj.my,
              has_contrib and " [contrib]" or " [no-contrib]")
          end
          _spot_us = clock_us() - _t_spot

          -- Capture which pathfinder produced spot_cost + the realized
          -- path tiles, so the pool detail panel can show the user
          -- exactly how this number was reached (mirrors smart_cost's
          -- own dijkstra-then-A* fallback order).
          -- Multi-slate Dijkstra trace: dijkstra_trace_path_by_kind
          -- mirrors lookup_by_kind's slate-walk so we trace from the
          -- exact slate that produced spot_cost. The single-slate
          -- variant picks freshest-active and routinely misses when an
          -- older slate is the one that actually reached dest. Then
          -- only fall back to A* (cost_to) trace when no slate has it.
          local _path_tiles = cpf.dijkstra_trace_path_by_kind(cpf.KIND_NORMAL,
                                                               best_spot.mx, best_spot.my)
          local _spot_method, _spot_slate
          if _path_tiles and #_path_tiles > 0 then
            _spot_method = "dijkstra"
            _spot_slate  = cpf.dijkstra_find_best(cpf.KIND_NORMAL)
          else
            -- smart_cost fell back to cost_to (one-shot A*). cost_to
            -- resets pf->status/dest at the end so cpf.trace_path()
            -- returns empty even when the search reached dest. Use
            -- the explicit-dest variant which walks the parent chain
            -- using the just-finished search's epoch state.
            _path_tiles  = cpf.trace_last_search(best_spot.mx, best_spot.my)
            _spot_method = (_path_tiles and #_path_tiles > 0) and "astar" or "(no path)"
            _spot_slate  = nil
          end
          -- Serialize the path. Keep the first SPOT_PATH_FRONT tiles
          -- (so the user sees how the bot leaves the tank) AND always
          -- the last SPOT_PATH_TAIL=5 tiles (those are inside the
          -- pill's range disk and matter most for the planner). For
          -- long paths the middle is collapsed to "(... +N ...)" so
          -- the segment stays under the renderer's per-segment cap.
          local SPOT_PATH_FRONT = 14
          local SPOT_PATH_TAIL  = 5
          local spot_path_str
          if not _path_tiles or #_path_tiles == 0 then
            spot_path_str = "(empty)"
          else
            local n = #_path_tiles // 2  -- waypoint count
            local parts = {}
            if n <= SPOT_PATH_FRONT + SPOT_PATH_TAIL then
              for i = 1, n do
                parts[#parts + 1] = string.format("(%d,%d)",
                  _path_tiles[2*i-1], _path_tiles[2*i])
              end
            else
              for i = 1, SPOT_PATH_FRONT do
                parts[#parts + 1] = string.format("(%d,%d)",
                  _path_tiles[2*i-1], _path_tiles[2*i])
              end
              parts[#parts + 1] = string.format("(... +%d ...)",
                n - SPOT_PATH_FRONT - SPOT_PATH_TAIL)
              for i = n - SPOT_PATH_TAIL + 1, n do
                parts[#parts + 1] = string.format("(%d,%d)",
                  _path_tiles[2*i-1], _path_tiles[2*i])
              end
            end
            spot_path_str = table.concat(parts, " ")
          end
          goal_spot_method   = _spot_method
          goal_spot_slate    = _spot_slate
          goal_spot_tick     = now
          goal_spot_path_str = spot_path_str
          goal_spot_path_len = _path_tiles and #_path_tiles // 2 or 0

          -- (self_dr removed: the offset-aware A* above bakes the
          -- as-if-pill-dead discount directly into spot_cost.)
        end
        if (_diff_us > 1000 or _spot_us > 1000) then
          if BRAIN_PROFILE_LOG then opt.append("optimize.log", string.format(
            "  [diag] pool6 cand id=%s diff=%.2f spot=%.2f just_evaluated=%s force_detailed=%s",
            tostring(id), _diff_us / 1000, _spot_us / 1000,
            tostring(just_evaluated), tostring(force_detailed))) end
        end
        -- Anger
        pill_anger = obj.anger or 0
        if pill_anger > C.ANGER_ATTACK_THRESHOLD then
          _ticks_to_calm = (pill_anger - C.ANGER_ATTACK_THRESHOLD) * C.PILL_ANGER_DECAY
          if _ticks_to_calm < C.ANGER_WAIT_MAX then
            anger_cost = _ticks_to_calm * C.ANGER_COST_PER_TICK
          else
            _ticks_to_calm = 0  -- capped to ANGER_WAIT_MAX, effectively no wait penalty
          end
        end
        -- Crossfire
        for _, pm in pairs(world.pills) do
          if (pm.mx ~= obj.mx or pm.my ~= obj.my)
             and (pm.owner == "hostile" or pm.owner == "neutral")
             and (pm.health or 0) > 0 then
            local d = U.mdist(obj.mx, obj.my, pm.mx, pm.my)
            if d <= C.PILL_FIRE_RANGE + C.ATTACK_PILL_STANDOFF then
              local max_d = C.PILL_FIRE_RANGE + C.ATTACK_PILL_STANDOFF
              local proximity = 1.0 - d / (max_d + 1)
              xfire_cost = xfire_cost + C.GOAL_CROSSFIRE_PENALTY * proximity
            end
          end
        end
        -- Intercept
        do
          local enemy_tanks = state.perc and state.perc.enemy_tanks or {}
          intcpt_cost = intercept_penalty_ttk(obj.mx, obj.my, obj.health or 0, enemy_tanks)
        end
        -- Wounded discount. 0.3x is the in-pool (sibling-pill)
        -- discount; commit-discount on top tilts vs unrelated goals.
        -- Both are multiplied into wound_mult so the final pool-cost
        -- visible to the cross-goal selector reflects the full bias.
        local _commit_mult = 1.0
        if state.wounded_pill and state.wounded_pill.mx == obj.mx and state.wounded_pill.my == obj.my then
          wound_mult = 0.3
          local wp = state.wounded_pill
          local age         = (state.tick or 0) - (wp.tick or 0)
          local time_factor = math.max(0, 1.0 - age / (C.WOUNDED_FINISH_DECAY_TICKS or 500))
          if time_factor > 0 then
            _commit_mult = 1.0 - (1.0 - (C.WOUNDED_COMMIT_DISCOUNT or 0.5)) * time_factor
            wound_mult = wound_mult * _commit_mult
          end
        end
        -- Ammo penalty: COST_INF if we don't carry enough shells to
        -- finish the pill at all (assumes 1 shell per HP). Otherwise
        -- escalate by 5 per shell that the take would leave us under
        -- SHELLS_LOW, assuming we use exactly pill.health shots. Replaces
        -- the old has_shells gate that cleared the whole pool when
        -- shells dropped below SHELLS_LOW (silently aborting in-flight
        -- takes). The pool stays populated; weak-shell takes naturally
        -- score themselves out of contention.
        local pill_hp_now = obj.health or 0
        if info.shells < pill_hp_now then
          ammo_cost = 1e30
        else
          local ending = info.shells - pill_hp_now
          if ending < C.SHELLS_LOW then
            ammo_cost = (C.SHELLS_LOW - ending) * 5
          end
        end
        -- "Finish what you started" penalty: every OTHER pill take
        -- (i.e. NOT the wounded one) gets bumped by a multiplier
        -- scaled by how close the wounded pill is to dead × time
        -- decay. Mirrors attack_pill_adjustments's logic so the per-
        -- tick eval queue stays consistent with the goal evaluator.
        -- Stashed on the entry below so the breakdown formula can
        -- show it explicitly.
        if state.wounded_pill
           and (state.wounded_pill.mx ~= obj.mx
                or state.wounded_pill.my ~= obj.my) then
          local wp = state.wounded_pill
          local wp_now = wp.id and world.pills and world.pills[wp.id] or nil
          local wp_hp  = wp_now and wp_now.health or wp.hp or 0
          local thresh = C.WOUNDED_FINISH_THRESHOLD or 10
          if wp_hp > 0 and wp_hp <= thresh then
            local hp_factor   = (thresh - wp_hp) / thresh
            local age         = (state.tick or 0) - (wp.tick or 0)
            local time_factor = math.max(0, 1.0 - age / (C.WOUNDED_FINISH_DECAY_TICKS or 500))
            local peak_mult   = C.WOUNDED_FINISH_OTHER_PENALTY or 3.0
            local m           = 1.0 + (peak_mult - 1.0) * hp_factor * time_factor
            if m > 1.001 then
              wound_mult     = wound_mult * m
              _finish_other_mult  = m
              _finish_other_wp_hp = wp_hp
              _finish_other_age   = age
              _finish_other_wp_id = wp.id or -1
            end
          end
        end
      end

      -- Spot cost (path to firing position) stays fixed. A* and other combat
      -- terms scale with hp/wound — a nearly-dead pill is easier to fight
      -- but still costs the same to reach a good firing spot. spot_cost
      -- itself is computed via offset-aware A* (target pill's contribution
      -- subtracted from the danger field), so the "as-if-pill-dead"
      -- discount is already baked in — no separate self_dr term.
      -- travel (dp, pool 6 only) gets its own wound factor at 2x the wound
      -- discount (clamped to 1): heavily wounded pill → big travel discount,
      -- fresh pill → no discount. Separate from hp*wound to avoid stacking.
      -- ammo_cost (pool 6 only) is the shells-budget penalty; goes to
      -- COST_INF when we lack the shells to finish the pill at all.
      local travel_wound = (pool_idx == 6) and math.min(1.0, wound_mult * 2) or 1.0
      local combat = (stale_cost + diff_cost + anger_cost + xfire_cost + intcpt_cost) * hp_mult * wound_mult
      local c = spot_cost + travel * travel_wound + combat * capture_mult + base_extra + threat_cost + ammo_cost

      -- danger_nearby multiplier (pool 6 only): if this pill was recently
      -- stamped (enemy LGM seen within danger radius during a prior take
      -- attempt), multiply cost so we don't bounce right back onto it
      -- for ~30 s.  Self-clears when the deadline passes.
      local _danger_nearby_mult = 1.0
      local _danger_nearby_left = 0
      if pool_idx == 6 and state.pill_danger_nearby then
        local until_tick = state.pill_danger_nearby[id]
        if until_tick and until_tick > now then
          _danger_nearby_mult = C.PILL_DANGER_NEARBY_MULT or 1.5
          _danger_nearby_left = until_tick - now
          c = c * _danger_nearby_mult
        end
      end

      -- capture_pill: replace flat-multiplier formula with distance^1.5 + danger.
      --   path^1.5 * DIST_SCALE  → cheap nearby, grows fast with distance
      --   threat * DANGER_WEIGHT → hot zones push cost up regardless of distance
      --   intcpt                  → penalize if enemy tank can beat us to the pill
      -- Distance is measured to the cheapest-to-reach ADJACENT tile of the
      -- pill, not the pill tile itself — the pill tile carries a 32767
      -- overlay (to block pathing THROUGH pills) which, run through ^1.5,
      -- blows the cost up to ~297000 and falsifies the whole formula.
      local _cpill_dist_score, _cpill_danger_val, _cpill_intcpt = 0, 0, 0
      local _cpill_dist_raw = raw_cost
      local _cpill_lgm_mult = 1
      local _cpill_dist_method = "dij"
      if pool_idx == 4 then
        c, _cpill_dist_raw, _cpill_dist_score, _cpill_danger_val, _cpill_intcpt, _cpill_lgm_mult, _cpill_dist_method =
          compute_pool4_cost(state, world, info, obj, tmx, tmy)
        -- Pool 4 skipped the smart_cost block (see above), so backfill
        -- raw_cost from compute_pool4_cost's distance — keeps the panel
        -- formula breakdown showing a meaningful raw value.
        raw_cost = _cpill_dist_raw
      end

      -- Ally-claimed handling for generic pools (2,3,4,5,6,7,8).
      --
      -- Pools 9 (attack_tank) and 13 (kill_lgm) are EXEMPT — time-
      -- critical, locally observed; stale broadcasts shouldn't pull
      -- us off a fight.  Pool 1 (refuel_at_base) has its own soft
      -- +100 penalty handled in the pool-1 branch above.
      --
      -- "Hard" REJECT pools (2,3,4,5,6,7): we don't mutate cost here
      -- any more.  Instead we just capture the ally's bid (kind +
      -- target match) on the cache entry, and a per-tick sync sweep
      -- (sync_ally_claimed_rejects, run before goal_competition)
      -- decides whether to set entry._reject = "ally_claimed".  This
      -- decouples REJECT freshness from the per-candidate eval cadence
      -- — when an ally drops their goal, the un-REJECT lands next
      -- tick, not on the next time this candidate happens to be
      -- re-evaluated.
      --
      -- Pool 8 (place_strategic) keeps the legacy hard +10000 penalty
      -- — the target is a tile and the existing behaviour is fine.
      local _ac_expected_kind = (pool_idx == 1) and "refuel_at_base"
                                or (pool_idx == 8) and "place_pill_strategic"
                                or POOL_NAMES[pool_idx]
      local _ac_pen, _ac_by, _ac_their_cost, _ac_heartbeat_left = 0, nil, nil, 0
      if pool_idx ~= 9 and pool_idx ~= 13 then
        for ally_pn, slot in ally_state.iter_active(now, 1750) do
          if ally_pn ~= info.player_number then
            local info_h = slot.info
            if info_h.goal == _ac_expected_kind then
              local aid = tonumber(info_h.target)
              local matched
              if aid and id then
                matched = (aid == id)
              else
                local amx = tonumber(info_h.mx)
                local amy = tonumber(info_h.my)
                if amx and amy then
                  matched = (amx == obj.mx and amy == obj.my)
                end
              end
              if matched then
                local their_cost = tonumber(info_h.cost)
                _ac_their_cost = their_cost
                _ac_by         = ally_pn
                _ac_heartbeat_left = 1750 - (now - slot.last_tick)
                if _ac_heartbeat_left < 0 then _ac_heartbeat_left = 0 end
                if pool_idx == 8 then
                  -- Legacy hard penalty path — kept for place_strategic.
                  local we_keep
                  if their_cost == nil then
                    we_keep = (info.player_number < ally_pn)
                  elseif c < their_cost then
                    we_keep = true
                  elseif c > their_cost then
                    we_keep = false
                  else
                    we_keep = (info.player_number < ally_pn)
                  end
                  if we_keep then break end
                  _ac_pen = C.ALLY_CLAIMED_PENALTY
                  c = c + _ac_pen
                end
                break
              end
            end
          end
        end
      end

      -- Store raw components for lazy formula building (get_formula on cold path).
      -- Fields are flattened directly into the cache entry (no sub-table) to
      -- avoid an extra Lua table allocation per candidate per tick.
      local entry = { cost = c, raw = raw_cost, tick = now, _p = pool_idx, _mx = obj.mx, _my = obj.my, _id = id }
      if _ac_pen > 0 then
        entry.ally_claimed_pen = _ac_pen
        entry.ally_claimed_by  = _ac_by
      end
      -- Cache the ally bid (if any) so the per-tick sync sweep can
      -- maintain entry._reject="ally_claimed" + the REJECT-row text
      -- without re-running cost compute. Cleared by the sync when
      -- the heartbeat expires or the ally drops the claim.
      if _ac_by and _ac_pen == 0 then  -- "hard REJECT pool" path, no penalty baked
        entry._ally_score      = _ac_their_cost
        entry._ally_by         = _ac_by
        entry._ally_heartbeat  = _ac_heartbeat_left
      end
      if pool_idx == 6 then
        entry._travel=travel; entry._travel_wound=travel_wound; entry._stale=stale_cost; entry._age=_gen_age
        entry._diff=diff_cost; entry._spot=spot_cost
        entry._spot_mx=spot_found_mx; entry._spot_my=spot_found_my
        entry._anger=anger_cost
        entry._xfire=xfire_cost; entry._intcpt=intcpt_cost; entry._hp=hp_mult
        entry._wound=wound_mult; entry._hpv=obj.health or C.PILLS_MAX_HEALTH
        entry._ttc=_ticks_to_calm; entry._pa=pill_anger
        entry._pickup_detail=goal_pickup_detail
        entry._pickup_path=goal_pickup_path
        entry._spot_method=goal_spot_method
        entry._spot_slate=goal_spot_slate
        entry._spot_tick=goal_spot_tick
        entry._spot_path=goal_spot_path_str
        entry._spot_path_len=goal_spot_path_len
        entry._ammo=ammo_cost
        entry._sh_now=info.shells
        entry._sh_end=(info.shells or 0) - (obj.health or 0)
        entry._fin_mult=_finish_other_mult
        entry._fin_wphp=_finish_other_wp_hp
        entry._fin_age=_finish_other_age
        entry._fin_wpid=_finish_other_wp_id
        entry._commit_mult=_commit_mult
        entry._danger_mult = (_danger_nearby_mult ~= 1.0) and _danger_nearby_mult or nil
        entry._danger_left = (_danger_nearby_left > 0) and _danger_nearby_left or nil
      elseif pool_idx == 7 then
        entry._base=base_extra; entry._tv=_threat_val; entry._thr=threat_cost
        entry._lgm_mult=_p7_lgm_mult
        entry._stale=stale_cost; entry._age=_gen_age
      elseif pool_idx == 4 then
        entry._ds=_cpill_dist_score; entry._dv=_cpill_danger_val
        entry._intcpt=_cpill_intcpt
        entry._lgm_mult=_cpill_lgm_mult
        entry._dist_method=_cpill_dist_method
      else
        entry._stale=stale_cost; entry._age=_gen_age
      end
      state.cost_cache[cache_key] = entry

      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, cost = c,
        own = obj.owner or "?", hp = obj.health or 0,
        stale = obj.last_seen and (now - obj.last_seen) or 0,
        obj = obj,  -- ref needed by rederive_pool_partial_best after sync
      }
      if c < pr.best_cost then
        pr.best_cost = c; pr.best_id = id; pr.best_obj = obj
        pr.best_shells_on_arrival = cand_shells_on_arrival
      end
    end

    -- Per-candidate timing summary so we can see what's slow.
    local _t_total = clock_us() - _t0
    -- Direct optimize.log diag for slow candidates so we can see them
    -- without needing print2 enabled. Threshold: 0.5 ms (anything that
    -- shows up on the per-tick summary). Includes sub-timings for the
    -- 8-neighbor adjacent sweep + smart_cost call so we can identify
    -- which inner step dominates.
    if BRAIN_PROFILE_LOG and _t_total > 500 then
      opt.append("optimize.log", string.format(
        "  [diag] slow cand pool=%d id=%s total=%.2f raw=%.2f adj=%.2f smart=%.2f diff=%.2f spot=%.2f cost=%.0f obj=(%d,%d) hp=%s",
        pool_idx, tostring(id),
        _t_total / 1000, _t_raw / 1000,
        _t_adj / 1000, _t_smart / 1000,
        _diff_us / 1000, _spot_us / 1000,
        raw_cost, obj.mx or -1, obj.my or -1, tostring(obj.health)))
    end
    ::continue::
  end

  state.eval_queue_pos = pos
end

-- =========================================================================
-- sync_ally_claimed_rejects — per-replan sweep over cost_cache that
-- maintains the ally-claimed REJECT flag against the live ally_state
-- broadcast slate.  For pools 2/3/4/5/6/7 we no longer bake +10000 into
-- entry.cost at eval time — we just stored entry._ally_score / _ally_by.
-- This pass converts that into entry._reject = "ally_claimed" when the
-- ally is cheaper by at least ALLY_CLAIMED_STEAL_THRESHOLD, and clears
-- the reject when the ally drops the goal, the heartbeat expires, or
-- our raw cost drops below theirs.
--
-- Result: ally-claim REJECT freshness decouples from per-candidate eval
-- cadence — an ally yielding mid-cycle un-REJECTs us on the very next
-- tick instead of waiting for the slow attack_pill re-eval window.
-- =========================================================================
local _REJECT_POOLS = {
  [2] = "defend_pill",
  [3] = "capture_base",
  [4] = "capture_pill",
  [5] = "repair_pill",
  [6] = "attack_pill",
  [7] = "attack_base",
}

-- Record every change in cost_cache[*]._reject to a per-pool/id history
-- list so the pool-grid breakdown can show "this entry was rejected for
-- reason X at tick T, cleared at tick U, ..." across the whole session.
-- Called at the END of sync_ally_claimed_rejects so it captures any
-- transitions sync just made, plus changes made elsewhere (build_eval_queue
-- writing _reject="alive" / "in_tank" / "stale" / "blocked" / "depleted"
-- etc.).  Stored on state.reject_history[pool:id] = { {tick, reason, by,
-- prev}, ... } — unbounded per session but trivial memory in practice.
local function record_reject_history(state)
  local cache = state.cost_cache
  if not cache then return end
  local now = state.tick or 0
  state.reject_history = state.reject_history or {}
  local hist = state.reject_history
  for _, e in pairs(cache) do
    local cur = e._reject or false
    local prev = e._last_reject_seen
    if prev == nil then prev = false end
    if cur ~= prev then
      local key = (e._p or "?") .. ":" .. (e._id or "?")
      local list = hist[key]
      if not list then list = {}; hist[key] = list end
      list[#list + 1] = {
        t      = now,
        reason = e._reject or nil,
        by     = e._priority_by or e._ally_by or nil,
        prev   = (prev ~= false) and prev or nil,
      }
      e._last_reject_seen = cur
    end
  end
end

local function sync_ally_claimed_rejects(state, info)
  local cache = state.cost_cache
  if not cache then return end
  local now = state.tick or 0
  local self_pn = (_SELF_PN ~= -1) and _SELF_PN or (state.player_number or -1)
  local threshold = C.ALLY_CLAIMED_STEAL_THRESHOLD or 0
  -- Prefer the live info passed by the caller; only fall back to the
  -- cached _last_info (which gets populated by get_pool_breakdown_json,
  -- not the per-tick brain loop) so the per-tick sync still sees the
  -- right armour on every tick, not just when the pool grid is open.
  local cur_armour = (info and info.armour)
                     or (state._last_info and state._last_info.armour) or 0

  local priority_ticks = C.ALLY_PILL_TAKE_PRIORITY_TICKS or 100
  -- Per-pill snapshot keyed by pill_id: { until_t, by }.  Set ONCE the
  -- first time a pool-4 entry for that pill appears in cost_cache; the
  -- countdown ticks down from there regardless of the ally's substate.
  -- Survives cache eviction-and-recreation (e._priority_check_done on
  -- the entry alone wouldn't, because step_eval_queue rebuilds the
  -- entry table on every re-eval).
  state.pill_priority_set = state.pill_priority_set or {}
  local pill_priority_set = state.pill_priority_set

  for _, e in pairs(cache) do
    local pool_idx = e._p
    -- Diagnostic: log every pool-6 cache entry we visit, so we can see
    -- whether the entry even reaches the sync loop and what its raw
    -- fields look like.  Helps catch the "pool 6 entry exists but
    -- sync skips it because _id is missing / _REJECT_POOLS gate
    -- fails" class of bugs.
    -- Pool 4 (capture_pill) "ally did the take" REJECT.  Snapshot at
    -- FIRST sighting: when a fresh capture_pill candidate appears (the
    -- killed pill just became eligible), peek ally_state once for any
    -- ally currently on attack_pill for the same pill_id.  If found,
    -- stamp an absolute expiry tick and NEVER refresh it — the timer
    -- counts down regardless of the ally's substate, so by the time
    -- their ~2 s swerve finishes the REJECT has decayed and standard
    -- ally_claimed takes over for the capture race.  Without the
    -- "no refresh" rule the countdown would only start after the
    -- swerve ended, leaving the killer's window closed by the time
    -- they actually reached capture_pill.
    if pool_idx == 4 and e._id then
      -- Co-attacker exemption: if WE'RE also targeting this pill
      -- (currently attack_pill or capture_pill on it), parallel ally
      -- broadcasts don't reject us — we have at least as much "did
      -- the take" priority. Cost comparison decides between co-killers.
      local g = state.goal
      local we_targeted_it = g
        and (g.kind == "attack_pill" or g.kind == "capture_pill")
        and ((g.target_id and g.target_id == e._id)
             or (g.mx == e._mx and g.my == e._my))
      -- First-sight snapshot per pill_id.  Mark "checked" even when no
      -- priority applies so we don't re-scan ally_state every tick.
      -- Gate on "actually a capture candidate": entries with _reject
      -- already set (alive / in_tank / blocked / stale) aren't real
      -- pool-4 contenders yet — snapshotting then would lock in a
      -- "no priority" stamp before the pill is even dead, then never
      -- re-check when it actually becomes capturable.  Skip the
      -- snapshot until the entry is reject-free OR was rejected for
      -- ally_pill_take_priority specifically (which we own).
      local entry_is_candidate = not e._reject
                                 or e._reject == "ally_pill_take_priority"
      local stamp = pill_priority_set[e._id]
      if stamp == nil and not we_targeted_it and entry_is_candidate then
        local found_pn
        for ally_pn, slot in ally_state.iter_active(now, 1750) do
          if ally_pn ~= self_pn then
            local h = slot.info
            if h.goal == "attack_pill" then
              local aid = tonumber(h.target)
              if aid == e._id then
                found_pn = ally_pn
                break
              end
            end
          end
        end
        if found_pn then
          stamp = { until_t = now + priority_ticks, by = found_pn, stamped_at = now }
        else
          stamp = { until_t = 0 }  -- "checked, no priority"
        end
        pill_priority_set[e._id] = stamp
      end
      -- Early-termination checks: end the priority window before until_t
      -- when either (1) the killer's tank died after we stamped, or
      -- (2) the killer's current goal is neither attack_pill nor
      -- capture_pill on this pill_id (they abandoned the take).  Either
      -- way the priority no longer serves its purpose so we hand the
      -- contest back to standard ally_claimed cost-based REJECT.
      if stamp and stamp.until_t > now and stamp.by then
        local dead_at = state.tank_dead_at and state.tank_dead_at[stamp.by]
        if dead_at and dead_at > (stamp.stamped_at or 0) then
          stamp.until_t = 0
        else
          local ally_slot = ally_state.get(stamp.by)
          if ally_slot and ally_slot.active then
            local ag = ally_slot.info.goal
            local at = tonumber(ally_slot.info.target)
            local still_on_it = (ag == "attack_pill" or ag == "capture_pill")
                                and at == e._id
            if not still_on_it then
              stamp.until_t = 0
            end
          end
        end
      end
      if stamp and stamp.until_t > now and not we_targeted_it then
        if e._reject ~= "ally_pill_take_priority" then
          e._reject = "ally_pill_take_priority"
          e.formula = nil
        end
        e._reject_remaining = stamp.until_t - now
        e._priority_by      = stamp.by
        goto continue_entry
      elseif e._reject == "ally_pill_take_priority" then
        e._reject = nil
        e._reject_remaining = 0
        e._priority_by = nil
        e.formula = nil
      end
    end
    -- Pool 6 (attack_pill) extra precondition: refuse to take a near-
    -- full-HP pill on low armour.  Higher priority than ally_claimed
    -- — if we can't safely take it, who's cheapest doesn't matter.
    -- Live-evaluated every tick against current info.armour so it
    -- self-clears the moment we refuel.
    if pool_idx == 6 and e._hpv then
      local pill_hp = e._hpv
      if pill_hp >= C.ATTACK_PILL_UNSAFE_HP_THRESHOLD
         and cur_armour > 0
         and cur_armour < C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR then
        if e._reject ~= "armour_too_low" then
          e._reject = "armour_too_low"
          e._reject_remaining = 0
          e._armour_at_reject = cur_armour
          e._pillhp_at_reject = pill_hp
          e.formula = nil  -- re-render with REJECT text
        else
          e._armour_at_reject = cur_armour
          e._pillhp_at_reject = pill_hp
        end
        goto continue_entry
      elseif e._reject == "armour_too_low" then
        -- Armour recovered (refueled) or pill HP dropped — clear.
        e._reject = nil
        e._reject_remaining = 0
        e._armour_at_reject = nil
        e._pillhp_at_reject = nil
        e.formula = nil
      end
    end
    if _REJECT_POOLS[pool_idx] then
      -- Look up any active ally currently claiming this same (kind, target).
      local kind = _REJECT_POOLS[pool_idx]
      local match_cost, match_pn, match_heartbeat = nil, nil, 0
      -- Pool 6 (attack_pill) flag: ally is past plan/approach — they're
      -- engaging the pill.  Once that's set, no cost can peel us off:
      -- we REJECT unconditionally (subject only to the we_hold
      -- co-attacker exemption).  You can't steal a kill mid-take.
      local force_engaging_reject = false
      local match_sub = nil
      local tank_dead_at = state.tank_dead_at
      local _diag_p6 = BRAIN_DEBUG_MODE and pool_idx == 6 and e._id
      local _diag_scanned = 0
      for ally_pn, slot in ally_state.iter_active(now, 1750) do
        if ally_pn ~= self_pn
           -- Skip allies whose tank is dead: any "they have the
           -- claim" REJECT we'd set based on their broadcast is
           -- stale.  They can't deliver on the goal until they
           -- respawn, so let standard contention resume now.  When
           -- they respawn and rebroadcast, the slot becomes valid
           -- again automatically.
           and not (tank_dead_at and tank_dead_at[ally_pn]
                    and tank_dead_at[ally_pn] > (slot.last_tick or 0)) then
          local h = slot.info
          if _diag_p6 then _diag_scanned = _diag_scanned + 1 end
          if h.goal == kind then
            local aid = tonumber(h.target)
            local matched
            if aid and e._id then
              matched = (aid == e._id)
            else
              local amx = tonumber(h.mx)
              local amy = tonumber(h.my)
              if amx and amy then
                matched = (amx == e._mx and amy == e._my)
              end
            end
            if matched then
              match_sub = h.sub
              -- Pool 6: if the ally is past plan/approach, flag the
              -- entry for unconditional REJECT below.  Cost is
              -- irrelevant once an ally has started actively engaging
              -- — nobody peels off a pill take mid-fight.
              if pool_idx == 6 then
                if match_sub ~= "approach" and match_sub ~= "plan_position" then
                  force_engaging_reject = true
                end
              end
              match_cost      = tonumber(h.cost)
              match_pn        = ally_pn
              match_heartbeat = 1750 - (now - slot.last_tick)
              if match_heartbeat < 0 then match_heartbeat = 0 end
              break
            end
          end
        end
      end
      if _diag_p6 then
        if match_pn then
        elseif _diag_scanned > 0 then
        end
      end

      if match_pn then
        local our_cost = e.cost
        local we_keep
        -- "First to claim it, keeps it" rule.  Within the steal band
        -- (neither side meaningfully cheaper), the current holder
        -- keeps and the challenger yields — avoids both-bots-keep
        -- which made two bots converge on the same target.  If
        -- NEITHER is currently committed (fresh contention), break
        -- the tie deterministically by player_number so one always
        -- wins and we don't get the dual-yield "nobody takes it"
        -- failure mode.
        local kind = _REJECT_POOLS[pool_idx]
        local g    = state.goal
        local we_hold = g and g.kind == kind
                       and ((g.target_id and e._id and g.target_id == e._id)
                            or (g.mx == e._mx and g.my == e._my))
        -- Pool 6 unconditional engaging REJECT.  Once the ally has
        -- gone past plan/approach the take is in flight — no cost can
        -- justify peeling them off, and arriving late as a "cheaper"
        -- co-attacker just wastes our cycles since the ally finishes
        -- the kill first.  Only exception is we_hold: a true race
        -- where we ALSO committed to the same target.
        if force_engaging_reject and not we_hold then
          we_keep = false
        elseif match_cost == nil then
          -- Ally hasn't broadcast a cost yet (first frame post-pick).
          -- They're still the holder (committed by goal).  We keep
          -- only if we also hold it.
          we_keep = we_hold
        elseif our_cost + threshold < match_cost then
          we_keep = true  -- we're meaningfully cheaper, keep
        elseif match_cost + threshold < our_cost then
          we_keep = false -- they're meaningfully cheaper, yield
        else
          -- Within the steal band: holder keeps.  Ally always holds
          -- (they matched on goal+target).  We keep only if we ALSO
          -- hold it (race: both committed to same target).  No more
          -- pn tiebreak — bot0 was beating bot2 on cost ties because
          -- pn=0 < pn=2, even though bot2 was the actual holder.
          we_keep = we_hold
        end
        if we_keep then
          if e._reject == "ally_claimed" then
            e._reject = nil
            e._reject_remaining = 0
            e.formula = nil  -- re-render
          end
          e._ally_score     = match_cost
          e._ally_by        = match_pn
          e._ally_heartbeat = match_heartbeat
        else
          e._reject           = "ally_claimed"
          e._reject_remaining = match_heartbeat
          e._ally_score       = match_cost
          e._ally_by          = match_pn
          e._ally_heartbeat   = match_heartbeat
          e.formula           = nil  -- re-render with REJECT text
        end
      else
        -- No ally claiming this target any more — clear any prior reject.
        if e._reject == "ally_claimed" then
          e._reject = nil
          e._reject_remaining = 0
          e.formula = nil
        end
        e._ally_score     = nil
        e._ally_by        = nil
        e._ally_heartbeat = nil
      end
    end
    ::continue_entry::
  end
  -- Snapshot any _reject transitions sync just made (or any made
  -- earlier this tick by build_eval_queue / pool finalizers) into
  -- state.reject_history so the pool-grid breakdown can surface the
  -- full per-entry timeline.
  record_reject_history(state)
end

-- Re-derive pool_partial best_cost/id/obj after sync_ally_claimed_rejects
-- so finalize_pools doesn't pick a candidate that just got REJECT-flagged
-- by the sync sweep.  Cheap: O(candidates per pool).
local function rederive_pool_partial_best(state)
  local partial = state.pool_partial
  local cache = state.cost_cache
  if not partial or not cache then return end
  for pool_idx, pr in pairs(partial) do
    if _REJECT_POOLS[pool_idx] and pr and pr.candidates then
      local best_cost = math.huge
      local best_id, best_obj = nil, nil
      for _, cand in ipairs(pr.candidates) do
        local ck = pool_idx .. ":" .. cand.id
        local ce = cache[ck]
        if ce and not ce._reject and (cand.cost or math.huge) < best_cost then
          best_cost = cand.cost
          best_id   = cand.id
          best_obj  = cand.obj
        end
      end
      pr.best_cost = best_cost
      pr.best_id   = best_id
      pr.best_obj  = best_obj
    end
  end
end

-- =========================================================================
-- finalize_pools — called at decision tick.  Converts partial results
-- into pool_cache entries (same format as the old evaluators returned).
-- Also runs cheap evaluators that don't need incremental evaluation.
-- =========================================================================
function M.finalize_pools(state, world, info)
  -- ── Ally-claimed REJECT sync (runs before partial → pool_cache) ──
  _SELF_PN = info.player_number or _SELF_PN
  sync_ally_claimed_rejects(state, info)
  rederive_pool_partial_best(state)
  local _t0 = clock_us()
  local tmx  = info.tankx >> 8
  local tmy  = info.tanky >> 8
  local boat = info.inboat
  local ammo = (info.shells or 0) + (info.mines or 0)
  local partial = state.pool_partial or {}
  local now = state.tick or 0
  local cache = state.cost_cache or {}

  -- Backfill unevaluated queue items from cost cache
  local queue = state.eval_queue
  local pos = state.eval_queue_pos or 1
  if queue then
    local boat_flag = boat and 1 or 0
    local shells = info.shells or 32
    local trees  = info.trees or 0
    local mines  = info.mines or 0
    local armour = info.armour or 40

    for qi = pos, #queue do
      local item = queue[qi]
      local pool_idx = item.pool
      local obj = item.obj
      local id = item.id
      local cache_key = pool_idx .. ":" .. id
      local cached = cache[cache_key]
      -- Use cache if less than 500 ticks old (warning suppressed; the
      -- queue advances on its own and the warning was firing every tick
      -- for every stale entry — pure spam).
      if cached and (now - cached.tick) < 500 then
        if not partial[pool_idx] then
          partial[pool_idx] = { candidates = {}, best_cost = math.huge, best_id = nil, best_obj = nil }
        end
        local pr = partial[pool_idx]
        local c = cached.cost
        if type(c) ~= "number" then c = math.huge end
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
          cached = true,
        }
        -- Skip rejected entries when picking best — sync just ran
        -- above and set _reject on entries that fail preconditions
        -- (armour_too_low, ally_claimed, ally_pill_take_priority).
        -- Without this guard the backfill loop would undo rederive's
        -- work and pool 6 would re-pick a rejected pill.
        if not cached._reject and c < pr.best_cost then
          pr.best_cost = c; pr.best_id = id; pr.best_obj = obj
        end
      end
    end
  end

  if BRAIN_PROFILE then opt(string.format("    fp backfill %.2f ms", (clock_us() - _t0) / 1000)) end
  local _t1 = clock_us()
  -- Fresh cache each cycle (don't carry stale entries from last cycle)
  state.pool_cache = {}
  local pc = state.pool_cache

  -- Pool 1: refuel — finalize from partial
  -- Combat look-ahead: if best non-refuel goal is attack_pill, factor in
  -- how much armour/shells we'll need for the fight
  local pr1 = partial[1]
  if pr1 and pr1.best_obj then
    local base = pr1.best_obj
    local bid = pr1.best_id
    local bscore = pr1.best_cost

    -- Base urgency from current supplies vs low thresholds.
    -- Squared so low resources discount more aggressively
    -- (armour=10/15 → 0.44 instead of 0.67).
    local arm_u = math.min(1.0, info.armour / C.ARMOUR_LOW)
    local sh_u  = math.min(1.0, info.shells / C.SHELLS_LOW)
    arm_u = arm_u * arm_u
    sh_u  = sh_u  * sh_u
    local urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(arm_u, sh_u))

    -- Combat look-ahead: check if attack_pill is the likely next goal
    local pr6 = partial[6]
    if pr6 and pr6.best_obj then
      local target_hp = pr6.best_obj.health or 0
      local needed_armour = target_hp * C.ARMOUR_PER_PILL_HP
      local needed_shells = target_hp  -- ~1 shell per HP
      if info.armour < needed_armour or info.shells < needed_shells then
        -- We'd be under-supplied for this fight — boost refuel urgency.
        -- Same squared curve as the baseline urgency above.
        local combat_arm_u = math.min(1.0, info.armour / math.max(1, needed_armour))
        local combat_sh_u  = math.min(1.0, info.shells / math.max(1, needed_shells))
        combat_arm_u = combat_arm_u * combat_arm_u
        combat_sh_u  = combat_sh_u  * combat_sh_u
        local combat_urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(combat_arm_u, combat_sh_u))
        if combat_urgency < urgency then
          urgency = combat_urgency  -- use the more urgent (lower = cheaper refuel)
        end
      end
    end

    local cost = bscore * urgency
    pc[1] = {
      cost = cost,
      goal = { kind = "refuel_at_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = BRAIN_POOL_VIZ and string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d sh=%d",
             bid, base.mx, base.my, bscore, urgency, cost, info.armour, info.shells) or "",
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
    local raw_cost3 = pr3.best_cost
    local imminent3 = false
    if base.health == 0
       and raw_cost3 <= C.IMMINENT_CAPTURE_PATH_COST
       and info.armour >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
      raw_cost3 = math.min(raw_cost3, C.IMMINENT_CAPTURE_FLOOR)
      imminent3 = true
    end
    local lm3, lr3 = strategic_location_mult(base.mx, base.my, state, world, info, "capture_base", nil)
    local desc3 = ""  -- pool viz string; populated only when BRAIN_POOL_VIZ
    if BRAIN_POOL_VIZ then
      desc3 = string.format("capture_base#%d@(%d,%d) cost=%.0f", bid, base.mx, base.my, raw_cost3)
      if imminent3 then desc3 = desc3 .. " IMMINENT" end
    end
    -- TODO: Phase 6 race-loss should clear race_mode on captures we've decided not to win.
    -- See note on race_mode above (line ~644). Same dead-branch.
    local race_mode3 = C.CAPTURE_RACE_MODE_CAPTURE
    pc[3] = {
      cost = raw_cost3,
      loc_mult = lm3, loc_reason = BRAIN_POOL_VIZ and lr3 or nil,
      imminent = imminent3,
      goal = { kind = "capture_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid,
               race_mode = race_mode3 },
      desc = desc3,
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
    local raw_cost4 = pr4.best_cost
    local imminent4 = false
    if pill.health == 0
       and raw_cost4 <= C.IMMINENT_CAPTURE_PATH_COST
       and info.armour >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
      raw_cost4 = math.min(raw_cost4, C.IMMINENT_CAPTURE_FLOOR)
      imminent4 = true
    end
    local lm4, lr4 = strategic_location_mult(pill.mx, pill.my, state, world, info, "capture_pill", pill)
    local desc4 = ""  -- pool viz string; populated only when BRAIN_POOL_VIZ
    if BRAIN_POOL_VIZ then
      desc4 = string.format("capture_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, raw_cost4)
      if imminent4 then desc4 = desc4 .. " IMMINENT" end
    end
    -- TODO: Phase 6 race-loss should clear race_mode on captures we've decided not to win.
    -- See note on race_mode above (line ~644). Same dead-branch.
    local race_mode4 = C.CAPTURE_RACE_MODE_CAPTURE
    pc[4] = {
      cost = raw_cost4,
      loc_mult = lm4, loc_reason = BRAIN_POOL_VIZ and lr4 or nil,
      imminent = imminent4,
      goal = { kind = "capture_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid,
               race_mode = race_mode4 },
      desc = desc4,
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
      desc = BRAIN_POOL_VIZ and string.format("repair_pill#%d@(%d,%d) cost=%.0f (path=%.0f -dam=%d×%d)",
             pid, pill.mx, pill.my, adj_cost, pcost, damage, C.REPAIR_DAMAGE_BONUS) or "",
      cands = pr5.candidates,
    }
  else
    pc[5] = nil
  end

  -- Pool 6: attack_pill — step_eval_queue already scored each candidate.
  local pr6 = partial[6]
  if pr6 and pr6.best_obj then
    local pill = pr6.best_obj
    local pid = pr6.best_id
    local pcost = pr6.best_cost
    -- Lock to the current target while we're committed to this attack.
    -- Hysteresis alone wasn't enough — a cheaper alternate pillbox can
    -- still flip the goal mid-take. Once we've committed (anything past
    -- plan_position), the eval queue's "best" is overridden by the
    -- current goal's pill so pool 6 surfaces THIS pill, not a cheaper
    -- one. Plan_position itself is not locked (we may legitimately
    -- want to switch before any real investment).
    -- LOCK_SUBS hoisted to module scope (see top of file).
    if state.goal and state.goal.kind == "attack_pill"
       and state.goal.mx and state.goal.my
       and LOCK_SUBS[state.goal.substate or ""] then
      local key = state.goal.my * 256 + state.goal.mx
      local cur_entries = world.pill_at and world.pill_at[key]
      local cur_pill, cur_pid
      -- During rush the pill is DEAD by definition (we're driving in
      -- to capture). Accept any pill record at the goal tile, alive
      -- or not — without this, the lock falls through and pool 6
      -- picks a different live pill, flipping the goal and aborting
      -- the rush mid-drive.
      local accept_dead = (state.goal.substate == "rush")
      if cur_entries then
        for _, e in ipairs(cur_entries) do
          if e.pill and (accept_dead or e.pill.health > 0) then
            cur_pill = e.pill
            cur_pid  = e.id
            break
          end
        end
      end
      if cur_pill then
        pill  = cur_pill
        pid   = cur_pid
        -- Mid-take cost override — tiered by commit level:
        --   shoot_pill + fired ≥ 3 → 10  (deep commit: shells sunk,
        --                                 only IMMINENT capture (5) or
        --                                 attack_tank engage-break (<9)
        --                                 should still preempt).
        --   shoot_pill + fired < 3 → 50  (settled at standoff and
        --                                 aiming but no shells yet —
        --                                 moderate lock so normal
        --                                 alternatives lose but a
        --                                 genuinely cheap special case
        --                                 can still win).
        --   engage    + fired ≥ 3 → 10  (legacy non-PPT path).
        --   everything else        → natural pool-6 cost.
        --
        -- Other locked substates (plan_position, approach, aim,
        -- charge, build_walls, in_range_*, ws_*) keep the natural
        -- cost.  pill SWAP above prevents flipping targets
        -- mid-substate-transition; a genuinely-cheaper alternative
        -- wins before we've sunk meaningful investment.
        local sub = state.goal.substate or ""
        local fired = state.goal._fired or 0
        if sub == "shoot_pill" then
          pcost = (fired >= 3) and 10 or 50
        elseif sub == "engage" and fired >= 3 then
          pcost = 10
        end
      end
    end
    local lm6, lr6 = strategic_location_mult(pill.mx, pill.my, state, world, info, "attack_pill", pill)
    pc[6] = {
      cost = pcost,
      loc_mult = lm6, loc_reason = BRAIN_POOL_VIZ and lr6 or nil,
      _pill = pill, _pill_id = pid,
      _shells_on_arrival = pr6.best_shells_on_arrival,
      goal = { kind = "attack_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my) },
      desc = BRAIN_POOL_VIZ and string.format("attack_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, pcost) or "",
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
    local lm7, lr7 = strategic_location_mult(base.mx, base.my, state, world, info, "attack_base", nil)
    pc[7] = {
      cost = adj_cost,
      loc_mult = lm7, loc_reason = BRAIN_POOL_VIZ and lr7 or nil,
      _shells_on_arrival = pr7.best_shells_on_arrival,
      goal = { kind = "attack_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = BRAIN_POOL_VIZ and string.format("attack_base#%d@(%d,%d) cost=%.0f (path=%.0f +base=%d +threat=%.0f×%d)",
             bid, base.mx, base.my, adj_cost, bcost, C.ATTACK_BASE_EXTRA_COST,
             threat_at_base, C.ATTACK_BASE_THREAT_WEIGHT) or "",
      cands = pr7.candidates,
    }
  else
    pc[7] = nil
  end

  if BRAIN_PROFILE then opt(string.format("    fp pool-finalizers %.2f ms", (clock_us() - _t1) / 1000)) end
  local _t2 = clock_us()
  -- Run cheap evaluators directly
  for _, idx in ipairs(FINALIZE_POOLS) do
    local _te = clock_us()
    local evaluator = POOL_EVALUATORS[idx]
    if evaluator then
      pc[idx] = evaluator(state, world, info, tmx, tmy, boat, ammo)
    end
    if BRAIN_PROFILE then opt(string.format("    fp eval[%d] %.2f ms", idx, (clock_us() - _te) / 1000)) end
  end

  -- Summary: which pools got finalized
  -- wait_for_lgm: extra "park and wait for the LGM" candidate at a
  -- fixed low cost so it competes with normal pool winners. See
  -- eval_wait_for_lgm for suppression conditions (won't fire while a
  -- goal that needs the LGM is already running).
  state.pool_cache[12] = eval_wait_for_lgm(state, info)
  -- kill_lgm: pool 13 was just wiped by `state.pool_cache = {}` above.
  -- Re-inject so an LGM-sighting urgent_replan doesn't miss it and
  -- pick_goal can see kill_lgm as a candidate this tick.
  M.refresh_kill_lgm(state, info, world)
end

-- =========================================================================
-- update_pool_cache — called every tick.
-- Tick 0 of cycle: build the eval queue (filter candidates).
-- Subsequent ticks: process 2 candidates from the queue.
-- =========================================================================
function M.update_pool_cache(state, world, info)
  -- Keep pool-8 stub fields fresh so get_pool_breakdown_json sees current state
  -- even between replans (e.g. tank just picked up a pill mid-cycle).
  if state._last_info then
    state._last_info.carried_pills = info.carried_pills or 0
    state._last_info.man_status    = info.man_status
    state._last_info.inboat        = info.inboat
  end

  -- Process candidates from the eval queue (2 per tick).
  -- The queue is built by init.lua after each replan decision,
  -- giving ~49 ticks to process before the next decision.
  local _t_seq0 = clock_us()
  M.step_eval_queue(state, world, info)
  if BRAIN_PROFILE then opt(string.format("  step_eval_queue done %.2f ms", (clock_us() - _t_seq0) / 1000)) end

  -- Staged-reveal pass for the pool-6 per-pill candidate overlays.
  -- Iterates the diff cache and emits each pill's spots with a mode
  -- that depends on how many ticks have passed since its scan:
  --   age 0 → all spots
  --   age 1 → only in-bucket spots
  --   age ≥ 2 → only the winner
  -- Cache TTL is ~50 ticks, so the winner stays visible until the
  -- next re-eval refreshes the entry and the cycle restarts.

  M.refresh_kill_lgm(state, info, world)
end

-- =========================================================================
-- refresh_kill_lgm — populate pool_cache[13] from perception.
--
-- Called from BOTH update_pool_cache (rolling per-tick refresh) AND
-- finalize_pools (which wipes pool_cache on every replan, so without a
-- second call here pool 13 would be missing at the moment pick_goal
-- runs and the bot would never pick kill_lgm).
--
-- Cheap (one walk over perc.enemy_lgms + a couple of table allocs), so
-- safe to call twice per tick.  Requires shells > 0.  When no LGM is
-- visible, clears pool_cache[13] + any stale 13:* cost_cache entries so
-- a ghost target doesn't linger after the LGM goes back into its tank.
-- =========================================================================
function M.refresh_kill_lgm(state, info, world)
  if not state.pool_cache then return end
  local elgms = state.perc and state.perc.enemy_lgms
  if elgms and #elgms > 0 and (info.shells or 0) > 0 then
    -- Cost model mirrors eval_attack_tank: per-target evaluation with
    -- LOS-fast-engage vs Manhattan-boundary-standoff branches, plus
    -- the same modifiers (low_shells, aim_bonus, crossfire, boat_mult,
    -- tank_tile_threat).  See goals.lua:1059+ for the tank version —
    -- wall_penalty is omitted here because the standoff scan already
    -- filters wall-blocked tiles (no walled engage spots survive), and
    -- the LOS branch is gated on wall_hp==0 by definition.
    -- Nav target sits INSIDE the engage trigger by KILL_LGM_NAV_INSET
    -- tiles.  Tank drives toward a spot at (shoot_range - inset) from
    -- the LGM but the engage-mode trigger in steering still fires at
    -- the full shoot_range.  Net effect: tank crosses into engage range
    -- still under throttle (momentum carries it through), instead of
    -- decelerating to a stop exactly at the boundary and letting the
    -- LGM escape further while we slow down.
    local NAV_INSET = C.KILL_LGM_NAV_INSET or 3
    local R     = math.max(1, (C.KILL_LGM_SHOOT_RANGE or 8) - NAV_INSET)
    local boat  = (info.inboat and 1) or 0
    local tmx   = info.tankx >> 8
    local tmy   = info.tanky >> 8
    local perc  = state.perc or {}
    local now_t = state.tick or 0
    local KILL_LGM_BASE_COST = 20  -- kill_lgm priority floor (parallels TANK_COMBAT_BASE_COST=30)

    -- Low-shells penalty (mirrors attack_tank low_shells_penalty).
    local low_shells_penalty = 0
    if info.shells < C.TANK_COMBAT_LOW_SHELLS_THRESHOLD then
      low_shells_penalty = (C.TANK_COMBAT_LOW_SHELLS_THRESHOLD - info.shells)
                         * C.TANK_COMBAT_LOW_SHELLS_COST_PER
    end
    local tank_tile_threat = threat.at(tmx, tmy) or 0

    local best_cost = math.huge
    local best_winner = nil       -- { lgm, shoot_mx, shoot_my, los_engage, formula, ... }
    local cand_rows = {}          -- pool viz cands array
    if not state.cost_cache then state.cost_cache = {} end

    for _, lgm in ipairs(elgms) do
      -- Boat vulnerability — applies if EITHER the LGM is on water OR
      -- its owning tank is on water.  Killing an LGM whose parent tank
      -- is exposed in water denies a rebuild/repair shuttle to an
      -- already-vulnerable target, so the engage is as valuable as
      -- killing a water-exposed tank.  Pick the BIGGEST discount
      -- (lowest mult) across both checks.
      local function water_mult_for(mx, my)
        local tt = U.ttype(mx, my)
        if tt == C.T_DEEPSEA then return C.TANK_COMBAT_DEEPSEA_MULT end
        if tt == C.T_RIVER or tt == C.T_BOAT then return C.TANK_COMBAT_BOAT_MULT end
        return 1.0
      end
      local boat_mult = water_mult_for(lgm.mx, lgm.my)
      if lgm.near_tank_idnum then
        for _, et in ipairs(perc.enemy_tanks or {}) do
          if et.id == lgm.near_tank_idnum then
            local owner_mult = water_mult_for(et.mx, et.my)
            if owner_mult < boat_mult then boat_mult = owner_mult end
            break
          end
        end
      end

      -- Aim bonus: already pointed near the LGM = cheaper to engage.
      local aim_dir = U.aim_at(info.tankx, info.tanky, U.m2w(lgm.mx), U.m2w(lgm.my))
      local aim_diff = math.abs(U.adiff(info.direction, aim_dir))
      local aim_bonus = 0
      if aim_diff < C.TANK_COMBAT_AIM_THRESHOLD then
        local aim_cap = (lgm.dist or 99) <= R and 25 or 10
        aim_bonus = math.min(C.TANK_COMBAT_AIM_BONUS, aim_cap)
      end

      -- Crossfire penalty: hostile pills near the LGM. Accumulates
      -- across all pills in range and scales by anger — a heated pill
      -- (anger ~1.0) is much deadlier than a calm one.
      local crossfire = 0
      for _, pt in ipairs(perc.pill_threats or {}) do
        local pd = U.mdist(lgm.mx, lgm.my, pt.pill.mx, pt.pill.my)
        if pd <= C.TANK_COMBAT_NEAR_PILL_RANGE then
          local anger_scale = math.max(0.5, (pt.anger or 0) + 0.5)
          crossfire = crossfire + C.TANK_COMBAT_NEAR_PILL_PENALTY * anger_scale
        end
      end

      -- LOS fast-engage: LGM in range + clear LOS = cheap LOS branch.
      local los_range = R + C.TANK_COMBAT_LOS_EXTRA_RANGE
      local los_engage = (lgm.dist or 1e9) <= los_range
                     and PF.wall_hp_between(tmx, tmy, lgm.mx, lgm.my) == 0

      local cost, formula_str, shoot_mx, shoot_my
      local best_shoot_cost = math.huge

      if los_engage then
        -- (LOS_BASE + dist*LOS_PER_TILE + low_sh) * boat + threat
        local raw = C.TANK_COMBAT_LOS_BASE_COST
                  + (lgm.dist or 0) * C.TANK_COMBAT_LOS_COST_PER_TILE
                  + low_shells_penalty
        cost = raw * boat_mult + tank_tile_threat
        -- LOS engage shoots from where we stand.
        shoot_mx, shoot_my = tmx, tmy
        formula_str = string.format(
          "kill_lgm@(%d,%d) LOS (los_base{%.0f} + dist{%.1f}*per_tile{%.0f} + low_sh{%.0f}) * boat{%.2f} + threat{%.0f} = %.0f"..
          "||dist=%.1f; aim_diff=%.1f; shells=%d; near_tank=%s",
          lgm.mx, lgm.my,
          C.TANK_COMBAT_LOS_BASE_COST, lgm.dist or 0,
          C.TANK_COMBAT_LOS_COST_PER_TILE,
          low_shells_penalty, boat_mult, tank_tile_threat, cost,
          lgm.dist or 0, aim_diff, info.shells or 0,
          tostring(lgm.near_tank_idnum))
      else
        -- Standoff: walk Manhattan boundary at R around the LGM's
        -- PREDICTED future tile (where it'll be when we arrive),
        -- filter for LOS to that point, pick lowest Dijkstra cost.
        -- Driving toward the predicted spot keeps the engage point
        -- valid as the LGM moves; otherwise by the time the tank
        -- arrives, the LGM has slid forward and the spot is stale.
        -- Falls back to geometric point on predicted→tank line at
        -- distance R when slate hasn't reached any boundary tile yet.
        local center_mx = lgm.predicted_mx or lgm.mx
        local center_my = lgm.predicted_my or lgm.my
        for dx = -R, R do
          local dy_abs = R - math.abs(dx)
          local _ys = (dy_abs == 0) and { 0 } or { dy_abs, -dy_abs }
          for _, dy in ipairs(_ys) do
            local mx = center_mx + dx
            local my = center_my + dy
            if U.in_map(mx, my)
               and PF.wall_hp_between(mx, my, center_mx, center_my) == 0 then
              local c = cpf.smart_cost_dij_only(KIND_NORMAL, mx, my, boat)
              if c and c < best_shoot_cost then
                best_shoot_cost = c
                shoot_mx, shoot_my = mx, my
              end
            end
          end
        end
        local path_cost
        if not shoot_mx then
          local vdx = tmx - center_mx
          local vdy = tmy - center_my
          local vlen = math.sqrt(vdx * vdx + vdy * vdy)
          if vlen > 0.5 then
            shoot_mx = math.floor(center_mx + (vdx / vlen) * R + 0.5)
            shoot_my = math.floor(center_my + (vdy / vlen) * R + 0.5)
          else
            shoot_mx, shoot_my = tmx, tmy
          end
          if shoot_mx < 0   then shoot_mx = 0   end
          if shoot_mx > 255 then shoot_mx = 255 end
          if shoot_my < 0   then shoot_my = 0   end
          if shoot_my > 255 then shoot_my = 255 end
          -- Cold-start fallback: scale manhattan dist (Dijkstra not warm).
          path_cost = (lgm.dist or 0) * 1.5
        else
          path_cost = best_shoot_cost
        end

        -- (A* + base + low_sh - aim + xfire) * boat (if <1) + threat
        local raw = path_cost + KILL_LGM_BASE_COST + low_shells_penalty
                  - aim_bonus + crossfire
        if boat_mult < 1.0 then raw = raw * boat_mult end
        cost = raw + tank_tile_threat
        local cold = best_shoot_cost >= 1e29
        formula_str = string.format(
          "kill_lgm@(%d,%d) standoff (A*{%.0f}%s + base{%.0f} + low_sh{%.0f} - aim{%.0f} + xfire{%.0f}) * boat{%.2f} + threat{%.0f} = %.0f"..
          "||shoot_from=(%d,%d); dist=%.1f; aim_diff=%.1f; shells=%d; near_tank=%s",
          lgm.mx, lgm.my,
          path_cost, cold and " (cold)" or "",
          KILL_LGM_BASE_COST, low_shells_penalty,
          aim_bonus, crossfire, boat_mult, tank_tile_threat, cost,
          shoot_mx, shoot_my, lgm.dist or 0, aim_diff, info.shells or 0,
          tostring(lgm.near_tank_idnum))
      end

      -- Repair-mission detection: if the LGM is walking a locked
      -- straight line toward a damaged hostile pill, and we can't get
      -- in shooting range of that pill before the LGM arrives, the
      -- kill is futile — inflate the cost heavily.
      if lgm.dest_locked and lgm.v_ema_x and lgm.v_ema_y then
        local speed_wu = math.sqrt(lgm.v_ema_x * lgm.v_ema_x + lgm.v_ema_y * lgm.v_ema_y)
        if speed_wu > 0.5 then
          -- Project trajectory and check each tile for a damaged pill.
          local repair_pill = nil
          local lgm_arrive_ticks = nil
          local steps = math.min(40, math.ceil(30 * 256 / (speed_wu + 0.01)))
          for s = 1, steps do
            local px = lgm.wx + lgm.v_ema_x * s * 10
            local py = lgm.wy + lgm.v_ema_y * s * 10
            local pmx = math.floor(px) >> 8
            local pmy = math.floor(py) >> 8
            if not U.in_map(pmx, pmy) then break end
            local plist = world.pill_at and world.pill_at[pmy * 256 + pmx]
            if plist then
              for _, e in ipairs(plist) do
                if e.pill and e.pill.health and e.pill.health > 0
                   and e.pill.health < C.PILLS_MAX_HEALTH
                   and (e.pill.owner == "hostile" or e.pill.owner == "neutral") then
                  -- Check if the trajectory aims near the pill center
                  local pcx = (pmx << 8) | 128
                  local pcy = (pmy << 8) | 128
                  local err = math.abs(px - pcx) + math.abs(py - pcy)
                  if err < 192 then  -- ~0.75 tile tolerance
                    repair_pill = e.pill
                    local dist_to_pill = math.sqrt(
                      (lgm.wx - pcx) * (lgm.wx - pcx) +
                      (lgm.wy - pcy) * (lgm.wy - pcy))
                    lgm_arrive_ticks = math.ceil(dist_to_pill / speed_wu)
                    break
                  end
                end
              end
              if repair_pill then break end
            end
          end
          if repair_pill and lgm_arrive_ticks then
            -- Can we get in range of that pill before the LGM arrives?
            local pill_mx = repair_pill.mx
            local pill_my = repair_pill.my
            local shoot_range = C.KILL_LGM_SHOOT_RANGE or 8
            local our_cost = cpf.smart_cost_dij_only(KIND_NORMAL, pill_mx, pill_my, boat)
            -- Rough travel time: dijkstra cost ≈ ticks at speed ~16 wu/tick
            local our_ticks = our_cost and (our_cost * 256 / 16) or math.huge
            if our_ticks > lgm_arrive_ticks then
              -- We can't intercept — inflate cost
              local repair_penalty = 500
              cost = cost + repair_penalty
              formula_str = formula_str ..
                string.format(" +REPAIR_FUTILE{%d}(pill@(%d,%d) hp=%d lgm_arr=%dt our=%dt)",
                  repair_penalty, pill_mx, pill_my,
                  repair_pill.health, lgm_arrive_ticks,
                  math.floor(our_ticks))
            end
          end
        end
      end

      cand_rows[#cand_rows + 1] = {
        id = lgm.idnum or 0, mx = lgm.mx, my = lgm.my,
        cost = cost, own = "hostile", hp = 0, stale = 0,
      }
      state.cost_cache["13:" .. (lgm.idnum or 0)] = {
        cost = cost,
        raw = (best_shoot_cost < 1e29) and best_shoot_cost or (lgm.dist or 0),
        tick = now_t, _p = 13,
        _mx = lgm.mx, _my = lgm.my,
        formula = formula_str,
      }

      if cost < best_cost then
        best_cost = cost
        best_winner = {
          lgm = lgm, shoot_mx = shoot_mx, shoot_my = shoot_my,
          los_engage = los_engage, best_shoot_cost = best_shoot_cost,
        }
      end
    end

    if best_winner then
      local w = best_winner
      state.pool_cache[13] = {
        goal = {
          kind = "kill_lgm", mx = w.lgm.mx, my = w.lgm.my,
          wx = U.m2w(w.lgm.mx), wy = U.m2w(w.lgm.my),
          target_id = w.lgm.idnum or -1,
          shoot_mx = w.shoot_mx, shoot_my = w.shoot_my,
          _lgm_track = w.lgm,
        },
        cost = best_cost,
        desc = string.format("kill_lgm@(%d,%d) %s shoot_from=(%d,%d) dij=%s",
                             w.lgm.mx, w.lgm.my,
                             w.los_engage and "LOS" or "standoff",
                             w.shoot_mx, w.shoot_my,
                             w.best_shoot_cost < 1e29
                               and string.format("%.0f", w.best_shoot_cost)
                               or "INF"),
        cands = cand_rows,
      }
    end
  else
    -- No LGM in view → drop the override so we don't chase a stale
    -- ghost target.
    state.pool_cache[13] = nil
    if state.cost_cache then
      for k in pairs(state.cost_cache) do
        if type(k) == "string" and k:sub(1, 3) == "13:" then
          state.cost_cache[k] = nil
        end
      end
    end
  end
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
  state.pool_cache[12] = eval_wait_for_lgm(state, info)
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
local function goal_selection(state, world, info, quiet)
  local _tgs0 = clock_us()  -- used by gs_diag slow-tick check (BRAIN_PROFILE)
  local _tgs_t = 0
  local _tgs_buf = state._pick_goal_timing  -- non-nil only when BRAIN_PROFILE; set by pick_goal
  if _tgs_buf then _tgs_t = _tgs0 end
  local function _tgs_log(label)
    if not _tgs_buf then return end
    local now = clock_us()
    _tgs_buf[#_tgs_buf + 1] = string.format("      [pick_goal] %s %.2fms", label, (now - _tgs_t) / 1000)
    _tgs_t = now
  end
  local tmx    = info.tankx >> 8
  local tmy    = info.tanky >> 8
  local boat   = info.inboat
  local ammo   = (info.shells or 0) + (info.mines or 0)
  local result = nil
  local desc   = nil

  -- Clear per-tick override breakdowns so stale data from a previous Override
  -- 2 tick doesn't persist when the goal switches back to pool competition.
  state.refuel_override_breakdown = nil

  -- Clear stranded LGM state when LGM is back in tank
  if info.man_status == C.LGM_INTANK then
    state.lgm_stranded = nil
    state.lgm_stranded_check_tick = nil
    state.lgm_chase = nil
  end

  -- Helper: are we on a friendly base that can actually resupply us?
  -- Uses state.perc.base_supply (computed once per tick by perception.lua)
  -- (Was: at_resupply_base computation — set but never read after
  -- Override 2 was removed. Removed in this pass; if a future
  -- re-introduction needs it, the bs/info.base reads + on-base
  -- comparison were the entire body.)

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
        local ec = smart_cost(KIND_NORMAL, tmx, tmy, b.mx, b.my, 0,
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
  -- During active pill take, only flee at hard critical (5), not dynamic threshold
  local in_pill_take = state.goal.kind == "attack_pill"
                       and state.goal.substate ~= "plan_position"
  local effective_threshold = in_pill_take and C.ARMOUR_CRITICAL or flee_threshold
  local critical = (info.armour <= effective_threshold)

  -- Held attack exception: don't abort an attack just because shells crossed
  -- the SHELLS_LOW watermark, if we have enough shells to finish the target
  -- and keep the reserve.
  if needs_resupply and not critical
     and info.armour > C.ARMOUR_LOW then
    local held = false
    if state.capture_objective then
      local p = world.pills[state.capture_objective.id]
      if p and p.health > 0 and info.shells > p.health + C.SHELL_RESERVE then
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "pill_place" then
      local p = W.pill_at(world, state.goal.mx, state.goal.my)
      if p and p.health > 0 and p.health <= 8 then
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "attack_pill" then
      local p = W.pill_at(world, state.goal.mx, state.goal.my)
      if p and p.health > 0 and info.shells > p.health + C.SHELL_RESERVE then
        held = true
      end
    end
    if not held and state.goal and state.goal.kind == "attack_base" then
      local b = W.base_at(world, state.goal.mx, state.goal.my)
      if b and b.owner == "hostile" and b.health > 0 and info.shells > C.SHELL_RESERVE then
        held = true
      end
    end
    if held then needs_resupply = false end
  end

  _tgs_log("flee_threshold+resupply")
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
      local lgm_dist = U.mdist(tmx, tmy, lgm_mx, lgm_my)

      -- During an active build dispatch (wall_shield mode in any of
      -- the build substates) the LGM is intentionally walking AWAY
      -- from the tank to a build site, and a freshly built wall can
      -- briefly defeat the greedy pathfinder used by the strict
      -- check below. Suppress the rescue check entirely while the
      -- LGM is doing its assigned work.
      -- Extended carve-out: while the LGM is OUT during ANY active
      -- attack_pill substate (not just the build dispatch itself), the
      -- LGM is in the middle of walking out to a build site, building,
      -- or walking back through the freshly-laid walls. The rescue
      -- check's path budget regularly fails on these tight wall mazes
      -- even though the LGM will physically make it back. Suppress for
      -- the whole pill take — once the take completes (substate goes
      -- to swerve / post_engage / loiter / "rush"), the carve-out
      -- lifts and a real rescue can fire if the LGM is genuinely
      -- enclosed.
      local in_build_dispatch =
            state.goal and state.goal.kind == "attack_pill"
        and (state.goal.substate == "build_walls"
             or state.goal.substate == "ws_prebuild"
             or state.goal.substate == "ws_rebuild"
             or state.goal.substate == "ws_prewait"
             or state.goal.substate == "ws_advance"
             or state.goal.substate == "ws_engage"
             or state.goal.substate == "aim"
             or state.goal.substate == "in_range_position"
             or state.goal.substate == "in_range_aim_pre"
             or state.goal.substate == "in_range_aim"
             or state.goal.substate == "in_range_aim_finetune"
             or state.goal.substate == "shoot_pill"
             or state.goal.substate == "engage"
             or state.goal.substate == "charge"
             or state.goal.substate == "detree")

      -- Closing-rate heuristic was too loose — flagged the LGM as
      -- stranded any time it walked around an obstacle without
      -- shortening the manhattan distance to the tank, even though
      -- it could clearly still reach. Removed; the hard pathfind
      -- below is the only stranded signal now.

      -- Immediate stranded: LGM can't path to tank at all.
      -- Same in_build_dispatch carve-out as the closing-rate check
      -- above: the LGM standing on or next to a freshly built wall
      -- can momentarily fail this pathfind (limited budget, walls
      -- to navigate around) even though it'll be fine in another
      -- few ticks. Suppressing during build_walls / ws_* prevents
      -- a spurious rescue_lgm right after a wall finishes.
      if not state.lgm_stranded and not in_build_dispatch then
        if not state.lgm_stranded_check_tick
           or now - state.lgm_stranded_check_tick >= 50 then
          state.lgm_stranded_check_tick = now
          local ticks = cpf_lgm_travel_ticks(
            info.man_x, info.man_y, info.tankx, info.tanky,
            0, 0, 2000, 150)
          if ticks == -1 then
            state.lgm_stranded = true
          end

          -- Smart pickup: if LGM is nearby and arriving soon, don't rescue
          state.builder.lgm_nearby = false
          if ticks and ticks > 0 and lgm_dist <= C.LGM_NEARBY_TILES
             and ticks < C.LGM_NEARBY_ARRIVAL_TICKS then
            state.builder.lgm_nearby = true
            state.builder.lgm_arrival_ticks = ticks
            state.lgm_stranded = false
          end
          -- Stash the per-factor result for the V dialog "LGM stranded
          -- dbg" overlay (drawn next to the LGM in init.lua). Read by
          -- the LGM viz code, never affects logic here.
          state._lgm_stranded_factors = {
            path_ticks    = ticks,
            path_fail     = (ticks == -1),
            build_suppress = false,  -- only set true on the suppression branch
            nearby_carve  = state.builder.lgm_nearby or false,
            lgm_dist      = lgm_dist,
            updated_tick  = now,
          }
        end
      elseif in_build_dispatch then
        -- Suppressed branch: still expose the suppression to the viz so
        -- the user can see WHY the rescue check isn't firing.
        state._lgm_stranded_factors = {
          path_ticks    = nil,
          path_fail     = false,
          build_suppress = true,
          nearby_carve  = false,
          lgm_dist      = lgm_dist,
          updated_tick  = now,
        }
      end

      if state.lgm_stranded and (lgm_mx > 0 or lgm_my > 0) then
        -- Use the LGM's actual world position (sub-tile precision)
        -- as the steering target instead of snapping to the tile
        -- center. The LGM is somewhere within the tile, not always
        -- dead-center; chasing the precise spot lets the tank meet
        -- it without overshoot.
        result = {
          kind = "rescue_lgm", mx = lgm_mx, my = lgm_my,
          wx = info.man_x, wy = info.man_y,
        }
        desc = string.format("rescue_lgm@(%d,%d) wpos=(%d,%d) [stranded]",
                             lgm_mx, lgm_my, info.man_x, info.man_y)
        log.reason("goal", {
          pick = "rescue_lgm", why = "LGM stranded (cannot reach tank)",
          mx = lgm_mx, my = lgm_my, wx = info.man_x, wy = info.man_y,
        })
      end
    else
      state.builder.lgm_nearby = false
    end
  end

  -- ════════════════════════════════════════════════════════════════════
  -- Critical armour → inject a very low-cost flee_to_base into pool 1 so
  -- it wins cost competition naturally (no bypass / no ⚡ override badge).
  -- Same base-selection logic as the old Override 1 (FLEE_DANGER_WEIGHT
  -- via nearest_resupply_base).
  -- Gated by C.CRITICAL_FLEE_ENABLED — when off, normal pool-1 refuel
  -- candidate competes via its usual shaping (REFUEL_DEFICIT_BONUS already
  -- pushes cost very low at critical armour).
  -- ════════════════════════════════════════════════════════════════════
  if critical and C.CRITICAL_FLEE_ENABLED then
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
      local win_cand = nil
      for _, c in ipairs(base_cands) do
        if c.id == bid then win_cand = c; break end
      end
      -- Overwrite pool 1 with the critical flee candidate. Cost is
      -- min(40, prior_pool1_cost) — 40 is the design ceiling ("beats
      -- most goals but close free captures still win") but since
      -- urgency is now squared, the non-critical refuel cost at e.g.
      -- armour=6 can already dip below 40; without the floor swap the
      -- curve would be non-monotonic (armour=5 more expensive than
      -- armour=6). _critical_flee skips normal pool-1 shaping so
      -- BASE_COST/DEFICIT/LGM_WAIT don't layer on top.
      local _prev_cost = (state.pool_cache[1] and state.pool_cache[1].cost) or math.huge
      local _crit_cost = math.min(40, _prev_cost)
      local flee_desc = BRAIN_POOL_VIZ and string.format("CRITICAL flee_to_base#%d arm=%.0f<%d dist=%.0f cost=%.0f",
                                       bid, info.armour, flee_threshold, bdist, _crit_cost) or ""
      state.pool_cache[1] = {
        goal = {
          kind = "flee_to_base", mx = base.mx, my = base.my,
          wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid,
        },
        cost = _crit_cost,
        desc = flee_desc,
        cands = {
          { id = bid, mx = base.mx, my = base.my,
            cost = _crit_cost,
            own = "friendly", hp = 0,
            stale = 0 },
        },
        _critical_flee = true,
      }
      -- Seed cost_cache so the display row can find a non-nil cost + formula.
      if not state.cost_cache then state.cost_cache = {} end
      state.cost_cache["1:" .. bid] = {
        cost = _crit_cost, raw = bdist, tick = state.tick or 0, _p = 1,
        _mx = base.mx, _my = base.my,
        _dv = 0, _dang = 0, _age = 0,
        _stale = 0, _contest = 0,
        _hyst = 0, _ratio = 1, _dep = 0,
        formula = string.format("CRITICAL flee_to_base#%d arm{%.0f}<thr{%d} dist{%.0f} cost{%.0f}"..
                                "||Critical-armour refuel injection: min(40, prior_pool1_cost)",
                                bid, info.armour, flee_threshold, bdist, _crit_cost),
      }
      state.flee_breakdown = {
        bid = bid, mx = base.mx, my = base.my, dist = bdist,
        armour = info.armour, threshold = flee_threshold,
        travel   = win_cand and win_cand.travel   or bdist,
        danger   = win_cand and win_cand.danger   or 0,
        dw       = C.FLEE_DANGER_WEIGHT,
        hyst     = win_cand and win_cand.hyst     or false,
        contested= win_cand and win_cand.contested or false,
        stale    = win_cand and win_cand.stale     or 0,
        cands    = base_cands,
      }
      log.reason("goal", {
        pick = "critical_flee_injection", why = "armour critical",
        arm = info.armour, sh = info.shells, critical = true,
        chosen_id = bid, chosen_score = bdist,
        candidates = base_cands,
      })
    end
  end

  _tgs_log("override_0_1")
  -- Override 2 was removed in favour of dynamic cost shaping on pool 1:
  -- refuel_at_base now competes naturally (see REFUEL_BASE_COST /
  -- REFUEL_DEFICIT_BONUS application during cost competition). base_shield
  -- attachment for refuel_at_base winners happens post-competition, just
  -- before `return result`.

  -- ════════════════════════════════════════════════════════════════════
  -- Override 3: Capture objective (cp command)
  -- ════════════════════════════════════════════════════════════════════
  if not result and state.capture_objective then
    local co = state.capture_objective
    local p  = world.pills[co.id]
    if not p or p.owner == "friendly" then
      state.command_reply     = string.format(C.BRAIN_NAME .. ": pill #%d captured!", co.id)
      state.capture_objective = nil
    elseif p.health == 0 then
      if not co.kill_tick then
        co.kill_tick = state.tick
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
        function(b) return b.owner ~= "friendly" end, boat, ammo, state, info, KIND_NORMAL)
      if not base then
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
          bco.id = nil; bco.mx = 0; bco.my = 0
          state.pf.status = "idle"
          state.stuck_for = 0
        else
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

  _tgs_log("override_3_4")
  -- ════════════════════════════════════════════════════════════════════
  -- Cost-based goal competition
  -- Pool results are pre-evaluated by update_pool_cache() one per tick
  -- in the ticks leading up to the decision.  Here we just assemble
  -- the cached results, apply hysteresis, and pick the winner.
  -- ════════════════════════════════════════════════════════════════════
  local _tgs_pre_pool = clock_us()
  _tgs_log("pre_pool")
  if not result then
    local pc = state.pool_cache or {}
    -- Build pool with copies so hysteresis doesn't mutate cached costs
    local phase_weights = C.PHASE_WEIGHTS[state.phase]
    local pool = {}
    local now = state.tick or 0
    -- Count pools available (verbose summary, suppressed in quiet mode)
    if not quiet and BRAIN_DEBUG_MODE then
      local pc_count = 0
      local pc_names = {}
      for idx, entry in pairs(pc) do
        if entry then
          pc_count = pc_count + 1
          pc_names[#pc_names + 1] = (POOL_NAMES[idx] or ("pool" .. idx))
        end
      end
    end
    -- ── Refuel live cost-shape (pool 1) ─────────────────────────────────
    -- Compute once per tick. The same values feed (a) the pool-1 cost
    -- competition shaping below and (b) the Term Breakdown panel via a
    -- stash onto every per-base pool-1 cost_cache entry. eval_refuel
    -- already baked the multiplicative urgency into pool_cache[1].cost,
    -- so we recompute it here purely for the breakdown display.
    local _ref_arm_def = math.max(0, (C.ARMOUR_LOW - info.armour) / C.ARMOUR_LOW)
    local _ref_sh_def  = math.max(0, (C.SHELLS_LOW  - info.shells) / C.SHELLS_LOW)
    local _ref_deficit = math.max(_ref_arm_def, _ref_sh_def)
    local _ref_bonus   = C.REFUEL_DEFICIT_BONUS * _ref_deficit
    local _ref_mult    = 1.0
    local _ref_fill    = 0.0
    if info.armour > C.ARMOUR_LOW and info.shells > C.SHELLS_LOW then
      local _fa = (info.armour - C.ARMOUR_LOW)
                  / math.max(1, state.armour_target - C.ARMOUR_LOW)
      local _fs = (info.shells - C.SHELLS_LOW)
                  / math.max(1, state.shell_target  - C.SHELLS_LOW)
      _ref_fill = math.min(_fa, _fs)
      _ref_mult = 1.0 + _ref_fill * (C.REFUEL_FULL_COST_MULT - 1.0)
    end
    local _ref_arm_u   = math.min(1.0, info.armour / C.ARMOUR_LOW)
    local _ref_sh_u    = math.min(1.0, info.shells / C.SHELLS_LOW)
    local _ref_urgency = math.max(C.REFUEL_URGENCY_MIN, math.min(_ref_arm_u, _ref_sh_u))
    -- Stash on every pool-1 cache entry so the panel sees the same shape
    -- it'd see if it called the live computation itself. _lgm_wait_floor
    -- starts as nil and only gets set on the at-this-base entry below.
    if BRAIN_POOL_VIZ and state.cost_cache then
      for _, e in pairs(state.cost_cache) do
        if e._p == 1 then
          e._urgency        = _ref_urgency
          e._base_floor     = C.REFUEL_BASE_COST
          e._defic_bonus    = _ref_bonus
          e._fill_mult      = _ref_mult
          e._fill           = _ref_fill
          e._arm            = info.armour
          e._sh             = info.shells
          e._arm_def        = _ref_arm_def
          e._sh_def         = _ref_sh_def
          e._lgm_wait_floor = nil
          e.formula         = nil  -- invalidate cached formula so live shape re-renders
        end
      end
    end

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
        -- Skip goals on abandon cooldown (prevent oscillation loops)
        if gmx and gmy and entry.goal and state.goal_cooldowns then
          local cd_key = entry.goal.kind .. ":" .. gmx .. "," .. gmy
          local cd_exp = state.goal_cooldowns[cd_key]
          if cd_exp and now < cd_exp then
            goto continue_pool
          end
        end
        -- Pool 1 (refuel): apply dynamic cost shaping every tick. The cached
        -- cost holds travel/danger/stale/contested/hyst/depletion. Here we
        -- add BASE_COST (flat floor so at-own-base isn't ~0), subtract a
        -- deficit-scaled BONUS (live from info.armour/info.shells — lets the
        -- bot peel off to a close opportunity as it refuels), then multiply
        -- by REFUEL_FULL_COST_MULT scaling between LOW and target.
        --   * At dynamic target: skip entirely.
        if idx == 1 and entry.goal and not entry._critical_flee then
          -- Stay-for-LGM: if we're standing on this base and our LGM is
          -- returning to us soon, keep refuel in the running at a low cost
          -- floor so it usually wins — but stays beatable by a close combat
          -- opportunity. Bypasses the at-target skip below.
          local at_this_base = (entry.goal.mx == tmx and entry.goal.my == tmy)
          local lgm_eta = state.builder and state.builder.lgm_eta
          local lgm_returning = lgm_eta
            and lgm_eta > (state.tick or 0) + C.LGM_ETA_DEPART_BUFFER
          local lgm_wait_here = at_this_base and lgm_returning

          if info.armour >= state.armour_target and info.shells >= state.shell_target
             and not lgm_wait_here then
            goto continue_pool   -- at dynamic target and no LGM waiting: don't compete
          end
          -- Reuse the live shape computed above (also stashed on cost_cache
          -- for the breakdown panel).
          local bonus = _ref_bonus
          local mult  = _ref_mult
          local base_cost = (entry.cost or 0) - bonus
          local final_cost = base_cost * mult
          -- LGM-wait floor: clamp cost down when waiting for LGM.
          -- Floor scales with threat at the base: safe spots clamp lower so
          -- sitting still is cheaper when there's no reason to move. Linear
          -- blend between LGM_WAIT_COST_SAFE (0 threat) and LGM_WAIT_COST
          -- (at/above LGM_WAIT_SAFE_THRESHOLD).
          if lgm_wait_here then
            local thr = threat.at(entry.goal.mx, entry.goal.my)
            local danger_frac = math.min(1.0, thr / C.LGM_WAIT_SAFE_THRESHOLD)
            local wait_floor = C.LGM_WAIT_COST_SAFE
              + (C.LGM_WAIT_COST - C.LGM_WAIT_COST_SAFE) * danger_frac
            if final_cost > wait_floor then
              final_cost = wait_floor
            end
            if BRAIN_POOL_VIZ and entry.goal.target_id and state.cost_cache then
              local ce = state.cost_cache["1:" .. entry.goal.target_id]
              if ce then ce._lgm_wait_floor = wait_floor end
            end
          end
          entry = { goal = entry.goal, desc = entry.desc,
                    cost = final_cost,
                    cands = entry.cands, _pill = entry._pill, _pill_id = entry._pill_id,
                    _lgm_wait = lgm_wait_here or nil }
        end
        local cost = entry.cost
        -- Apply phase-dependent weight as a multiplier on the full cost,
        -- but loc_mult only scales a fixed base (STRATEGIC_LOC_BASE) as
        -- an additive adjustment. This keeps location bonus/penalty
        -- consistent regardless of travel distance.
        local pool_name = POOL_NAMES[idx]
        local pw = phase_weights and pool_name and phase_weights[pool_name] or 1.0
        local lm = entry.loc_mult or 1.0
        local is_cur = state.goal and state.goal.kind and entry.goal
          and entry.goal.kind == state.goal.kind
          and entry.goal.mx   == state.goal.mx
          and entry.goal.my   == state.goal.my
        if is_cur then lm = 1.0 end
        if cost and cost > 0 then
          -- Phase weight still scales the full cost
          cost = cost * pw
          -- Loc multiplier applies to capture_pill base cost only (additive)
          local loc_adj = C.CAPTURE_PILL_BASE_COST * (lm - 1.0)
          cost = cost + loc_adj
        end
        -- Mid-take engage-break: hard-cap under pool-6's mid-take floor
        -- (10) so phase weight / loc_adj can't push it back over.
        if entry._engage_break_lock then
          cost = math.min(cost, 9)
        end
        pool[#pool + 1] = {
          cost = cost, _base_cost = cost,  -- _base_cost preserved for breakdown display
          goal = entry.goal, desc = entry.desc,
          cands = entry.cands, _pill = entry._pill, _pill_id = entry._pill_id,
          phase_weight = pw,
          loc_mult = lm, loc_reason = entry.loc_reason or "",
          _engage_break_lock = entry._engage_break_lock,
        }
        ::continue_pool::
      end
    end

    -- ── Influence-based cost scaling ──
    -- Goals in hostile territory (influence < -50) cost 2×; goals in
    -- friendly territory (influence > 50) cost 0.5×. Skipped during
    -- opening phase (territory not established yet).
    if state.phase ~= "opening" then
      local INF_EXEMPT = {
        refuel_at_base=true,
      }
      for _, c in ipairs(pool) do
        if c.goal and c.goal.mx and c.goal.my
           and not INF_EXEMPT[c.goal.kind] then
          local inf = cpf.influence_at(c.goal.mx, c.goal.my) or 0
          if inf < -50 then
            c.cost = c.cost * 2.0
            c._inf_mult = 2.0
          elseif inf > 50 then
            c.cost = c.cost * 0.5
            c._inf_mult = 0.5
          end
        end
      end
    end

    -- ── Apply hysteresis to discourage thrashing ──
    -- High-value opportunistic goals are exempt so they can win on raw
    -- cost (flee_to_base / rescue_lgm skip this pool entirely as
    -- overrides). capture_pill is ALWAYS exempt — pills die in finite
    -- time and the capture window is short, so paying switch+commitment
    -- to skip it would lose us the resource. attack_tank only exempts
    -- when we're NOT mid-attack_pill — during a take, the engage-break
    -- lock (cost < 9) handles real tank threats; everything else
    -- shouldn't yank us off the attack.
    local cur_is_attack_pill = (state.goal.kind == "attack_pill")
    -- kill_lgm: ALWAYS exempt.  Enemy LGMs are extremely time-critical
    -- (1-shot kill, ~10s of vulnerability before they return to tank)
    -- and the cost formula (20 + dist*1.5) is already very low — adding
    -- switch+commitment penalty on top reliably pushes it above any
    -- attack_pill incumbent, so the bot never actually picks the kill.
    local HYST_EXEMPT = { capture_pill = true, kill_lgm = true }
    if not cur_is_attack_pill or state._shot_by_tank then
      HYST_EXEMPT.attack_tank = true
    end
    if C.EARLY_CAPTURE_BASE_HYST_EXEMPT and state.phase == "opening" then
      HYST_EXEMPT.capture_base = true
    end
    if state.urgent_capture_base then
      local uc = state.urgent_capture_base
      if (state.tick or 0) - uc.tick < 500 then
        HYST_EXEMPT.capture_base = true
      end
    end
    local cur_group = goal_group(state.goal.kind)
    local ticks_on_goal = (state.tick or 0) - (state.goal_set_tick or 0)
    local commitment = math.min(ticks_on_goal * C.GOAL_COMMITMENT_PER_TICK, C.GOAL_COMMITMENT_CAP)
    -- Wall-shield + shielded pill takes are a big investment (trees,
    -- LGM time, positioning); add extra penalty to discourage abandoning
    -- them mid-attack just because another pillbox got slightly cheaper.
    -- Includes both the legacy ws_* substates and the newer shielded
    -- pill-take substates introduced with PPT (gather_trees through
    -- shoot_pill — each represents real progress that resets if we
    -- swap targets).
    -- WS_SUBS hoisted to module scope (see top of file).
    local cur_sub = state.goal and state.goal.substate
    if cur_sub and WS_SUBS[cur_sub] then
      commitment = commitment + C.WALL_SHIELD_COMMITMENT
    end
    -- Track per-goal commitment bonuses. Bonuses stack on TOP of the capped
    -- base commitment so they aren't swallowed by GOAL_COMMITMENT_CAP.
    local cur_is_attack_tank = (state.goal.kind == "attack_tank")
    for _, c in ipairs(pool) do
      -- Engage-break: an attack_tank goal that detected a mid-take
      -- threat (tank in range, further from pill than us) is exempt
      -- from both additive SW+CM penalty AND the multiplicative ratio
      -- gate (the gate only fires for entries with c.hysteresis set,
      -- which we leave nil here).
      if HYST_EXEMPT[c.goal.kind] or c._engage_break_lock then
        -- Exempt goals skip the type-switch penalty (switching from
        -- another group is free). But within the same group, the
        -- target-switch penalty still applies to prevent spinning
        -- between targets (e.g. two capture_base candidates).
        local cg = goal_group(c.goal.kind)
        if cg == cur_group
           and (c.goal.mx ~= state.goal.mx or c.goal.my ~= state.goal.my) then
          c.cost = c.cost + C.GOAL_TARGET_SWITCH_PENALTY
          c.hysteresis = "target"
          c.switch_flat = C.GOAL_TARGET_SWITCH_PENALTY
          c.commit_val  = 0
        end
        goto continue_hyst
      end
      local cg = goal_group(c.goal.kind)
      local effective_commit = commitment
      if cur_is_attack_tank then
        effective_commit = effective_commit + C.ATTACK_TANK_COMMITMENT_BONUS
      end
      if cur_is_attack_pill and c.goal.kind ~= "attack_tank" then
        effective_commit = effective_commit + C.ATTACK_PILL_COMMITMENT_BONUS
      end
      if cg ~= cur_group then
        c.cost = c.cost + C.GOAL_SWITCH_PENALTY + effective_commit
        c.hysteresis = "type"
        c.switch_flat = C.GOAL_SWITCH_PENALTY
        c.commit_val  = effective_commit
      elseif c.goal.mx ~= state.goal.mx or c.goal.my ~= state.goal.my then
        c.cost = c.cost + C.GOAL_TARGET_SWITCH_PENALTY + effective_commit
        c.hysteresis = "target"
        c.switch_flat = C.GOAL_TARGET_SWITCH_PENALTY
        c.commit_val  = effective_commit
      end
      ::continue_hyst::
    end

    -- ── Ally-claimed pass-through ──
    -- The actual cost penalty is applied per-target in step_eval_queue
    -- (so cost_cache + per-pool row reflect it regardless of whether
    -- goal_selection has run yet).  Here we just propagate the flag
    -- from cost_cache onto the pool entry so the WINNERS row breakdown
    -- can carry ally_claimed_pen through to goal_competition.
    for _, c in ipairs(pool) do
      local pidx = KIND_TO_POOL[c.goal.kind]
      if pidx and c.goal.target_id and state.cost_cache then
        local ce = state.cost_cache[pidx .. ":" .. c.goal.target_id]
        if ce and ce.ally_claimed_pen and ce.ally_claimed_pen > 0 then
          c.ally_claimed_pen = ce.ally_claimed_pen
          c.ally_claimed_by  = ce.ally_claimed_by
          -- (per-match audit log is now in step_eval_queue where the
          -- cost is actually applied.)
        end
      end
    end

    -- ── Oscillation history penalty (exponential by recurrence count) ──
    -- Count how often each candidate's (kind, mx, my) and just (kind)
    -- have appeared in the recent goal history. Apply 2^count * BASE
    -- penalty to break refuel→capture→refuel-style loops where the bot
    -- cycles through the same goals.
    -- The CURRENT goal is exempt: the whole history trail is *its own*
    -- appearances, so penalising it here would perversely favour any
    -- switch away from it.
    local hist = state.goal_history or {}
    if #hist > 0 then
      for _, c in ipairs(pool) do
        local is_current = state.goal
          and c.goal.kind == state.goal.kind
          and c.goal.mx   == state.goal.mx
          and c.goal.my   == state.goal.my
        if is_current then goto continue_hist end
        -- capture_pill is exempt from history thrash penalty too —
        -- same rationale as the HYST_EXEMPT block above: pills die in
        -- finite time and the capture window is short.
        if HYST_EXEMPT[c.goal.kind] then goto continue_hist end
        local target_count = 0
        local kind_count = 0
        for _, h in ipairs(hist) do
          if h.kind == c.goal.kind then
            kind_count = kind_count + 1
            if h.mx == (c.goal.mx or 0) and h.my == (c.goal.my or 0) then
              target_count = target_count + 1
            end
          end
        end
        local target_pen = 0
        local kind_pen   = 0
        if target_count > 0 then
          target_pen = C.GOAL_HISTORY_TARGET_BASE * (C.GOAL_HISTORY_EXP ^ target_count - 1)
        end
        if kind_count > 0 then
          kind_pen = C.GOAL_HISTORY_KIND_BASE * (C.GOAL_HISTORY_EXP ^ kind_count - 1)
        end
        if target_pen + kind_pen > 0 then
          c.cost = c.cost + target_pen + kind_pen
          c.hist_target = target_count
          c.hist_kind   = kind_count
        end
        ::continue_hist::
      end
    end

    if not quiet and BRAIN_DEBUG_MODE then
    end
    -- ── Sort by cost, pick winner ──
    table.sort(pool, function(a, b) return a.cost < b.cost end)
    -- Save the post-penalty competition for the pool breakdown display.
    -- Phase 0 scaffolding: loc_mult/density/pickup/wsim_add are carried
    -- through with safe defaults; Phases 1–4 will populate them on `c`
    -- before we reach this point. wsim_add is patched in after the wsim
    -- pass below since wsim runs later in the pipeline.
    -- Pool-grid panel data only — wrapped so lua_strip removes it from opt/.
    if not quiet and BRAIN_DEBUG_MODE and #pool > 0 then
      for i = 1, math.min(5, #pool) do
        local c = pool[i]
        local hist_str = ""
        if c.hist_target or c.hist_kind then
          hist_str = string.format(" hist(t=%d k=%d)",
                                   c.hist_target or 0, c.hist_kind or 0)
        end
      end
    end

    -- ── Multiplicative hysteresis ──
    -- After the additive penalty + sort, if the winner is a different
    -- group/target from the current goal, require it to be at least
    -- (1 - GOAL_SWITCH_RATIO) cheaper than the current target's
    -- post-penalty cost. Otherwise fall back to the current target.
    --
    -- The current target's pool entry has hysteresis == nil (additive
    -- penalty was zero) so its cost is comparable to the candidates'
    -- post-penalty costs.
    if #pool >= 2 and state.goal and state.goal.kind ~= "none" then
      local winner = pool[1]
      local switching = (winner.hysteresis == "type"
                         or winner.hysteresis == "target")
      if switching then
        local cur_entry = nil
        for _, c in ipairs(pool) do
          if c.goal.kind == state.goal.kind
             and c.goal.mx == state.goal.mx
             and c.goal.my == state.goal.my then
            cur_entry = c
            break
          end
        end
        if cur_entry and winner.cost > cur_entry.cost * C.GOAL_SWITCH_RATIO then
          -- Promote current to position 1 so the rest of the pipeline
          -- (wsim adjustment, picking pool[1]) chooses it.
          for i, c in ipairs(pool) do
            if c == cur_entry then
              table.remove(pool, i)
              break
            end
          end
          table.insert(pool, 1, cur_entry)
        end
      end
    end

    -- ── Forward simulation: adjust costs and reject lethal paths ──
    -- Skip wsim for attack_pill when we're already in an active attack substate —
    -- the sim uses a straight-line path and doesn't know about wall shields,
    -- standoff positions, etc. The commitment hysteresis handles these cases.
    local cur_attack_active = state.goal
      and (state.goal.kind == "attack_pill" or state.goal.kind == "attack_pill"
           or state.goal.kind == "pill_place")
      and state.goal.substate ~= nil

    -- Opening-phase wsim disable. The sim uses a straight-line path
    -- through the current danger field, which in opening overlaps with
    -- spawn pills and produces wildly pessimistic damage estimates that
    -- penalize every advance. C.WSIM_OPENING_ENABLED toggles whether
    -- wsim runs at all during the opening phase.
    local wsim_active = C.WSIM_ENABLED
    if wsim_active and state.phase == "opening"
       and C.WSIM_OPENING_ENABLED == false then
      wsim_active = false
    end
    -- Capacity tier wsim cap: nil = sim every entry (default), integer N =
    -- sim top-N of the cost-sorted pool only, false = skip entirely.
    local _wsim_cap = nil
    if state._capacity then
      _wsim_cap = state._capacity.wsim
      if _wsim_cap == false then wsim_active = false end
    end
    local _tgs_pre_wsim = clock_us()
    _tgs_log("pool_build")
    if wsim_active then
      local _wsim_done = 0
      for _, c in ipairs(pool) do
        if _wsim_cap and _wsim_done >= _wsim_cap then break end
        _wsim_done = _wsim_done + 1
        -- Only sim goals that travel through danger (skip refuel/explore)
        local sim_kinds = { capture_base=true, capture_pill=true,
                            attack_pill=true, attack_base=true,
                            attack_tank=true, kill_lgm=true,
                            place_pill_strategic=true }
        -- Don't sim our current attack target if we're mid-attack
        local skip = cur_attack_active
          and c.goal.kind == state.goal.kind
          and c.goal.mx == state.goal.mx and c.goal.my == state.goal.my

        if sim_kinds[c.goal.kind] and not skip then
          local attack_id = nil
          local spot_x, spot_y = nil, nil
          if c.goal.kind == "attack_pill" and c._pill_id then
            attack_id = c._pill_id
            -- Look up the best firing position from the cost cache
            local ck = "6:" .. c._pill_id
            local cached = state.cost_cache and state.cost_cache[ck]
            if cached and cached._spot_mx and cached._spot_mx > 0 then
              spot_x, spot_y = cached._spot_mx, cached._spot_my
            end
          end
          local extra, killed, sdesc, wsim_path, wsim_result = wsim_evaluate_goal(c.goal, world, info, attack_id, spot_x, spot_y)
          -- Debug print: every wsim run, even 0-damage survivors.
          c.wsim_add = (c.wsim_add or 0) + extra
          c.cost = c.cost + extra
          if BRAIN_POOL_VIZ then c.desc = c.desc .. sdesc end
          c.wsim_ran = true
          c.wsim_damage = wsim_result and wsim_result.damage or 0
          c.wsim_arm_before = info.armour
          c.wsim_arm_after  = wsim_result and wsim_result.armour or info.armour
          c.wsim_ticks = wsim_result and wsim_result.ticks or 0
          -- Predicted-death reject. In opening phase the sim is too
          -- twitchy to trust (every advance looks fatal vs. spawn
          -- pills), so by default we suppress the reject there. Flip
          -- C.WSIM_KILL_REJECT_OPENING true to enforce it everywhere.
          local in_opening = (state.phase == "opening")
          local desperate = state._stuck_desperate or false
          local enforce = C.WSIM_KILL_REJECT
                          and (C.WSIM_KILL_REJECT_OPENING or not in_opening)
                          and not desperate
          if killed and enforce then
            c.cost = c.cost + 99999  -- effectively reject
            c.wsim_killed = true
          end
          -- Store wsim path for visualization on killed/high-damage goals
          if wsim_path and (killed or extra > 50) then
            c.wsim_path = wsim_path
            c.wsim_detail = sdesc
            c.wsim_hits = wsim_result and wsim_result.hits or nil
          end
        end
      end
      -- Re-sort after sim adjustments
      table.sort(pool, function(a, b) return a.cost < b.cost end)
    end
    local _tgs_post_wsim = clock_us()
    _tgs_log("wsim")
    if BRAIN_PROFILE_LOG and (_tgs_post_wsim - _tgs0) > 500 then
      opt.append("optimize.log", string.format(
        "  [gs_diag] total=%.2fms pre_pool=%.2fms pool_build=%.2fms wsim=%.2fms pool_size=%d",
        (_tgs_post_wsim-_tgs0)/1000, (_tgs_pre_pool-_tgs0)/1000,
        (_tgs_pre_wsim-_tgs_pre_pool)/1000, (_tgs_post_wsim-_tgs_pre_wsim)/1000, #pool))
    end

    -- Phase 0 scaffolding: patch wsim_add + final total back into the
    -- goal_competition entries built pre-wsim so the pool window shows
    -- post-wsim totals and the +wsim{N} breakdown row.
    -- Pool-grid panel data + diagnostic prints + viz paths — wrapped so
    -- lua_strip removes it from opt/.

    if #pool > 0 then
      -- Dump the FINAL post-everything scores for every candidate so a
      -- replan tick can be reconstructed after the fact. Stripped from
      -- opt/ via print2.
      for i, c in ipairs(pool) do
        local base = c._base_cost or c.cost
        local penalty = (c.cost or 0) - base
      end

      local winner = pool[1]

      -- Pool log + winner_cands + log.reason are debug/log-only — wrapped
      -- so lua_strip removes them from opt/.

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

  _tgs_log("post_wsim_winner")
  -- ── Log strategic goal changes ──
  -- Compare on stable key (kind+target) so wsim tick changes don't spam
  local goal_key = result and string.format("%s@%d,%d", result.kind, result.mx or 0, result.my or 0) or nil
  if goal_key ~= last_strategic_goal then
    if not quiet and BRAIN_DEBUG_MODE then
      if desc then
        print(TAG .. " GOAL: " .. desc)
      elseif last_strategic_goal then
        print(TAG .. " GOAL: strategic goals clear")
      end
    end
    last_strategic_goal = goal_key
  end

  _tgs_log("goal_log")
  -- Base shield: if refuel_at_base won and we're standing on the base, try
  -- to have the LGM build a wall between us and a nearby hostile pill so it
  -- can't shoot us while we sit. Two triggers: calm pill in range (proactive),
  -- or active fire but the wall tile is ≥4 tiles from the pill (reactive).
  if result and result.kind == "refuel_at_base"
     and result.mx == tmx and result.my == tmy then
    local shield_pill, shield_dist = nil, math.huge
    local pill_threats = state.perc and state.perc.pill_threats or {}
    for _, pt in ipairs(pill_threats) do
      if pt.dist >= C.BASE_SHIELD_MIN_DIST and pt.dist < shield_dist then
        if pt.anger < C.BASE_SHIELD_MAX_ANGER then
          shield_pill = pt.pill
          shield_dist = pt.dist
        elseif pt.anger > 0.3 and pt.dist >= 4 then
          shield_pill = pt.pill
          shield_dist = pt.dist
        end
      end
    end
    if shield_pill then
      local dx = shield_pill.mx - tmx
      local dy = shield_pill.my - tmy
      local wx, wy
      if math.abs(dx) >= math.abs(dy) then
        wx = tmx + (dx > 0 and 1 or -1); wy = tmy
      else
        wx = tmx; wy = tmy + (dy > 0 and 1 or -1)
      end
      local wtt = U.ttype(wx, wy)
      if wtt ~= C.T_BUILDING and wtt ~= C.T_HALFBUILD
         and wtt ~= C.T_RIVER and wtt ~= C.T_DEEPSEA then
        result.base_shield = true
        result.shield_wall_mx = wx
        result.shield_wall_my = wy
        log.reason("goal", {
          pick = "base_shield", why = "pill in range while refueling",
          pill_mx = shield_pill.mx, pill_my = shield_pill.my,
          pill_anger = shield_pill.anger or 0, pill_dist = shield_dist,
          wall_mx = wx, wall_my = wy,
        })
      end
    end
  end

  _tgs_log("base_shield")
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
    -- Echo to optimize.log so we can debug "stuck on explore" without
    -- needing print2 enabled. Includes which pools had cached winners
    -- so we can see whether pool_cache was empty or just got filtered
    -- out (blocked / cooldown / phase weight zero).
    if BRAIN_PROFILE_LOG then
      do
        local pc_summary = {}
        local pc = state.pool_cache or {}
        for idx, e in pairs(pc) do
          if e then
            pc_summary[#pc_summary + 1] = string.format(
              "p%d:%s@(%d,%d)cost=%.0f",
              idx, e.goal and e.goal.kind or "?",
              e.goal and e.goal.mx or 0, e.goal and e.goal.my or 0,
              e.cost or -1)
          end
        end
        opt.append("optimize.log", string.format(
          "  [diag] goal_selection returned nil tick=%d skips=[%s] pool_cache=[%s]",
          state.tick or 0,
          table.concat(skip_reasons, "; "),
          table.concat(pc_summary, " | ")))
      end
    end
  end

  _tgs_log("no_result_log")
  return result
end

-- Main goal picker: commands > strategic > exploration
function M.pick_goal(state, world, info, quiet)
  -- Cache the few info fields the breakdown viz needs (pool_breakdown has
  -- only `state` in scope).
  state._last_info = {
    tmx = info.tankx >> 8, tmy = info.tanky >> 8,
    at_base = (info.base and info.base.id and info.base.id > 0) or false,
    man_status = info.man_status,
    inboat = info.inboat,
    carried_pills = info.carried_pills or 0,
    player_number = info.player_number,
    num_players = info.num_players,
    max_players = info.max_players,
    allies = info.allies or 0,
    player_names = info.player_names,
    armour = info.armour, shells = info.shells,
  }
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
    elseif cg.kind == "attack_pill" then
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
      state.command_reply = string.format(C.BRAIN_NAME .. ": arrived at %s #%d (%d,%d)",
        cg.kind, cg.id, cg.mx, cg.my)
      state.command_goal = nil
      -- Fall through to exploration (if enabled)
    else
      local g = { kind = cg.kind, mx = cg.mx, my = cg.my, wx = cg.wx, wy = cg.wy }
      -- For pill attacks via command, run through plan_position to
      -- compute standoff + approach point properly.
      if cg.kind == "attack_pill" then
        g.substate = "plan_position"
        g.target_id = cg.id
      elseif cg.kind == "pill_place" then
        -- Pill placement command: set substate if not already set
        if not g.substate then
          g.substate = "select_pill"
        end
      end
      return g
    end
  end

  -- Strategic goal selection (timing buf populated only when perf-log is active)
  state._pick_goal_timing = _G.BRAIN_PROFILE and {} or nil
  local strategic = goal_selection(state, world, info, quiet)
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
    expl.frontier_pop(state.frontier)
    fx, fy = expl.best_frontier(state)
  end

  if fx then
    local dist = U.mdist(tmx, tmy, fx, fy)
    state.explore_breakdown = {
      mx = fx, my = fy, dist = dist,
      source = "frontier_heap",
      frontier_size = #(state.frontier or {}),
    }
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
      expl.frontier_push(state.frontier, best_dist, best_fx, best_fy)
    end
    state.explore_breakdown = {
      mx = best_fx, my = best_fy, dist = best_dist,
      source = "nearest_scan",
      frontier_size = 0,
    }
    return { kind = "explore", mx = best_fx, my = best_fy,
             wx = U.m2w(best_fx), wy = U.m2w(best_fy) }
  end

  state.explore_breakdown = nil
  return { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
end

-- =========================================================================
-- get_queue_status — returns formatted string of all queue items,
-- their scores, and cache ages.  Called from C for the log window.
-- =========================================================================
-- Returns tab-delimited rows for C-side colored rendering.
-- Format per row: pool_idx \t pool_name \t id \t mx \t my \t cost \t age \t status
-- First row is header: "QUEUE" \t count \t pos \t total
function M.get_queue_status(state)
  local queue = state.eval_queue
  if not queue or #queue == 0 then return "QUEUE\t0\t0\t0" end

  local now = state.tick or 0
  local cache = state.cost_cache or {}
  local pos = state.eval_queue_pos or 1
  local phase_weights = C.PHASE_WEIGHTS[state.phase]
  local lines = {}
  lines[#lines+1] = string.format("QUEUE\t%d\t%d\t%d\t%s", #queue, pos, #queue, state.phase or "?")

  -- Collect all entries, then sort by weighted cost
  local entries = {}
  for i, item in ipairs(queue) do
    local pname = POOL_NAMES[item.pool] or ("pool" .. item.pool)
    local obj = item.obj
    local id = item.id
    local cache_key = item.pool .. ":" .. id
    local cached = cache[cache_key]

    local cost_val = cached and cached.cost or -1
    local age = cached and (now - cached.tick) or -1
    local status = (i < pos) and "done" or "pending"
    local formula = cached and get_formula(cached) or ""
    local raw_val = cached and cached.raw or -1

    -- Look up phase weight for this pool
    local pw = phase_weights and pname and phase_weights[pname] or 1.0
    local weighted = cost_val >= 0 and (cost_val * pw) or -1

    -- Check if this goal is on abandon cooldown or blocked
    local flags = ""
    local gmx, gmy = obj.mx or 0, obj.my or 0
    if state.goal_cooldowns then
      local kind = cached and cached.goal and cached.goal.kind or pname
      local cd_key = kind .. ":" .. gmx .. "," .. gmy
      local cd_exp = state.goal_cooldowns[cd_key]
      if cd_exp and now < cd_exp then
        flags = flags .. "CD:" .. (cd_exp - now) .. "t "
      end
    end
    if state.blocked then
      local bk = U.mkey(gmx, gmy)
      if state.blocked[bk] and now < state.blocked[bk] then
        flags = flags .. "BLK:" .. (state.blocked[bk] - now) .. "t "
      end
    end

    entries[#entries+1] = {
      pool = item.pool, pname = pname, id = id,
      mx = gmx, my = gmy,
      cost = cost_val, weighted = weighted, raw = raw_val,
      pw = pw, age = age, status = status,
      formula = formula, flags = flags,
    }
  end
  -- Append finalize-time pools that don't go through the eval queue
  -- but should still be visible in the UI. These live in
  -- state.pool_cache[idx] populated by finalize_pools each replan
  -- cycle. Currently surfaced: place_strategic (8), attack_tank (9).
  local pc = state.pool_cache
  local function append_finalize(idx)
    local entry = pc and pc[idx]
    if not entry then return end
    local goal = entry.goal or {}
    local pname = POOL_NAMES[idx] or ("pool" .. idx)
    local cost = entry.cost or -1
    local pw = phase_weights and phase_weights[pname] or 1.0
    local weighted = cost >= 0 and (cost * pw) or -1
    entries[#entries+1] = {
      pool = idx, pname = pname, id = 0,
      mx = goal.mx or 0, my = goal.my or 0,
      cost = cost, weighted = weighted, raw = cost,
      pw = pw, age = 0, status = "done",
      formula = entry.desc or "", flags = "",
    }
  end
  append_finalize(8)  -- place_strategic
  append_finalize(9)  -- attack_tank

  table.sort(entries, function(a, b)
    if a.weighted < 0 and b.weighted >= 0 then return false end
    if a.weighted >= 0 and b.weighted < 0 then return true end
    if a.weighted < 0 and b.weighted < 0 then return false end
    return a.weighted < b.weighted
  end)
  for _, e in ipairs(entries) do
    lines[#lines+1] = string.format("%d\t%s\t%d\t%d\t%d\t%.1f\t%.1f\t%.2f\t%.1f\t%d\t%s\t%s\t%s",
      e.pool, e.pname, e.id, e.mx, e.my, e.cost, e.raw, e.pw, e.weighted, e.age, e.status,
      e.flags ~= "" and e.flags or "-", e.formula)
  end

  return table.concat(lines, "\n")
end


-- =========================================================================
-- get_pool_breakdown_json — structured (JSON) version of the pool data
-- consumed by BrainTest's pool_grid panel renderer. Builds the same
-- per-pool candidate lists the text version emits, but as a Lua table
-- the host parses with cJSON. Keeps the brain's data shape explicit and
-- the host's renderer free of tab-delimited string parsing.
--
-- MVP shape — covers the essentials. The text version still has more
-- detail (formula breakdown after ||, hyst suffixes, phase metrics);
-- those land here as sub-fields when needed.
--
-- Schema:
--   {
--     phase: string,
--     tick:  number,
--     replan_left: number,
--     sections: [
--       { idx: number, name: string, weight: number, winner_id: number,
--         layout_cell: [row, col],   -- 1-indexed for the 2x5 grid
--         rows: [
--           { id: number, mx: number, my: number,
--             cost: number, weighted: number,
--             is_winner: bool, formula: string },
--           ...
--         ]
--       },
--       ...
--     ]
--   }
-- =========================================================================
-- Public alias so init.lua can run the per-tick sync regardless of
-- whether the pool grid is being rendered.
M.sync_ally_claimed_rejects = sync_ally_claimed_rejects

function M.get_pool_breakdown_json(state)
  -- Run the ally-claimed REJECT sync every tick the grid is read so
  -- the displayed _reject flags track live ally_state without waiting
  -- for the next replan cycle.
  if state.player_number then _SELF_PN = state.player_number end
  sync_ally_claimed_rejects(state)
  if not BRAIN_POOL_VIZ then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      state.phase or "?", state.tick or 0, state.player_number or 0)
  end
  local now = state.tick or 0
  local cache = state.cost_cache or {}
  local pc = state.pool_cache or {}
  local phase_weights = C.PHASE_WEIGHTS[state.phase]

  local replan_left = 0
  if state.replan_offset then
    replan_left = C.GOAL_REPLAN_INTERVAL
        - ((now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL)
    if replan_left == C.GOAL_REPLAN_INTERVAL then replan_left = 0 end
  end

  -- Fixed 2x5 layout matching the optimize-branch poolwindow.
  -- (Indexes 11/12 used by def_build/wait_for_lgm strips below the grid.)
  local LAYOUT_CELL = {
    [1] = {1,1}, [2] = {1,2}, [3] = {1,3}, [4] = {1,4}, [5] = {1,5},
    [6] = {2,1}, [7] = {2,2}, [8] = {2,3}, [9] = {2,4}, [10]= {2,5},
  }

  -- Active-goal lookup. The renderer marks the row whose (pool, id)
  -- matches the bot's currently-committed goal so the user can see at
  -- a glance which candidate is actually being acted on (vs. just the
  -- sort-winner). Pool index is derived from the goal kind name.
  local KIND_TO_POOL = {}
  for i, n in pairs(POOL_NAMES) do KIND_TO_POOL[n] = i end
  local active_pool, active_id = nil, nil
  if state.goal and state.goal.kind then
    active_pool = KIND_TO_POOL[state.goal.kind]
    active_id   = state.goal.target_id
  end

  -- Group eval_queue items by pool, same iteration as the text version.
  -- Format a per-entry reject-history segment that gets appended to the
  -- formula breakdown so the pool panel shows the full timeline of
  -- _reject changes for this (pool, id) across the session.  Compact
  -- form: "|history: t=NNNN <action>[/by=pN] / t=NNNN <action> / ...".
  -- Cap at the last 8 events to keep the formula string under
  -- pool_grid.cpp's 400-byte segment cap.
  local function format_reject_history(rh)
    if not rh or #rh == 0 then return "" end
    local first = math.max(1, #rh - 7)
    local parts = { "|history:" }
    for i = first, #rh do
      local h = rh[i]
      local act
      if h.reason then
        act = "set:" .. h.reason
        if h.by then act = act .. " by=p" .. tostring(h.by) end
      else
        act = "cleared"
        if h.prev then act = act .. " (was " .. h.prev .. ")" end
      end
      parts[#parts + 1] = string.format(" t=%d %s", h.t or 0, act)
      if i < #rh then parts[#parts + 1] = " /" end
    end
    return table.concat(parts)
  end
  local reject_history = state.reject_history or {}

  -- Capture cached.tick so we can compute staleness per row.
  local by_pool = {}
  for _, item in ipairs(state.eval_queue or {}) do
    local p = item.pool
    local obj = item.obj
    if obj then
      by_pool[p] = by_pool[p] or {}
      local cached = cache[p .. ":" .. item.id]
      local formula_str = (cached and get_formula(cached)) or ""
      local hist_str = format_reject_history(reject_history[p .. ":" .. item.id])
      if hist_str ~= "" then
        local sep = formula_str:find("||", 1, true)
        if sep then
          formula_str = formula_str .. hist_str
        else
          formula_str = formula_str .. "||" .. hist_str:sub(2)
        end
      end
      by_pool[p][#by_pool[p] + 1] = {
        id = item.id, mx = obj.mx or 0, my = obj.my or 0,
        -- 1e30 (≥ renderer's 1e9 INF threshold) for candidates not yet
        -- evaluated, so the panel shows INF rather than a misleading -1.
        cost = (cached and cached.cost) or 1e30,
        formula = formula_str,
        stale = (cached and cached.tick) and (now - cached.tick) or -1,
        reject = cached and cached._reject or nil,
        reject_remaining = cached and cached._reject_remaining or 0,
        ally_score = cached and cached._ally_score or nil,
        ally_by    = cached and cached._ally_by    or nil,
        imminent = (cached and cached.imminent) or false,
      }
    end
  end

  local function attack_tank_formula(b)
    local reason = b.skipped
    if reason then
      local who = b.player_name and (" player=" .. b.player_name) or ""
      return string.format(
        "REJECT %s @(%d,%d)%s||reject:%s; dist=%d max=%d shells=%d armour=%d in_boat=%s%s",
        reason, b.mx or 0, b.my or 0, who, reason,
        b.dist or 0, C.TANK_COMBAT_MAX_RANGE,
        b.tank_shells or 0, state._last_info and state._last_info.armour or 0,
        tostring(state._last_info and state._last_info.inboat or false), who)
    end
    if b.los_engage then
      return string.format(
        "LOS (los_base{%.0f} + dist{%.1f}*per_tile{%.0f} + low_sh{%.0f}) * boat{%.2f} + threat{%.0f} = %.0f" ..
        "||dist=%.1f; shells_now=%d; aim_diff=%.1f",
        C.TANK_COMBAT_LOS_BASE_COST, b.dist or 0, C.TANK_COMBAT_LOS_COST_PER_TILE,
        b.low_shells_penalty or 0, b.boat_mult or 1.0,
        b.tank_tile_threat or 0, b.cost or 0,
        b.dist or 0, b.tank_shells or 0, b.aim_diff or 0)
    end
    return string.format(
      "standoff (A*{%.0f} + base{%.0f} + wall{%.0f} + low_sh{%.0f} - aim{%.0f} + xfire{%.0f}) * boat{%.2f} + threat{%.0f} = %.0f" ..
      "||dist=%d; shells_now=%d; shells_arrival=%s; aim_diff=%.1f; wall_hp=%s; standoff=(%s,%s) deg=%s",
      b.path_cost or 0, b.base or 0, b.wall_penalty or 0,
      b.low_shells_penalty or 0, b.aim_bonus or 0, b.crossfire or 0,
      b.boat_mult or 1.0, b.tank_tile_threat or 0, b.cost or 0,
      b.dist or 0, b.tank_shells or 0, tostring(b.shells_on_arrival),
      b.aim_diff or 0, tostring(b.wall_hp),
      tostring(b.standoff_mx), tostring(b.standoff_my), tostring(b.standoff_deg))
  end

  local function append_attack_tank_breakdown()
    local bd = state.attack_tank_breakdown or {}
    by_pool[9] = {}
    local seen_ids = {}

    local function append_row(b)
      local id = b.id
      if id == nil or id < 0 then id = (b.my or 0) * 256 + (b.mx or 0) end
      if id ~= nil then seen_ids[id] = true end
      by_pool[9][#by_pool[9] + 1] = {
        id = id, mx = b.mx or 0, my = b.my or 0,
        cost = b.cost or 1e30,
        formula = attack_tank_formula(b),
        stale = b.stale or 0,
        reject = b.skipped,
        reject_remaining = 0,
      }
    end

    for i, b in ipairs(bd) do
      append_row(b)
    end

    -- eval_attack_tank only runs when the planner evaluates that pool, but
    -- perception is refreshed every tick. Fill the panel from perception so
    -- visible tanks appear even before/after a full attack_tank scoring pass.
    local perc = state.perc
    for _, et in ipairs((perc and perc.enemy_tanks) or {}) do
      local id = et.id
      if id == nil or not seen_ids[id] then
        local skipped = "pending_eval"
        append_row({
          id = id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
          tank_shells = state._last_info and state._last_info.shells or 0,
          cost = 1e30, shells_on_arrival = 0, skipped = skipped,
          stale = 0,
        })
      end
    end

    -- Also list every known enemy player slot whose tank is not currently
    -- visible. This keeps the pool informative when no hostile tank object is
    -- in perception range or line of sight.
    local li = state._last_info
    if li and li.player_names then
      local allies = li.allies or 0
      for pn = 0, (li.max_players or 0) - 1 do
        if pn ~= li.player_number and not seen_ids[pn] then
          local name = li.player_names[pn + 1]
          local active = (type(name) == "string" and name ~= "")
                      or pn < (li.num_players or 0)
          local allied = (allies & (1 << pn)) ~= 0
          if active and not allied then
            if type(name) ~= "string" or name == "" then name = "player " .. tostring(pn) end
            append_row({
              id = pn, mx = -1, my = -1, dist = 0, speed = 0,
              tank_shells = li.shells or 0,
              cost = 1e30, shells_on_arrival = 0,
              skipped = "not_visible", player_name = name,
              stale = 0,
            })
          end
        end
      end
    end
  end

  append_attack_tank_breakdown()

  -- Inject place_strategic (pool 8) candidates from finalize_pools result.
  -- When not evaluated, inject a single stub row explaining why.
  if pc[8] and pc[8].cands then
    by_pool[8] = pc[8].cands
  else
    local li = state._last_info
    local reason
    if not li or (li.carried_pills or 0) < 1 then
      reason = "no_pill_in_tank"
    elseif li.inboat then
      reason = "in_boat"
    elseif li.man_status ~= C.LGM_INTANK then
      reason = "lgm_not_in_tank"
    else
      reason = "not_evaluated"
    end
    by_pool[8] = {{
      id = 0, mx = 0, my = 0,
      cost = -1,
      formula = "SKIP " .. reason .. "||" .. reason,
      stale = 0,
      reject = reason,
      reject_remaining = 0,
    }}
  end

  -- Build a normal pool section. Used for indexes 1..9 and the
  -- def_build (11) / wait_for_lgm (12) strips below the main grid.
  local function build_section(idx)
    local pname = POOL_NAMES[idx] or ("p"..idx)
    local pw = (phase_weights and phase_weights[idx]) or 1.0
    local rows_raw = by_pool[idx] or {}
    table.sort(rows_raw, function(a, b)
      local ac = (a.cost >= 0) and a.cost * pw or math.huge
      local bc = (b.cost >= 0) and b.cost * pw or math.huge
      return ac < bc
    end)
    local rows = {}
    for i, r in ipairs(rows_raw) do
      rows[i] = {
        id = r.id, mx = r.mx, my = r.my,
        cost = r.cost,
        weighted = (r.cost >= 0) and (r.cost * pw) or -1,
        is_winner = (i == 1 and r.cost >= 0 and r.cost < 1e29 and not r.reject),
        active_goal = (active_pool == idx and active_id == r.id),
        stale = r.stale,
        formula = r.formula,
        reject = r.reject,
        reject_remaining = r.reject_remaining,
        imminent = r.imminent or false,
      }
    end
    local winner_id = -1
    if rows[1] and rows[1].is_winner then winner_id = rows[1].id end
    return {
      idx = idx, name = pname, weight = pw, winner_id = winner_id,
      layout_cell = LAYOUT_CELL[idx], rows = rows,
    }, rows[1]
  end

  -- Group by-pool results with 'imminent' awareness for the winners section.
  -- The by_pool data from eval_queue is augmented with results from finalize_pools
  -- (which calculates imminent floor costs).
  for idx = 1, 9 do
    local winner_data = pc[idx]
    if winner_data and winner_data.imminent then
      local rows = by_pool[idx] or {}
      for _, r in ipairs(rows) do
        if r.id == winner_data.goal.target_id then
          r.imminent = true
          break
        end
      end
    end
  end

  -- Fetch the post-penalty competition results from the last goal_selection
  -- pass so we can show why a winner was or wasn't picked (switch penalties,
  -- commitment, oscillation history).
  local comp = {}
  for _, gc in ipairs(state.goal_competition or {}) do
    local k = string.format("%s@%d,%d", gc.kind, gc.mx or 0, gc.my or 0)
    comp[k] = gc
  end

  local sections = {}
  local winners = {}
  -- Iterate 1..9 (main grid pools) plus 13 (kill_lgm strip), so the
  -- WINNERS cross-pool table includes a kill_lgm row whenever an LGM
  -- is in view and pool 13 holds a candidate.
  for idx = 1, 9 do
    local sec, w = build_section(idx)
    sections[#sections + 1] = sec
    if w and w.cost >= 0 and w.cost < 1e29 and not w.reject then
      -- Cross-pool WINNERS row carries src_pool so the renderer can
      -- color it with its origin pool's hue.
      local pname = POOL_NAMES[idx] or ("p"..idx)
      local pw = (phase_weights and phase_weights[idx]) or 1.0

      -- Look up the actual post-penalty total used by goal_selection.
      -- Goal kinds often differ from UI pool names (e.g. refuel_at_base vs refuel).
      local k = string.format("%s@%d,%d", pname, w.mx or 0, w.my or 0)
      local gc = comp[k]

      -- Fallback: try mapping pname to goal kind
      if not gc then
        local kind_map = {
          refuel = "refuel_at_base",
          place_strategic = "place_pill_strategic",
        }
        local alt_k = string.format("%s@%d,%d", kind_map[pname] or "??", w.mx or 0, w.my or 0)
        gc = comp[alt_k]
      end

      -- Second fallback: try fuzzy kind match (if mx/my match)
      if not gc then
        for _, entry in pairs(comp) do
          if (entry.mx == w.mx and entry.my == w.my) then
            -- Check if entry.kind "contains" pname or vice-versa
            if (entry.kind and entry.kind:find(pname)) or pname:find(entry.kind or "??") then
              gc = entry; break
            end
          end
        end
      end

      local final_cost = gc and gc.total or w.weighted
      local penalty = gc and gc.penalty or 0   -- additive penalties BEFORE wsim
      local wsim_add = gc and gc.wsim_add or 0  -- wsim is tracked separately

      -- gc.base is the phase-weighted cost actually used by goal_selection
      -- (may differ from w.cost*pw for pools with extra adjustments like refuel).
      -- Fall back to w.weighted when no gc match was found.
      local base_cost = gc and gc.base or w.weighted

      -- hist = whatever is left of penalty after removing the named components.
      -- NOTE: do NOT subtract wsim_add here — wsim is not inside gc.penalty
      -- (penalty is captured before wsim runs). Subtracting it would make
      -- hist go negative and drop wsim_add from the displayed sum.
      local switch_flat = gc and gc.switch_flat or 0
      local commit_val  = gc and gc.commit_val  or 0
      local ally_pen    = gc and gc.ally_claimed_pen or 0
      local ally_by     = gc and gc.ally_claimed_by
      local hist_pen    = penalty - switch_flat - commit_val - ally_pen

      -- Build a rich formula for the detail popup
      local detail_formula = string.format("%s(x%.1f): %s", pname, pw, w.formula)
      local detail_map = {}

      detail_map[#detail_map + 1] = string.format("base:%.0f (raw=%.0f x pw=%.1f, %s phase)",
        base_cost, w.cost or 0, pw, state.phase or "unknown")

      if w.imminent then
        detail_map[#detail_map + 1] = string.format("imminent:cost_forced_to_floor(%.0f) because neutral and close", C.IMMINENT_CAPTURE_FLOOR)
      end

      if (penalty > 0 or wsim_add > 0) then
        local parts = {}
        if switch_flat > 0 then
          parts[#parts + 1] = string.format("hyst{%.0f}", switch_flat)
        end
        if commit_val > 0 then
          parts[#parts + 1] = string.format("hyst{%.0f}", commit_val)
        end
        if hist_pen > 1 then
          parts[#parts + 1] = string.format("hist{%.0f}", hist_pen)
        end
        if ally_pen > 0 then
          parts[#parts + 1] = string.format("ally_claimed{%.0f}", ally_pen)
        end
        if wsim_add > 0 then
          parts[#parts + 1] = string.format("wsim{%.0f}", wsim_add)
        end

        if #parts > 0 then
          detail_formula = string.format("%s + %s = %.0f", detail_formula, table.concat(parts, "+"), final_cost)
        end

        if switch_flat > 0 or commit_val > 0 then
          local hparts = {}
          if switch_flat > 0 then hparts[#hparts + 1] = string.format("switch(%.0f)", switch_flat) end
          if commit_val  > 0 then hparts[#hparts + 1] = string.format("commit(%.0f)", commit_val)  end
          detail_map[#detail_map + 1] = "hyst:" .. table.concat(hparts, " + ")
        end
        if hist_pen > 1 then
          detail_map[#detail_map + 1] = string.format("hist:recurrence_penalty(%.0f)", hist_pen)
        end
        if ally_pen > 0 then
          detail_map[#detail_map + 1] = string.format("ally_claimed:p%s holds same goal(+%.0f)",
                                                       tostring(ally_by), ally_pen)
        end
        if wsim_add > 0 then
          detail_map[#detail_map + 1] = string.format("wsim:damage_prediction(%.0f)", wsim_add)
        end
      end

      if gc and gc.hyst then
        detail_formula = string.format("%s (must be < cur*%.1f)", detail_formula, C.GOAL_SWITCH_RATIO)
      end

      -- Row second line: base + penalties = total (all numbers must balance).
      -- Use base_cost (from gc.base) so the equation holds even for pools
      -- like refuel where goal_selection adds constants before phase-weighting.
      local phase_abbrev = ({
        opening = "OP", early = "EA", middle = "MD", late = "LT",
        endgame = "EG", unknown = "??"
      })[(state.phase or "unknown")] or (state.phase or "??"):sub(1,2):upper()
      -- Prefix with pool kind so the WINNERS row tells you WHAT goal
      -- the cost belongs to at a glance (was: "34 MD@1.00" → now:
      -- "attack_tank 34 MD@1.00"). Most-recently asked for on
      -- attack_tank winners but applies uniformly to all main pools.
      local row_summary = string.format("%s %.0f %s@%.2f",
        pname, base_cost, phase_abbrev, pw)

      if penalty > 0 or wsim_add > 0 or w.imminent then
        local parts = {}
        if w.imminent then
          parts[#parts + 1] = "IM"
        end
        if switch_flat > 0 then
          parts[#parts + 1] = string.format("%.0f SW", switch_flat)
        end
        if commit_val > 0 then
          parts[#parts + 1] = string.format("%.0f CM", commit_val)
        end
        if hist_pen > 1 then
          parts[#parts + 1] = string.format("%.0f HT", hist_pen)
        end
        if ally_pen > 0 then
          parts[#parts + 1] = string.format("%.0f AC", ally_pen)
        end
        if wsim_add > 0 then
          parts[#parts + 1] = string.format("%.0f WS", wsim_add)
        end
        if #parts > 0 then
          row_summary = row_summary .. " +" .. table.concat(parts, " +")
        end
        row_summary = row_summary .. " =" .. string.format("%.0f", final_cost)
      end

      -- Final formula format: "RowSummary !! FullDisplayFormula || TermMapping"
      local final_formula = string.format("%s !! %s", row_summary, detail_formula)
      if #detail_map > 0 then
        final_formula = final_formula .. " || " .. table.concat(detail_map, "|")
      end

      -- Generate a unique synthetic ID for the WINNERS pool to avoid collisions
      -- between different source pools (e.g. attack_pill #0 and attack_base #0).
      -- Use src_pool in the high bits.
      local synthetic_id = (idx << 16) | (w.id or 0)

      winners[#winners + 1] = {
        id = synthetic_id, src_pool = idx,
        mx = w.mx, my = w.my,
        -- cost   = RAW per-pool cost (pre-phase, pre-loc_adj) so the
        --          detail popup's "Cost" matches the per-pool row.
        -- weighted = FINAL cost goal_selection used (post-phase weight,
        --            post-loc_adj, post-penalties).  Same value that
        --            ranks the WINNERS row.  Was set to final_cost for
        --            both fields, making "Cost: N    Weighted: N"
        --            uninformative when the two genuinely differ.
        cost = w.cost, weighted = final_cost,
        is_winner = false,
        active_goal = w.active_goal,
        stale = w.stale,
        formula = final_formula,
      }
    end
  end

  -- Strip pools (11/12/13) — injected directly into pool_cache by
  -- their respective evaluators, not routed through eval_queue or
  -- goal_competition.  We synthesize a WINNERS row from pool_cache so
  -- they compete on the same cost axis as the regular pools.  Note:
  -- 11 (def_build) is reserved but currently unused — the entry will
  -- show up here as soon as something writes pool_cache[11].
  --
  -- Strip rows get the same "<pool_name> <cost> <phase>@<pw>" row
  -- summary as main pools so the WINNERS column is uniformly readable.
  -- Their detail formula comes from cost_cache (richer breakdown) when
  -- available, falling back to sw.desc (the short tagline).
  for _, idx in ipairs({11, 12, 13}) do
    local sw = pc[idx]
    if sw and sw.goal and sw.cost and sw.cost >= 0 and sw.cost < 1e29 then
      local synthetic_id = (idx << 16) | (sw.goal.target_id or 0)
      local pname = POOL_NAMES[idx] or ("p"..idx)
      local pw   = (phase_weights and phase_weights[idx]) or 1.0
      local phase_abbrev = ({
        opening = "OP", early = "EA", middle = "MD", late = "LT",
        endgame = "EG", unknown = "??"
      })[(state.phase or "unknown")] or (state.phase or "??"):sub(1,2):upper()
      local row_summary = string.format("%s %.0f %s@%.2f",
        pname, sw.cost, phase_abbrev, pw)
      -- Prefer the cost_cache formula (full base + breakdown + detail
      -- map) over sw.desc (one-line tagline).  Both kill_lgm and
      -- wait_for_lgm stamp cost_cache under "<idx>:<target_id>" so we
      -- look that up first.
      local cc_key = string.format("%d:%s", idx, sw.goal.target_id or 0)
      local cc = state.cost_cache and state.cost_cache[cc_key]
      local detail = (cc and get_formula(cc)) or sw.desc or ""
      local final_formula = string.format("%s !! %s", row_summary, detail)
      winners[#winners + 1] = {
        id = synthetic_id, src_pool = idx,
        mx = sw.goal.mx, my = sw.goal.my,
        cost = sw.cost, weighted = sw.cost,
        is_winner = false,
        active_goal = (active_pool == idx and active_id == (sw.goal.target_id or 0)),
        stale = 0,
        formula = final_formula,
      }
    end
  end

  -- Cell 10 = cross-pool WINNERS table (ranked ascending).
  table.sort(winners, function(a, b) return a.cost < b.cost end)
  if winners[1] then winners[1].is_winner = true end
  sections[#sections + 1] = {
    idx = 10, name = "WINNERS", weight = 1.0,
    winner_id = (winners[1] and winners[1].id) or -1,
    layout_cell = LAYOUT_CELL[10], rows = winners,
  }

  -- Strips below the grid: def_build (11), wait_for_lgm (12),
  -- kill_lgm (13).  Only emit the section if the brain actually
  -- produced candidates for that pool this tick — keeps the renderer
  -- from drawing empty placeholders when the brain doesn't use the slot.
  -- kill_lgm doesn't go through eval_queue (it's injected directly into
  -- pool_cache from perception); seed by_pool[13] from pool_cache[13]
  -- here so build_section finds rows.
  if pc[13] and pc[13].cands then by_pool[13] = pc[13].cands end
  for _, idx in ipairs({11, 12, 13}) do
    if by_pool[idx] and #by_pool[idx] > 0 then
      sections[#sections + 1] = (build_section(idx))
    end
  end

  return json.encode({
    -- Bump schema_version when the shape changes in a way that
    -- breaks existing renderers / recorded snapshots. Renderers
    -- check this and surface a warning for unknown versions
    -- instead of silently misparsing.
    schema_version = 1,
    phase = state.phase or "?",
    tick = now,
    replan_left = replan_left,
    bot = state.player_number or 0,
    debug_session = _G.DEBUG_SESSION_DIR or "",
    sections = sections,
  })
end

-- Draw persistent wsim path overlays (called every tick from init.lua)
function M.draw_wsim_paths(state)
  if not state._wsim_viz_paths then return end
  for _, vp in ipairs(state._wsim_viz_paths) do
    local pr, pg, pb = 255, 100, 0  -- orange = high damage
    if vp.killed then pr, pg, pb = 255, 0, 0 end  -- red = kill reject
    local nwp = #vp.path // 2
    for j = 2, nwp do
      local p1x, p1y = vp.path[2*j-3], vp.path[2*j-2]
      local p2x, p2y = vp.path[2*j-1], vp.path[2*j]
      vizmod.line("wsim_paths", p1x + 0.5, p1y + 0.5, p2x + 0.5, p2y + 0.5, pr, pg, pb, 150)
    end
    -- Draw hit markers: red squares with total hits and final armour per tile.
    -- Deduplicate by tile so overlapping hits don't produce unreadable text.
    if vp.hits then
      local by_tile = {}
      for _, h in ipairs(vp.hits) do
        local key = h.mx * 256 + h.my
        local t = by_tile[key]
        if not t then
          t = { mx = h.mx, my = h.my, count = 0, armour = h.armour }
          by_tile[key] = t
        end
        t.count = t.count + 1
        t.armour = h.armour  -- last hit's armour (lowest)
      end
      for _, t in pairs(by_tile) do
        vizmod.rect("wsim_paths", t.mx, t.my, t.mx + 1, t.my + 1, 255, 0, 0, 80)
        local label = t.count > 1
          and string.format("%dx->%d", t.count, t.armour)
          or tostring(t.armour)
        vizmod.text("wsim_paths", t.mx + 0.5, t.my + 0.5,
          label, "center", 255, 50, 50, 255)
      end
    end
    local last_x, last_y = vp.path[#vp.path - 1], vp.path[#vp.path]
    if vp.killed then
      vizmod.text("wsim_paths", last_x + 0.5, last_y - 0.8,
        string.format("DEATH %s@(%d,%d)", vp.kind, vp.mx, vp.my),
        "center", 255, 0, 0, 255)
      if vp.detail and #vp.detail > 0 then
        vizmod.text("wsim_paths", last_x + 0.5, last_y - 0.2,
          vp.detail, "center", 255, 80, 80, 255)
      end
    end
  end
end

-- Draw attack_tank detection and precondition overlays (navy blue theme).
-- Called every tick from init.lua to show WHY the bot does/doesn't attack.
function M.draw_attack_tank_viz(state, info)
  local perc = state.perc
  if not perc then return end

  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local tcx = tmx + 0.5
  local tcy = tmy + 0.5

  -- Navy blue color for detection overlays
  local NR, NG, NB = 40, 80, 180

  -- 1. Detection range circle
  vizmod.circle("tank_combat_viz", tcx, tcy, C.TANK_COMBAT_MAX_RANGE, NR, NG, NB, 60)
  -- Engage range (inner)
  vizmod.circle("tank_combat_viz", tcx, tcy, C.TANK_COMBAT_ENGAGE_RANGE, NR, NG, NB + 40, 40)

  -- 2. Gate status on HUD (top-left, below replan line)
  local gate = nil
  if not C.TANK_COMBAT_ENABLED then gate = "COMBAT OFF"
  elseif info.inboat then gate = "IN BOAT"
  elseif info.shells < C.TANK_COMBAT_MIN_SHELLS then
    gate = string.format("LOW SHELLS %d/%d", info.shells, C.TANK_COMBAT_MIN_SHELLS)
  elseif info.armour < C.TANK_COMBAT_MIN_ARMOUR then
    gate = string.format("LOW ARMOUR %d/%d", info.armour, C.TANK_COMBAT_MIN_ARMOUR)
  end
  if gate then
    vizmod.hud_text("tank_combat_viz", 10, 68, "Tank combat: " .. gate, "topleft", NR, NG, NB)
  end

  -- 3. Per-enemy-tank indicators from breakdown
  local bd = state.attack_tank_breakdown
  if not bd then return end

  for _, b in ipairs(bd) do
    -- Skip world-space viz for rows without a real position (e.g. the
    -- "not_visible" stubs inserted with mx=my=-1 so the pool panel
    -- still lists hidden enemies). Otherwise we draw a line from the
    -- tank all the way to (0.5, 0.5) — the top-left corner of the map.
    -- Panel/HUD readers still see the row; only the spatial overlay
    -- is suppressed here.
    if (b.mx or 0) < 0 or (b.my or 0) < 0 then goto continue_bd end
    local ex, ey = b.mx + 0.5, b.my + 0.5

    -- Line from bot to enemy tank
    local lr, lg, lb, la = NR, NG, NB, 120
    if b.winner then lr, lg, lb, la = 0, 255, 100, 200 end
    vizmod.line("tank_combat_viz", tcx, tcy, ex, ey, lr, lg, lb, la)

    -- Label: skip reason or cost
    if b.skipped then
      vizmod.text("tank_combat_viz", ex, ey - 0.7,
        b.skipped, "center", NR + 60, NG + 40, NB + 40, 200)
    else
      local label = string.format("d=%d c=%.0f", b.dist, b.cost)
      if b.los_engage then label = "LOS " .. label end
      if b.wall_penalty and b.wall_penalty > 0 then
        label = label .. string.format(" w=%d", b.wall_penalty)
      end
      if b.crossfire and b.crossfire > 0 then
        label = label .. " XF"
      end
      local tr, tg, tb = NR + 80, NG + 60, 255
      if b.winner then tr, tg, tb = 0, 255, 100 end
      vizmod.text("tank_combat_viz", ex, ey - 0.7, label, "center", tr, tg, tb, 255)
    end

    -- Standoff position for non-LOS candidates
    if b.standoff_mx and not b.los_engage and not b.skipped then
      local sx, sy = b.standoff_mx + 0.5, b.standoff_my + 0.5
      vizmod.line("tank_combat_viz", ex, ey, sx, sy, NR, NG + 40, NB, 80)
      vizmod.rect("tank_combat_viz", b.standoff_mx, b.standoff_my,
        b.standoff_mx + 1, b.standoff_my + 1, NR, NG, NB, 60)
    end
    ::continue_bd::
  end
end

return M
