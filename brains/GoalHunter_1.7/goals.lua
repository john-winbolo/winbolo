local function __idiv(a,b) return math.floor(a/b) end
local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/goals.lua — strategic goal selection + exploration fallback
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
local circles    = require("circles")
local squad  = require("squad")
local builder = require("builder")   -- shared panic guard-spot search (M.guard_build_spot)
local PP     = require("pill_portfolio")
local _SELF_PN = -1   -- updated each tick by step_eval_queue / get_pool_breakdown_json

local M = {}

-- Local aliases for the smart_cost helper / kind constants in cpathfinder.
local smart_cost = cpf.smart_cost
local KIND_NORMAL = cpf.KIND_NORMAL
local KIND_PILL   = cpf.KIND_PILL

local last_strategic_goal = nil  -- track to avoid spamming logs
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

-- Save/restore last_strategic_goal around lookahead calls so the nested
-- pick_goal doesn't contaminate the real goal-change log messages.
function M.save_goal_state()    return last_strategic_goal end
function M.restore_goal_state(s) last_strategic_goal = s end

-- Forward simulation: predict armor damage along a path to a goal.
-- Returns (extra_cost, killed, sim_desc) where extra_cost is added to
-- the goal's cost, killed=true means the path is lethal.
local function wsim_evaluate_goal(goal, world, info, attack_pill_idx, spot_mx, spot_my)
  if not C.WSIM_ENABLED then return 0, false, "" end

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
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
    local nwp = __idiv(#path, 2)
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

  -- Dwell + tick budget. Default for every goal kind is the old behaviour:
  -- stop the sim on arrival, 300 ticks. snapshot() clears the sim, which
  -- resets the dwell to 0, so nothing has to opt out.
  --
  -- A pill placement isn't over when the tank arrives. The tank then sits
  -- on the spot while the LGM walks out, builds and walks back, and that
  -- stationary window is where a bot parked next to a hostile pillbox
  -- actually dies -- the sim used to stop dead on arrival and never price
  -- it at all. So: dwell = LGM round trip, and enough ticks that a long
  -- drive plus its dwell doesn't get cut off.
  local max_ticks = C.WSIM_MAX_TICKS
  local dwell = 0
  if goal.kind == "place_pill_strategic" then
    -- Walk distance is from where the TANK ends up (the path endpoint) to
    -- the drop spot, not from where it stands now -- the drive itself is
    -- already simulated. Driving right onto the spot makes these the same
    -- tile and the floor applies; parking short of it costs the walk.
    local walk = 0
    local np = __idiv(#path, 2)
    if np > 0 then
      local ex, ey = path[2*np - 1], path[2*np]
      walk = math.max(math.abs(gmx - ex), math.abs(gmy - ey))
    end
    dwell = 2 * walk * C.WSIM_DWELL_TICKS_PER_TILE + C.WSIM_DWELL_BUILD_TICKS
    if dwell < C.WSIM_DWELL_MIN_TICKS then dwell = C.WSIM_DWELL_MIN_TICKS end
    if dwell > C.WSIM_DWELL_MAX_TICKS then dwell = C.WSIM_DWELL_MAX_TICKS end
    wsim.set_dwell(dwell)
    max_ticks = C.WSIM_PLACE_MAX_TICKS
  end

  local r = wsim.run(max_ticks)
  local _tw2 = clock_us()
  if BRAIN_PROFILE_LOG and (_tw2 - _tw0) > 200 then
    opt.append("optimize.log", string.format(
      "  [wsim] goal=%s(%d,%d) snap=%.3fms run=%.3fms npath=%d",
      goal.kind, gmx, gmy, (_tw1-_tw0)/1000, (_tw2-_tw1)/1000, __idiv(#path, 2)))
  end
  -- PROFILING LITE: same _tw0/_tw1/_tw2 clocks, but reported into print2 so a
  -- slow wsim shows up in the debug brain (BRAIN_PROFILE_LOG forces the opt
  -- brain, so the two can't be combined). Threshold 1.0 ms = 1000 us; nothing
  -- formats on a normal wsim. No `state` param here — the tick comes from the
  -- _BRAIN_TICK global init.lua publishes each think.
  if BRAIN_DEBUG_MODE and (_tw2 - _tw0) > 1000 then
    print2(string.format("WSIM_SLOW t=%d goal=%s@(%d,%d) snap=%.2f run=%.2f path=%d",
      _G._BRAIN_TICK or 0, tostring(goal.kind), gmx or -1, gmy or -1,
      (_tw1 - _tw0) / 1000, (_tw2 - _tw1) / 1000, __idiv(#path, 2)))
  end

  local extra_cost = r.damage * C.WSIM_DAMAGE_COST_WEIGHT
  local sim_desc = string.format(" wsim:%ddmg %.1fs arm=%d->%d",
    r.damage, r.ticks / 50.0, info.armour, r.armour)
  if dwell > 0 then
    -- How much of that damage was taken standing still at the destination,
    -- and over how long a dwell we asked for.
    sim_desc = sim_desc .. string.format(" dwell:%dt/%ddmg",
      dwell, r.dwell_damage or 0)
  end
  if r.truncated then
    -- Ran out of sim ticks instead of reaching a natural end, so the damage
    -- number above only covers a PREFIX of the trip. Price the unknown as a
    -- risk. Left inert it reads as safety, which is backwards -- the part
    -- that never got simulated is the far end of the route, and that is
    -- where a bot driving into a defended area dies.
    --
    -- Flat, not a multiplier on r.damage: a truncated run that saw zero
    -- damage is the case most in need of the nudge, and a multiplier gives
    -- it nothing. Scaling the observed damage up by the fraction of the
    -- route we actually covered would be the more honest correction, but
    -- the result carries no path-progress field, so recovering that
    -- fraction would mean guessing a terrain-dependent ticks-per-tile --
    -- more machinery than an unknown deserves.
    --
    -- SCOPED TO PLACEMENTS on purpose. Truncation also shows up on attack and
    -- capture goals -- they keep the 300-tick WSIM_MAX_TICKS while only place
    -- goals got the raised cap, so a long approach hits it -- and pricing
    -- those is very likely right too. But that mis-pricing pre-dates the
    -- dwell work (the flag only made it visible), and their costs have been
    -- tuned for years against the current, unpriced behaviour. Charging them
    -- here would shift attack/capture goal selection as a side effect of a
    -- pill-placement change. Drop the `is_place` term to apply it everywhere;
    -- do that as its own change, with its own match to measure it.
    if goal.kind == "place_pill_strategic" then
      extra_cost = extra_cost + C.WSIM_TRUNCATED_COST
      sim_desc = sim_desc .. string.format(" TRUNC(+%d)", C.WSIM_TRUNCATED_COST)
    else
      sim_desc = sim_desc .. " TRUNC(unpriced)"
    end
  end
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
    dwell = dwell, dwell_damage = r.dwell_damage, truncated = r.truncated,
  })

  return extra_cost, r.killed, sim_desc, path, r
end

-- Helper: find the cheapest-to-reach object matching a filter.
-- Uses smart_cost (dijkstra fast-path with cost_to fallback).
-- `kind` selects the dijkstra slate (KIND_NORMAL or KIND_PILL).
-- Returns best, best_id, best_cost, candidates (array of all evaluated)
local function nearest_where(collection, world, tmx, tmy, filter, in_boat, ammo, state, info, kind, danger_scale_override, extra_cost_fn)
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
  -- filter(obj, id): the id is passed so a filter can consult per-object
  -- state keyed by id (e.g. the plan_position sweep blacklist in
  -- eval_attack_pill). Filters that don't care just ignore the extra arg.
  for id, obj in pairs(collection) do
    if filter(obj, id) then
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
      -- Staleness is handled as a GROWING COST below (soft penalty), not a hard
      -- skip. A pill/base we haven't re-seen in a while may be captured/changed,
      -- so we deprioritize it — but never REMOVE it, or a death (which freezes
      -- last_seen) would permanently hide every object the bot can't currently
      -- see, even ones it legitimately knows about. Kept as a last-resort
      -- candidate it self-recovers by eventually re-engaging + re-seeing it.
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
        -- Optional per-candidate extra cost (e.g. base pill-cover penalty), so it
        -- affects WHICH candidate wins, not just the chosen one's final cost.
        if extra_cost_fn then c = c + (extra_cost_fn(obj) or 0) end
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

-- Count live hostile/neutral pills whose fire-range covers tile (spot_mx,spot_my)
-- but does NOT already cover our current tile (tmx,tmy) — i.e. NEW exposure only
-- (a pill already shooting us where we stand isn't extra cost) — AND that have a
-- clear line of fire to the spot (a walled-off pill can't actually hit it).
-- Iterates all live pills (<=16); the LOS check runs last (priciest) so it only
-- fires once the cheap range gates pass. Shared by the engage-spot crossfire
-- (new_pill_crossfire) and the per-base pill-cover penalty.
local function count_new_exposure_pills(world, spot_mx, spot_my, tmx, tmy, neutral_needs_hot)
  if not (spot_mx and spot_my) then return 0 end
  local fr = C.PILL_FIRE_RANGE or 8
  local n = 0
  for _, pm in pairs(world.pills) do
    -- HOSTILE pills always count: they fire at any enemy in range, and shooting/
    -- stealing an enemy base heats them further, so even a calm hostile opens up.
    -- NEUTRAL pills also count by default (a slow neutral shot still matters when
    -- you're parked shooting a different pill — the crossfire caller), BUT when
    -- neutral_needs_hot is set (base capture/attack callers) a CALM neutral is
    -- ignored — it won't fire during a quick base grab, so it must not penalize
    -- the steal. (Ally pills never shoot us and never count.) Plus ALIVE (health>0),
    -- DEPLOYED (not in a tank), and the range/new-exposure/LOS gates.
    local neutral_ok = (pm.owner == "neutral")
                       and (not neutral_needs_hot or (pm.anger or 0) > (C.PPT_ANGER_THRESHOLD or 0.34))
    if (pm.owner == "hostile" or neutral_ok) and (pm.health or 0) > 0
       and not pm.in_tank
       and U.mdist(spot_mx, spot_my, pm.mx, pm.my) <= fr
       and U.mdist(tmx, tmy, pm.mx, pm.my) > fr
       and PF.wall_hp_between(pm.mx, pm.my, spot_mx, spot_my) == 0 then
      n = n + 1
    end
  end
  return n
end

-- Helper: find the best friendly or neutral base for resupply.
-- danger_weight: REFUEL_DANGER_WEIGHT for normal top-up, FLEE_DANGER_WEIGHT
--               when health is critical (scales pill-danger penalty steeply).
-- cur_mx/cur_my: if the tank is already heading to a base, give that base a
--               REFUEL_SWITCH_THRESHOLD score bonus so we only switch when
--               there is a genuinely better option (prevents oscillation).
-- danger_reject: if non-nil, skip bases with pill danger above this value.
-- Used when fleeing at critical armour — sitting at a dangerous base = death.
-- =========================================================================
-- Contested-base penalty for a refuel base at (bmx,bmy).
-- Returns pen, n_unhandled, n_handled — shared by BOTH refuel scoring paths
-- (nearest_resupply_base's candidate loop and the pool-1 cost_cache builder)
-- so the two can never drift apart.
--
--   * QUALIFY — a MOVING enemy tank (speed > 0, straight off TankSnapshot)
--     within CONTESTED_BASE_RANGE of the base. Distance is U.mdist
--     (Manhattan), matching the range test this replaced.
--   * HANDLED — skip a tank entirely when an ALLY is already broadcasting an
--     attack_tank goal against THAT tank id: someone owns that threat, and it
--     shouldn't also scare us off our refuel. Ally goals arrive on the /info
--     state slate as goal=attack_tank + target=<tank id>; attack_tank sets
--     target_id = best_tank.id, and perc.enemy_tanks[].id is the object idnum,
--     which for a tank IS the player number — so the two are the same id space
--     (target is the STRING form, hence the tostring compare).
--   * PROXIMITY — each remaining tank costs
--     CONTESTED_BASE_PENALTY * (1 - dist/RANGE): the full penalty sitting on
--     the base, fading linearly to nothing at the range edge. Replaces a flat
--     step that treated a tank 14 tiles out like one parked on the pumps.
--   * SUM — the per-tank terms simply ADD. Deliberately no outnumbering
--     multiplier: two tanks at half range already cost a full 120 between
--     them, and scaling that by the head count priced refuelling out of
--     reach entirely.
-- =========================================================================
local function contested_penalty(state, bmx, bmy, info, now)
  local ets = state and state.perc and state.perc.enemy_tanks
  if not ets or #ets == 0 then return 0, 0, 0 end
  local R = C.CONTESTED_BASE_RANGE or 15
  if R <= 0 then return 0, 0, 0 end

  -- Ally-claimed tank ids, built LAZILY: most bases have no qualifying tank at
  -- all, so the ally_state sweep only runs when it could change the answer.
  local claimed, claimed_built = nil, false
  local function ally_handling(id)
    if id == nil then return false end
    if not claimed_built then
      claimed_built = true
      local self_pn = info and info.player_number
      if ally_state.iter_active then
        for apn, slot in ally_state.iter_active(now or (state and state.tick) or 0, 1750) do
          local h = slot.info
          if apn ~= self_pn and h and h.goal == "attack_tank" and h.target then
            claimed = claimed or {}
            claimed[h.target] = true
          end
        end
      end
    end
    return (claimed ~= nil) and (claimed[tostring(id)] or false) or false
  end

  local sum, n, handled = 0, 0, 0
  for _, et in ipairs(ets) do
    if (et.speed or 0) > 0 then
      local d = U.mdist(et.mx, et.my, bmx, bmy)
      if d <= R then
        if ally_handling(et.id) then
          handled = handled + 1
        else
          n   = n + 1
          sum = sum + (C.CONTESTED_BASE_PENALTY or 120) * (1 - d / R)
        end
      end
    end
  end
  return sum, n, handled
end

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
          print2(string.format("REFUEL_CAND base#%d @(%d,%d) REJECT blocked until t=%d", id, b.mx, b.my, state.blocked[bk]))
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
          print2(string.format("REFUEL_CAND base#%d @(%d,%d) REJECT empty :: DRAINED owner=%s health=%s last_health=%s obs_shells=%s obs_armour=%s obs_tick=%s(%dt ago) last_seen=%s(%dt ago) | need_a=%s(arm %d/%d) need_s=%s(sh %d/%d) MIN_STOCK=%d",
            id, b.mx, b.my, tostring(b.owner), tostring(b.health), tostring(b.last_health),
            tostring(b.obs_shells), tostring(b.obs_armour), tostring(b.obs_tick), now - (b.obs_tick or now),
            tostring(b.last_seen), now - (b.last_seen or now),
            tostring(need_a), info.armour or 0, state.armour_target or 0,
            tostring(need_s), info.shells or 0, state.shell_target or 0, C.REFUEL_MIN_STOCK))
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
          print2(string.format("REFUEL_CAND base#%d @(%d,%d) REJECT low_stock :: DRAINED owner=%s health=%s last_health=%s obs_shells=%s obs_armour=%s obs_tick=%s(%dt ago) last_seen=%s(%dt ago) | need_a=%s(arm %d/%d) need_s=%s(sh %d/%d) MIN_STOCK=%d",
            id, b.mx, b.my, tostring(b.owner), tostring(b.health), tostring(b.last_health),
            tostring(b.obs_shells), tostring(b.obs_armour), tostring(b.obs_tick), now - (b.obs_tick or now),
            tostring(b.last_seen), now - (b.last_seen or now),
            tostring(info.armour < state.armour_target), info.armour or 0, state.armour_target or 0,
            tostring(info.shells < state.shell_target), info.shells or 0, state.shell_target or 0, C.REFUEL_MIN_STOCK))
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
        print2(string.format("REFUEL_CAND base#%d @(%d,%d) REJECT stale_neutral unseen=%dt limit=%d", id, b.mx, b.my, now - b.last_seen, C.STALE_SKIP_TICKS))
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
          print2(string.format("REFUEL_CAND base#%d @(%d,%d) REJECT danger %.1f > reject %.1f", id, b.mx, b.my, danger, danger_reject))
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
        -- Feature 4: contested base avoidance — penalise bases with moving
        -- enemy tanks nearby, scaled by how close they are, discounting any
        -- tank an ally is already attacking, and amplified when they
        -- outnumber us. See contested_penalty for the full shape.
        local contest_pen, contest_n, contest_h =
          contested_penalty(state, b.mx, b.my, info, now)
        local contested = contest_pen > 0
        score = score + contest_pen

        -- Anti-base-hop (mirrors the cost_cache path): while standing ON a refuel
        -- base, every OTHER base costs more so we finish here instead of bouncing
        -- between bases (the GOAL_TARGET_SWITCH_PENALTY at the goal layer is too
        -- small to overcome a cheaper rival base). A depleted current base is
        -- rejected above, so the move-on case still works.
        if info.base and info.base.x == tmx and info.base.y == tmy
           and not (b.mx == tmx and b.my == tmy) then
          score = score + (C.REFUEL_BASE_HOP_PENALTY or 500)
        end
        -- Self-base hysteresis REMOVED: goal_selection already applies a
        -- GOAL_TARGET_SWITCH_PENALTY + commitment to any refuel base that isn't
        -- our current refuel goal (goals.lua ~6761), so giving the base under our
        -- tile a -REFUEL_SWITCH_THRESHOLD here was double-damping the same switch.
        -- Let base choice be driven purely by travel/danger; the goal layer keeps
        -- us from flip-flopping between bases.
        local hysteresis = false
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
        -- Survival exemption: when armour is at/below ARMOUR_LOW (or we're dry
        -- of shells) the anti-thrash ratchet must never out-price staying
        -- alive. A bot that has bounced between bases 7× at 0 armour needs the
        -- base MORE, not less. Skipping the term entirely (rather than capping
        -- it) keeps base choice on pure travel/danger in that state.
        local _survival = (info.armour or 99) <= C.ARMOUR_LOW
                       or (info.shells or 99) <= 0
        if hist_count > 0 and not _survival then
          local hist_pen = C.GOAL_HISTORY_TARGET_BASE * (C.GOAL_HISTORY_EXP ^ hist_count - 1)
          -- Same cap as goal_selection's combined penalty: BASE*(EXP^n - 1) is
          -- unbounded and would otherwise make a repeatedly-used base
          -- permanently unpickable.
          hist_pen = math.min(hist_pen, C.GOAL_HISTORY_PEN_CAP or math.huge)
          score = score + hist_pen
        end
        candidates[#candidates + 1] = {
          id = id, mx = b.mx, my = b.my, own = b.owner,
          travel = travel, danger = danger, dw = danger_weight,
          score = score, hyst = hysteresis, contested = contested,
          contest_pen = contest_pen, contest_n = contest_n, contest_h = contest_h,
          stale = b.last_seen and (now - b.last_seen) or 0,
        }
        -- Contested chip carries the whole computation: how many moving
        -- enemies counted, how many were dropped as already-handled by an
        -- ally, and the summed proximity-scaled cost they added.
        local _c_tok = ""
        if contest_n > 0 or contest_h > 0 then
          _c_tok = string.format(" CONTESTED{n=%d handled=%d pen=%.0f}",
                                 contest_n, contest_h, contest_pen)
        end
        print2(string.format("REFUEL_CAND base#%d @(%d,%d) OK score=%.1f travel=%.1f danger=%.1f×%.1f%s%s obs_sh=%d obs_arm=%d", id, b.mx, b.my, score, travel, danger, danger_weight, hysteresis and " HYST" or "", _c_tok, b.obs_shells or -1, b.obs_armour or -1))
        if score < best_score then
          best_score = score; best_id = id; best = b
        end
      end
      ::skip::
    end
  end
  print2(string.format("REFUEL_WINNER base#%s @(%s,%s) score=%.1f (from %d candidates, arm=%d/%d sh=%d/%d)", tostring(best_id), best and tostring(best.mx) or "-", best and tostring(best.my) or "-", best_score, #candidates, info.armour or 0, state and state.armour_target or 0, info.shells or 0, state and state.shell_target or 0))
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
-- Pillbox-suicider goal-cost shaping (see C.PILL_SUICIDER_* in constants).
-- A suicider is only willing to do two things: kill pills, and keep itself
-- fuelled. Everything else is priced out of reach with a flat multiplier
-- applied at ONE choke point — goal_selection's pool loop, where every pool's
-- assembled cost passes through — rather than inside each pool's evaluator.
--
--   attack_pill                    x1  exempt (the one job)
--   refuel_at_base / flee_to_base  x1  exempt (the whole "refuel" GOAL_GROUP)
--   defend_pill                    x PILL_SUICIDER_DEFEND_MULT  (6)
--   everything else                x PILL_SUICIDER_OTHER_MULT   (3)
--
-- Keyed on goal.kind (not pool index) so kinds with no numbered pool — explore,
-- reposition, rescue_lgm, offensive_build — are covered by the same rule. Returns
-- 1.0 for every non-suicider, so this is a no-op on a normal map.
-- =========================================================================
local SUICIDER_EXEMPT_KINDS = {
  attack_pill    = true,   -- the role's entire purpose
  refuel_at_base = true,   -- the "refuel" GOAL_GROUP, both members: a suicider
  flee_to_base   = true,   -- still resupplies (and still flees at critical armour)
  capture_pill   = true,   -- scooping the pills it kills is part of the job
  place_pill_strategic = true, -- and so is fielding what it carries
  offensive_build      = true,   -- panic drop under fire: survival, not a side quest
  wait_for_lgm   = true,   -- companion to place/capture — x3 here would let the
                           -- pool yank the tank away while its LGM is still out
}
local function suicider_cost_mult(state, kind)
  if not (state and state.is_pill_suicider) then return 1.0 end
  if not kind or SUICIDER_EXEMPT_KINDS[kind] then return 1.0 end
  if kind == "defend_pill" then return C.PILL_SUICIDER_DEFEND_MULT or 1.0 end
  return C.PILL_SUICIDER_OTHER_MULT or 1.0
end

-- =========================================================================
-- Resolve an attack pill winner into a concrete goal: always the unified
-- plan_position (PPT) take with standoff positioning. Wall-shield vs. hardline
-- vs. pillbox-as-blocker is decided later inside the attack_pill substate
-- machine, not here. Uses attack.get_standoff to seed the standoff tile.
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


-- Shared refuel cost-shape (pool 1). Used by BOTH the live cost-competition
-- path and the term-breakdown panel so the displayed shape can never diverge
-- from the real cost. Returns: bonus (deficit discount), mult (top-off
-- multiplier, scarcity-scaled), fill (0..1 between LOW and target), scarcity,
-- mine_cost (additive), urgency, arm_def, sh_def.
--
-- "Gotta share": the top-off ramp (REFUEL_FULL_COST_MULT) is steepened by team
-- base-scarcity (team tanks per friendly base) plus how many teammates are
-- crowding this base right now, so a bot leaves near the LOW floor when bases
-- are scarce and fills toward target only when they're plentiful. Mines never
-- gate leaving; topping past REFUEL_MINE_FREE just adds an exponential cost.
local function refuel_shape(info, state, now)
  local arm = info.armour or 0
  local sh  = info.shells or 0
  local arm_target = state.armour_target or C.TANK_FULL_ARMOUR
  local sh_target  = state.shell_target  or C.TANK_FULL_SHELLS
  local arm_def = math.max(0, (C.ARMOUR_LOW - arm) / C.ARMOUR_LOW)
  local sh_def  = math.max(0, (C.SHELLS_LOW - sh) / C.SHELLS_LOW)
  local bonus   = C.REFUEL_DEFICIT_BONUS * math.max(arm_def, sh_def)
  local fill = 0.0
  if arm > C.ARMOUR_LOW and sh > C.SHELLS_LOW then
    local fa = (arm - C.ARMOUR_LOW) / math.max(1, arm_target - C.ARMOUR_LOW)
    local fs = (sh  - C.SHELLS_LOW) / math.max(1, sh_target  - C.SHELLS_LOW)
    fill = math.min(fa, fs)
  end
  local scarcity = 1.0
  if C.REFUEL_SHARE_ENABLED then
    local team = 1                                  -- our team size (self + ally bits)
    local ab = info.allies or 0
    while ab > 0 do team = team + (bit.band(ab, 1)); ab = bit.rshift(ab, 1) end
    local fbases = (state.perc and state.perc.friendly_base_count) or 0
    local apb = team / math.max(1, fbases)          -- team tanks per friendly base
    local local_near = 0
    if info.tankx and info.tanky then
      local tx, ty = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
      for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
        local si = slot.info
        if pn ~= info.player_number and si and si.mx and si.my
           and U.mdist(tx, ty, tonumber(si.mx) or tx, tonumber(si.my) or ty)
               <= (C.REFUEL_SHARE_LOCAL_TILES or 20) then
          local_near = local_near + 1
        end
      end
    end
    scarcity = 1.0 + (C.REFUEL_SHARE_RATIO_K or 0) * math.max(0, apb - 1)
                   + (C.REFUEL_SHARE_LOCAL_K or 0) * local_near
    scarcity = math.min(scarcity, C.REFUEL_SHARE_SCARCITY_CAP or 8.0)
  end
  -- Quadratic in fill: gentle while genuinely low (fill 0.3 → ~×1.6 at
  -- MULT 8), punishing when nearly full (fill 0.9 → ~×6.7). The old linear
  -- ×3 ramp let a full-armour tank with ~60% shells price refuel at ~×2.2 —
  -- low enough to outbid attack goals at 250-500 and read as "NEED TO
  -- REFUEL" when nothing was actually low (20260703_221238 t=24898).
  local mult = 1.0 + (fill * fill) * (C.REFUEL_FULL_COST_MULT - 1.0) * scarcity
  local mines_over = math.max(0, (info.mines or 0) - (C.REFUEL_MINE_FREE or 5))
  local mine_cost = 0.0
  if mines_over > 0 then
    mine_cost = (C.REFUEL_MINE_HOARD_WEIGHT or 0)
                * ((C.REFUEL_MINE_HOARD_BASE or 1.3) ^ mines_over - 1.0)
  end
  local urgency = math.max(C.REFUEL_URGENCY_MIN,
                           math.min(math.min(1.0, arm / C.ARMOUR_LOW),
                                    math.min(1.0, sh  / C.SHELLS_LOW)))
  -- Critical-armour need floor — mirrors eval_refuel and the pool-1 finalize
  -- path so the Term Breakdown panel shows the same number the cost path uses.
  -- `urgency` is inverse need (lower = more urgent), hence the 1-x mapping.
  if arm <= C.ARMOUR_LOW then
    local need = math.max(1.0 - urgency, C.REFUEL_CRITICAL_NEED_MIN or 0)
    urgency = 1.0 - need
  end
  return bonus, mult, fill, scarcity, mine_cost, urgency, arm_def, sh_def
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
      U.set_blocked(state, U.mkey(tmx, tmy), state.tick + 200, "refuel_base_useless")
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
  -- danger_reject = nil: danger is already in the score (travel +
  -- danger * REFUEL_DANGER_WEIGHT), so we never hard-drop a base for
  -- being dangerous — it just costs more.
  local base, bid, bscore, bcands = nearest_resupply_base(world, tmx, tmy, boat, ammo, state, info,
                                                C.REFUEL_DANGER_WEIGHT, cur_mx, cur_my,
                                                nil)
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
  -- ── Critical-armour need floor ──
  -- `urgency` is INVERSE need (lower = cheaper = more urgent) and it bottoms
  -- out at REFUEL_URGENCY_MIN (0.37). That floor made 0 armour price exactly
  -- like 14 armour, so armour could never dominate a full shell tank and
  -- refuel kept losing its seat to the anti-thrash stack. Restate the value in
  -- the brain's usual "need" convention (0..1, higher = more needed), floor it
  -- at REFUEL_CRITICAL_NEED_MIN whenever armour is at/below ARMOUR_LOW, and map
  -- it back. The formula above is untouched; this can only make refuel cheaper.
  if (info.armour or 99) <= C.ARMOUR_LOW then
    local need = math.max(1.0 - urgency, C.REFUEL_CRITICAL_NEED_MIN or 0)
    urgency = 1.0 - need
  end
  local cost = bscore * urgency
  -- Practical cost floor: routine goals live at ≥ ~20; the band below is
  -- reserved for survival-critical work. A top-off while parked on the base
  -- otherwise collapses to ~4 (bscore ≈ 12 × urgency floor 0.37) and outbids
  -- free-pill grabs and every other real opportunity (20260703_210207
  -- t=5747). Flee-level armour keeps the raw cost — critical refuel is
  -- exactly what the reserved band is for.
  if (info.armour or 0) > C.ARMOUR_CRITICAL then
    cost = math.max(cost, C.REFUEL_MIN_COST)
  end
  -- Ammo-deprived SUICIDE decoy: it has written off resupply (shells AND armour),
  -- so heavily deprioritise refuel/flee so attack_pill/blitz out-bids it. Applied
  -- last (even over the critical-armour band) — it's meant to die charging, and the
  -- ammo_deprived flag resets on death / after AMMO_DEPRIVED_MAX_TICKS so it does
  -- get periodic windows to refuel normally again.
  if state.ammo_deprived then
    cost = cost * (C.AMMO_DEPRIVED_REFUEL_MULT or 3)
  end
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
    boat, ammo, state, info, KIND_NORMAL, C.CAPTURE_THREAT_WEIGHT,
    -- +BASE_PILL_COVER_PEN per enemy pill whose fire covers this base but not our
    -- current tile (new exposure only) — biases toward capturing safer bases.
    function(b) return (C.BASE_PILL_COVER_PEN or 3) * count_new_exposure_pills(world, b.mx, b.my, tmx, tmy, true) end)
  if not base then return nil end

  local raw_cost = bcost
  local imminent = false
  if base.health == 0
     and raw_cost <= C.IMMINENT_CAPTURE_PATH_COST
     and info.armour >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
    raw_cost = math.min(raw_cost, C.IMMINENT_CAPTURE_FLOOR)
    imminent = true
  end
  -- Urgent capture: we just killed a base via attack_base. Heavily
  -- discount the cost so the bot commits to capturing before the base
  -- recharges for the enemy. Decays after 500 ticks (~10s).
  local urgent = state.urgent_capture_base
  if urgent and (state.tick or 0) - urgent.tick < 500 then
    if urgent.mx == base.mx and urgent.my == base.my then
      raw_cost = math.min(raw_cost, C.IMMINENT_CAPTURE_FLOOR)
      imminent = true
    else
      -- nearest_where picked a different (closer) capturable base, so the
      -- urgent base never surfaced as the candidate. Evaluate it directly
      -- and switch to it if it's still capturable and reachable — otherwise
      -- the bot wanders off the base it just neutralized.
      for ubid, ub in pairs(world.bases) do
        if ub.mx == urgent.mx and ub.my == urgent.my then
          if ub.owner == "neutral" or (ub.owner == "hostile" and ub.health == 0) then
            local uc = smart_cost(KIND_NORMAL, tmx, tmy, ub.mx, ub.my,
                                  boat and 1 or 0, info.shells or 32, info.trees or 0,
                                  info.mines or 0, info.armour or 40)
            if uc and uc <= C.IMMINENT_CAPTURE_PATH_COST then
              base, bid = ub, ubid
              raw_cost = math.min(uc, C.IMMINENT_CAPTURE_FLOOR)
              imminent = true
              -- Refresh the viz candidate list to the base we switched to.
              bcands = { { id = ubid, mx = ub.mx, my = ub.my, cost = raw_cost } }
            end
          end
          break
        end
      end
    end
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
    imminent = imminent,
    goal = { kind = "capture_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid,
             race_mode = race_mode },
    desc = desc,
    cands = pcands,
  }
end

-- compute_repair_dead_cost — cost to rebuild a friendly 0-HP pill IN PLACE.
-- The LGM walks out with wood and the engine repairs it. Capture-in-tank is
-- preferred when safe (flexible placement); this wins when capturing would walk
-- the TANK into pill fire — rebuilding risks the expendable LGM instead. So
-- pill-fire danger is deliberately ignored; only terrain (LGM walk time +
-- reachability) and enemy-TANK snipe risk (scaled down by our tank-count
-- advantage) drive the cost. Returns math.huge if the LGM can't reach the pill.
local function compute_repair_dead_cost(state, world, info, pill, tmx, tmy)
  -- LGM-travel sim: terrain-aware walk ticks, or -1 if unreachable. Bless the
  -- pill tile so the LGM may step onto its own target. The sim has NO danger
  -- gate — this is the "force it into a hot tile" path.
  local walk_ticks = cpf.lgm_travel_ticks_map(
    tmx, tmy, pill.mx, pill.my, pill.mx, pill.my,
    C.REPAIR_DEAD_LGM_MAX_TICKS or 2000, C.REPAIR_DEAD_LGM_STUCK_TICKS or 150)
  if not walk_ticks or walk_ticks < 0 then
    print2(string.format("REPAIR_DEAD t=%d pill@(%d,%d) UNREACHABLE (lgm sim -1) -> REJECT", state.tick or 0, pill.mx, pill.my))
    return math.huge  -- builder can't arrive
  end

  -- Distance curve on TILE distance: flat in the sweet zone, gentle to the
  -- knee, exponential beyond (cross-map repairs self-reject).
  local tiles  = U.mdist(tmx, tmy, pill.mx, pill.my)
  local sweet  = C.REPAIR_DEAD_SWEET_TILES or 8
  local knee   = C.REPAIR_DEAD_KNEE_TILES or 14
  local near_w = C.REPAIR_DEAD_NEAR_W or 4.8
  local mid_w  = C.REPAIR_DEAD_MID_W or 15.6
  local dist_term
  if tiles <= sweet then
    dist_term = tiles * near_w
  elseif tiles <= knee then
    dist_term = sweet * near_w + (tiles - sweet) * mid_w
  else
    local knee_val = sweet * near_w + (knee - sweet) * mid_w
    local g     = C.REPAIR_DEAD_EXP_BASE or 2.0
    local step  = C.REPAIR_DEAD_EXP_STEP_TILES or 2.0
    local scale = C.REPAIR_DEAD_EXP_SCALE or 30
    dist_term = knee_val + scale * (g ^ ((tiles - knee) / step) - 1)
  end

  -- Mild terrain surcharge: LGM walk-ticks beyond an all-grass walk of the same
  -- tile distance (swamp/forest/detours read as extra ticks → extra cost).
  local grass_ticks = tiles * (C.REPAIR_DEAD_GRASS_TICKS_PER_TILE or 16)
  local terrain_pen = math.max(0, walk_ticks - grass_ticks) * (C.REPAIR_DEAD_TERRAIN_W or 0.1)

  -- Snipe: enemy tanks within range of the pill that can pick off the builder,
  -- scaled down by our tank-count advantage ("we've got the numbers, who cares").
  local snipe, snipe_n, snipe_adv = 0, 0, 1
  local enemy_tanks = state.perc and state.perc.enemy_tanks
  if enemy_tanks then
    local range = C.REPAIR_DEAD_SNIPE_RANGE or 8
    for _, et in ipairs(enemy_tanks) do
      if U.mdist(et.mx, et.my, pill.mx, pill.my) <= range then snipe_n = snipe_n + 1 end
    end
    if snipe_n > 0 then
      local adv    = math.max(0, (state.perc and state.perc.team_advantage) or 0)
      local relief = C.REPAIR_DEAD_ADV_RELIEF_PER_TANK or 0.25
      local floor  = C.REPAIR_DEAD_ADV_FLOOR or 0.1
      snipe_adv = 1 - adv * relief
      if snipe_adv < floor then snipe_adv = floor elseif snipe_adv > 1 then snipe_adv = 1 end
      snipe = snipe_n * (C.REPAIR_DEAD_SNIPE_PEN_PER_TANK or 60) * snipe_adv
    end
  end

  local total = (C.REPAIR_DEAD_BASE_COST or 40) + dist_term + terrain_pen + snipe
  print2(string.format("REPAIR_DEAD t=%d pill@(%d,%d) cost=%.0f = base%.0f + dist%.0f(tiles=%d) + terr%.0f(ticks=%d) + snipe%.0f(n=%d adv=%.0f f=%.2f)", state.tick or 0, pill.mx, pill.my, total, C.REPAIR_DEAD_BASE_COST or 40, dist_term, tiles, terrain_pen, walk_ticks, snipe, snipe_n, math.max(0, (state.perc and state.perc.team_advantage) or 0), snipe_adv))
  return total
end

local function eval_repair_pill(state, world, info, tmx, tmy, boat, ammo)
  if not C.REPAIR_FIX_ENABLED then
    -- 1.90-beta1 repair behavior: alive-damaged friendly pills only; plain
    -- `max(0, path - dmg*BONUS)` cost; no dead-pill rebuild, no base-cost floor,
    -- no contested ×3, no friendly-fire / reposition / take-blocker guards.
    local has_damaged = not state.perc or (state.perc.friendly_pills_damaged > 0)
    if not (has_damaged and info.man_status == C.LGM_INTANK and info.trees > 0) then return nil end
    local dmx, dmy
    if state._demolish_tick
       and (state.tick - state._demolish_tick) < (C.REPOSITION_DEMOLISH_GRACE_TICKS or 1500) then
      dmx, dmy = state._demolish_mx, state._demolish_my
    end
    local ally_demolish = state._ally_demolish_tiles
    local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
      function(p)
        return p.owner == "friendly" and p.health > 0
               and p.health < C.PILLS_MAX_HEALTH
               and not (dmx and p.mx == dmx and p.my == dmy)
               and not (ally_demolish and ally_demolish[p.my * C.MAP_W + p.mx])
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
  local has_damaged = not state.perc or (state.perc.friendly_pills_damaged > 0)
  if not (has_damaged and info.man_status == C.LGM_INTANK and info.trees > 0) then return nil end
  -- Don't repair a pill we're actively demolishing for a reposition (set by
  -- reposition_steer). A damaged pill is cheaper to repair, so without this
  -- the bot would heal the pill it's shooting down — shoot→repair→shoot.
  local dmx, dmy
  if state._demolish_tick
     and (state.tick - state._demolish_tick) < (C.REPOSITION_DEMOLISH_GRACE_TICKS or 1500) then
    dmx, dmy = state._demolish_mx, state._demolish_my
  end
  -- Tiles allies broadcast as their reposition target (init.lua, from repos=1).
  local ally_demolish = state._ally_demolish_tiles
  -- Our own reposition target (live goal, refreshed every tick + an 8 s tail
  -- after the goal ends). The authoritative block — the _demolish tile guard
  -- above stops refreshing the moment the pill dies, leaving a window where the
  -- freed pool heals the pill we just shot down.
  local repos_block
  local rb = state._reposition_block
  if rb and rb.tiles
     and (state.tick - (rb.tick or 0)) < (C.REPAIR_REPOSITION_BLOCK_TICKS or 400) then
    repos_block = rb.tiles
  end
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p)
      return p.owner == "friendly"
             and p.health < C.PILLS_MAX_HEALTH   -- includes 0-HP (rebuild in place)
             -- Don't repair a pill the team declared a take blocker (init.lua
             -- unions team pblk tiles → _in_use). Includes partial-health
             -- freshly-built blockers, which would otherwise read as "damaged".
             and not p._in_use
             and not (dmx and p.mx == dmx and p.my == dmy)
             and not (ally_demolish and ally_demolish[p.my * C.MAP_W + p.mx])
             and not (repos_block and repos_block[p.my * C.MAP_W + p.mx])
             -- A friendly shot (own or ally) just landed on it → the team is
             -- shooting it down to reposition; don't heal it for 8 s.
             and not (p._friendly_shot_tick
                      and (state.tick - p._friendly_shot_tick)
                          < (C.REPAIR_FRIENDLY_FIRE_REJECT_TICKS or 400))
    end, boat, ammo, state, info, KIND_NORMAL)
  if not pill then return nil end
  local damage = C.PILLS_MAX_HEALTH - pill.health
  local adj_cost
  if pill.health == 0 then
    -- Dead pill → rebuild-in-place model (own terrain + tank-snipe cost; the
    -- contested ×3 below is skipped — snipe already prices in nearby enemy tanks).
    adj_cost = compute_repair_dead_cost(state, world, info, pill, tmx, tmy)
  else
    adj_cost = (C.REPAIR_BASE_COST or 30) + math.max(0, pcost - damage * C.REPAIR_DAMAGE_BONUS)
    -- Contested repair: if an enemy tank is CLOSER to the pill than we are, the
    -- repair is likely futile (they'll re-damage/kill it while we work the LGM
    -- under fire). Don't skip it outright — triple the cost so it loses to better
    -- goals but can still win if nothing else is worth doing.
    local contested = false
    local enemy_tanks = state.perc and state.perc.enemy_tanks
    if enemy_tanks then
      local our_d = U.mdist(tmx, tmy, pill.mx, pill.my)
      for _, e in ipairs(enemy_tanks) do
        if U.mdist(e.mx, e.my, pill.mx, pill.my) < our_d then contested = true; break end
      end
    end
    -- Also contested on a fresh enemy sighting stamped AT the pill
    -- (perception's _enemy_near_tick) — mirrors the live pool-5 copy.
    if not contested and pill._enemy_near_tick
       and ((state.tick or 0) - pill._enemy_near_tick) < (C.DEFEND_SIGHT_FRESH_TICKS or 600) then
      contested = true
    end
    if contested then adj_cost = adj_cost * (C.REPAIR_CONTESTED_MULT or 3.0) end
  end
  -- Pool-viz: surface friendly damaged pills we DIDN'T repair because they're a
  -- team-declared take blocker (filtered out above via not p._in_use). Shown as
  -- a rejected [blocker] candidate so the panel makes the protection visible.
  if BRAIN_POOL_VIZ and pcands then
    for bpid, bp in pairs(world.pills) do
      if bp._in_use and bp.owner == "friendly" and bp.health > 0
         and bp.health < C.PILLS_MAX_HEALTH then
        pcands[#pcands + 1] = { id = bpid, mx = bp.mx, my = bp.my, cost = -1, reject = "blocker" }
      end
    end
  end
  return {
    cost = adj_cost,
    goal = { kind = "repair_pill", mx = pill.mx, my = pill.my,
             wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
    desc = BRAIN_POOL_VIZ and string.format("repair_pill#%d@(%d,%d) cost=%.0f (base=%.0f +path=%.0f -dam=%d×%d)%s",
           pid, pill.mx, pill.my, adj_cost, C.REPAIR_BASE_COST or 30, pcost, damage, C.REPAIR_DAMAGE_BONUS,
           contested and " ×3 CONTESTED(enemy closer)" or "") or "",
    cands = pcands,
  }
end

-- Harasser distance de-emphasis. A harasser ("h") roams to fight, so every
-- distance-tied cost term in its combat goals (attack_pill / attack_tank /
-- kill_lgm) is toned down by HARASSER_TRAVEL_MULT. Returns the multiplier to
-- apply to a path_cost / dist×per_tile / far-preempt term (1.0 for non-harassers).
local function harass_dist_mult(state)
  return state.is_harasser and (C.HARASSER_TRAVEL_MULT or 1.0) or 1.0
end

-- Spiking-pill detection. A "spike" is a hostile/neutral pill parked within
-- PILL_FIRE_RANGE of a friendly base — it shoots us while we sit refueling,
-- denying the base until it's cleared. Shared by the live pool-6 cost in
-- step_eval_queue, the attack_pill_adjustments mirror, and the spike_pills
-- map overlay. Recomputed at most once per tick (pills × bases mdist scan).
--
-- DECISIVENESS: what matters is whether killing THIS pill actually frees a
-- base. cover = how many spikes sit on the pill's least-contested base;
-- dec = 1/cover. One lone spike → dec 1.0 (removal fully frees the base,
-- top value). Four pills co-spiking an area → dec 0.25 each (that area is
-- kinda lost; clearing one changes nothing, so the pull is weak).
-- Populates:
--   state._spike_pills[my*256+mx] = { pmx, pmy, bmx, bmy, n, bases,
--     cover, dec }  (bmx/bmy = first denied base, n = bases denied,
--     bases = {{mx,my},...})
--   state._spike_present   = true while any spike exists
--   state._spike_pen_scale = max dec across all spikes (scales the
--     cross-penalty: only a decisive spike justifies taxing other takes)
local function refresh_spike_detection(state, world)
  if state._spike_tick == state.tick then return end
  state._spike_tick = state.tick
  -- Pass 1: per-friendly-base spike cover count.
  local cover = nil
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      local cnt = 0
      for _, p in pairs(world.pills) do
        if (p.owner == "hostile" or p.owner == "neutral") and p.health > 0
           and U.mdist(p.mx, p.my, b.mx, b.my) <= C.PILL_FIRE_RANGE then
          cnt = cnt + 1
        end
      end
      if cnt > 0 then
        cover = cover or {}
        cover[b.my * 256 + b.mx] = cnt
      end
    end
  end
  -- Pass 2: per-pill records with decisiveness from the least-covered base.
  local spikes, present, pen_scale = nil, false, 0
  if cover then
    for _, p in pairs(world.pills) do
      if (p.owner == "hostile" or p.owner == "neutral") and p.health > 0 then
        local n, bx, by, blist, mincov = 0, nil, nil, nil, math.huge
        for _, b in pairs(world.bases) do
          if b.owner == "friendly"
             and U.mdist(p.mx, p.my, b.mx, b.my) <= C.PILL_FIRE_RANGE then
            n = n + 1
            if not bx then bx, by = b.mx, b.my end
            blist = blist or {}
            blist[#blist + 1] = { mx = b.mx, my = b.my }
            local bc = cover[b.my * 256 + b.mx] or 1
            if bc < mincov then mincov = bc end
          end
        end
        if n > 0 then
          local dec = 1.0 / mincov
          spikes = spikes or {}
          spikes[p.my * 256 + p.mx] = { pmx = p.mx, pmy = p.my, bmx = bx, bmy = by,
                                        n = n, bases = blist, cover = mincov, dec = dec }
          present = true
          if dec > pen_scale then pen_scale = dec end
        end
      end
    end
  end
  state._spike_pills = spikes
  state._spike_present = present
  state._spike_pen_scale = pen_scale
end

-- Combat-cost multiplier for a spiking pill. Base pull: SPIKE_PILL_DISCOUNT
-- at full decisiveness, fading toward x1.0 as the pill's least-contested base
-- gets co-spiked (dec = 1/cover — clearing one of many frees nothing). On top
-- of that, BREADTH: each ADDITIONAL base the pill denies strengthens the pull
-- by SPIKE_BASES_BONUS (a pill spiking 3 bases matters more than a 1-base
-- spike at equal decisiveness). Floored at SPIKE_DISCOUNT_FLOOR so a decisive
-- wide spike can't drive the combat block below half cost.
local function spike_discount_mult(sp)
  local effect = (1.0 - (C.SPIKE_PILL_DISCOUNT or 0.8)) * (sp.dec or 1.0)
                 * (1.0 + (C.SPIKE_BASES_BONUS or 0.35) * math.max(0, (sp.n or 1) - 1))
  local sm = 1.0 - effect
  local floor = C.SPIKE_DISCOUNT_FLOOR or 0.5
  if sm < floor then sm = floor end
  return sm
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

  -- Spike shaping (MIRROR of the live copy in step_eval_queue's pool-6
  -- assembly — this evaluator is only reachable via fill_pool_cache, which
  -- currently has no callers): a spiking pill (within firing range of a
  -- friendly base, denying refuel) gets a small combat discount; while ANY
  -- spike exists, every non-spiking pill instead has its WHOLE cost
  -- multiplied (spike_pen_mult, applied to `final` below). Both scale with
  -- decisiveness (1/cover). Replaces the old flat *baseThreat 0.5.
  local spike_pen_mult = 1.0
  do
    refresh_spike_detection(state, world)
    local sp = state._spike_pills and state._spike_pills[pill.my * 256 + pill.mx]
    if sp then
      local sm = spike_discount_mult(sp)
      combat_cost = combat_cost * sm
      if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" *spike(x%.2f n=%d cover=%d)", sm, sp.n or 1, sp.cover or 1) end
    elseif state._spike_present then
      spike_pen_mult = 1.0 + ((C.SPIKE_OTHER_PENALTY_MULT or 1.15) - 1.0) * (state._spike_pen_scale or 1.0)
      if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" *spike_pen(x%.3f)", spike_pen_mult) end
    end
  end

  -- Final cost = flat base + fixed path cost + scaled combat cost. The flat
  -- base keeps a pill take from being effectively free vs. other goals.
  local final
  if state.is_harasser then
    -- Harasser: discount the path/travel term, 2× the engage portion (flat base
    -- + combat). Mirrors the split in update_pool_cache's pool-6 assembly.
    final = pcost * harass_dist_mult(state)
          + ((C.ATTACK_PILL_BASE_COST or 0) + combat_cost) * (C.HARASSER_PILL_COST_MULT or 1.0)
    if BRAIN_POOL_VIZ then antic_desc = antic_desc .. string.format(" *harass(travel x%.1f, engage x%.1f)",
      C.HARASSER_TRAVEL_MULT or 1.0, C.HARASSER_PILL_COST_MULT or 1.0) end
  else
    final = (C.ATTACK_PILL_BASE_COST or 0) + pcost + combat_cost
  end
  -- Spike cross-penalty: multiply the WHOLE cost (travel included) —
  -- mirrors the post-assembly multiply in step_eval_queue's pool 6.
  final = final * spike_pen_mult
  return final, antic_desc
end

local function eval_attack_pill(state, world, info, tmx, tmy, boat, ammo)
  local has_hostile = not state.perc or (state.perc.attackable_pill_count > 0)
  if not (has_hostile and info.shells > C.SHELLS_LOW) then return nil end
  -- Lower armour threshold for wounded pills (few shots needed)
  local min_armour = C.ATTACK_PILL_MIN_ARMOUR
  if state.wounded_pill then min_armour = 15 end
  -- pill_suicider: no armour floor — see the live pool-6 gate for rationale.
  if info.armour < min_armour and not state.is_pill_suicider then return nil end
  -- attack.pp_blacklisted: a pill whose plan_position angle sweep was abandoned
  -- (its chunk kept getting killed by the tick budget) stays out of the pool for
  -- PP_BLACKLIST_TICKS — otherwise pick_goal re-adopts the same take and the
  -- substate walks straight back into the unfittable sweep.
  local pill, pid, pcost, pcands = nearest_where(world.pills, world, tmx, tmy,
    function(p, id) return (p.owner == "hostile" or p.owner == "neutral") and p.health > 0
                           and not attack.pp_blacklisted(state, id) end,
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

  return {
    cost = adj_cost,
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
  if not has_hbases then return nil end
  -- Only attack hostile bases that are still alive (health > 0).
  -- health=0 means capturable — eval_capture_base handles those.
  local base, bid, bcost, bcands = nearest_where(world.bases, world, tmx, tmy,
    function(b) return b.owner == "hostile" and b.health > 0 end, boat, ammo, state, info, KIND_NORMAL, nil,
    -- +BASE_PILL_COVER_PEN per enemy pill whose fire covers this base but not our
    -- current tile (new exposure only) — biases toward attacking less-covered bases.
    function(b) return (C.BASE_PILL_COVER_PEN or 3) * count_new_exposure_pills(world, b.mx, b.my, tmx, tmy, true) end)
  if not base then return nil end
  -- Shells gate — with a CLOSE-OUT exception. A nearly-dead base (health =
  -- armour/5) can be finished with any ammo, so don't apply the SHELLS_LOW gate
  -- that would peel us off to refuel one shot short (20260706_231404 t=17961:
  -- base#3 at armour 5, bot had 8 shells, refuel won and the base healed back).
  local closeout = (base.health or 99) <= (C.ATTACK_BASE_CLOSEOUT_HEALTH or 3)
  local min_shells = closeout and (C.SHELL_RESERVE or 0) or C.SHELLS_LOW
  if info.shells <= min_shells then return nil end
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
  -- Close-out priority: a few shots from neutralizing an enemy base and we have
  -- ammo — crush the cost so nothing routine (esp. refuel) outbids finishing it.
  if closeout then adj_cost = math.min(adj_cost, C.ATTACK_BASE_CLOSEOUT_COST or 8) end
  return {
    cost = adj_cost,
    _shells_on_arrival = shells_on_arrival,
    _closeout = closeout or nil,
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

-- Count friendly bases within radius of a map position (for protective-pill
-- coverage scoring — how many bases a spot can defend within fire range).
local function count_friendly_bases_near(world, mx, my, radius)
  local count = 0
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" and U.mdist(mx, my, b.mx, b.my) <= radius then
      count = count + 1
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

-- Engage-spot crossfire: count live hostile/neutral pills whose fire-range
-- covers the chosen ENGAGE SPOT but does NOT already cover our current tile —
-- only the NEW exposure we take on by repositioning there counts. We're already
-- eating the pills that cover us now, and A* already prices travel-through-fire,
-- so don't double-count. Escalating: +BASE for the 1st new pill, +STEP more for
-- each additional one (default 40, 90, 150, 230, ...). Neutral and hostile pills
-- both count (both fire at any tank in range). Iterates all live pills (<=16),
-- not perc.pill_threats (which is anchored to our CURRENT tile, so it'd miss
-- pills that only cover a distant engage spot).
local function new_pill_crossfire(world, spot_mx, spot_my, tmx, tmy)
  local n = count_new_exposure_pills(world, spot_mx, spot_my, tmx, tmy)
  if n == 0 then return 0 end
  local base = C.GOAL_CROSSFIRE_NEW_PILL_BASE or 40
  local step = C.GOAL_CROSSFIRE_NEW_PILL_STEP or 10
  return base * n + step * (n * (n - 1) / 2)
end

local function eval_attack_tank(state, world, info, tmx, tmy, boat, ammo)
  state.attack_tank_breakdown = nil

  -- Check gates but don't return early — always populate breakdown so
  -- every visible tank shows in the pool window with its rejection reason.
  -- NOTE: the low_shells gate is evaluated per-candidate below, not globally,
  -- because a target in deep sea is a boat (1 shot to sink) and we only need
  -- 1 shell to engage it. Other gates (DISABLED, low_armour, in_boat) still
  -- apply globally.
  -- NOTE: the low-armour hard-rejection was removed intentionally — a
  -- critically-wounded bot may still need/want to engage a tank in front
  -- of it (e.g. it has no other escape). Armour still governs in-combat
  -- behaviour (flee/disengage at TANK_COMBAT_FLEE_ARMOUR) elsewhere; this
  -- only stops armour from blocking the attack_tank GOAL from being picked.
  local gate_reason = nil
  if not C.TANK_COMBAT_ENABLED then gate_reason = "DISABLED"
  -- (No in_boat gate: a tank can fire from a boat, so an enemy tank still
  -- triggers attack_tank while we're afloat. The steering handles the boat
  -- cases — boat-vs-boat fights normally; if the enemy is on LAND we disembark
  -- to the nearest reachable land tile first, see tank_combat_steer.)
  -- Pillbox-crossfire gate: if our own tile sits in heavy enemy PILL danger,
  -- don't surface ANY tank candidate. Mirrors the attack_tank goal-validity
  -- crossfire check (init.lua) so the two agree — otherwise the validity
  -- check clears the goal to flee, but this evaluator instantly re-targets an
  -- adjacent tank, flickering the goal and pinning us in the crossfire.
  elseif threat.pill_at(tmx, tmy) >= (C.TANK_COMBAT_DEFENDED_DANGER or math.huge) then
    gate_reason = "pill_crossfire"
  end
  local low_shells_global = info.shells < C.TANK_COMBAT_MIN_SHELLS
  if gate_reason and BRAIN_DEBUG_MODE then print2("eval_attack_tank: " .. gate_reason) end

  local perc = state.perc
  -- Hunting candidate list = REAL sightings + extrapolated GHOSTS (out-of-sight
  -- tanks we keep chasing for GHOST_TANK_TTL_TICKS). Only this path and the
  -- steering target-acquisition merge ghosts; everything else sees real only.
  local enemy_tanks = (perc and perc.enemy_tanks) or {}
  local ghost_tanks = (perc and perc.ghost_tanks) or {}
  if #ghost_tanks > 0 then
    local merged = {}
    for _, et in ipairs(enemy_tanks) do merged[#merged + 1] = et end
    for _, gt in ipairs(ghost_tanks) do merged[#merged + 1] = gt end
    enemy_tanks = merged
  end
  if #enemy_tanks == 0 then
    if BRAIN_DEBUG_MODE then
      print2(string.format("eval_attack_tank: no enemy tanks (perc=%s, et=%s)",
        perc and "yes" or "nil",
        (perc and perc.enemy_tanks) and tostring(#perc.enemy_tanks) or "nil"))
    end
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
        cost = (C.TANK_COMBAT_LOS_BASE_COST
               + et.dist * C.TANK_COMBAT_LOS_COST_PER_TILE * harass_dist_mult(state)
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
        cost = path_cost * harass_dist_mult(state) + C.TANK_COMBAT_BASE_COST + wall_penalty + low_shells_penalty

        -- Aim bonus: if we're already pointed roughly at this tank, cheaper to engage.
        -- Capped at 10 if out of shooting range, 25 if in range.
        local aim_dir = U.aim_at(info.tankx, info.tanky, U.m2w(et.mx), U.m2w(et.my))
        aim_diff = math.abs(U.adiff(info.direction, aim_dir))
        if aim_diff < C.TANK_COMBAT_AIM_THRESHOLD then
          local aim_cap = et.dist <= C.TANK_COMBAT_ENGAGE_RANGE and 25 or 10
          aim_bonus = math.min(C.TANK_COMBAT_AIM_BONUS, aim_cap)
          cost = cost - aim_bonus
        end

        -- Crossfire penalty: NEW pill exposure at the chosen standoff
        -- (so_mx,so_my) — pills whose fire-range covers it but DON'T already
        -- cover our current tile (we're already eating those; A* prices the
        -- travel). Escalating per new pill, see new_pill_crossfire.
        crossfire = new_pill_crossfire(world, so_mx, so_my, tmx, tmy)
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

      -- Ghost penalty: an extrapolated (out-of-sight) tank is a guess, so make
      -- it slightly costlier than a tank we can actually see — a real target
      -- always wins, but with no real target we'll still go hunt the ghost.
      if et.ghost then cost = cost + (C.GHOST_TANK_COST_PENALTY or 40) end

      -- Distance guard (GLOBAL): a tank beyond our shooting range gets a penalty
      -- that climbs EXPONENTIALLY with euclidean distance past shoot range — ~0
      -- at the range edge, runaway by a handful of tiles out — so only a
      -- genuinely CLOSE (actually-threatening) tank is worth engaging. Applies on
      -- EVERY goal now, not just during an attack_pill take (was gated on
      -- attack_pill; made global by design): the bot won't chase a far tank
      -- across the map whatever it's doing. Still fixes a 26-tile tank yanking a
      -- blitz commander off its charge.
      local far_preempt_pen = 0
      do
        local _ex, _ey = (et.mx - tmx), (et.my - tmy)
        local _edist = math.sqrt(_ex * _ex + _ey * _ey)
        local _shoot_r = C.ATTACK_FAR_PREEMPT_RANGE or C.TANK_COMBAT_ENGAGE_RANGE or 7
        if _edist > _shoot_r then
          far_preempt_pen = math.min(
            (C.ATTACK_FAR_PREEMPT_BASE or 1.7) ^ (_edist - _shoot_r)
              * (C.ATTACK_FAR_PREEMPT_K or 8),
            C.ATTACK_FAR_PREEMPT_CAP or 1e6)
          far_preempt_pen = far_preempt_pen * harass_dist_mult(state)
          cost = cost + far_preempt_pen
        end
      end

      breakdown[#breakdown + 1] = {
        id = et.id, mx = et.mx, my = et.my, dist = et.dist, speed = et.speed,
        ghost = et.ghost, ghost_age = et.ghost_age,
        path_cost = path_cost, base = los_engage and 0 or C.TANK_COMBAT_BASE_COST,
        aim_bonus = aim_bonus, aim_diff = aim_diff, crossfire = crossfire,
        wall_hp = wall_hp, wall_penalty = wall_penalty,
        low_shells_penalty = low_shells_penalty, boat_mult = boat_mult,
        tank_tile_threat = tank_tile_threat, far_preempt_pen = far_preempt_pen,
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
        local allied = (bit.band(allies, (bit.lshift(1, pn)))) ~= 0
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
    -- No viable enemy tank this eval → attack_tank is "rejected": clear the
    -- shared flag (drives the attack_pill cross-penalty + panic_build viz).
    state._attack_tank_present = false
    state._attack_tank_threat  = nil
    if BRAIN_DEBUG_MODE then print2(string.format("eval_attack_tank: no best_tank (%d candidates examined)", #breakdown)) end
    return nil
  end
  -- A non-rejected attack_tank candidate exists. Flag it so attack_pill gets a
  -- flat cross-penalty (prefer dealing with the tank) and the panic_build viz
  -- shows. Stash the threat tile for the viz.
  state._attack_tank_present = true
  state._attack_tank_threat  = { mx = best_tank.mx, my = best_tank.my, dist = best_tank.dist,
                                 have_pill = (info.carried_pills or 0) >= 1 }
  if BRAIN_DEBUG_MODE then print2(string.format("eval_attack_tank: WINNER @(%d,%d) cost=%.1f dist=%d",
    best_tank.mx, best_tank.my, best_cost, best_tank.dist)) end

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

-- Flat combat-zone penalty for a (non-emergency) strategic placement: an enemy
-- tank near the chosen spot makes dropping a pill there risky. CLOSE supersedes
-- NEAR. Euclidean distance from the spot to the nearest visible enemy tank.
local function place_near_tank_penalty(state, smx, smy)
  local et = state.perc and state.perc.enemy_tanks
  if not et or not smx then return 0 end
  local best = math.huge
  for _, e in ipairs(et) do
    local dx, dy = e.mx - smx, e.my - smy
    local d = math.sqrt(dx * dx + dy * dy)
    if d < best then best = d end
  end
  if best <= (C.STRATEGIC_PLACE_CLOSE_TANK_DIST or 7) then return C.STRATEGIC_PLACE_CLOSE_TANK_PENALTY or 60 end
  if best <= (C.STRATEGIC_PLACE_NEAR_TANK_DIST or 10) then return C.STRATEGIC_PLACE_NEAR_TANK_PENALTY or 30 end
  return 0
end

-- (eval_place_pill_fallback removed: placement is now a single tank-centric,
-- portfolio-aware search in eval_place_pill_strategic. When that finds no
-- placeable non-surplus spot, the bot keeps carrying — there is no fallback.)

-- find_safe_forest: best forest tile to harvest. Two cheap phases:
--   1. Ring-scan outward from the tank collecting forest tiles into a list. The
--      per-tile test is a BARE in-memory array read — get_terrain is a C closure
--      over (*worldPtr)[y*256+x], so "is this forest?" is one index, no U.ttype
--      fog-of-war bookkeeping (metrics/terrain_prev/changes appends) that made the
--      old map-wide scan ~18ms. Once the nearest forest ring is found we expand
--      RING_SLACK more rings (so a slightly-farther, safer one can still win by
--      Dijkstra cost below) and stop.
--   2. Rank the collected tiles by the already-computed, danger-weighted Dijkstra
--      travel cost (smart_cost_dij_only — an O(1) slate read). That inherently
--      skips enemy territory (danger inflates the cost) and prefers our own turf,
--      replacing the old per-tile threat.at + influence_at recompute. Fall back to
--      the geometric-nearest forest if the slate hasn't expanded to any of them
--      yet (or Dijkstra-for-goals is off) so we never deadlock while forest exists.
-- Returns fx, fy or nil.
local function find_safe_forest(tmx, tmy)
  local get_terrain  = get_terrain      -- C global: raw (*worldPtr)[y*256+x] read
  local TERRAIN_MASK = TERRAIN_MASK     -- C global
  local T_FOREST     = C.T_FOREST
  local max_r        = C.SEEK_TREES_MAX_RADIUS or 120
  local slack        = C.SEEK_TREES_RING_SLACK or 6

  local fx, fy, nf, found_r = {}, {}, 0, nil
  local function scan(x, y, r)
    if x >= 0 and x <= 255 and y >= 0 and y <= 255
       and (bit.band(get_terrain(x, y), TERRAIN_MASK)) == T_FOREST then
      nf = nf + 1; fx[nf] = x; fy[nf] = y; found_r = found_r or r
    end
  end
  for r = 1, max_r do
    for i = -r, r do
      scan(tmx + i, tmy - r, r)   -- top edge
      scan(tmx + i, tmy + r, r)   -- bottom edge
      if i > -r and i < r then
        scan(tmx - r, tmy + i, r) -- left edge
        scan(tmx + r, tmy + i, r) -- right edge
      end
    end
    if found_r and r >= found_r + slack then break end
  end
  if nf == 0 then return nil end

  local best_x, best_y, best_cost = nil, nil, math.huge
  local near_x, near_y, near_d    = nil, nil, math.huge
  for k = 1, nf do
    local x, y = fx[k], fy[k]
    local cost = cpf.smart_cost_dij_only(KIND_NORMAL, x, y, 0)
    if cost < best_cost then best_cost = cost; best_x = x; best_y = y end
    local d = U.mdist(tmx, tmy, x, y)
    if d < near_d then near_d = d; near_x = x; near_y = y end
  end
  if best_x then return best_x, best_y end   -- cheapest reachable per the slate
  return near_x, near_y                      -- slate had none yet: nearest known
end

local function eval_place_pill_strategic(state, world, info, tmx, tmy, boat, ammo)
  if not C.STRATEGIC_PLACE_ENABLED then return nil end

  -- Seek-trees redirect (BEFORE the LGM-in-tank actionable gate below, so it
  -- persists while the LGM is out harvesting): carrying pills we can't afford to
  -- place (each needs PILL_PLACE_TREE_COST wood) and out of trees -> travel to a
  -- SAFE forest and gather rather than deadlocking on an unpayable build
  -- (20260707_044217 t=127262: carry=6, tr=0, frozen 764 ticks re-issuing
  -- BUILDMODE_PBOX). Pressure scales with pills carried; distance barely dents it.
  if (info.carried_pills or 0) >= 1 and not info.inboat
     and (info.trees or 0) < (C.PILL_PLACE_TREE_COST or 4) then
    -- Cache the chosen forest (static terrain) so the expensive map-wide ring
    -- scan runs rarely — not on every pool re-eval (it was spiking pool_cache to
    -- ~9ms). Re-search when the cache is stale or the tile got harvested to grass.
    local now = state.tick or 0
    local sf  = state._seek_forest
    local fresh = sf and (now - (sf.tick or 0)) < (C.SEEK_TREES_CACHE_TICKS or 150)
    local ok    = fresh and sf.mx and U.in_map(sf.mx, sf.my)
                        and U.ttype(sf.mx, sf.my) == C.T_FOREST
    local fx, fy
    if fresh and (ok or not sf.mx) then
      fx, fy = sf.mx, sf.my                       -- reuse (valid forest, or cached "none")
    else
      -- Arm the cache cooldown BEFORE the scan as a cheap safety net: if
      -- find_safe_forest (Lua) ever overruns the per-tick budget it's killed
      -- MID-LOOP, before the post-scan cache write below. Pre-stamping `now`
      -- bounds re-scans to once per SEEK_TREES_CACHE_TICKS even on overrun, so a
      -- killed scan can't re-fire every tick (the seek_trees t=525 spiral). The
      -- scan itself is now near-instant (bare terrain reads + O(1) Dijkstra
      -- ranking), so this is belt-and-suspenders rather than load-bearing.
      state._seek_forest = { mx = sf and sf.mx or nil, my = sf and sf.my or nil, tick = now }
      fx, fy = find_safe_forest(tmx, tmy)
      state._seek_forest = { mx = fx, my = fy, tick = now }
    end
    if fx then
      local d    = U.mdist(tmx, tmy, fx, fy)
      local cost = math.max(
        (C.SEEK_TREES_BASE_COST or 40)
          - (info.carried_pills or 0) * (C.SEEK_TREES_CARRY_DISCOUNT or 6)
          + d * (C.SEEK_TREES_DIST_WEIGHT or 0.4),
        C.SEEK_TREES_MIN_COST or 4)
      return {
        cost = cost,
        -- seek_trees is a SUBSTATE of place_pill_strategic (goal.substate) so it's
        -- visible in the debug/goal trace, and the builder routes it to gather.
        goal = { kind = "place_pill_strategic", substate = "seek_trees",
                 mx = fx, my = fy, wx = U.m2w(fx), wy = U.m2w(fy) },
        desc = BRAIN_POOL_VIZ and string.format(
               "seek_trees@(%d,%d) cost=%.0f d=%d carry=%d tr=%d", fx, fy, cost, d,
               info.carried_pills or 0, info.trees or 0) or "",
      }
    end
    -- No safe forest anywhere in range — fall through; normal path returns nil
    -- (can't place either), so the bot keeps its pills until forest is reachable.
  end

  -- Can we actually place right now?
  local actionable = (info.carried_pills or 0) >= 1
                     and info.man_status == C.LGM_INTANK
                     and not info.inboat
  -- In DEBUG only, still run the scan to feed the best-spot overlays even when
  -- we can't place — gated on an overlay being on, so a live game (where
  -- BRAIN_DEBUG_MODE is false) never pays for this and just returns here.
  local viz_only = BRAIN_DEBUG_MODE
                   and (vizmod.is_on("pill_best_spots_back") or vizmod.is_on("pill_best_spots_aggro"))
  if not actionable and not viz_only then return nil end

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
  -- Urgency overrides: cancel carry penalty when placement is critical. Also
  -- flags place_urgent so the spot-quality floor below is bypassed — when a base
  -- is naked under fire or we're about to die, ANY placeable spot beats holding.
  local place_urgent = false
  local fbx, fby, fb_dist = nearest_friendly_base_pos(world, tmx, tmy)
  if fbx then
    local base_pills = count_pills_near(world, fbx, fby, C.STRATEGIC_PLACE_DEFENSE_RADIUS, "friendly")
    local enemy_near = state.perc and state.perc.enemy_tanks and #state.perc.enemy_tanks > 0
    if base_pills == 0 and enemy_near then
      carry_value_penalty = 0  -- base naked + enemy visible: place NOW
      place_urgent = true
    end
  end
  if info.armour <= C.ARMOUR_CRITICAL then
    carry_value_penalty = 0  -- drop before we die
    place_urgent = true
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
  local _db_et = (state.perc and state.perc.enemy_tanks) and #state.perc.enemy_tanks or 0
  -- Defensive build DROPS a carried pill — so it must only fire when we actually
  -- hold one. Without it the goal wins the pool on a cheap score, then dead-ends
  -- at PLACE_PILL_SETMODE "no-dispatch" (carried=0, man=0), stealing a goal cycle
  -- from attack_tank and flip-flopping the aim. Gate on carried_pills > 0.
  local _db_carrying = (info.carried_pills or 0) > 0
  -- Panic build: at PANIC_BUILD_ARMOUR or below while carrying (and the LGM is in
  -- the tank to place it), DUMP a pill into the ground NOW — no matter who's
  -- around (or not). At rock-bottom health we can't count on reaching a base, so
  -- bank the carried pill (and gain a guard) before dying and gifting it to the
  -- enemy. A DEAD/out builder can't place — the haul-protection flee covers that.
  local _panic = _db_carrying and info.man_status == C.LGM_INTANK
                 and (info.armour or 99) <= (C.PANIC_BUILD_ARMOUR or 10)
  local _db_skip = ((not _db_carrying) and " -> SKIP(not carrying a pill)")
                or ((_db_et == 0 and not _panic) and " -> SKIP(no visible enemy tank, armour ok)") or ""
  print2(string.format("OFF_BUILD t=%d gate carried=%d man=%s inboat=%s enemy_tanks=%d panic=%s arm=%d%s", state.tick or 0, info.carried_pills or 0, tostring(info.man_status), tostring(info.inboat), _db_et, tostring(_panic), info.armour or -1, _db_skip))
  if (_db_et > 0 or _panic) and _db_carrying then
    local closest_et, closest_dist = nil, math.huge
    for _, et in ipairs(state.perc.enemy_tanks) do
      if et.dist < closest_dist then closest_dist = et.dist; closest_et = et end
    end
    -- Distance protection: no need to panic-build if the nearest enemy tank is
    -- out of SHOOTING range — it can't actually hit us, so dropping a guard pill
    -- mid-carry is wasted. Euclidean (like the attack_tank pill-take guard);
    -- et.dist is mdist, so recompute. Past the range, drop the trigger.
    if closest_et and not _panic then
      local _ex, _ey = closest_et.mx - tmx, closest_et.my - tmy
      local _ed = math.sqrt(_ex * _ex + _ey * _ey)
      if _ed > (C.OFF_BUILD_THREAT_RANGE or 8) then
        print2(string.format("OFF_BUILD t=%d SKIP — nearest tank @(%d,%d) euclid=%.1f > %d (out of shoot range, no panic)", state.tick or 0, closest_et.mx, closest_et.my, _ed, C.OFF_BUILD_THREAT_RANGE or 8))
        closest_et = nil
      end
    end
    -- Desperate ("about to die") override: low armour AND actively taking hits
    -- (a hit within DEATH_BUILD_HIT_WINDOW ticks), with a threat in shoot range.
    -- A tank that dies carrying pills DROPS them for anyone to grab — so plant
    -- them as our guard NOW rather than losing them on death. Bypasses the cover
    -- dedup below (that cover clearly isn't keeping us alive) and forces a
    -- rock-bottom cost so the build decisively wins the pool. Placement geometry
    -- is unchanged. Re-fires each cycle while carried>0, so it plants BOTH pills.
    local _tick = state.tick or 0
    local _desperate = (closest_et ~= nil)
      and (info.armour or 99) <= (C.DEATH_BUILD_ARMOUR or 30)
      and state._last_damage_tick ~= nil
      and (_tick - state._last_damage_tick) <= (C.DEATH_BUILD_HIT_WINDOW or 50)

    -- Cover dedup (shared with builder's in-combat drop): a healthy friendly
    -- pill already within fire range of the tank is the guard this build would
    -- provide — don't drop a second pill beside it. Nearly-dead cover
    -- (<= SUPPORT_PILL_MIN_HP) doesn't count; build its replacement. Skipped
    -- when desperate — we plant regardless of existing cover.
    if closest_et and not _desperate and not _panic then
      -- Two-distance test: the pill must cover BOTH us and the enemy to count
      -- as support for this fight. A pill behind us covers us but cannot shoot
      -- the tank we are engaging, and vetoing on it declines to build
      -- reinforcement nothing of ours can reach. Safe here because an enemy
      -- within OFF_BUILD_THREAT_RANGE is this build's trigger, so the enemy is
      -- close by construction -- unlike the in-combat guard drop, which keeps
      -- the tank-only form.
      local _cov = builder.nearby_support_pill(world, tmx, tmy,
                                               closest_et.mx, closest_et.my)
      if _cov then
        print2(string.format(
          "BUILD_VETO t=%d kind=offensive support=(%d,%d) hp=%d d_self=%.1f d_enemy=%.1f",
          state.tick or 0, _cov.mx, _cov.my, _cov.health or 0,
          U.edist(tmx, tmy, _cov.mx, _cov.my),
          U.edist(closest_et.mx, closest_et.my, _cov.mx, _cov.my)))
        closest_et = nil
      end
    elseif closest_et and _desperate then
      print2(string.format("OFF_BUILD t=%d DESPERATE — arm=%d <= %d, hit %d ticks ago; ignoring cover dedup, forcing win", state.tick or 0, info.armour or 0, C.DEATH_BUILD_ARMOUR or 30, _tick - (state._last_damage_tick or _tick)))
    end
    if closest_et or _panic then
      -- Guard-spot DIRECTION: the nearest enemy tank if we have one, else (panic
      -- with nobody around) the nearest hostile pill, else a default offset so we
      -- still plant SOMEWHERE valid. Placement direction barely matters for a pure
      -- bank-the-pill panic; when a threat exists it makes the drop a real guard.
      local _thr_mx, _thr_my
      if closest_et then
        _thr_mx, _thr_my = closest_et.mx, closest_et.my
      else
        local _hpx, _hpy = nearest_hostile_pill_pos(world, tmx, tmy)
        if _hpx then _thr_mx, _thr_my = _hpx, _hpy else _thr_mx, _thr_my = tmx, tmy - 4 end
      end
      -- Shared panic guard-spot search (the SAME code builder.lua's in-combat
      -- guard drop uses, so they can't drift): nearest-first ±45° from the threat,
      -- grass/road preferred over swamp/rubble/crater, placeable + wall-free +
      -- LGM-reachable, nearest tier-2 fallback. Returns the spot + all considered
      -- tiles (dcands) for the panic_build overlay.
      local best_cx, best_cy, best_tier, dcands =
        builder.guard_build_spot(world, info, tmx, tmy, _thr_mx, _thr_my, state)
      if BRAIN_DEBUG_MODE then state._guard_build_viz = { tick = state.tick or 0, spots = dcands, best_cx = best_cx, best_cy = best_cy, threat_mx = _thr_mx, threat_my = _thr_my, tank_mx = tmx, tank_my = tmy } end
      if best_cx then
        local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_cx, best_cy, 0,
                           info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
        local raw_cost = path_cost + C.STRATEGIC_PLACE_BASE_COST - carry_discount
        local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT)
        -- Desperate or panic: floor the cost so the plant decisively wins.
        if _desperate or _panic then cost = 1 end
        local cands = {}
        if BRAIN_DEBUG_MODE then
          for _, c in ipairs(dcands) do
            local is_win = (c.mx == best_cx and c.my == best_cy)
            cands[#cands + 1] = {
              id = c.my * 256 + c.mx, mx = c.mx, my = c.my,
              cost = is_win and cost or -1,
              formula = is_win
                and string.format("offensive_build WIN dist=%d aoff=%d (A*{%.0f}+base{%.0f}-carry{%.0f})*%.2f",
                      c.dist, c.aoff, path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_discount, C.STRATEGIC_PLACE_COST_MULT)
                or string.format("offensive_build dist=%d aoff=%d SKIP %s", c.dist, c.aoff, tostring(c.rej)),
              stale = 0,
              reject = (not is_win) and c.rej or nil,
              reject_remaining = 0,
            }
          end
        end
        print2(string.format("OFF_BUILD t=%d FIRE%s spot=(%d,%d) tier=%d cost=%.0f threat=(%d,%d) d=%.1f", state.tick or 0, _panic and "(PANIC)" or "", best_cx, best_cy, best_tier, cost, _thr_mx, _thr_my, (closest_et and closest_dist or -1)))
        -- Build-gate urgency for the panic drop. This return happens BEFORE the
        -- portfolio block below runs, so there is no pf_max_deficit to pass —
        -- deficit 0 is the honest answer here, and the flat emergency term is
        -- what actually buys the raise. Without this the path that most needs a
        -- raised gate was the one path getting urgency 0: in the 9k-tick check,
        -- 24 of 25 PLACE_PILL_GATE lines came from here, all reading urgency{0}
        -- while the tank sat at 10 armour holding 2 pills.
        local _du, _duc, _dud, _due =
          builder.place_urgency(info.carried_pills, 0, true)
        return {
          cost = cost,
          -- _place_forced: this is the threat-reactive "build while fighting"
          -- drop — exempt from the "place must lose to attack_tank" rule, and
          -- the flag builder.set_mode reads for the PLACE_EMERGENCY_MAX_DIST
          -- dispatch relaxation.
          goal = { kind = "place_pill_strategic", mx = best_cx, my = best_cy,
                   wx = U.m2w(best_cx), wy = U.m2w(best_cy), _place_forced = true,
                   _urgency = _du, _urg_carry = _duc, _urg_deficit = _dud,
                   _urg_emerg = _due },
          desc = BRAIN_POOL_VIZ and string.format("offensive_build@(%d,%d) cost=%.0f thr@(%d,%d) (A*{%.0f}+base{%.0f}-carry{%.0f})*%.2f",
                 best_cx, best_cy, cost, _thr_mx, _thr_my,
                 path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_discount, C.STRATEGIC_PLACE_COST_MULT) or "",
          cands = cands,
        }
      end
      print2(string.format("OFF_BUILD t=%d NO SPOT%s — all candidates rejected (forest/water/wall/unplaceable) threat=(%d,%d) d=%.1f", state.tick or 0, _panic and "(PANIC)" or "", _thr_mx, _thr_my, (closest_et and closest_dist or -1)))
    end
  end

  -- Don't interrupt active combat goals (normal strategic only — defensive
  -- build above is allowed to preempt since it's directly threat-reactive).
  if gk == "attack_pill" or gk == "pill_place" then return nil end

  -- ── Strategic-bias center (priority chain) ──────────────────────────────
  -- This is no longer the SEARCH center — the scan below is tank-centric (we
  -- look around our own position for the best placeable spot). The center
  -- picked here is only a soft BIAS: spots near it score higher. It's optional;
  -- if no base exists (early game / all bases lost) it stays nil and placement
  -- is driven purely by per-tile quality + portfolio. There is deliberately NO
  -- fallback path — if the tank-centric scan finds nothing placeable, we keep
  -- carrying (return nil) rather than dumping a pill somewhere worse.
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
    if hp_mx and fbx then
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
    if pua and fbx then
      search_mx = math.floor((fbx + pua.mx) / 2 + 0.5)
      search_my = math.floor((fby + pua.my) / 2 + 0.5)
      search_reason = "reinforce"
    end
  end

  -- 5. Default bias: nearest friendly base (pure defense, esp. early game)
  if not search_mx and fbx then
    search_mx, search_my = fbx, fby
    search_reason = "base_defense"
  end

  -- ── Scoring grid ────────────────────────────────────────────────────────
  -- Capacity tier place_r: cap the heatmap search radius. (2R+1)² tiles
  -- get scored, so halving R quarters the work.
  -- R is finalised just below, once the portfolio deficit (pf_need_cat) is
  -- known — an aggro build widens the scan. The CPU/capacity clamp is applied
  -- after that widening so a constrained tick still bounds the work.
  local R = C.STRATEGIC_PLACE_SEARCH_RADIUS
  local best_score = -math.huge
  local best_mx, best_my = nil, nil
  local all_cands = {}

  -- Portfolio state: classify existing friendly pills and compute targets for
  -- the projected total (current + the one we're about to place). Candidate
  -- tiles in an under-target category get a strong bonus so placement fills
  -- the deficit role (35% back / 45% front / 20% aggressive, >=1 back).
  local pf_counts  = PP.counts(world, state.tick)
  local pf_total   = pf_counts.back + pf_counts.front + pf_counts.aggro
  -- Project the back/front/aggro targets over the pills we actually have to
  -- place (built + THIS tank's carried hoard), not just +1. A category is
  -- buildable while its built count is under its projected target (N < T), so a
  -- hoard can fill back/front/aggro up to the 35/45/20 ratio instead of being
  -- carried forever when every category was "full" at the old +1 projection.
  local pf_place_n = math.max(1, info.carried_pills or 1)
  local pf_targets = PP.targets(pf_total + pf_place_n)
  -- Biggest category deficit → placement is cheaper (more urgent) when our
  -- pill types are out of ratio, and 0 when balanced.
  local pf_max_deficit = 0
  local pf_need_cat = nil   -- the single most-needed type (biggest deficit)
  -- Order = tie-break priority: front, then back, then aggro (strict > keeps
  -- the first-listed category when deficits are equal).
  for _, cat in ipairs({ "front", "back", "aggro" }) do
    local d = (pf_targets[cat] or 0) - (pf_counts[cat] or 0)
    if d > pf_max_deficit then pf_max_deficit = d; pf_need_cat = cat end
  end
  state._place_need_cat = pf_need_cat   -- shared with the heatmap viz

  -- LGM build-gate urgency, hung on the goal below as _urgency and read by
  -- builder.decide()'s place_pill branch. The inputs are the ones already
  -- computed here to DISCOUNT the goal's cost — carried pills (multi_carry_mult)
  -- and pf_max_deficit (imbalance_mult) — because they say the same thing about
  -- the BUILD as they do about the choice: this pill needs to be in the ground.
  -- Without it the gate is a fixed LGM_DANGER_HIGH (80) that a single predicted
  -- shell path (DANGER_SHELL_IMPACT = 100) closes for good, so the tank most in
  -- need of a guard pill is the one that can never place it. Not an emergency:
  -- this is the routine, chosen-from-the-pool placement, so it gets no
  -- emergency term (see the LGM_GATE_URGENCY_* comment in constants.lua).
  -- Two clamped multiplies of integers we already have: no extra scanning, O(1).
  -- NOT the same thing as state._place_urgency further down — that one is a
  -- 0..1 scalar from carry TIME that only widens the search radius. This is a
  -- danger-threshold raise in danger_at units.
  local place_urgency, urg_carry, urg_deficit, urg_emerg =
      builder.place_urgency(info.carried_pills, pf_max_deficit, false)

  -- Aggro builds sit deeper in enemy influence than the default radius reaches:
  -- the scan is tank-centric and the tank usually sits behind the front, so a
  -- good aggro tile (negative influence, beyond the line) can be >8 tiles out.
  -- When aggro is the role we're filling, widen the scan to find + place one up
  -- to AGGRO_SEARCH_RADIUS tiles away. Capacity clamp applied AFTER so a CPU-
  -- constrained tick still bounds the (2R+1)^2 cell count.
  if pf_need_cat == "aggro" and (C.STRATEGIC_PLACE_AGGRO_SEARCH_RADIUS or 0) > R then
    R = C.STRATEGIC_PLACE_AGGRO_SEARCH_RADIUS
  end
  -- Build-urgency range widening: the longer we've carried and/or the more
  -- pills in THIS tank, the further the scan reaches for a good spot of the
  -- needed type — a desperate builder shouldn't wait for a great tile to
  -- appear inside the default bubble. urgency 0..1 from time carried (same
  -- normalization as carry_discount: ticks × PER_TICK / MAX) and multi-carry
  -- ((carried-1)/2, so 2 pills → 0.5, 3+ → 1.0); extra tiles =
  -- floor(urgency × URGENCY_RANGE_BONUS). Applied BEFORE the capacity clamp
  -- so a CPU-constrained tick still bounds the (2R+1)^2 cell count.
  do
    local u_time = 0
    if state.carrying_pill_since then
      local tc = (state.tick or 0) - state.carrying_pill_since
      u_time = math.min(1.0, math.max(0, tc * (C.STRATEGIC_PLACE_CARRY_DISCOUNT_PER_TICK or 0.5))
                             / (C.STRATEGIC_PLACE_CARRY_DISCOUNT_MAX or 300))
    end
    local u_carry = math.min(1.0, math.max(0, (info.carried_pills or 0) - 1) / 2)
    local urgency = math.max(u_time, u_carry)
    if urgency > 0 then
      R = R + math.floor(urgency * (C.STRATEGIC_PLACE_URGENCY_RANGE_BONUS or 6))
      state._place_urgency = urgency  -- viz/debug hint
    else
      state._place_urgency = nil
    end
  end
  if state._capacity and state._capacity.place_r and state._capacity.place_r < R then
    R = state._capacity.place_r
  end

  -- Util-reserve guard: a pill in our tank counts as "utility" (PP.counts
  -- treats in_tank + in_use pills as util). The utility reserve is for pill
  -- takes / blocking, NOT strategic deployment, and util is the highest-
  -- priority role — so never strategically place while the team is at or below
  -- its utility reserve. Only deploy a pill once we hold MORE util pills than
  -- the reserve target (a genuine surplus). (Emergency offensive_build returned
  -- earlier, so it's exempt — a dying-base drop still happens.)
  -- Util reserve = the portfolio's utility target, computed EXACTLY as the
  -- pill-table viz does (PP.targets over back+front+aggro+utility) so the gate
  -- and the displayed target always agree. (Was floor(pf_total*0.15) which
  -- EXCLUDED utility from the total and could disagree with the viz.)
  local pf_all       = pf_total + (pf_counts.utility or 0)
  local util_reserve = PP.targets(pf_all).utility
  local util_surplus = (pf_counts.utility or 0) - util_reserve
  -- Multi-carry bypass: the reserve argument only justifies holding ONE pill
  -- in this tank — a bot carrying 2+ places its extras even while team util
  -- is at/below reserve (concentrated in one tank the reserve is fragile:
  -- one death loses all of it, and carried pills can't block takes). After
  -- placing down to 1 carried, the normal hold re-engages.
  if util_surplus <= 0 and (info.carried_pills or 0) < 2 then
    state._place_need_cat = "util_reserve"   -- viz hint
    print2(string.format("PLACE_HOLD_UTIL t=%d util=%d <= reserve=%d — hold carried pill as utility reserve",
      state.tick or 0, pf_counts.utility or 0, util_reserve))
    return nil
  end

  -- Base-guardian priority: every friendly base should have >=1 pill in
  -- shooting range. Precompute the friendly bases that currently have NONE,
  -- so a candidate covering one gets a big bonus (place the first guardian).
  local unguarded_bases = {}
  for _, b in pairs(world.bases) do
    if b.owner == "friendly"
       and count_pills_near(world, b.mx, b.my, C.PILL_FIRE_RANGE, "friendly") == 0 then
      unguarded_bases[#unguarded_bases + 1] = b
    end
  end

  -- Reposition-origin exclusion: never place near the hole a reposition just
  -- made. Picking the pill up CREATES the coverage gap (unguarded base, role
  -- deficit) that this scorer then top-ranks a fix for — without this the
  -- vacated tile re-wins and the pill gets rebuilt exactly where it stood
  -- (20260704_022107 t=5625, pill#3 @(137,117)). _repos_guard carries every
  -- in-flight/recent reposition tile (ours AND allies'), ~30s past the move.
  local function near_repos_origin(cx, cy)
    local g = state._repos_guard
    if not g then return false end
    local RG    = C.REPOS_PLACE_EXCLUDE_RADIUS or 5
    local now_t = state.tick or 0
    for gk, untl in pairs(g) do
      if untl > now_t then
        local gmx = gk % 256
        local gmy = (gk - gmx) / 256
        if math.abs(cx - gmx) <= RG and math.abs(cy - gmy) <= RG then
          return true
        end
      end
    end
    return false
  end

  -- Abandoned-spot exclusion. builder.decide() set_blocks a drop spot whose LGM
  -- build gate refused PLACE_GATE_FAIL_TICKS in a row, and pick_goal already
  -- drops any pool entry sitting on a blocked tile. Honour the block HERE too:
  -- without it the scan keeps electing the same abandoned tile as the winner,
  -- pick_goal throws the whole candidate away, and placement offers NOTHING for
  -- the block's duration instead of simply taking the next-best spot. One hash
  -- lookup per cell, hoisted out of the loop.
  local blocked_tiles = state.blocked
  local blk_now       = state.tick or 0
  local MAPW          = C.MAP_W

  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(tmx + dx)   -- TANK-centric: scan around our position
      local cy = U.mclamp(tmy + dy)
      if U.is_placeable(cx, cy, world) then
        if near_repos_origin(cx, cy) then goto skip_cell end
        if blocked_tiles then
          local _blk_until = blocked_tiles[cy * MAPW + cx]
          if _blk_until and blk_now < _blk_until then goto skip_cell end
        end
        -- Surplus skip: never overfill a category already at/over its projected
        -- target (e.g. another BACK pill when back is 2/1). Unlike the old hard
        -- "only the most-needed type" gate, any category with room is allowed;
        -- the portfolio-deficit bias (sc11) still steers toward the neediest
        -- one. If every placeable spot is a surplus category, best_mx stays nil
        -- and we keep carrying — there is no fallback.
        local cell_inf = cpf.influence_at(cx, cy)
        local cell_cat = PP.classify(cx, cy, false, true)
        local _ctgt = pf_targets[cell_cat]
        if _ctgt and (pf_counts[cell_cat] or 0) >= _ctgt then goto skip_cell end
        -- HARD balance gate (STRATEGIC_PLACE_STRICT_NEED): non-panic
        -- placement fills ONLY the single most-needed category. Merely
        -- being under target isn't enough — classification drifts with
        -- the front line (a "front" cell placed mid-firefight rereads
        -- as back once the wave recedes: 20260825_194741 bot11 t=444,
        -- back 5/1), so any slack here bleeds the portfolio out of
        -- balance. Panic/offensive_build/desperate drops bypass this scan
        -- entirely and stay exempt.
        if (C.STRATEGIC_PLACE_STRICT_NEED ~= false)
           and pf_need_cat and cell_cat ~= pf_need_cat then
          goto skip_cell
        end
        local score = 0
        local sc1, sc2, sc3, sc4, sc5, sc6, sc7, sc8, sc9, sc10, sc11, sc12, sc13 = 0,0,0,0,0,0,0,0,0,0,0,0,0
        local sc_center = 0

        -- 1. Base proximity (soft bonus only — no hard cutoff. A far spot just
        --    misses the bonus; it isn't rejected, so aggressive pills can still
        --    be placed deep. nil base_dist = no friendly base, contributes 0.)
        local _, _, base_dist = nearest_friendly_base_pos(world, cx, cy)
        if base_dist then
          sc1 = math.max(0, C.STRATEGIC_PLACE_MAX_BASE_DIST - base_dist) * C.STRATEGIC_PLACE_BASE_WEIGHT
          score = score + sc1
        end

        -- 1b. Strategic-center bias: nudge toward the chosen center (war zone /
        --     base-vs-threat / contested pill). Optional — 0 when no center.
        if search_mx then
          local cdist = U.mdist(cx, cy, search_mx, search_my)
          sc_center = math.max(0, (C.STRATEGIC_PLACE_CENTER_BIAS_CAP or 16) - cdist)
                      * (C.STRATEGIC_PLACE_CENTER_BIAS_WEIGHT or 4)
          score = score + sc_center
        end

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

        -- 4. Pill spacing: exponential clustering penalty inside the target gap,
        --    small bonus for a well-spaced-but-still-supporting spot. Aims for a
        --    >= PILL_SPACING-tile gap between friendly pills (see constants).
        do
          local s0 = score
          local pill_dist = nearest_friendly_pill_dist(world, cx, cy)
          if pill_dist < C.STRATEGIC_PLACE_PILL_SPACING then
            local deficit = C.STRATEGIC_PLACE_PILL_SPACING - pill_dist
            local pen = C.STRATEGIC_PLACE_PILL_PENALTY_W
                      * (C.STRATEGIC_PLACE_PILL_PENALTY_BASE ^ deficit - 1)
            score = score - math.min(C.STRATEGIC_PLACE_PILL_PENALTY_CAP, pen)
          elseif pill_dist <= C.STRATEGIC_PLACE_SPACING_BONUS_MAX then
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

        -- 11. Portfolio deficit bias: push placement toward the under-target
        --     category (back/front/aggressive). Strong weight so a deficit
        --     role gets filled — and so aggressive placement can overcome the
        --     beyond-front penalty (sc3) when aggro is short.
        do
          local deficit = (pf_targets[cell_cat] or 0) - (pf_counts[cell_cat] or 0)
          sc11 = deficit * C.STRATEGIC_PLACE_PORTFOLIO_WEIGHT
          score = score + sc11
        end

        -- 12. Protective coverage: how many friendly pills (+ bases) this spot
        --     covers within fire range — the key "good back protector" signal.
        do
          local pills_cov = count_pills_near(world, cx, cy, C.PILL_FIRE_RANGE, "friendly")
          local bases_cov = count_friendly_bases_near(world, cx, cy, C.PILL_FIRE_RANGE)
          sc12 = pills_cov * C.STRATEGIC_PLACE_COVERAGE_PILL_WEIGHT
               + bases_cov * C.STRATEGIC_PLACE_COVERAGE_BASE_WEIGHT
          score = score + sc12
        end

        -- 13. Base-guardian: every friendly base should have >=1 pill in
        --     shooting range. Big bonus per currently-unguarded base this spot
        --     would cover — top placement priority.
        --     SURPLUS CAP: guardian coverage outranks portfolio balance,
        --     but not without limit — once the spot's category already
        --     holds >= CAP x its target, the bonus is withheld so a
        --     many-based team doesn't guard-place the same category
        --     forever (20260825_194741 bot11 t=444: back pill #5 at 4/1
        --     to guard one of the wave's ten bases). Modest surplus
        --     still guards naked bases.
        if #unguarded_bases > 0 then
          local gcap = C.STRATEGIC_PLACE_GUARDIAN_SURPLUS_CAP or 3
          local gat  = pf_counts[cell_cat] or 0
          local gtgt = pf_targets[cell_cat] or 0
          if gtgt < 1 then gtgt = 1 end
          if gat < gtgt * gcap then
            for _, ub in ipairs(unguarded_bases) do
              if U.mdist(cx, cy, ub.mx, ub.my) <= C.PILL_FIRE_RANGE then
                sc13 = sc13 + C.STRATEGIC_PLACE_GUARDIAN_BONUS
              end
            end
            score = score + sc13
          end
        end

        all_cands[#all_cands + 1] = {
          mx = cx, my = cy, score = score,
          inf = cell_inf,
          cat = cell_cat,
          sc1=sc1, sc2=sc2, sc3=sc3, sc4=sc4, sc5=sc5,
          sc6=sc6, sc7=sc7, sc8=sc8, sc9=sc9, sc10=sc10,
          sc11=sc11, sc12=sc12, sc13=sc13, sc_center=sc_center,
        }
        if score > best_score then
          best_score = score; best_mx = cx; best_my = cy
        end
      end
      ::skip_cell::
    end
  end

  if not best_mx then
    -- No placeable, non-surplus spot anywhere in range. The right move is to
    -- KEEP CARRYING (return nil) until we're somewhere a needed pill belongs —
    -- there is intentionally no fallback that would dump a surplus pill nearby.
    print2(string.format("PLACE_NO_SPOT t=%d center=(%d,%d) R=%d util_surplus=%d need=%s counts(b/f/a)=%d/%d/%d targets=%d/%d/%d — every placeable tile unplaceable or surplus-skipped; keep carrying",
      state.tick or 0, search_mx or -1, search_my or -1, R, util_surplus or 0, tostring(pf_need_cat),
      pf_counts.back or 0, pf_counts.front or 0, pf_counts.aggro or 0,
      pf_targets.back or 0, pf_targets.front or 0, pf_targets.aggro or 0))
    return nil
  end

  -- Spot-quality floor: being EAGER to deploy (cheap cost from the carry/util-
  -- surplus discounts) must NOT lower the bar for WHERE the pill goes. The cost
  -- discounts only decide whether placement wins the goal competition; this floor
  -- guards the SPOT. A well-placed pill scores 250-400 (role-fill 120 + guardian
  -- 150 + positioning); a deficit-role spot at a poor position can sit near ~120
  -- or below. Below the floor we KEEP CARRYING until a genuinely good spot exists.
  -- Bypassed only when placement is urgent (naked base under fire / about to die).
  -- Effective floor relaxes as the team hoards util pills over the reserve — a
  -- big pile in tanks is itself a problem, so accept a less-perfect spot rather
  -- than carry forever. Never drops below FLOOR_MIN (exposed/purposeless spots
  -- stay held regardless of hoard size).
  local eff_min = math.max(C.STRATEGIC_PLACE_MIN_SCORE_FLOOR or 0,
                           (C.STRATEGIC_PLACE_MIN_SCORE or 0)
                           - math.max(0, util_surplus) * (C.STRATEGIC_PLACE_MIN_SCORE_SURPLUS_DROP or 0))
  if not place_urgent and best_score < eff_min and not viz_only then
    print2(string.format("PLACE_HOLD_LOWSCORE t=%d best=(%d,%d) score=%.0f < eff_min=%.0f (base=%d surplus=%d) — keep carrying (no good spot yet)",
      state.tick or 0, best_mx, best_my, best_score, eff_min, C.STRATEGIC_PLACE_MIN_SCORE or 0, util_surplus or 0))
    return nil
  end

  local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_mx, best_my, boat and 1 or 0,
                                info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
  local raw_cost = path_cost + C.STRATEGIC_PLACE_BASE_COST + carry_value_penalty - carry_discount
  -- Last pill: 1.5× cost so the bot holds on to its only pill — but ONLY while
  -- we're short on blocker/utility pills (so a spare can be dropped as a
  -- blocker for a take). Once we have enough utility pills, don't hoard — place
  -- the spare normally.
  local last_pill_mult = 1.0
  if (info.carried_pills or 0) == 1 and (pf_counts.utility or 0) < util_reserve then
    -- (Reuses the same util_reserve as the gate above so all three util numbers
    -- agree. In practice the gate already guarantees utility > util_reserve here,
    -- so this hoard branch is a belt-and-braces guard.)
    last_pill_mult = 1.5
  end
  local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT * last_pill_mult)
  -- Out-of-ratio discount: cheaper (more urgent) to place when a pill type is
  -- in deficit; no change when balanced. Capped so it never goes free.
  local imbalance_mult = 1.0
  if pf_max_deficit > 0 then
    imbalance_mult = 1.0 - math.min(C.STRATEGIC_PLACE_IMBALANCE_MAX_DISCOUNT,
                                    pf_max_deficit * C.STRATEGIC_PLACE_IMBALANCE_DISCOUNT)
    cost = math.max(1, cost * imbalance_mult)
  end
  -- Util-surplus discount: the more pills we hold OVER the utility reserve, the
  -- cheaper it is to deploy one — actively push the surplus out of tanks instead
  -- of just unlocking placement at normal cost. Each surplus pill knocks off
  -- STRATEGIC_PLACE_UTIL_SURPLUS_DISCOUNT, capped at _MAX_DISCOUNT.
  local surplus_mult = 1.0
  if util_surplus > 0 then
    surplus_mult = 1.0 - math.min(C.STRATEGIC_PLACE_UTIL_SURPLUS_MAX_DISCOUNT or 0.6,
                                  util_surplus * (C.STRATEGIC_PLACE_UTIL_SURPLUS_DISCOUNT or 0.25))
    cost = math.max(1, cost * surplus_mult)
  end
  -- Per-TANK multi-carry discount: each pill THIS tank holds beyond the
  -- first knocks off MULTI_CARRY_DISCOUNT (capped). Separate from the
  -- team-wide surplus above — a triple-carrier should shed its extras even
  -- when the team total looks fine (fragile: one death loses them all, and
  -- carried pills can't block takes).
  local multi_carry_mult = 1.0
  local mc_extra = math.max(0, (info.carried_pills or 0) - 1)
  if mc_extra > 0 then
    multi_carry_mult = 1.0 - math.min(C.STRATEGIC_PLACE_MULTI_CARRY_MAX_DISCOUNT or 0.5,
                                      mc_extra * (C.STRATEGIC_PLACE_MULTI_CARRY_DISCOUNT or 0.25))
    cost = math.max(1, cost * multi_carry_mult)
  end
  -- Combat-zone penalty: enemy tank near the chosen spot (flat add, shown as the
  -- tankpen term). Emergency offensive_build is exempt — it returns earlier.
  local tank_pen = place_near_tank_penalty(state, best_mx, best_my)
  cost = cost + tank_pen

  -- Build pool-grid candidate list: winner gets actual cost, others get cost + score delta.
  -- Pool-grid panel data only — wrapped so lua_strip removes it from opt/.
  local cands = {}
  if BRAIN_DEBUG_MODE then
    table.sort(all_cands, function(a, b) return a.score > b.score end)
    -- Stash the top back / aggro candidate spots for the map overlays
    -- (pill_best_spots_back = orange, pill_best_spots_aggro = red).
    -- Highest-scoring first.
    do
      local sb, sa = {}, {}
      for _, c in ipairs(all_cands) do
        -- Overlay only shows strongly-positioned spots: back = deep in our
        -- influence (>= 50), aggro = deep in enemy influence (< -50).
        if c.cat == "back"  and (c.inf or 0) >=  50 and #sb < 8 then sb[#sb + 1] = { mx = c.mx, my = c.my } end
        if c.cat == "aggro" and (c.inf or 0) <  -50 and #sa < 8 then sa[#sa + 1] = { mx = c.mx, my = c.my } end
        if #sb >= 8 and #sa >= 8 then break end
      end
      state._place_spots_back  = sb
      state._place_spots_aggro = sa
    end
    for _, c in ipairs(all_cands) do
      local is_win = (c.mx == best_mx and c.my == best_my)
      local cand_cost = is_win and cost or math.max(0.01, cost + (best_score - c.score))
      local fmt = string.format(
        "score{%.0f} = prx{%.0f} + bdef{%.0f} + inf{%.0f} + spc{%.0f} + los{%.0f} + thr{%.0f} + dst{%.0f} + spk{%.0f} + ep{%.0f} + wz{%.0f} + port{%.0f} + cov{%.0f} + grd{%.0f} + ctr{%.0f}%s",
        c.score, c.sc1, c.sc2, c.sc3, c.sc4, c.sc5, c.sc6, c.sc7, c.sc8, c.sc9, c.sc10,
        c.sc11 or 0, c.sc12 or 0, c.sc13 or 0, c.sc_center or 0,
        is_win and string.format("  ||  cost{%.0f} = (path{%.0f} + base{%.0f} + carry_pen{%.0f} - carry{%.0f}) x mult{%.2f} x lastpill{%.2f} x bal{%.2f} x surplus{%.2f} x multi{%.2f} + tankpen{%.0f}",
          cost, path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_value_penalty, carry_discount,
          C.STRATEGIC_PLACE_COST_MULT, last_pill_mult, imbalance_mult, surplus_mult, multi_carry_mult, tank_pen) or "")
      cands[#cands + 1] = {
        id = c.my * 256 + c.mx,
        mx = c.mx, my = c.my,
        cost = cand_cost,
        formula = fmt,
        stale = 0,
        reject_remaining = 0,
      }
    end
  end

  -- viz_only (debug overlay): scan + spot stash done above; emit no goal.
  if not actionable then return nil end

  return {
    cost = cost,
    -- _urgency: how badly this pill wants to be in the ground — builder.decide()
    -- raises the LGM danger gate by it. The three _urg_* components ride along
    -- purely so the PLACE_PILL_GATE log line prints every term and the threshold
    -- stays hand-computable from that one line.
    goal = { kind = "place_pill_strategic", mx = best_mx, my = best_my,
             wx = U.m2w(best_mx), wy = U.m2w(best_my),
             _urgency = place_urgency, _urg_carry = urg_carry,
             _urg_deficit = urg_deficit, _urg_emerg = urg_emerg },
    -- Every multiplier that actually shapes `cost` has to appear here — this
    -- desc is what FINAL_SCORES prints, and lastpill/surplus/multi were missing,
    -- so the printed formula did not reproduce the printed number. Order matches
    -- the code above: (path + base + carry_pen - carry) x mult x lastpill, then
    -- x bal x surplus x multi, then + tankpen.
    desc = BRAIN_POOL_VIZ and string.format("(A*{%.0f}+base{%.0f}+carry_pen{%.0f}-carry{%.0f})*mult{%.2f}*lastpill{%.2f}*bal{%.2f}*surplus{%.2f}*multi{%.2f}+tankpen{%.0f} = cost{%.1f} urgency{%d} center=%s score=%.0f | balance back %d/%d front %d/%d aggro %d/%d unguarded=%d",
           path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_value_penalty, carry_discount,
           C.STRATEGIC_PLACE_COST_MULT, last_pill_mult, imbalance_mult, surplus_mult,
           multi_carry_mult, tank_pen, cost, place_urgency, search_reason, best_score,
           pf_counts.back, pf_targets.back, pf_counts.front, pf_targets.front,
           pf_counts.aggro, pf_targets.aggro, #unguarded_bases) or "",
    cands = cands,
  }
end

-- Map overlay: the best evaluated placement spots by category — orange for a
-- "back" pill, purple for a "front" pill (top-scoring first, brightest = best).
-- Fed by the candidate scan in eval_place_pill_strategic (state._place_spots_*).
function M.draw_pill_spots(viz, state)
  if not viz or not viz.is_on or not viz.rect or not state then return end
  local function draw(id, list, r, g, b)
    if not viz.is_on(id) or not list then return end
    for i, s in ipairs(list) do
      -- Bold filled translucent square so it stands out; best spot most opaque.
      local a = (i == 1) and 170 or 90
      viz.rect(id, s.mx, s.my, s.mx + 1, s.my + 1, r, g, b, a, true)
    end
  end
  draw("pill_best_spots_back",  state._place_spots_back,  255, 150,  0)  -- orange = back
  draw("pill_best_spots_aggro", state._place_spots_aggro, 255,  70,  70) -- red = aggro

  -- Danger-aware wait_for_lgm spot: scored candidate ring (yellow=safe,
  -- red=dangerous, grey=unreachable, green=chosen), the chosen wait tile,
  -- a line to the returning LGM, and the danger that triggered the move.
  if viz.is_on("wait_spot") and state._wait_spot_viz
     and state.goal and state.goal.kind == "wait_for_lgm" then
    local v = state._wait_spot_viz
    if (state.tick or 0) - (v.tick or 0) <= 120 then
      for _, c in ipairs(v.cands or {}) do
        local r, g, b = 235, 220, 70
        if c.rej == "danger" then r, g, b = 230, 70, 70
        elseif c.rej == "unreachable" then r, g, b = 120, 120, 120 end
        if c.mx == v.wmx and c.my == v.wmy then r, g, b = 60, 230, 60 end
        viz.rect("wait_spot", c.mx, c.my, c.mx + 1, c.my + 1, r, g, b, 80, true)
      end
      viz.circle("wait_spot", v.wmx + 0.5, v.wmy + 0.5, 0.6, 60, 230, 60, 230, false, false)
      viz.line("wait_spot", v.wmx + 0.5, v.wmy + 0.5, v.lmx + 0.5, v.lmy + 0.5, 120, 255, 160, 200)
      viz.text("wait_spot", v.wmx + 0.5, v.wmy - 0.7,
               string.format("WAIT SPOT (danger@tank=%.0f)", v.d_here or 0), "center", 120, 255, 160, 255)
    end
  end

  -- Spiking pills: hostile/neutral pill within firing range of a friendly
  -- base (denies refuel). Magenta square on the pill, a line to EACH denied
  -- base, and a "SPIKE n=N" label. Data from refresh_spike_detection (goal
  -- scoring shares the same table, so what you see is what the cost used).
  if viz.is_on("spike_pills") and state._spike_pills then
    for _, sp in pairs(state._spike_pills) do
      viz.rect("spike_pills", sp.pmx, sp.pmy, sp.pmx + 1, sp.pmy + 1, 255, 40, 200, 150, true)
      if viz.line and sp.bases then
        for _, b in ipairs(sp.bases) do
          viz.line("spike_pills", sp.pmx + 0.5, sp.pmy + 0.5, b.mx + 0.5, b.my + 0.5, 255, 40, 200, 200)
          viz.rect("spike_pills", b.mx, b.my, b.mx + 1, b.my + 1, 255, 40, 200, 80, true)
        end
      end
      if viz.text then
        local lbl = (sp.cover or 1) > 1
          and string.format("SPIKE n=%d (shared x%d)", sp.n or 1, sp.cover)
          or  string.format("SPIKE n=%d", sp.n or 1)
        viz.text("spike_pills", sp.pmx + 0.5, sp.pmy - 0.6, lbl, "center", 255, 120, 230, 255)
      end
    end
  end

  -- Panic (emergency offensive_build) overlay: shown whenever a non-rejected enemy
  -- tank is present (attack_tank viable). Draws the emergency build candidate
  -- spots (green=chosen, yellow=valid, red=rejected w/ reason), the threat tank,
  -- a line from us to it, and a "PANIC BUILD" label. Visible only while the
  -- offensive_build eval ran this/last tick (carrying a pill); if the tank is present
  -- but we have no pill, just a "PANIC (no pill)" marker on the threat.
  if viz.is_on("panic_build") then
    local v = state._guard_build_viz
    local now = state.tick or 0
    if v and (now - (v.tick or 0)) <= 2 then
      for _, c in ipairs(v.spots or {}) do
        local is_win = v.best_cx and c.mx == v.best_cx and c.my == v.best_cy
        local r, g, b = 230, 70, 70                      -- red = rejected
        if is_win then r, g, b = 60, 230, 60             -- green = chosen
        elseif c.tier then r, g, b = 230, 220, 70 end    -- yellow = valid, not best
        viz.rect("panic_build", c.mx, c.my, c.mx + 1, c.my + 1, r, g, b, is_win and 170 or 90, true)
        if c.rej and viz.text then
          viz.text("panic_build", c.mx + 0.5, c.my + 0.5, c.rej, "center", 255, 200, 200, 200, 0.35)
        end
      end
      if viz.line then viz.line("panic_build", v.tank_mx + 0.5, v.tank_my + 0.5, v.threat_mx + 0.5, v.threat_my + 0.5, 255, 80, 80, 200) end
      viz.rect("panic_build", v.threat_mx, v.threat_my, v.threat_mx + 1, v.threat_my + 1, 255, 0, 0, 130, true)
      -- Closeness gauge: how near the threat tank is (proxy for panic urgency).
      -- A 5-segment bar above the tank fills as the enemy closes; label shows the
      -- tile distance + percent. Uses the threat position already stashed above.
      local td    = U.mdist(v.tank_mx, v.tank_my, v.threat_mx, v.threat_my)
      local near  = C.PANIC_BUILD_NEAR_TILES or 12
      local close = math.max(0, math.min(1, 1 - td / near))
      local segs  = math.floor(close * 5 + 0.5)
      for i = 0, 4 do
        local on = i < segs
        viz.rect("panic_build", v.tank_mx - 2 + i + 0.1, v.tank_my - 1.55, v.tank_mx - 2 + i + 0.9, v.tank_my - 1.15,
                 on and 255 or 70, on and (230 - math.floor(170 * close)) or 70, 60, on and 235 or 110, true)
      end
      if viz.text then viz.text("panic_build", v.tank_mx + 0.5, v.tank_my - 2.0, string.format("PANIC BUILD  %dt %d%%", td, math.floor(close * 100)), "center", 255, 80, 80, 255) end
    elseif state._attack_tank_present and state._attack_tank_threat and viz.text then
      local t = state._attack_tank_threat
      viz.rect("panic_build", t.mx, t.my, t.mx + 1, t.my + 1, 255, 0, 0, 130, true)
      -- This marker means "an enemy tank is present but offensive_build produced no
      -- plan this tick" — which is NOT necessarily "no pill". Only say no-pill
      -- when we actually have none; otherwise it's threat-out-of-panic-range (the
      -- offensive_build distance gate) or the panic eval just wasn't the active one.
      local _plabel = t.have_pill
        and string.format("THREAT d=%d (no panic build)", t.dist or -1)
        or "PANIC (no pill)"
      viz.text("panic_build", t.mx + 0.5, t.my - 1.0, _plabel, "center", 255, 120, 120, 255)
    end
  end
end

-- =========================================================================
-- Strategic placement heatmap (for BrainTest visualization key 8)
-- Returns a string: "CENTER\tmx\tmy\tR\treason\n" then "mx\tmy\tscore\n" per tile.
-- Uses the same search center + scoring as eval_place_pill_strategic.
-- =========================================================================
function M.get_strategic_place_heatmap(state, world, info)
  if not info then return nil end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)

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

  -- Mirror eval_place_pill_strategic exactly so the heatmap == the real
  -- candidate set: TANK-centric scan, skip-surplus categories, soft base
  -- proximity, strategic-center bias, and the portfolio/coverage/guardian
  -- terms. (Kept in sync by hand — if you change the eval scan, change this.)
  local pf_counts  = PP.counts(world, state.tick)
  local pf_total   = pf_counts.back + pf_counts.front + pf_counts.aggro
  -- Project the back/front/aggro targets over the pills we actually have to
  -- place (built + THIS tank's carried hoard), not just +1. A category is
  -- buildable while its built count is under its projected target (N < T), so a
  -- hoard can fill back/front/aggro up to the 35/45/20 ratio instead of being
  -- carried forever when every category was "full" at the old +1 projection.
  local pf_place_n = math.max(1, info.carried_pills or 1)
  local pf_targets = PP.targets(pf_total + pf_place_n)
  local unguarded_bases = {}
  for _, b in pairs(world.bases) do
    if b.owner == "friendly"
       and count_pills_near(world, b.mx, b.my, C.PILL_FIRE_RANGE, "friendly") == 0 then
      unguarded_bases[#unguarded_bases + 1] = b
    end
  end

  for dy = -R, R do
    for dx = -R, R do
      local cx = U.mclamp(tmx + dx)   -- TANK-centric (matches the eval scan)
      local cy = U.mclamp(tmy + dy)
      if U.is_placeable(cx, cy, world) then
        -- Skip a category already at/over its projected target (no room).
        local cell_cat = PP.classify(cx, cy, false, true)
        local _ctgt = pf_targets[cell_cat]
        if _ctgt and (pf_counts[cell_cat] or 0) >= _ctgt then goto skip_hm end
        local score = 0

        -- Strategic-center bias.
        if search_mx then
          score = score + math.max(0, (C.STRATEGIC_PLACE_CENTER_BIAS_CAP or 16)
                                       - U.mdist(cx, cy, search_mx, search_my))
                         * (C.STRATEGIC_PLACE_CENTER_BIAS_WEIGHT or 4)
        end

        local _, _, base_dist = nearest_friendly_base_pos(world, cx, cy)
        if base_dist then
          score = score + math.max(0, C.STRATEGIC_PLACE_MAX_BASE_DIST - base_dist) * C.STRATEGIC_PLACE_BASE_WEIGHT
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
          if pd < C.STRATEGIC_PLACE_PILL_SPACING then
            local deficit = C.STRATEGIC_PLACE_PILL_SPACING - pd
            local pen = C.STRATEGIC_PLACE_PILL_PENALTY_W
                      * (C.STRATEGIC_PLACE_PILL_PENALTY_BASE ^ deficit - 1)
            score = score - math.min(C.STRATEGIC_PLACE_PILL_PENALTY_CAP, pen)
          elseif pd <= C.STRATEGIC_PLACE_SPACING_BONUS_MAX then
            score = score + C.STRATEGIC_PLACE_SPACING_BONUS
          end
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

        -- 11. Portfolio deficit bias (push toward the under-target category).
        score = score + ((pf_targets[cell_cat] or 0) - (pf_counts[cell_cat] or 0))
                        * C.STRATEGIC_PLACE_PORTFOLIO_WEIGHT
        -- 12. Protective coverage (friendly pills + bases in fire range).
        score = score + count_pills_near(world, cx, cy, C.PILL_FIRE_RANGE, "friendly") * C.STRATEGIC_PLACE_COVERAGE_PILL_WEIGHT
                      + count_friendly_bases_near(world, cx, cy, C.PILL_FIRE_RANGE) * C.STRATEGIC_PLACE_COVERAGE_BASE_WEIGHT
        -- 13. Base-guardian (cover a currently-unguarded friendly base).
        for _, ub in ipairs(unguarded_bases) do
          if U.mdist(cx, cy, ub.mx, ub.my) <= C.PILL_FIRE_RANGE then
            score = score + C.STRATEGIC_PLACE_GUARDIAN_BONUS
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
-- Travel cost to REACH a (live) pill. A live pillbox tile is impassable in the
-- cost surface (you can't drive onto it), so smart_cost to the pill tile itself
-- returns COST_INF and wrongly makes the pill look infinitely expensive. We
-- can't stand on it anyway — cost to the cheapest passable NEIGHBOUR instead,
-- using Dijkstra-slate lookups only (no A* fallback). Returns math.huge if the
-- pill is genuinely unreachable (then the caller simply won't pick it).
-- Shared by eval_defend_pill and eval_reposition_pill.
-- =========================================================================
local function travel_cost_to_pill(pmx, pmy, boat)
  local bf   = boat and 1 or 0
  local best = math.huge
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        local c = cpf.smart_cost_dij_only(KIND_NORMAL, pmx + dx, pmy + dy, bf)
        if c and c < best then best = c end
      end
    end
  end
  return best
end

-- =========================================================================
-- Defend pill: respond to sustained attacks on friendly pills
-- =========================================================================
-- Travel cost to REACH a (live) pill. A live pillbox tile is impassable in the
-- cost surface (you can't drive onto it), so a path to the pill tile itself
-- returns COST_INF / forces an A* detour. We can't stand on it anyway — cost to
-- the cheapest passable NEIGHBOUR instead, using Dijkstra-slate lookups only
-- (no A* fallback). Returns math.huge if genuinely unreachable. Shared by
-- defend_pill and reposition. (Defined here so eval_defend_pill can use it.)
local function travel_cost_to_pill(pmx, pmy, boat)
  local bf   = boat and 1 or 0
  local best = math.huge
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        local c = cpf.smart_cost_dij_only(KIND_NORMAL, pmx + dx, pmy + dy, bf)
        if c and c < best then best = c end
      end
    end
  end
  return best
end

-- defend_pill_score — the defend formula for one built team pill.
--
--   live tier : cost = max(FLOOR, (base + travel) * threat_mult
--                            * lateness * readiness)
--   quiet     : cost = (DEFEND_QUIET_DMG_COST[hits taken] + travel)
--                            * readiness      -- no base, no threat_mult
--   both then pass the well-defended clamp + flat-cost tiebreaker.
--
-- Design invariant: threat evidence only ever LOWERS the cost, and
-- infeasibility only ever RAISES it — degraded information degrades
-- gracefully instead of flipping behavior. The urgency ladder (tiers
-- stack; each traces to a live signal):
--   siege : last_hit_tick fresh (DEFEND_DMG_FRESH_TICKS) — someone is
--           shelling the pill NOW. Flat SIEGE_URGENCY plus cumulative
--           attack_damage x DMG_URGENCY (depth of the bite, NOT a
--           damage-rate estimate — attacks are bursty: line up, volley,
--           swerve — so no rate math anywhere).
--   setup : hostile LGM seen within DEFEND_LGM_NEAR_RADIUS recently
--           (perception stamp _lgm_near_tick) — wall-shield / pill-plant
--           prep. Fires BEFORE damage exists; interrupting setup is the
--           cheapest defense, hence a siege-sized discount. Linear decay
--           over DEFEND_SIGHT_FRESH_TICKS.
--   sight : hostile tank seen within DEFEND_ENEMY_NEAR_RADIUS recently
--           (_enemy_near_tick) — prevention tier, same linear decay.
--   cover : other alive team pills whose fire reaches this pill make the
--           defense cheaper (arrive into friendly cover; heat-up fodder).
--           Threat-gated: coverage alone is no reason to drive anywhere.
--   quiet : no evidence at all -> the DEFEND_QUIET_DMG_COST curve on
--           HITS TAKEN (PILLS_MAX_HEALTH - health) REPLACES the whole
--           (base+travel)*mult product: 0 hits ~1500 (an untouched pill
--           is barely worth leaving your post for) easing down to the
--           250 floor as the pill gets chewed up. Travel is added on
--           top (small) and readiness still multiplies. NOTE this now
--           makes every quiet pill a finite bidder, where the old code
--           returned NO BID for a healthy quiet pill.
-- Feasibility (siege only): assumed constant damage rate — TTL =
-- hp x DEFEND_ASSUMED_TICKS_PER_HP vs ETA = travel x DEFEND_ETA_PER_COST.
-- Arriving late scales cost up toward DEFEND_FUTILITY_MAX (never INF —
-- a late arrival still degrades into rebuild/capture recovery).
--
-- ARRIVAL HANDOFF: within DEFEND_ARRIVE_RADIUS (Euclidean tiles) the
-- travel phase is COMPLETE — the formula above must not keep bidding
-- ~100 and beat real close-range goals (attack_tank ~11, close repair).
-- Inside the radius the pill's bid becomes the HEAT action alone: a flat
-- DEFEND_HEAT_COST when every heat condition holds, or NO bid at all
-- (cost=huge, chip explains which condition blocked). Heat conditions:
--   taking_damage — enemy shells are heating it for free, no need
--   already_hot   — anger >= HEAT_PILL_MAX_ANGER (3 hits saturate)
--   low_hp        — hp < HEAT_PILL_MIN_HP (each shell costs ~1 HP)
--   low_shells    — shells < HEAT_PILL_SHOTS + SHELL_RESERVE
--   no_live_enemy — no hostile tank VISIBLE within
--                   HEAT_REQUIRE_ENEMY_RANGE of the pill right now
--                   (stale sightings justify the drive, never the
--                   self-shelling — heat needs a present target)
--   lgm_out       — our LGM is walking (possibly repairing this pill);
--                   never shell over our own man
--   ally_repair   — a teammate advertises repair_pill on this pill
-- Returns (cost, bd) — bd carries the per-term breakdown for the panel.
-- Quiet-tier price of a pill by HITS TAKEN (PILLS_MAX_HEALTH - health).
-- Straight table lookup up to the table's top index; beyond it the cost
-- eases HALFWAY toward DEFEND_QUIET_DMG_FLOOR per extra hit, so 5+ hits
-- converge on the floor (5 -> 275, 6 -> 262, 7 -> 256 ...) instead of
-- stepping off a cliff. Pure function of hits — travel and readiness are
-- applied by the caller.
local function quiet_dmg_cost(hits)
  local tbl = C.DEFEND_QUIET_DMG_COST or { [0] = 1500 }
  local flr = C.DEFEND_QUIET_DMG_FLOOR or 250
  local top = 0
  while tbl[top + 1] do top = top + 1 end
  if hits <= top then return tbl[hits] or flr end
  local c = tbl[top] or flr
  return flr + (c - flr) * (0.5 ^ (hits - top))
end

local function defend_pill_score(state, world, info, p, travel, now, tmx, tmy)
  local bd = { travel = travel }
  local hp  = p.health or 0
  local dmg = p.attack_damage or 0

  local hit_age   = (p.last_hit_tick and p.last_hit_tick > 0)
                    and (now - p.last_hit_tick) or math.huge
  local sight_age = p._enemy_near_tick and (now - p._enemy_near_tick) or math.huge
  local setup_age = p._lgm_near_tick and (now - p._lgm_near_tick) or math.huge

  -- ── Arrival handoff ────────────────────────────────────────────────
  local ddx, ddy = (tmx or 0) - p.mx, (tmy or 0) - p.my
  if math.sqrt(ddx * ddx + ddy * ddy) <= (C.DEFEND_ARRIVE_RADIUS or 10) then
    bd.arrived = true
    -- LIVE enemy requirement for the heat action: a hostile tank must be
    -- VISIBLE within HEAT_REQUIRE_ENEMY_RANGE of the pill RIGHT NOW.
    -- Stale sightings (sight/setup ages up to 600t) justify driving
    -- here, but never shelling our own pill for an enemy that already
    -- left — heat costs HP and shells and only pays against a present
    -- target the angered pill can actually shoot at.
    local live_enemy = false
    do
      local hr = C.HEAT_REQUIRE_ENEMY_RANGE or 10
      for _, et in ipairs((state.perc and state.perc.enemy_tanks) or {}) do
        local edx, edy = (et.mx or 0) - p.mx, (et.my or 0) - p.my
        if edx * edx + edy * edy <= hr * hr then live_enemy = true break end
      end
    end
    local block
    if hit_age < (C.DEFEND_DMG_FRESH_TICKS or 400) then
      block = "taking_damage"
    elseif (p.anger or 0) >= (C.HEAT_PILL_MAX_ANGER or 0.4) then
      block = "already_hot"
    elseif hp < (C.HEAT_PILL_MIN_HP or 6) then
      block = "low_hp"
    elseif (info.shells or 0) < (C.HEAT_PILL_SHOTS or 3) + (C.SHELL_RESERVE or 0) then
      block = "low_shells"
    elseif not live_enemy then
      block = "no_live_enemy"
    elseif info.man_status ~= C.LGM_INTANK then
      block = "lgm_out"
    else
      -- Ally repair overlap — TIMED: block heating only when the ally's
      -- repair would actually land inside our shoot window (aim + 3
      -- reload cycles + shell flight = HEAT_SEQUENCE_TICKS, plus
      -- HEAT_REPAIR_OVERLAP_MARGIN). An ally whose repair arrives well
      -- AFTER our volley finishes is no reason to hold fire. Sources:
      --   * lgmd advert (/info extra): ally LGM DISPATCHED to this tile,
      --     hex XXYY EEEE — real walk-sim ETA at send time, aged by the
      --     heartbeat gap. "-" = no dispatch.
      --   * goal advert: ally repair_pill CLAIM on this tile; arrival
      --     estimated from their broadcast pool cost (cost x
      --     DEFEND_ETA_PER_COST). No cost seen yet -> assume imminent.
      local our_window = (C.HEAT_SEQUENCE_TICKS or 150)
                         + (C.HEAT_REPAIR_OVERLAP_MARGIN or 100)
      for ally_pn, slot in ally_state.iter_active(now, 1750) do
        if ally_pn ~= info.player_number then
          local h = slot.info
          local their_eta, src = nil, nil
          local ld = h and h.lgmd
          if ld and ld ~= "-" and #ld >= 8 then
            local lx = tonumber(string.sub(ld, 1, 2), 16)
            local ly = tonumber(string.sub(ld, 3, 4), 16)
            if lx == p.mx and ly == p.my then
              local eta = tonumber(string.sub(ld, 5, 8), 16) or 0
              local age = now - (slot.last_tick or now)
              their_eta = eta - age
              if their_eta < 0 then their_eta = 0 end
              src = "lgmd"
            end
          end
          if their_eta == nil and h and h.goal == "repair_pill"
             and tonumber(h.mx) == p.mx and tonumber(h.my) == p.my then
            local hc = tonumber(h.cost)
            their_eta = hc and (hc * (C.DEFEND_ETA_PER_COST or 6)) or 0
            src = hc and "goal_cost" or "goal_nocost"
          end
          if their_eta ~= nil then
            if their_eta < our_window then
              block = "ally_repair"
              print2(string.format(
                "HEAT_GATE t=%d pill@(%d,%d) BLOCK ally_repair: p%d %s eta=%dt < window=%dt",
                now, p.mx, p.my, ally_pn, src, their_eta, our_window))
              break
            else
              print2(string.format(
                "HEAT_GATE t=%d pill@(%d,%d) ally p%d repair intent (%s eta=%dt) OUTSIDE window=%dt -> heat still allowed",
                now, p.mx, p.my, ally_pn, src, their_eta, our_window))
            end
          end
        end
      end
    end
    if block then
      bd.heat_block = block
      bd.cost = math.huge
      print2(string.format(
        "HEAT_GATE t=%d pill@(%d,%d) NO-BID (%s) hp=%d anger=%.2f shells=%d hit_age=%s sight_age=%s setup_age=%s",
        now, p.mx, p.my, block, hp, p.anger or 0, info.shells or 0,
        hit_age < math.huge and tostring(hit_age) or "-",
        sight_age < math.huge and tostring(sight_age) or "-",
        setup_age < math.huge and tostring(setup_age) or "-"))
      return math.huge, bd
    end
    bd.heat = true
    bd.cost = C.DEFEND_HEAT_COST or 200
    print2(string.format(
      "HEAT_GATE t=%d pill@(%d,%d) HEAT BID %.0f hp=%d anger=%.2f shells=%d evidence(hit=%s sight=%s setup=%s)",
      now, p.mx, p.my, bd.cost, hp, p.anger or 0, info.shells or 0,
      hit_age < math.huge and tostring(hit_age) or "-",
      sight_age < math.huge and tostring(sight_age) or "-",
      setup_age < math.huge and tostring(setup_age) or "-"))
    return bd.cost, bd
  end

  local siege       = hit_age < (C.DEFEND_DMG_FRESH_TICKS or 400)
  local sight_fresh = C.DEFEND_SIGHT_FRESH_TICKS or 600
  local sight_f     = (sight_age < sight_fresh) and (1 - sight_age / sight_fresh) or 0
  local setup_f     = (setup_age < sight_fresh) and (1 - setup_age / sight_fresh) or 0

  -- Threat tiers as MULTIPLIERS on (base + travel); the strongest live
  -- tier wins (min). Multiplicative keeps bids proportional — severity
  -- and distance stay ordered instead of everything slamming into the
  -- MIN_COST floor — and with base ~250 the common threatened cases land
  -- naturally around 100-300.
  local mult = 1.0
  if siege then
    -- Savability-scaled: a healthy pill under fresh attack pulls hardest
    -- (SIEGE_MULT_MIN); an almost-dead one is mostly lost (recovery is
    -- capture/rebuild territory) so its multiplier decays toward 1.
    -- Damage depth thus works AGAINST the bid — weaker pull here,
    -- shorter TTL below.
    bd.siege_m = 1 - (1 - (C.DEFEND_SIEGE_MULT or 0.30))
                     * (hp / (C.PILLS_MAX_HEALTH or 15))
    if bd.siege_m < mult then mult = bd.siege_m; bd.tier = "siege" end
  end
  if setup_f > 0 then
    -- Wall/pill-plant tell: the MOST savable moment (nothing lost yet,
    -- build interruptible) -> strongest tier. Decays toward 1 with age.
    local m = C.DEFEND_SETUP_MULT or 0.25
    bd.setup_m = m + (1 - m) * (1 - setup_f)
    if bd.setup_m < mult then mult = bd.setup_m; bd.tier = "setup" end
  end
  if sight_f > 0 then
    local m = C.DEFEND_SIGHT_MULT or 0.50
    bd.sight_m = m + (1 - m) * (1 - sight_f)
    if bd.sight_m < mult then mult = bd.sight_m; bd.tier = "sight" end
  end

  -- Quiet (no fresh threat evidence at all — no siege, no setup tell, no
  -- sighting): priced by the DEFEND_QUIET_DMG_COST curve on HITS TAKEN,
  -- NOT by the (base+travel)*mult product. An untouched pill is barely
  -- worth leaving your post for (~1500); each hit it has already taken
  -- raises urgency (1 -> 1000, 2 -> 800, 3 -> 500, 4 -> 300, 5+ easing to
  -- the 250 floor). Travel rides along additively (small next to the
  -- curve) so the closer responder still wins; readiness still scales it
  -- (an empty tank defending is still bad); and the bid then flows
  -- through the SAME well-defended clamp + tiebreaker as the live tiers,
  -- so a cheap worn-pill bid that allies already cover gets RAISED to
  -- DEFEND_WELL_DEFENDED_COST while an expensive quiet dmg-0 bid (1500)
  -- is left alone. The two-tier floor is NOT applied to quiet bids — the
  -- curve supersedes it (see the live-tier branch below).
  -- hits comes from HEALTH, not p.attack_damage: that field is a
  -- recent-burst accumulator world.lua zeroes PILL_ATTACK_COOLDOWN ticks
  -- after the last hit, so it reads 0 on every quiet pill however worn.
  -- Readiness: low shells/armour makes THIS bot's defend costlier, so
  -- the total-score steal hands the pill to the best-equipped responder
  -- among comparable distances (an empty tank arriving first defends
  -- nothing). Linear in each deficit below the resupply thresholds,
  -- +1.0x max per resource, capped overall.
  local ready = 1.0
    + (1 - math.min(1, (info.shells or 0) / (C.DEFEND_READY_SHELLS or 20)))
    + (1 - math.min(1, (info.armour or 0) / (C.DEFEND_READY_ARMOUR or 15)))
  local ready_cap = C.DEFEND_READY_MAX_MULT or 2.5
  if ready > ready_cap then ready = ready_cap end
  bd.ready = ready   -- ALWAYS recorded so the rdy{} chip is never invisible

  local cost
  if mult >= 1.0 then
    -- ── QUIET tier: the damage curve replaces (base+travel)*mult ──────
    -- Covers both the old WORN case (quiet + damaged) and the old
    -- healthy-quiet no-bid case; both now bid, priced by wear alone.
    bd.mult = 1.0
    bd.feas = 1.0
    local hits = (C.PILLS_MAX_HEALTH or 15) - hp
    if hits < 0 then hits = 0 end
    bd.quiet_hits  = hits
    bd.quiet_curve = quiet_dmg_cost(hits)
    if hits > 0 then bd.worn = true else bd.quiet = true end
    cost = (bd.quiet_curve + travel) * ready
  else
    -- Coverage, only while some threat tier is live: each other alive
    -- team pill whose fire reaches this one shaves a little more off.
    local cover = 0
    for _, q in pairs(world.pills) do
      if q ~= p and q.owner == "friendly" and (q.health or 0) > 0
         and not (q.in_tank or q.carrier or q._synth_carry)
         and U.mdist(q.mx, q.my, p.mx, p.my) <= (C.PILL_FIRE_RANGE or 8) then
        cover = cover + 1
      end
    end
    if cover > 0 then
      bd.cover_n = cover
      bd.cover_m = (C.DEFEND_COVERAGE_MULT or 0.95) ^ cover
      mult = mult * bd.cover_m
    end
    bd.mult = mult

    local feas = 1.0
    if siege and travel < math.huge then
      bd.ttl = hp * (C.DEFEND_ASSUMED_TICKS_PER_HP or 80)
      bd.eta = travel * (C.DEFEND_ETA_PER_COST or 6)
      if bd.ttl > 0 and bd.eta > bd.ttl then
        feas = math.min(bd.eta / bd.ttl, C.DEFEND_FUTILITY_MAX or 3.0)
      end
    end
    bd.feas = feas

    cost = ((C.DEFEND_PILL_BASE_COST or 250) + travel) * mult * feas * ready
    -- Two-tier floor — LIVE TIERS ONLY (the quiet curve above supersedes
    -- it; its own 250 floor is the quiet backstop). With NO fresh damage
    -- (siege) and NO setup tell (the "pill block going up -> take
    -- incoming" LGM sighting), the evidence is a mere enemy drive-by —
    -- precaution bids floor at the higher DEFEND_SIGHT_MIN_COST (~200) so
    -- they never outbid real rescues or productive work. Siege/setup keep
    -- the low floor: those are live.
    local floor_c = C.DEFEND_MIN_COST or 100
    if not siege and setup_f <= 0 then
      floor_c = C.DEFEND_SIGHT_MIN_COST or 200
    end
    if cost < floor_c then
      bd.floored = floor_c   -- panel: the printed product got clamped up
      cost = floor_c
    end
  end

  -- ── Well-defended gate ─────────────────────────────────────────────
  -- Allies ALREADY at the pill (fresh /info positions, bidder excluded)
  -- covering the enemies there in team-ratio proportion means this pill
  -- doesn't need US too — the whole team swarming one threatened pill
  -- strips every other front. Coverage requirement rounds in the
  -- defenders' favour: R = ceil(their_team/our_team), well-defended
  -- when foes <= allies_near * R (no visible foes + any ally = held).
  local wd_cost = C.DEFEND_WELL_DEFENDED_COST or 500
  if cost < wd_cost then
    local wr  = C.DEFEND_WELL_DEFENDED_RADIUS or 10
    local wr2 = wr * wr
    -- Defenders already on it, from two live signals:
    --   (a) VISIBLE allied tanks parked within the radius (info.objects
    --       hostility bit — real presence, only when we can see them);
    --   (b) allies whose broadcast goal is a defense RESPONSE targeting
    --       this pill area (defend_pill / repair_pill claims) — covers
    --       the fog case AND bots still en route, which is exactly the
    --       everyone-swarms window. max() of the two, since a visible
    --       defender usually also claims.
    local allies_near = 0
    for _, ob in ipairs(info.objects or {}) do
      if ob.type == OBJECT_TANK
         and bit.band(ob.info, OBJECT_HOSTILE) == 0 then
        local adx = bit.rshift(ob.x, 8) - p.mx
        local ady = bit.rshift(ob.y, 8) - p.my
        if adx * adx + ady * ady <= wr2 then
          allies_near = allies_near + 1
        end
      end
    end
    local responders = 0
    for ally_pn, slot in ally_state.iter_active(now, 1750) do
      if ally_pn ~= info.player_number then
        local h = slot.info
        if h and (h.goal == "defend_pill" or h.goal == "repair_pill") then
          local gx, gy = tonumber(h.mx), tonumber(h.my)
          if gx and gy then
            local adx, ady = gx - p.mx, gy - p.my
            if adx * adx + ady * ady <= wr2 then
              responders = responders + 1
            end
          end
        end
      end
    end
    if responders > allies_near then allies_near = responders end
    if allies_near > 0 then
      local foes_near = 0
      for _, et in ipairs((state.perc and state.perc.enemy_tanks) or {}) do
        local edx, edy = (et.mx or 0) - p.mx, (et.my or 0) - p.my
        if edx * edx + edy * edy <= wr2 then
          foes_near = foes_near + 1
        end
      end
      local a, ours_total = info.allies or 0, 0
      while a > 0 do
        ours_total = ours_total + (a % 2)
        a = math.floor(a / 2)
      end
      if ours_total < 1 then ours_total = 1 end
      local theirs_total = (info.num_players or ours_total) - ours_total
      if theirs_total < 1 then theirs_total = 1 end
      local ratio = math.ceil(theirs_total / ours_total)
      if foes_near <= allies_near * ratio then
        bd.welldef = { ours = allies_near, foes = foes_near, ratio = ratio }
        cost = wd_cost
      end
    end
  end

  -- Tiebreaker on the FLAT costs (floor / well-defended): flat clamps
  -- erase the travel AND readiness ordering, which let a far/empty
  -- bot's claim stick just because it re-scored first — fold both back
  -- in as a sliver so the defend_pill steal band (~0.5%, see
  -- sync_ally_claimed_rejects) hands the job to the closer and (at
  -- comparable distance) better-equipped responder.
  if bd.floored or bd.welldef then
    bd.tb = travel * 0.01 + (ready - 1.0) * 2.0   -- panel: tb{} chip
    cost = cost + bd.tb
  end

  bd.cost = cost
  return cost, bd
end

-- eval_defend_pill — internal scoring pool over ALL BUILT team pills
-- (own "friendly" pills AND teammates' "allied" pills).
-- The legacy implementation (single perc.pill_under_attack target, silent
-- whole-pool nil gates — armour/busy/repos/min-damage/max-travel/under-
-- attack — and the travel+base-urgency formula) is fully gutted. Every
-- deployed friendly pill gets a row with the raw ingredients (dij travel,
-- hp, attack damage, last-hit age, repair readiness); carried pills
-- (in_tank) aren't on the map and get no row. The only reject left is
-- "dead" (hp=0 — nothing to defend). Attack state gates NOTHING — it
-- feeds defend_pill_score above.
--
-- Unreachable pills score math.huge and lose naturally (no travel gate).
-- Cheapest pill is the pool's winner (pc[2]); if it wins the WINNERS
-- competition it becomes the main goal and the tank travels to the pill.
--
-- Rows persist in state.defend_breakdown even on replans where NOTHING is
-- defendable, so the panel never goes blank. Runs at finalize cadence;
-- travel is O(1) dij-slate lookups per pill (travel_cost_to_pill).
local function eval_defend_pill(state, world, info, tmx, tmy, boat, ammo)
  local now = state.tick or 0
  local rows = BRAIN_POOL_VIZ and {} or nil

  -- Selection-layer preview for the panel rows: mirror the EXACT phase
  -- weight (distance-attenuated, goal_selection's lerp) and influence
  -- scaling that will hit this pool's bid downstream, so a clicked
  -- row's number reconciles with the WINNERS view instead of reading
  -- ~2x off (middle-phase defend_pill weight is 0.5 — the panel used
  -- to show only the raw pool cost with no hint of that).
  local pw0 = (C.PHASE_WEIGHTS and C.PHASE_WEIGHTS[state.phase]
               and C.PHASE_WEIGHTS[state.phase].defend_pill) or 1.0
  local _fot = C.PHASE_WEIGHT_DIST_FALLOFF
  local pw_falloff = (type(_fot) == "table" and (_fot.defend_pill or _fot.default))
                     or (type(_fot) == "number" and _fot) or 40
  local function sel_preview(mx, my, cost)
    if not cost or cost >= math.huge then return "" end
    local pw = pw0
    if pw ~= 1.0 then
      local gd = U.mdist(tmx or 0, tmy or 0, mx, my)
      pw = 1.0 + (pw - 1.0) * (1 - math.min(1.0, gd / pw_falloff))
    end
    local im = 1.0
    if state.phase ~= "opening" then
      local inf = cpf.influence_at(mx, my) or 0
      if inf < -50 then im = 2.0 elseif inf > 50 then im = 0.5 end
    end
    -- ALWAYS rendered, even when both factors are neutral (xph1.00
    -- xinf1.0): the term breakdown has to compute the end score from
    -- what is VISIBLE, so no factor may be silently omitted.
    return string.format(
      " ->sel{%.0f xph%.2f xinf%.1f} (+switch/commit at selection)",
      cost * pw * im, pw, im)
  end

  -- Repair-readiness (row detail only): could we patch the pill up on
  -- arrival? Actual dispatch stays repair_pill/builder territory.
  local repair_ready = (info.man_status == C.LGM_INTANK) and (info.trees or 0) > 0

  local best, best_id, best_cost, best_travel, best_dmg =
        nil, nil, math.huge, 0, 0
  local best_bd = nil
  -- BUILT team pills only (own "friendly" + teammates' "allied"): a pill
  -- riding in a tank isn't on the map and can't be defended — no row.
  for id, p in pairs(world.pills) do
    if (p.owner == "friendly" or p.owner == "allied")
       and not (p.in_tank or p.carrier or p._synth_carry) then
      local dmg = p.attack_damage or 0
      local hp  = p.health or 0
      local reject = (hp == 0) and "dead" or nil
      -- Score everything on the map (O(1) dij lookups) — dead rows too, so
      -- their would-be cost shows in the panel. An unreachable pill scores
      -- math.huge and simply never wins; no gate needed. Attack state does
      -- NOT gate anything — it feeds defend_pill_score.
      local travel = travel_cost_to_pill(p.mx, p.my, boat)
      local cost, bd
      if not reject then
        cost, bd = defend_pill_score(state, world, info, p, travel, now, tmx, tmy)
        if cost < best_cost then
          best, best_id, best_cost, best_travel, best_dmg = p, id, cost, travel, dmg
          best_bd = bd
        end
      end
      if rows then
        local hit_age   = (p.last_hit_tick and p.last_hit_tick > 0) and (now - p.last_hit_tick) or -1
        local sight_age = p._enemy_near_tick and (now - p._enemy_near_tick) or -1
        local setup_age = p._lgm_near_tick and (now - p._lgm_near_tick) or -1
        local detail = string.format(
          "hp=%d/%d hits=%d dmg=%d%s%s%s; repair-ready=%s (lgm=%s trees=%d)",
          hp, C.PILLS_MAX_HEALTH,
          math.max(0, (C.PILLS_MAX_HEALTH or 15) - hp), dmg,
          hit_age >= 0 and string.format(" last_hit=%dt", hit_age) or "",
          sight_age >= 0 and string.format(" enemy_seen=%dt", sight_age) or "",
          setup_age >= 0 and string.format(" setup_seen=%dt", setup_age) or "",
          tostring(repair_ready),
          (info.man_status == C.LGM_INTANK) and "in_tank" or "out", info.trees or 0)
        local formula
        if not reject then
          if bd and bd.arrived then
            if bd.heat then
              formula = string.format(
                "ARRIVED heat{%.0f}%s||within %d tiles: travel phase done; bidding the heat-up action only (%d shells to anger the pill); %s",
                cost, sel_preview(p.mx, p.my, cost),
                C.DEFEND_ARRIVE_RADIUS or 10, C.HEAT_PILL_SHOTS or 3, detail)
            else
              formula = string.format(
                "ARRIVED no-bid (%s)||within %d tiles: travel phase done; heat blocked by %s -> defend yields to attack_tank / repair_pill / whatever else bids; %s",
                bd.heat_block, C.DEFEND_ARRIVE_RADIUS or 10, bd.heat_block, detail)
            end
          elseif cost >= 1e29 or travel >= math.huge then
            formula = string.format("base{%.0f}+dij{unreachable} = INF||%s",
              C.DEFEND_PILL_BASE_COST, detail)
          else
            -- Threat-tier chips, only the live ones (strongest wins).
            local u = ""
            if bd.worn then u = " WORN(quiet + damaged: curve-priced)" end
            if bd.siege_m then u = u .. string.format(" siege{%.2f}", bd.siege_m) end
            if bd.setup_m then u = u .. string.format(" setup{%.2f}", bd.setup_m) end
            if bd.sight_m then u = u .. string.format(" sight{%.2f}", bd.sight_m) end
            if bd.cover_m then u = u .. string.format(" cover{%.2fx%d}", bd.cover_m, bd.cover_n) end
            -- FULL VISIBILITY: every factor that touches the number gets a
            -- chip UNCONDITIONALLY (rdy/late even at 1.00), and each clamp
            -- prints the value it clamps TO, so reading the chain left to
            -- right reproduces the printed total exactly.
            --   live tier : (base+dij)*m *rdy *late [floor] [WELLDEF] [+tb]
            --   quiet     : (quiet_dmg+dij)  *rdy   [WELLDEF] [+tb]
            local f = bd.quiet_curve and ""
              or string.format("*late{%.2f eta=%.0f ttl=%.0f}",
                               bd.feas or 1.0, bd.eta or 0, bd.ttl or 0)
            local fl = bd.floored
              and string.format(" floor{%.0f}", bd.floored) or ""
            local wd = bd.welldef
              and string.format(" WELLDEF{%d ally vs %d foe, R=%d -> %.0f}",
                                bd.welldef.ours, bd.welldef.foes,
                                bd.welldef.ratio, C.DEFEND_WELL_DEFENDED_COST or 500)
              or ""
            local tb = bd.tb and string.format(" tb{+%.2f}", bd.tb) or ""
            local rd = string.format("*rdy{%.2f sh=%d arm=%d}",
                                bd.ready or 1.0, info.shells or 0, info.armour or 0)
            local head = bd.quiet_curve
              and string.format("(quiet_dmg{%d hits -> %.0f}+dij{%.0f})",
                                bd.quiet_hits or 0, bd.quiet_curve, travel)
              or string.format("(base{%.0f}+dij{%.0f})*m{%.2f}",
                               C.DEFEND_PILL_BASE_COST, travel, bd.mult)
            formula = string.format(
              "%s%s%s%s%s%s%s = %.0f%s||%s",
              head, rd, f, fl, wd, tb, u, cost,
              sel_preview(p.mx, p.my, cost), detail)
          end
        else
          local why = "hp=0 — rebuild/capture territory, not defend"
          local wb = ""
          if travel and travel ~= math.huge then
            wb = string.format("; would-be base{%.0f}+dij{%.0f}",
                 C.DEFEND_PILL_BASE_COST, travel)
          end
          formula = string.format("REJECT %s||reject:%s%s; %s", reject, why, wb, detail)
        end
        rows[#rows + 1] = {
          id = id, mx = p.mx, my = p.my,
          -- 1e30 = renderer INF: rejects AND unreachable (dij=math.huge,
          -- which json.lua would otherwise encode as a real-looking 9999).
          cost = (reject or cost >= math.huge) and 1e30 or cost,
          formula = formula,
          stale = 0,  -- overwritten with rows' age at panel-read time
          reject = reject,
          reject_remaining = 0,
          -- Tier tag for the defend_pill_viz overlay (init.lua draws from
          -- these rows every tick).
          tier = reject and "dead"
                 or (bd.heat and "heat") or (bd.heat_block and "no_heat")
                 or (bd.quiet and "quiet") or (bd.worn and "worn")
                 or (bd.welldef and "welldef")
                 or bd.tier or "quiet",
        }
      end
    end
  end

  -- Stash rows for the pool grid BEFORE any nil return — the cell lists
  -- every owned pill regardless of whether the pool produced a winner.
  state.defend_breakdown = rows and { tick = now, rows = rows } or nil

  if not best then return nil end
  local is_heat = best_bd and best_bd.heat or nil
  -- Winner desc carries the SAME full chip chain as the candidate rows:
  -- the WINNERS strip must reproduce the final pool cost from what it
  -- shows alone (head, readiness, lateness, floor, WELLDEF, tiebreaker).
  local best_desc = ""
  if BRAIN_POOL_VIZ then
    local b = best_bd or {}
    if is_heat then
      best_desc = string.format("defend#%d@(%d,%d) ARRIVED heat{%.0f}",
                                best_id, best.mx, best.my, best_cost)
    else
      local head = b.quiet_curve
        and string.format("(quiet_dmg{%d hits -> %.0f}+dij{%.0f})",
                          b.quiet_hits or 0, b.quiet_curve, best_travel)
        or string.format("(base{%.0f}+dij{%.0f})*m{%.2f}*late{%.2f}",
                         C.DEFEND_PILL_BASE_COST, best_travel,
                         b.mult or 1.0, b.feas or 1.0)
      best_desc = string.format(
        "defend#%d@(%d,%d) %s*rdy{%.2f}%s%s%s = %.0f hits=%d dmg=%d",
        best_id, best.mx, best.my, head, b.ready or 1.0,
        -- Post-product clamps, in application order, so the printed
        -- formula multiplies out to the final number instead of
        -- silently jumping (the "=500 but the parts say 253" report).
        b.floored and string.format(" floor{%.0f}", b.floored) or "",
        b.welldef and string.format(" WELLDEF{%d ally vs %d foe R=%d -> %.0f}",
            b.welldef.ours, b.welldef.foes, b.welldef.ratio,
            C.DEFEND_WELL_DEFENDED_COST or 500) or "",
        b.tb and string.format(" tb{+%.2f}", b.tb) or "",
        best_cost,
        math.max(0, (C.PILLS_MAX_HEALTH or 15) - (best.health or 0)),
        best_dmg)
    end
  end
  return {
    cost = best_cost,
    goal = { kind = "defend_pill", mx = best.mx, my = best.my,
             wx = U.m2w(best.mx), wy = U.m2w(best.my),
             target_id = best_id,
             -- Arrival-phase win: the bid is the heat-up action, not a
             -- drive. Phase-3 heat substates key off this flag.
             heat = is_heat },
    desc = best_desc,
    cands = rows,
  }
end

-- =========================================================================
-- Pill repositioning (aIndy: "pissing" — move badly-positioned friendly pills)
-- Evaluates friendly pills for bad positioning: too far from any base,
-- exposed to many enemy pills, on bad terrain. If a pill scores badly,
-- create a goal to pick it up (drive over dead/pissed pill). The existing
-- place_pill_strategic system handles replanting once we carry it.
-- =========================================================================
-- Sentinel cost for a reposition candidate that can't actually act this
-- replan. Large enough to never win the competition, but < 1e29 so the
-- WINNERS strip still renders it (so its score stays visible every replan).
-- The competition also skips any candidate carrying a `_reject`.
local REPOSITION_REJECT_COST = 1e8

-- score_only=true (called via M.rescan_reposition from init.lua's quiet-tick
-- scheduler) runs ONLY the heavy position scan, caches it, and returns nil. The
-- normal replan call reads that cache (cheap) and adds the fresh per-tick terms.
local function eval_reposition_pill(state, world, info, tmx, tmy, boat, ammo, score_only)
  -- Reposition is governed by the C.PILL_REPOSITION_ENABLED flag. PR #92
  -- (beta prep) had hard-disabled it via an unconditional reject because bots
  -- were shooting their own pills too much; that hardcoded block is removed
  -- here. Our reposition risk-scoring (few-pills / enemy-tank penalties below)
  -- plus reposition claiming (R3a, ally_claimed-scoped) are the actual remedy,
  -- so the flag defaults ON in constants.lua. Flip it off to A/B.
  if not C.PILL_REPOSITION_ENABLED then return nil end

  -- Human teammate on the roster: repositioning is off entirely
  -- (REPOSITION_DISABLE_WITH_HUMAN_ALLIES, a user-tunable flag). A human hasn't
  -- opted into the bots' consensus, can't vote in it, and is reading a back
  -- line we'd be rearranging under them. Bailing here means no candidate, so no
  -- vote is ever opened; reposition_vote's OPEN gate and every ally's ballot
  -- re-check it independently. Detection is engine-authoritative
  -- (info.allies & ~info.player_bots) — see util.human_ally_count.
  if C.REPOSITION_DISABLE_WITH_HUMAN_ALLIES and U.human_ally_count(info) > 0 then
    state._repo_candidate = nil
    return nil
  end

  -- Always return a candidate so the WINNERS pool shows reposition's score
  -- every replan. When it can't actually act, the candidate carries a
  -- `_reject` reason (skipped by the goal competition, still rendered).
  -- The only "no target" reject is having no friendly pills at all; the
  -- rest are genuine "can't reposition right now" states.

  -- Reposition cost is driven primarily by our pill-type BALANCE: a pill in a
  -- category (back/front/aggressive) that's OVER its 35/45/20 allotment gets a
  -- big surplus discount, so the bot sheds from over-full roles. Secondary
  -- terms keep good spots / break ties within a category:
  --   + coverage  (friendly pills + bases in fire range — keep good protectors
  --                / mutual support; raises cost = leave it)
  --   - surplus   (category over allotment — PRIMARY; lowers cost = move out)
  --   - adjacency (friendly pill in the 8 surrounding tiles — double-take risk)
  --   - overext   (aggressive pill deeper than -50 influence — pull it back)
  -- We pick the LOWEST position-cost pill (most worth moving), then add travel.
  -- In-tank pills count as util inside PP.counts. Total spans every category so
  -- the 20/45/20/15 target shares are computed over the whole pool.
  -- ── Decoupled position scan (heavy O(pills^2) coverage loop) ───────────
  -- Skip it entirely on a normal replan tick if the quiet-tick scheduler has
  -- already cached a result; only compute when forced (score_only) or cold.
  if score_only or not state._repo_score then
  local counts  = PP.counts(world, state.tick)
  local total   = counts.back + counts.front + counts.aggro + counts.utility
  local targets = PP.targets(total)

  -- Enemy-tank threat: hoisted once. A pill with hostile tanks loitering
  -- within (fire range + pad) is dangerous to demolish for a reposition.
  local enemy_tanks = (state.perc and state.perc.enemy_tanks) or {}
  local tank_radius = C.PILL_FIRE_RANGE + (C.PILL_REPOSITION_ENEMY_TANK_PAD or 5)

  -- Reposition claiming (R3a): a pill an ALLY is already repositioning is
  -- claimed — skip it so two bots don't independently demolish the same
  -- friendly pill. Allies broadcast goal=capture_pill + repos=1 + the pill's
  -- target id and tile (mx/my), parsed into ally_state. Keyed by both pill id
  -- and tile so a match on either claims it. Self is excluded (our own
  -- committed pill is forced back in via the lock-in below).
  local ally_repos = nil
  if ally_state.iter_active and info.player_number then
    for apn, slot in ally_state.iter_active(state.tick, 1750) do
      if apn ~= info.player_number then
        local ai = slot.info
        if ai and ai.repos == "1" then
          ally_repos = ally_repos or {}
          local atid = tonumber(ai.target)
          if atid then ally_repos[atid] = true end
          local amx, amy = tonumber(ai.mx), tonumber(ai.my)
          if amx and amy then ally_repos["t:" .. amx .. "," .. amy] = true end
        end
      end
    end
  end

  -- R1 excess gate: only roll a back pill forward when the BACK category is
  -- actually OVER its target allotment. A balanced (or under-strength) back
  -- line shouldn't be thinned just because a movable pill exists — that made
  -- reposition fire far too often. Same value for every back pill, so compute
  -- once. (A committed reposition still finishes via the lock-in below.)
  local back_surplus = math.max(0, (counts.back or 0) - (targets.back or 0))

  -- Back-section over-proportion, as a fraction of the back target (0 = at or
  -- under target). Reposition scales its propose cooldown DOWN by this: the
  -- more the BACK line is over its allotment, the more eagerly bots roll a
  -- back pill forward. Cached for reposition_vote's OPEN gate (which runs
  -- every tick, off the decoupled scan cadence). Back is the only shed
  -- category, so this is exactly "how out of proportion the section we'd
  -- move from is."
  state._repo_imbalance = back_surplus / math.max(1, targets.back or 1)

  -- Hard floor: never reposition while the team has few BUILT (deployed) pills.
  -- Below this we can't afford to take one offline at all, regardless of balance.
  local built_count = 0
  for _, bp in pairs(world.pills) do
    if bp.owner == "friendly" and (bp.health or 0) > 0 and not bp.in_tank then
      built_count = built_count + 1
    end
  end
  -- Team-pill floor removed: reposition regardless of how many pills are built.
  local built_block = false

  local cands = {}   -- EVERY live friendly/allied pill, scored, for the score board
  local enemy_act = state._repo_enemy_activity or {}
  -- List EVERY live friendly/allied pill. Only BACK-role pills are ELIGIBLE to
  -- win + be moved (front/aggro/util are shown in the board but never picked).
  -- Position score only here (travel added fresh per-replan in the read path so
  -- distance-to-us influences WHICH pill wins). Lower = more worth moving.
  for pid, p in pairs(world.pills) do
    if (p.owner == "friendly" or p.owner == "allied")
       and (p.health or 0) > 0 and not p.in_tank and not p._in_use then
      local cat = PP.role_of(p, state.tick)
      -- Fresh reclassify (cached "back" can be ~60s stale) + ally-claim skip
      -- (a pill an ally is already repositioning is off-limits) gate eligibility.
      -- Also OFF-LIMITS: a pill placed within PILL_JUST_BUILT_TICKS. Rejecting
      -- it here (not just in the vote) means we never even OPEN a doomed vote
      -- on a pill every ally is going to veto as "just_built" — a failed vote
      -- costs the whole team FAIL_COOLDOWN. placed_tick is nil for pills we
      -- never saw placed, which are by definition not fresh.
      local _placed  = p.placed_tick
      local _fresh   = _placed and (state.tick - _placed) < (C.PILL_JUST_BUILT_TICKS or 1500)
      local eligible = (cat == "back")
                       and PP.role_of(p, state.tick, true) == "back"
                       and not _fresh
                       and not (ally_repos and (ally_repos[pid]
                                or ally_repos["t:" .. p.mx .. "," .. p.my]))

      -- imbalance discount: how far THIS pill's category is over its allotment.
      local surplus   = math.max(0, (counts[cat] or 0) - (targets[cat] or 0))
      local surp_disc = surplus * (C.PILL_REPOSITION_SURPLUS_W or 100)

      -- cardinal-adjacency discount: a FRIENDLY pill 1 tile N/S/E/W (redundant
      -- clustering worth thinning). Cardinal only — diagonals don't count.
      local card = 0
      if world.pill_at then
        local COFF = { -256, 256, -1, 1 }   -- N, S, W, E in my*256+mx keyspace
        for _, off in ipairs(COFF) do
          local entries = world.pill_at[p.my * 256 + p.mx + off]
          if entries then
            for _, e in ipairs(entries) do
              local op = e.pill
              if op and (op.owner == "friendly" or op.owner == "allied")
                 and (op.health or 0) > 0 then card = card + 1 end
            end
          end
        end
      end
      local card_disc = card * (C.PILL_REPOSITION_CARDINAL_ADJ_W or 40)

      -- base-protection penalty: friendly bases the pill covers (x W). A pill
      -- guarding bases has a real job — more bases = more penalty to move it.
      local bases_cov = count_friendly_bases_near(world, p.mx, p.my, C.PILL_FIRE_RANGE)
      local base_pen  = bases_cov * (C.PILL_REPOSITION_BASE_PROTECT_W or 80)

      -- enemy-activity penalty (decaying): stamped per-tick by reposition_vote
      -- when a hostile pill/LGM was seen near this pill. Repositioning kills the
      -- pill temporarily — dangerous while enemies are (or recently were) near.
      local act_pen = 0
      local seen = enemy_act[pid]
      if seen then
        local decay = C.PILL_REPOSITION_ENEMY_ACTIVITY_DECAY_TICKS or 1500
        act_pen = (C.PILL_REPOSITION_ENEMY_ACTIVITY_W or 250)
                  * math.max(0, 1 - ((state.tick or 0) - seen) / decay)
      end

      -- enemy-tank penalty (safety): hostile tanks loitering within range.
      local tanks_near = 0
      for _, et in ipairs(enemy_tanks) do
        if U.mdist(p.mx, p.my, et.mx, et.my) <= tank_radius then tanks_near = tanks_near + 1 end
      end
      local tank_pen = tanks_near * (C.PILL_REPOSITION_ENEMY_TANK_W or 150)

      local pos = (C.PILL_REPOSITION_BASE_COST or 500)
                  + base_pen + act_pen + tank_pen - surp_disc - card_disc

      cands[#cands + 1] = { pid = pid, mx = p.mx, my = p.my, pos = pos,
                            cat = cat, eligible = eligible,
                            surp = surp_disc, card = card_disc, base = base_pen,
                            act = act_pen, tank = tank_pen }
    end
  end

  -- Cache the travel-free position scan (survives across ticks). The read path
  -- below adds fresh per-replan travel to each candidate, re-ranks, and picks
  -- the winner — so distance-to-us influences WHICH pill is chosen. _repo_topN /
  -- _repo_candidate are (re)built there each replan.
  state._repo_score = {
    tick = state.tick, cands = cands,
    counts = counts, targets = targets, back_surplus = back_surplus, built_count = built_count,
  }
  if score_only then return nil end
  end   -- end decoupled position-scan block (run on a quiet tick via M.rescan_reposition)

  -- ── Read path (every replan): add fresh travel to each cached candidate,
  --    re-rank, pick the winner among BACK-eligible pills, then decide cost. ──
  local sc = state._repo_score
  local counts, targets = sc.counts, sc.targets
  local back_surplus = sc.back_surplus
  local now_tick = state.tick or 0
  local DIST_W = C.PILL_REPOSITION_DISTANCE_W or 1.0

  -- Total = position score + Dijkstra travel (tank -> pill). Unreachable = huge.
  -- Winner = lowest-total BACK-eligible pill; every pill keeps its total for the
  -- score board (ineligible ones shown but never selected).
  local best_pill, best_pid, best_total, best_cand
  for _, c in ipairs(sc.cands or {}) do
    local tv = travel_cost_to_pill(c.mx, c.my, boat)
    c.travel = (tv == math.huge) and 1e7 or (tv * DIST_W)
    c.score  = c.pos + c.travel
    local lp = world.pills[c.pid]
    if c.eligible and lp and (lp.health or 0) > 0
       and (not best_total or c.score < best_total) then
      best_total = c.score; best_pid = c.pid; best_pill = lp; best_cand = c
    end
  end

  -- Score board: EVERY pill, sorted by total (ineligible kept, tagged in viz).
  if sc.cands and #sc.cands > 0 then
    table.sort(sc.cands, function(a, b) return (a.score or 0) < (b.score or 0) end)
    state._repo_topN = sc.cands
  else
    state._repo_topN = nil
  end
  if best_pill then
    local cand = state._repo_candidate or {}
    cand.pid = best_pid; cand.mx = best_pill.mx; cand.my = best_pill.my
    cand.score = best_total or 0; cand.tick = now_tick
    state._repo_candidate = cand   -- can_carry refreshed each tick by reposition_vote
  else
    state._repo_candidate = nil
  end

  -- ── Reposition lock-in (gated on APPROVAL) ──────────────────────────────
  -- In win-then-vote the goal becomes state.goal at its honest BID cost BEFORE
  -- any vote, so the lock must NOT engage merely on "this is the current goal"
  -- (that would freeze the bid at lock cost and skip the vote). It engages only
  -- once the team vote APPROVED this pill for us, then holds the move at the
  -- fixed lock cost through the drive+shoot phase so nothing steals it mid-swap.
  -- Released when the pill is gone/dead (pickup handoff) or LOCK_TICKS elapses.
  local repos_locked = false
  if state.goal and state.goal.kind == "capture_pill" and state.goal.reposition
     and state.goal.target_id and state._repo_approved_pid == state.goal.target_id then
    local lp = world.pills[state.goal.target_id]
    if lp and lp.owner == "friendly" and (lp.health or 0) > 0 then
      if not state._reposition_lock_tick then state._reposition_lock_tick = now_tick end
      if (now_tick - state._reposition_lock_tick) <= (C.PILL_REPOSITION_LOCK_TICKS or 750) then
        best_pill, best_pid = lp, state.goal.target_id   -- force the committed pill
        repos_locked = true
      else
        state._reposition_lock_tick = nil
      end
    else
      state._reposition_lock_tick = nil
    end
  else
    state._reposition_lock_tick = nil
  end

  local approved  = (state._repo_approved_pid ~= nil) and (best_pid ~= nil)
                    and (state._repo_approved_pid == best_pid)
  local my_voting = state._repo_my_vote and best_pid and state._repo_my_vote.pid == best_pid

  -- Finishing a swap: if our committed reposition pill is already DEAD (we shot
  -- it down), the reposition is EARNED and we're in the pickup/place phase —
  -- driven by the held capture_pill goal. STOP offering a fresh reposition here,
  -- or the lock releases on death and best_pill jumps to the next live pill,
  -- chaining the bot into a swap it never earned (and re-locking it next tick).
  local finishing_swap = false
  if state.goal and state.goal.kind == "capture_pill" and state.goal.reposition
     and state.goal.target_id then
    local tp = world.pills[state.goal.target_id]
    finishing_swap = (tp and tp.owner == "friendly" and (tp.health or 0) <= 0) or false
  end

  -- Reject reasons (the explicit one is "no friendly pills at all").
  local reject = nil
  if finishing_swap then
    reject = "finishing_swap"   -- killed our pill; let the pickup/place complete
  elseif not best_pill then
    -- Distinguish "back line is at/under target" (the common, healthy case)
    -- from genuinely having no friendly pills, so the pool viz reads clearly.
    reject = (back_surplus <= 0) and "no_back_surplus" or "no_team_pills"
  elseif not repos_locked and state._reposition_cooldown_tick
         and ((state.tick or 0) - state._reposition_cooldown_tick)
             < (C.PILL_REPOSITION_COOLDOWN_TICKS or 0) then
    -- Per-bot rate limit: we just finished (or bailed on) a reposition. Hold off
    -- starting a fresh one for PILL_REPOSITION_COOLDOWN_TICKS so a single tank
    -- doesn't churn reposition after reposition. (A committed/locked reposition
    -- is exempt above — repos_locked — so an in-progress swap still completes.)
    reject = "cooldown"
  elseif (info.shells or 0) < (C.PILL_REPOSITION_MIN_SHELLS or 15)
         and not (repos_locked and (info.shells or 0) > 0) then
    -- Repositioning means shooting our own pill down to 0 to pick it up, then
    -- replacing it — pointless to START if we can't afford to kill it. But once
    -- we've COMMITTED (it's the current reposition goal, repos_locked), see it
    -- through unless ammo is truly 0 — bailing on low_ammo mid-reposition leaves
    -- our pill half-dead and the swap unfinished.
    reject = "low_ammo"
  elseif (info.carried_pills or 0) >= 2 then
    -- One carried pill (the PLACE_HOLD_UTIL utility reserve) doesn't block a
    -- reposition — see can_carry_now in reposition_vote.lua. Two+ = hands full.
    reject = "carrying"
  elseif info.man_status ~= C.LGM_INTANK then
    reject = "lgm_busy"
  elseif info.inboat then
    reject = "inboat"
  elseif state.phase == "opening" then
    reject = "opening"   -- need pills in place during the opening
  elseif not repos_locked and not approved and not my_voting then
    -- Win-then-vote pacing: a just-failed team vote or a very recently executed
    -- move blacks out fresh proposals. If we can't open a vote, don't win the
    -- pool (else we'd sit on the pill unable to act). Mirrors reposition_vote's
    -- OPEN pacing gates.
    if state._repo_fail_tick
       and (now_tick - state._repo_fail_tick) < (C.REPOSITION_VOTE_FAIL_COOLDOWN or 1500) then
      reject = "vote_cooldown"
    elseif state._repo_last_seen_tick
       and (now_tick - state._repo_last_seen_tick) < (C.REPOSITION_VOTE_RECENT_MEMORY_TICKS or 1500) then
      reject = "recent_move"
    end
  end

  if reject then
    local mx = best_pill and best_pill.mx or tmx
    local my = best_pill and best_pill.my or tmy
    return {
      cost = REPOSITION_REJECT_COST,
      _reject = reject,
      goal = { kind = "capture_pill", mx = mx, my = my,
               wx = U.m2w(mx), wy = U.m2w(my),
               target_id = best_pid or 0, reposition = true },
      desc = BRAIN_POOL_VIZ and string.format(
             "REJECT{%s} | balance back %d/%d front %d/%d aggro %d/%d",
             reject, counts.back, targets.back, counts.front, targets.front,
             counts.aggro, targets.aggro) or "",
    }
  end

  -- Win-then-vote cost:
  --   * locked / vote-APPROVED → fixed LOCK cost so the committed swap wins and
  --     completes without churn (only sub-lock survival goals preempt);
  --   * otherwise (a BID)      → the HONEST total (position + travel). It
  --     competes in the pool on merit; only if it WINS does reposition_vote open
  --     a team vote. The destructive shoot is separately gated on approval.
  local cost
  if repos_locked or approved then
    cost = C.PILL_REPOSITION_LOCK_COST or 30
  else
    cost = math.max(1, best_total)
  end

  local bc = best_cand or {}
  return {
    cost = cost,
    -- Reposition = capture_pill on our OWN pill: drive up to it, the reposition
    -- shoot substate fires until it's dead, then the normal dead-pill capture
    -- pickup + place re-drop runs. awaiting_vote is true on a BID (not yet
    -- approved) — the destructive shoot is gated on approval in steering.lua.
    goal = { kind = "capture_pill", mx = best_pill.mx, my = best_pill.my,
             wx = U.m2w(best_pill.mx), wy = U.m2w(best_pill.my),
             target_id = best_pid, reposition = true,
             awaiting_vote = not (repos_locked or approved) },
    desc = BRAIN_POOL_VIZ and string.format(
           "%s{%.0f} reposition pill#%d@(%d,%d) cat=%s | base%.0f -surp%.0f -card%.0f +basep%.0f +act%.0f +tank%.0f +trav%.0f"..
           "||list EVERY friendly pill, only BACK eligible; lower=more worth moving. if this BID wins the pool a team vote opens; the shoot is gated on approval. balance back %d/%d front %d/%d aggro %d/%d",
           (repos_locked and "LOCK" or (approved and "APPROVED" or "BID")),
           cost, best_pid, best_pill.mx, best_pill.my, tostring(bc.cat),
           C.PILL_REPOSITION_BASE_COST or 500, -(bc.surp or 0), -(bc.card or 0),
           (bc.base or 0), (bc.act or 0), (bc.tank or 0), (bc.travel or 0),
           counts.back, targets.back, counts.front, targets.front, counts.aggro, targets.aggro) or "",
  }
end

-- Run ONLY the heavy reposition position scan and cache it (state._repo_score /
-- _repo_topN / _repo_candidate). Called by init.lua's quiet-tick scheduler so the
-- O(pills^2) coverage loop stays off the already-busy replan tick.
function M.rescan_reposition(state, world, info)
  eval_reposition_pill(state, world, info,
                       bit.rshift((info.tankx or 0), 8), bit.rshift((info.tanky or 0), 8),
                       info.inboat, info.shells, true)
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
  [10] = "reposition",
  [12] = "wait_for_lgm",
  [13] = "kill_lgm",
}

-- Reverse map: actual goal.kind → pool index, for looking up cost_cache
-- entries by candidate.  Note pool 1 (refuel) and pool 8 (place_strategic)
-- have different UI labels than their goal.kind values.
-- Pool 10 in the JSON is the WINNERS section, 11 is offensive_build, 12 is
-- wait_for_lgm, 13 is kill_lgm — those four render as strips below the
-- main 2x5 grid.
local KIND_TO_POOL = {
  refuel_at_base = 1, defend_pill = 2, capture_base = 3, capture_pill = 4,
  repair_pill = 5, attack_pill = 6, attack_base = 7,
  place_pill_strategic = 8, attack_tank = 9, wait_for_lgm = 12,
  kill_lgm = 13,
}

-- Pool DISPLAY name → goal.kind, for the two labels that differ (see the
-- KIND_TO_POOL note above). Lets the panel/grid renderers ask
-- suicider_cost_mult the same question goal_selection asked, so a suicider's
-- displayed `weighted` cost and row ordering match the cost actually competed.
local POOL_NAME_TO_KIND = {
  refuel = "refuel_at_base", place_strategic = "place_pill_strategic",
}
local function suicider_mult_for_pool(state, pname)
  return suicider_cost_mult(state, POOL_NAME_TO_KIND[pname] or pname)
end

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

-- defend_pill-vs-attack_pill hysteresis tiers (see the hysteresis block in
-- finalize goal_selection). defend_pill is a reactive defense — a friendly pill
-- is being destroyed NOW — so it should only be held back from preempting an
-- attack_pill by the take's REAL invested loss.
--   ATK_SHOOTING_SUBS → at the standoff aiming or firing: shells + position at
--     risk → FULL hysteresis (don't abandon a live shot).
--   ATK_BUILD_SUBS → only building the shield (LGM/trees, no shells, recoverable)
--     → MODERATE hysteresis.
--   anything else (approach / plan_position / disengage / swerve …) → FREE.
local ATK_SHOOTING_SUBS = {
  charge=true, engage=true, ws_engage=true, aim=true, rush=true,
  in_range_position=true, in_range_aim_pre=true, in_range_aim=true,
  in_range_aim_finetune=true, shoot_pill=true,
}
local ATK_BUILD_SUBS = {
  build_walls=true, gather_trees=true,
  ws_prebuild=true, ws_prewait=true, ws_advance=true, ws_retreat=true, ws_rebuild=true,
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

-- Danger-aware wait spot for wait_for_lgm. Parking in place under pill
-- fire while the LGM walks home is how tanks die for nothing
-- (20260703_230556 t=2944: armour 10, hostile shells landing 2 tiles away,
-- tank motionless on wait_for_lgm). When the tank's tile is dangerous,
-- pick a SAFE tile that balances our danger-weighted travel against the
-- LGM's extra walk (WAIT_LGM_BETA_COST per tile): tiles toward the LGM win
-- when that side is safe (meet it + exit danger in one move), tiles away
-- from the pill win when it isn't. Exceptions:
--   * LGM arriving within WAIT_LGM_HOLD_ARRIVAL_TICKS → hold (moving drags
--     the pickup point and resets its path) — UNLESS low armour AND under
--     fire, where holding is lethal.
--   * previous wait spot still safe → sticky (no churn while driving there).
-- Returns wait_mx, wait_my, reason.
local function pick_wait_spot(state, info, tmx, tmy)
  local ok_thresh = C.WAIT_LGM_DANGER_OK or 0
  local d_here = threat.at(tmx, tmy) or 0
  local under_fire = state.perc and state.perc.under_fire or false
  if d_here <= ok_thresh and not under_fire then
    state._wait_spot = nil
    return tmx, tmy, "safe_here"
  end
  local b = state.builder
  local critical = (info.armour or 40) <= (C.ARMOUR_LOW or 15) and under_fire
  if not critical and b and b.lgm_nearby
     and (b.lgm_arrival_ticks or 1e9) <= (C.WAIT_LGM_HOLD_ARRIVAL_TICKS or 150) then
    return tmx, tmy, "lgm_imminent"
  end
  local ws = state._wait_spot
  if ws and (threat.at(ws.mx, ws.my) or 0) <= ok_thresh
     and U.mdist(tmx, tmy, ws.mx, ws.my) <= 12
     and ((state.tick or 0) - (ws.tick or 0)) <= 500 then
    return ws.mx, ws.my, "sticky"
  end
  local lmx = bit.rshift((info.man_x or info.tankx), 8)
  local lmy = bit.rshift((info.man_y or info.tanky), 8)
  local boat_flag = info.inboat and 1 or 0
  local beta = C.WAIT_LGM_BETA_COST or 6
  local best_mx, best_my, best_s
  local fb_mx, fb_my, fb_s   -- fallback: least-bad tile if nothing is fully safe
  local dbg = BRAIN_DEBUG_MODE and {} or nil
  local function consider(cx, cy)
    cx, cy = U.mclamp(cx), U.mclamp(cy)
    if cx == tmx and cy == tmy then return end
    local trav = smart_cost(KIND_NORMAL, tmx, tmy, cx, cy, boat_flag,
                            info.shells or 32, info.trees or 0,
                            info.mines or 0, info.armour or 40)
    if not trav or trav >= 1e8 then
      if dbg then dbg[#dbg + 1] = { mx = cx, my = cy, rej = "unreachable" } end
      return
    end
    local dgr = threat.at(cx, cy) or 0
    local s = trav + beta * U.mdist(lmx, lmy, cx, cy)
    if dgr <= ok_thresh then
      if not best_s or s < best_s then best_s, best_mx, best_my = s, cx, cy end
      if dbg then dbg[#dbg + 1] = { mx = cx, my = cy, score = s } end
    else
      local fs = s + dgr * (C.WAIT_LGM_DANGER_W or 25)
      if not fb_s or fs < fb_s then fb_s, fb_mx, fb_my = fs, cx, cy end
      if dbg then dbg[#dbg + 1] = { mx = cx, my = cy, score = fs, rej = "danger" } end
    end
  end
  consider(lmx, lmy)   -- the LGM's own tile: the full meet
  for _, r in ipairs({ 3, 6, 9 }) do
    for i = 0, 7 do
      local ang = i * math.pi / 4
      consider(tmx + math.floor(r * math.sin(ang) + 0.5),
               tmy - math.floor(r * math.cos(ang) + 0.5))
    end
  end
  if not best_mx and fb_mx then best_mx, best_my = fb_mx, fb_my end
  if best_mx then
    state._wait_spot = { mx = best_mx, my = best_my, tick = state.tick }
    if dbg then
      state._wait_spot_viz = { tick = state.tick, wmx = best_mx, wmy = best_my,
                               lmx = lmx, lmy = lmy, d_here = d_here, cands = dbg }
    end
    return best_mx, best_my, "moved"
  end
  return tmx, tmy, "no_alternative"
end

-- Inject a low-cost wait_for_lgm candidate so the bot prefers to wait
-- when the LGM is out (e.g. farming) and we'd otherwise wander off.
-- Skipped during goals that ARE actively driving the LGM to do
-- something (build_walls, ws_*, pill_place, repair_pill, capture_pill,
-- rescue_lgm) so we don't preempt a real LGM-using mission.
local function eval_wait_for_lgm(state, info)
  -- Carrying a pillbox forces this candidate ON (even with the master toggle off)
  -- and at a higher priority below — a tank holding a pill should wait for its LGM
  -- to return so it can build quickly, not wander off.
  local carrying = info and (info.carried_pills or 0) >= 1 or false
  if not (C.WAIT_FOR_LGM_ENABLED or carrying) then return nil end
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
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local cost = carrying and (C.WAIT_FOR_LGM_COST_CARRYING or 20) or (C.WAIT_FOR_LGM_COST or 50)
  -- Danger-aware wait spot (see pick_wait_spot above): usually the tank's
  -- own tile, but a safe tile toward/away from the LGM when parked ground
  -- is under fire. Steering drives to goal.mx/my then stands still.
  local wmx, wmy, wreason = pick_wait_spot(state, info, tmx, tmy)
  return {
    cost = cost,
    goal = { kind = "wait_for_lgm", mx = wmx, my = wmy,
             wx = U.m2w(wmx), wy = U.m2w(wmy) },
    desc = BRAIN_POOL_VIZ and string.format("wait_for_lgm@(%d,%d) lgm=(%d,%d) cost=%d spot=%s%s",
                         wmx, wmy,
                         bit.rshift((info.man_x or 0), 8), bit.rshift((info.man_y or 0), 8), cost, wreason,
                         (wmx ~= tmx or wmy ~= tmy) and string.format(" (tank@%d,%d)", tmx, tmy) or "") or "",
    cands = {
      { id = 0, mx = wmx, my = wmy, cost = cost,
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
local FINALIZE_POOLS = { 2, 8, 9, 10 }  -- defend_pill, place_pill, attack_tank, reposition_pill

-- Filter functions for each incremental pool.
-- Return true if the object is a valid candidate.
-- filter_refuel: returns nil if the base qualifies for scoring, or a
-- reject descriptor table { reason = "<short>", remaining = <ticks> }
-- when it should be SHOWN in the pool grid but greyed out (so the
-- user can see "this base exists, here's why we're not picking it").
-- The only HARD reject (drop from queue entirely) is "hostile" — we
-- never refuel at an enemy base.
local function filter_refuel(obj, state, info)
  -- TEST AID: a never-refuel bot (state.test_never_refuel, rolled per bot
  -- from TEST_NEVER_REFUEL_CHANCE) rejects EVERY refuel candidate so the
  -- ammo-deprivation/decoy path can be exercised without engineering a
  -- base-starved map. Shows as reject "test_no_refuel" in the pool grid.
  if state and state.test_never_refuel then
    return { reason = "test_no_refuel" }
  end
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
  -- Danger is NOT a reject: it's already folded into the path cost via
  -- REFUEL_DANGER_WEIGHT (score = travel + danger * weight), so a risky
  -- base just scores higher rather than dropping out of the pool. Hard-
  -- rejecting on danger could leave a low-armour bot with no refuel
  -- option when every base is under pill fire.
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
  -- Carried-but-in_tank-unset guard: a pill the team is carrying (carrier /
  -- _synth_carry stamped by world.sync_ally_carried from a carry= advert) is in
  -- flight even if the in_tank flag itself didn't get set. Never a ground take.
  if obj.carrier or obj._synth_carry then return { reason = "in_tank" } end
  -- kill_claimed: a blitz member has claimed this fresh kill and is going HARD
  -- for it (Override 3b). Don't contest — recognize the claim and move on. The
  -- claimer itself never reaches this filter (its Override 3b force-wins before
  -- pool competition), so this only rejects non-grabbers.
  if state and state._kill_claimed and state._kill_claimed[U.mkey(obj.mx, obj.my)] then
    return { reason = "kill_claimed" }
  end
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
  -- health < MAX includes 0-HP (dead) friendly pills: those rebuild IN PLACE
  -- (the LGM walks out with wood). 0-HP is the 4× wood tier, so we additionally
  -- require enough trees for a meaningful rebuild — else the engine only
  -- partial-repairs and we've wasted the trip. Alive-damaged keeps trees>0.
  -- REPAIR_FIX off → 1.90-beta1: alive-damaged only (health > 0). On → also
  -- accept 0-HP friendly pills for rebuild-in-place (with the dead-pill wood gate).
  if not (obj.owner == "friendly"
          and obj.health < C.PILLS_MAX_HEALTH
          and (C.REPAIR_FIX_ENABLED or obj.health > 0)) then return false end
  if not (info.man_status == C.LGM_INTANK and info.trees > 0) then return false end
  if C.REPAIR_FIX_ENABLED and obj.health == 0 then
    print2(string.format("REPAIR_DEAD_FILTER t=%d pill@(%d,%d) dead trees=%d/%d %s", state and state.tick or 0, obj.mx, obj.my, info.trees or 0, C.REPAIR_DEAD_MIN_TREES or 4, ((info.trees or 0) < (C.REPAIR_DEAD_MIN_TREES or 4)) and "REJECT(low_trees)" or "enqueue"))
    if (info.trees or 0) < (C.REPAIR_DEAD_MIN_TREES or 4) then return false end
    -- Capture outranks rebuild: a corpse that capture_pill can take (on the
    -- ground, unclaimed, unblocked, fresh) is a FREE pill — rebuilding it in
    -- place makes it alive and un-grabbable, wasting both the kill and the
    -- wood. Repair dispatch only fires within LGM range anyway, so any
    -- rebuildable corpse is by definition close enough to just drive over.
    -- Rebuild-in-place stays allowed only when capture itself rejects the
    -- pill for a reason that also rules out OUR grab (blocked tile / stale
    -- memory) — an ally's kill_claimed corpse is their grab, not our rebuild.
    -- (20260703_210207 t=6997: repair won a replan against an empty pool 4
    -- and the LGM rebuilt the bot's own capture target 2 tiles away.)
    local cap_reject = filter_capture_pill(obj, state)
    if not cap_reject then return false end
    if cap_reject.reason ~= "blocked" and cap_reject.reason ~= "stale" then return false end
  end
  if state and state.blocked then
    local bk = U.mkey(obj.mx, obj.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return false end
  end
  -- Never repair/rebuild the pill we're currently CAPTURING (any capture —
  -- reposition pickup or plain dead-pill grab): rebuilding it makes it alive
  -- and un-capturable, wasting the shells that killed it (20260703_210207
  -- t=6997: bot rebuilt its own reposition corpse mid-pickup).
  if state and state.goal and state.goal.kind == "capture_pill"
     and state.goal.mx == obj.mx and state.goal.my == obj.my then
    return false
  end
  -- Reposition-target guard (tile-keyed TTL map maintained every tick by
  -- reposition_vote.update): covers OUR committed/approved reposition target
  -- and any ally-broadcast repos target, and — via the TTL — bridges the
  -- one-tick vacuum where the reposition goal drops (finishing_swap) before
  -- pool 4 re-scores the freshly dead pill for pickup.
  if state and state._repos_guard then
    local gu = state._repos_guard[obj.my * 256 + obj.mx]
    if gu and (state.tick or 0) < gu then return false end
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
  -- NOTE: deliberately NO staleness hard-skip here. A pill we haven't re-seen
  -- in a while stays in the pool (just cost-penalized in attack_pill_adjustments)
  -- so it's never permanently dropped — the old `(now-last_seen)>STALE_SKIP`
  -- return-false permanently hid pills whenever a death froze last_seen.
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
-- capture_pill DIRECT-ROUTE probe
--
-- The pill we are going for is DEAD, so it radiates no danger of its own.
-- The danger-weighted slate still routes us around every OTHER pill's fire
-- field, which on a body two tiles behind a hot pill means a long detour for
-- a drive we would have survived. So for the pill we are actually going for,
-- ask the direct question:
--   1. A* with danger_scale = 0 → the true DIRECT route and its cost.
--   2. wsim that path → "do I die driving it?"
--   3. survived → keep the direct route and its cost.
--      killed   → re-ask A* with danger ON and use that route and cost.
--
-- COST CONTROL. An A* is ~7-10 ms and the per-bot think budget is ~12 ms, so
-- this can never be a per-candidate-per-tick cost. The bounds, in order:
--   * only the FOCUS pill is probed (see the caller in step_eval_queue) —
--     every other candidate keeps the cheap Dijkstra lookup for ranking.
--   * at most ONE A* per bot per tick. Step 3's danger re-ask runs on the
--     NEXT tick; the entry sits at mode="pending_danger" until then and the
--     slate cost stands in the meantime.
--   * the verdict is cached per pill TILE and reused until it goes stale:
--     older than CAPTURE_ROUTE_TTL, the tank moved CAPTURE_ROUTE_MOVE_TILES,
--     or threat rebuilt the danger grid underneath it.
--   * skipped entirely below CAPTURE_ROUTE_MIN_TIER.
-- Worst case per bot per tick: 1 A* + 1 wsim.
-- =========================================================================
local function capture_route_stale(ent, now, tmx, tmy)
  if not ent then return true end
  if (now - (ent.tick or 0)) > (C.CAPTURE_ROUTE_TTL or 100) then return true end
  -- A threat rebuild moves the danger grid both A* runs were measured against.
  if (threat.last_rebuild_tick or 0) > (ent.tick or 0) then return true end
  local moved = math.abs(tmx - (ent.tmx or tmx)) + math.abs(tmy - (ent.tmy or tmy))
  if moved >= (C.CAPTURE_ROUTE_MOVE_TILES or 6) then return true end
  return false
end

-- One strict-A* run at the given danger weighting. danger_scale is a GLOBAL
-- knob, so it is set / used / restored to 1.0 on EVERY exit including an error
-- — the same discipline nearest_where and attack.lua use. Leaving it set would
-- silently rescale every other cost query for the rest of the tick.
-- Returns (cost, path) with path a flat {x1,y1,x2,y2,...} tile list, or nil.
local function capture_route_astar(tmx, tmy, dx, dy, info, danger_scale)
  local boat = info.inboat and 1 or 0
  cpf.set_config("danger_scale", danger_scale)
  local ok, cost = pcall(cpf.cost_to_astar, tmx, tmy, dx, dy, boat,
                         info.shells or 32, info.trees or 0, info.mines or 0,
                         info.armour or 40, C.CAPTURE_ROUTE_ASTAR_BUDGET or 4000,
                         false)
  local path
  if ok and cost and cost < 1e29 then
    -- trace_last_search reads the parent chain the cost_to call just left
    -- behind; it must run before anything else touches the pathfinder.
    local ok2, p = pcall(cpf.trace_last_search, dx, dy)
    if ok2 then path = p end
  end
  cpf.set_config("danger_scale", 1.0)
  if not ok or not cost then return nil end
  return cost, path
end

-- Trim the leading waypoint when it is the tile we are standing on (the A*
-- trace starts AT the source), matching what wsim_evaluate_goal does to the
-- Dijkstra path before handing it to the sim.
local function capture_route_trim(path, tmx, tmy)
  if not path or #path < 2 then return path end
  if path[1] ~= tmx or path[2] ~= tmy then return path end
  local t = {}
  for i = 3, #path do t[i - 2] = path[i] end
  return t
end

local function capture_route_probe(state, world, info, pill, pid, tmx, tmy)
  if not C.CAPTURE_ROUTE_DIRECT then return end
  if (state._capacity_tier or 10) < (C.CAPTURE_ROUTE_MIN_TIER or 6) then return end
  local now = state.tick or 0
  if not state.capture_route then state.capture_route = {} end
  local key = pill.my * 256 + pill.mx
  -- RANGE GATE, before any A* work: only probe once we're reasonably close.
  -- Same tank→pill Manhattan measure the intercept and free-pill terms below
  -- use, so "10 tiles" means the same thing everywhere in pool 4. We DELETE any
  -- verdict instead of caching a "too far" one — both because a verdict earned
  -- while we were close must stop overriding the slate the moment we drive away,
  -- and because leaving nothing behind is what lets a pill that comes back into
  -- range get probed the same tick it crosses (capture_route_stale(nil) is true,
  -- so the probe runs immediately instead of honouring a cached refusal).
  local gate_dist = U.mdist(tmx, tmy, pill.mx, pill.my)
  if gate_dist > (C.CAPTURE_ROUTE_MAX_TILES or 10) then
    if state.capture_route[key] then
      print2(string.format("CAPTURE_ROUTE t=%d pill#%s NOT PROBED — %d tiles away (> %d), dropping stale verdict",
        now, tostring(pid), gate_dist, C.CAPTURE_ROUTE_MAX_TILES or 10))
      state.capture_route[key] = nil
    end
    return
  end
  local ent = state.capture_route[key]
  local fresh = not capture_route_stale(ent, now, tmx, tmy)

  -- Destination: the same cheapest-adjacent tile compute_pool4_cost measures
  -- to, so the A* number is comparable with the slate numbers the other
  -- candidates carry. Falls back to the pill tile (a dead pill carries no
  -- impassable overlay, so its own tile is a legal destination).
  local dx, dy = pill.mx, pill.my
  local _, ax, ay = cpf.cheapest_adjacent_dij(KIND_NORMAL, pill.mx, pill.my,
                                              info.inboat and 1 or 0)
  if ax then dx, dy = ax, ay end

  -- Second half of a split probe: the direct route was lethal last tick, so
  -- this tick buys the danger-weighted A*. One A* per think, never two.
  if fresh and ent.mode == "pending_danger" then
    local cost, path = capture_route_astar(tmx, tmy, dx, dy, info, 1.0)
    ent.mode = "danger"
    ent.cost = (cost and cost < 1e29) and cost or nil
    ent.npath = path and __idiv(#path, 2) or 0
    if not ent.cost then ent.mode = "none" end
    print2(string.format(
      "CAPTURE_ROUTE t=%d pill#%s dest=(%d,%d) DANGER-A*=%s (direct %.0f was LETHAL, %d dmg) mode=%s",
      now, tostring(pid), dx, dy,
      ent.cost and string.format("%.0f", ent.cost) or "INF",
      ent.direct_cost or -1, ent.damage or 0, ent.mode))
    return
  end
  if fresh then return end

  -- Step 1+2: the DIRECT run (danger off) and the survivability question.
  local cost, path = capture_route_astar(tmx, tmy, dx, dy, info, 0)
  if not cost or cost >= 1e29 or not path or #path < 2 then
    -- No direct route inside the A* budget (walled in, or simply too far).
    -- Record the miss so we don't re-probe every tick; the slate cost stands.
    state.capture_route[key] = { id = pid, tick = now, tmx = tmx, tmy = tmy,
                                 mode = "none" }
    print2(string.format("CAPTURE_ROUTE t=%d pill#%s dest=(%d,%d) DIRECT-A*=INF — slate cost stands",
      now, tostring(pid), dx, dy))
    return
  end
  path = capture_route_trim(path, tmx, tmy)

  local killed, damage = false, 0
  if C.WSIM_ENABLED and path and #path >= 2 then
    wsim.snapshot(world, info, path, nil)
    local r = wsim.run(C.WSIM_MAX_TICKS)
    killed = r.killed and true or false
    damage = r.damage or 0
  end

  ent = { id = pid, tick = now, tmx = tmx, tmy = tmy,
          direct_cost = cost, killed = killed, damage = damage,
          npath = __idiv(#path, 2) }
  if killed then
    -- Lethal direct run: hand the decision to the danger-weighted A* next tick.
    ent.mode = "pending_danger"
  else
    ent.mode = "direct"
    ent.cost = cost
  end
  state.capture_route[key] = ent
  print2(string.format(
    "CAPTURE_ROUTE t=%d pill#%s dest=(%d,%d) DIRECT-A*=%.0f path=%d wsim dmg=%d killed=%s -> %s",
    now, tostring(pid), dx, dy, cost, ent.npath, damage, tostring(killed), ent.mode))
end

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
  -- Tank→pill Manhattan distance. Hoisted above the DIRECT-ROUTE override
  -- because the probe's range gate measures the same way; the intercept and
  -- free-pill terms below reuse this one value.
  local our_dist = U.mdist(tmx, tmy, obj.mx, obj.my)
  -- DIRECT-ROUTE override (capture_route_probe, above). For the FOCUS pill the
  -- probe has already decided between the danger-free direct A* run and the
  -- danger-weighted one; when it holds a verdict its cost REPLACES the slate
  -- lookup, so the pool ranks — and the goal commits — on the route we would
  -- actually drive. Other candidates never have an entry and are untouched.
  -- _route_far is set when there is no verdict BECAUSE the pill is out of the
  -- probe's range, so the panel can say "not probed" rather than leaving it
  -- looking like the direct route was tried and lost.
  local _route_dmg, _route_far = 0, nil
  local _rent = state.capture_route and state.capture_route[obj.my * 256 + obj.mx]
  if _rent and _rent.cost and _rent.cost < 1e29
     and (_rent.mode == "direct" or _rent.mode == "danger") then
    dist_raw     = _rent.cost
    dist_method  = (_rent.mode == "direct") and "astar_direct" or "astar_danger"
    _route_dmg   = _rent.damage or 0
  elseif our_dist > (C.CAPTURE_ROUTE_MAX_TILES or 10) then
    _route_far = our_dist
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
  -- our_dist computed above (shared with the DIRECT-ROUTE range check).
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
  -- "Free pill" value bonus (scaled, not binary): a close, safe dead pill is
  -- worth a flat CAPTURE_FREE_PILL_VALUE — subtract it so the grab lands near
  -- MIN_COST and outranks routine goals, including a near-zero-cost on-base
  -- refuel top-off (a multiplicative discount can never get under that floor:
  -- 20260703_210207 t=5747, refuel 4.5 beat capture 31.6×0.5=15.8 and the
  -- enemy took both free pills). The bonus scales to zero by a "badness" =
  -- max(distance-over, danger), each ramping to full price quickly.
  --   dist_bad: 0 within shooting reach × RANGE_MULT, → 1 over DIST_FALLOFF tiles.
  --   danger_bad: danger_val (pillbox crossfire + enemy-tank radius, already
  --     LOS/blocker/distance-reduced) / DANGER_FALLOFF. One covering pill keeps a
  --     strong bonus; ~two calm pills wash it out. our_dist is tank→pill
  --     Manhattan (same measure as the intercept term above).
  local grab_range = C.TANK_COMBAT_ENGAGE_RANGE * C.CAPTURE_FREE_PILL_RANGE_MULT
  local dist_bad   = math.min(1, math.max(0, our_dist - grab_range) / C.CAPTURE_FREE_PILL_DIST_FALLOFF)
  local danger_bad = math.min(1, danger_val / C.CAPTURE_FREE_PILL_DANGER_FALLOFF)
  local badness    = math.max(dist_bad, danger_bad)
  local free_bonus = C.CAPTURE_FREE_PILL_VALUE * (1 - badness)
  if free_bonus > 0 then
    c = math.max(C.CAPTURE_FREE_PILL_MIN_COST, c - free_bonus)
  end
  return c, dist_raw, dist_score, danger_val, intercept, _lgm_mult, dist_method, free_bonus, _route_dmg, _route_far
end

-- Public: the capture_pill (pool 4) score for a SPECIFIC pill, used by the
-- fresh-kill handoff so blitz members compare the same balanced metric
-- (distance + danger + wound + intercept) the goal selector already trusts,
-- instead of raw path distance. The lowest score grabs. Large sentinel if the
-- pill is gone/unreachable.
function M.kill_pickup_score(state, world, info, pill)
  if not pill then return 1e30 end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local c = compute_pool4_cost(state, world, info, pill, tmx, tmy)
  return c or 1e30
end

-- build_eval_queue — called at the start of each replan cycle.
-- Iterates all incremental pools, applies filters, and builds a flat
-- work queue of candidates to evaluate (2 per tick).
-- =========================================================================
-- =========================================================================
-- rescore_nearby_bases — force a strict-A* cost for every live base within
-- `radius` tiles, overwriting the partial pool entries the imminent replan
-- reads. The incremental base eval (update_pool_cache) costs bases with
-- smart_cost_dij_only — dijkstra-ONLY, no A* fallback — so during the opening
-- cold-start a base the dijkstra surface hasn't reached yet sits at INF and
-- can't be picked, even when it's right next to the tank. Called ONE TICK
-- BEFORE each replan (opening phase only) so the decision ranks nearby bases on
-- real A* costs instead of a not-yet-ready surface. Updates pool_partial[3]
-- (capture_base) and [7] (attack_base): refreshes each nearby candidate's cost
-- and re-derives the pool best.
-- =========================================================================
function M.rescore_nearby_bases(state, world, info, radius)
  if not (world and world.bases and info and state.pool_partial) then return end
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
  local boat   = info.inboat and 1 or 0
  local shells = info.shells or 32
  local trees  = info.trees or 0
  local mines  = info.mines or 0
  local armour = info.armour or 40
  local now    = state.tick or 0
  local rescored = 0
  for _, pool_idx in ipairs({ 3, 7 }) do
    local pr = state.pool_partial[pool_idx]
    if pr and pr.candidates then
      for _, cand in ipairs(pr.candidates) do
        if not cand.reject and U.mdist(tmx, tmy, cand.mx, cand.my) <= radius then
          -- Route to the cheapest adjacent tile for a live base, same as the
          -- incremental eval does, then pay a strict A* (bypassing dij-only).
          local obj = cand.obj or world.bases[cand.id]
          local dx, dy = cand.mx, cand.my
          if obj and (obj.health or 0) > 0 then
            local _, ax, ay = cpf.cheapest_adjacent(KIND_NORMAL, tmx, tmy,
              cand.mx, cand.my, boat, shells, trees, mines, armour)
            if ax then dx, dy = ax, ay end
          end
          local c = cpf.cost_to_astar(tmx, tmy, dx, dy, boat,
            shells, trees, mines, armour, 4000, false)
          if c and c < 1e29 then
            local old = cand.cost
            cand.cost = c
            rescored = rescored + 1
            -- Bump the cost-cache timestamp too, so the pool-grid "age" shows
            -- this as a fresh eval (not the stale last-dijkstra-sweep tick).
            local ck = pool_idx .. ":" .. cand.id
            local entry = state.cost_cache and state.cost_cache[ck]
            if entry then entry.cost = c; entry.tick = now; entry._age = 0 end
            if BRAIN_DEBUG_MODE then print2(string.format("  BASE_RESCORE_HIT pool=%d base#%s (%d,%d) %s -> %.0f", pool_idx, tostring(cand.id), cand.mx, cand.my, old and (old >= 1e29 and "INF" or string.format("%.0f", old)) or "?", c)) end
          end
        end
      end
      -- Re-derive the pool best from the refreshed candidate costs.
      local bc, bi, bo = math.huge, nil, nil
      for _, cand in ipairs(pr.candidates) do
        if not cand.reject and cand.cost and cand.cost < bc then
          bc, bi, bo = cand.cost, cand.id, (cand.obj or world.bases[cand.id])
        end
      end
      if bi then pr.best_cost, pr.best_id, pr.best_obj = bc, bi, bo end
    end
  end
  if BRAIN_DEBUG_MODE then print2(string.format("BASE_RESCORE t=%d rescored=%d bases within %d tiles (strict A* over dij-only INF)", now, rescored, radius)) end
end

function M.build_eval_queue(state, world, info)
  -- Fresh queue → not swept yet (warm_ready's sparse-map escape requires a full
  -- sweep of the CURRENT queue). Harmless once _warm_ready has latched.
  state._eval_swept = false
  local queue = {}
  local now = state.tick or 0
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)

  -- Fresh-kill claim recognition: any dead pill a blitz member is committing
  -- to (broadcast `kg`) is OFF-LIMITS to our normal capture_pill pool — the
  -- lowest-score claimer goes HARD on it (Override 3b) and everyone else must
  -- not contest. Keyed by tile so filter_capture_pill can reject it. Our OWN
  -- claim (if any) is handled by Override 3b, which sets the goal as a direct
  -- result and bypasses the pool entirely — so excluding self here is right.
  state._kill_claimed = state._kill_claimed or {}
  for k in pairs(state._kill_claimed) do state._kill_claimed[k] = nil end
  if C.KILL_PICKUP_ENABLED and ally_state.iter_active and info.player_number then
    local tdead = state.tank_dead_at
    for apn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
      -- Skip a DEAD claimer: its kg broadcast lingers in the slate for up to
      -- SQUAD_ALLY_MAX_AGE, but a dead bot can't grab anything until it
      -- respawns. Honoring it would leave the pill kill_claimed (rejected by
      -- everyone) even though the killer got shot — a wasted kill nobody picks
      -- up. Once the slot is dead-flagged, the pill drops back to a normal
      -- capture_pill candidate for whoever's still alive.
      local is_dead = tdead and tdead[apn] and tdead[apn] > (slot.last_tick or 0)
      if apn ~= info.player_number and not is_dead and slot.info and slot.info.kg then
        local pid = tonumber(slot.info.kg)
        local pp = pid and world.pills[pid]
        if pp and (pp.health or 0) == 0 and not pp.in_tank then
          state._kill_claimed[U.mkey(pp.mx, pp.my)] = pid
        end
      end
    end
  end

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
      U.set_blocked(state, U.mkey(tmx, tmy), now + 200, "refuel_base_useless")
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
  -- Also queue while a refuel_at_base goal is ACTIVE even if needs_refuel went
  -- false: we keep topping off to full, so the winners panel must still show the
  -- base we're refuelling at instead of going blank mid-top-off.
  if needs_refuel or (state.goal and state.goal.kind == "refuel_at_base") then
    for id, obj in pairs(world.bases) do
      local reject = filter_refuel(obj, state, info)
      if not reject or reject.reason ~= "hostile" then
        queue[#queue + 1] = { pool = 1, id = id, obj = obj, reject = reject }
        _diag_log_first_pool_add(1, "refuel", id, obj)
        if reject then
          print2(string.format("REFUEL_QUEUE base#%s @(%d,%d) FILTER-REJECT %s%s", tostring(id), obj.mx, obj.my, tostring(reject.reason), reject.remaining and (" rem=" .. reject.remaining) or ""))
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
    -- Deep-sea "bait" pills: a dead pill sitting on deep water can't be taken on
    -- foot (the tank/LGM would drown). Reject for capture_pill unless we're
    -- afloat. (perc.deepsea_pill_ids flags them; the bait_pill_marker viz.)
    if not reject and perc and perc.deepsea_pill_ids and perc.deepsea_pill_ids[id]
       and not (info and info.inboat) then
      reject = { reason = "deepsea_no_boat" }
    end
    if BRAIN_DEBUG_MODE and (obj.health or 0) == 0 then print2(string.format("CAPTURE_CAND t=%d id=%s @(%s,%s) hp=%s owner=%s in_tank=%s carrier=%s synth=%s last_seen=%s reject=%s", now, tostring(id), tostring(obj.mx), tostring(obj.my), tostring(obj.health), tostring(obj.owner), tostring(obj.in_tank), tostring(obj.carrier), tostring(obj._synth_carry), tostring(obj.last_seen), reject and reject.reason or "nil")) end
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
          local c, _draw, dscore, dval, intcpt, _lm4, _dm4, _free4, _rdmg4, _rfar4 =
            compute_pool4_cost(state, world, info, obj, tmx, tmy)
          state.cost_cache[ck] = {
            cost = c, raw = _draw, tick = now, _p = 4, _id = id,
            _mx = obj.mx, _my = obj.my,
            _ds = dscore, _dv = dval, _intcpt = intcpt,
            _dist_method = _dm4, _free = _free4,  -- _free = scaled value bonus subtracted
            _route_dmg = _rdmg4,                  -- wsim damage on the probed direct route
            _route_far = _rfar4,                  -- set = too far to probe, slate cost stands
          }
        end
      end
    end
  end

  -- Evict STALE capture-pill cache entries. The per-pill loop above only
  -- refreshes ids still present in world.pills AND not "alive". Two paths leave
  -- a stale low-cost dead-pill entry that finalize_pools could still select:
  --   (1) the pill came back ALIVE (recaptured / redeployed) — the "alive"
  --       branch skips it without clearing the old entry; and
  --   (2) the pill left world.pills entirely (picked up, carrier out of view,
  --       no carry= advert) — the loop never visits that id at all.
  -- Sweep cost_cache and drop any pool-4 entry whose LIVE pill is gone, alive,
  -- or known carried. Setting an existing field to nil mid-pairs() is safe.
  for ck, ce in pairs(state.cost_cache) do
    if ce._p == 4 then
      local lp = world.pills[ce._id]
      if (not lp) or (lp.health or 0) > 0 or lp.in_tank or lp.carrier or lp._synth_carry then
        if BRAIN_DEBUG_MODE then print2(string.format("CAPTURE_EVICT t=%d id=%s reason=%s cached=(%s,%s) cost=%.0f", now, tostring(ce._id), (not lp) and "gone" or (((lp.health or 0) > 0) and "alive" or "carried"), tostring(ce._mx), tostring(ce._my), ce.cost or -1)) end
        state.cost_cache[ck] = nil
      end
    end
  end

  -- Same sweep for the DIRECT-ROUTE verdicts (capture_route_probe). Entries are
  -- keyed by pill TILE, so a pill rebuilt alive on the same tile would otherwise
  -- inherit the dead body's route decision. Drop any verdict whose pill is gone,
  -- alive again, or carried; staleness by age/movement is handled in the probe.
  if state.capture_route then
    for rk, re in pairs(state.capture_route) do
      local lp = re.id and world.pills[re.id]
      if (not lp) or (lp.health or 0) > 0 or lp.in_tank or lp.carrier
         or (lp.my * 256 + lp.mx) ~= rk then
        state.capture_route[rk] = nil
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
      -- pp_blacklisted: sweep abandoned after repeated tick-budget kills —
      -- keep the pill out of the queue entirely for its cooldown window, or the
      -- winner is re-adopted and plan_position walks back into the same sweep.
      if filter_attack_pill(obj, state) and not attack.pp_blacklisted(state, id) then
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
        -- Close-out: keep a nearly-dead hostile base in the queue even when shells
        -- are below SHELLS_LOW (as long as we have any ammo), so eval_attack_base
        -- runs and can finish it instead of the bot peeling off to refuel.
        local closeout = (obj.health or 99) <= (C.ATTACK_BASE_CLOSEOUT_HEALTH or 3)
                         and info.shells > (C.SHELL_RESERVE or 0)
        if has_shells or is_current or closeout then
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
  if BRAIN_DEBUG_MODE then
    local pool_counts = {}
    for _, q in ipairs(queue) do
      pool_counts[q.pool] = (pool_counts[q.pool] or 0) + 1
    end
    print2(string.format("build_eval_queue: %d total  p1=%d p3=%d p4=%d p5=%d p6=%d p7=%d  shells=%d has_shells=%s",
      #queue,
      pool_counts[1] or 0, pool_counts[3] or 0, pool_counts[4] or 0,
      pool_counts[5] or 0, pool_counts[6] or 0, pool_counts[7] or 0,
      info.shells, tostring(has_shells)))
  end

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
local reject_with_breakdown   -- forward decl (mutual recursion w/ get_formula_inner)
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
    -- Joinable-blitz case: an ally has an OPEN blitz call on this pill, so it's
    -- not a solo ally_claim we're locked out of — it's a blitz we *can* join and
    -- simply aren't (yet), e.g. too far. Label it honestly; "ally_claimed" is
    -- reserved for a truly-closed take we can't participate in.
    if e._reject_joinable_blitz then
      return reject_with_breakdown(e,
        string.format("REJECT blitz(joinable) @(%d,%d)", e._mx or 0, e._my or 0),
        string.format("reject:blitz — p%s is running a JOINABLE blitz here; join via the squad layer (in help range) rather than soloing it. Not joining now (out of join range / busy).",
          tostring(by or "?")))
    end
    local their_str = their and string.format("%.0f", their) or "?"
    local diff_str  = (their and string.format(" (we_more_by=%.0f)", our - their)) or ""
    return reject_with_breakdown(e,
      string.format("REJECT ally_claimed %dt @(%d,%d)", rem, e._mx or 0, e._my or 0),
      string.format("reject:ally_claimed %dt — p%s bid %s < ours %.0f%s",
        rem, tostring(by or "?"), their_str, our, diff_str))
  end
  if e._reject == "armour_too_low" then
    return reject_with_breakdown(e,
      string.format("REJECT armour_too_low @(%d,%d)", e._mx or 0, e._my or 0),
      string.format("reject:armour_too_low — arm=%d (need >= %d) vs pillHP=%d (>= %d)",
        e._armour_at_reject or 0, C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR,
        e._pillhp_at_reject or 0, C.ATTACK_PILL_UNSAFE_HP_THRESHOLD))
  end
  if e._reject == "ally_pill_take_priority" then
    local rem = e._reject_remaining or 0
    local by  = e._priority_by
    return reject_with_breakdown(e,
      string.format("REJECT ally_pill_take_priority %dt @(%d,%d)", rem, e._mx or 0, e._my or 0),
      string.format("reject:ally_pill_take_priority %dt — p%s is doing the take, holding off ~%.1fs",
        rem, tostring(by or "?"), rem / 50.0))
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
    -- Contested: sum over MOVING enemy tanks inside the range of
    -- PENALTY x (1 - dist/RANGE), skipping any tank an ally's attack_tank
    -- already owns. Show the head counts so the number reconciles.
    local _ct_n = e._contest_n or 0
    local _ct_h = e._contest_h or 0
    local _d_contest
    if e._contest > 0 then
      _d_contest = string.format(
        "%d moving enemy tank(s) within %.0f[CONTESTED_BASE_RANGE] tiles, "
        .. "sum of %.0f[CONTESTED_BASE_PENALTY] x (1 - dist/%.0f) = %.0f%s",
        _ct_n, C.CONTESTED_BASE_RANGE, C.CONTESTED_BASE_PENALTY,
        C.CONTESTED_BASE_RANGE, e._contest,
        _ct_h > 0 and string.format(" (%d more skipped — ally attack_tank on them)", _ct_h) or "")
    elseif _ct_h > 0 then
      _d_contest = string.format(
        "all %d enemy tank(s) in range handled by an ally's attack_tank → 0", _ct_h)
    else
      _d_contest = string.format("no moving enemy tank within %.0f[CONTESTED_BASE_RANGE] tiles → 0",
            C.CONTESTED_BASE_RANGE)
    end
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
          "fill=%.2f (above LOW) → 1 + %.2f² x (%.2f[FULL_MULT]-1) = %.2f",
          _fill, _fill, C.REFUEL_FULL_COST_MULT, _fm)
        or  "fill=0 (at/below LOW thresholds) → 1.00"
      local _d_lgm   = _lgm
        and string.format("LGM returning, at this base → floor=%.0f (cost capped)", _lgm)
        or  "no LGM-wait active → no floor"
      _shape_detail = string.format("|urgency:%s|deficit:%s|fill:%s|lgm:%s",
        _d_urgency, _d_def, _d_fill, _d_lgm)
    end

    local _danger_pen = C.REFUEL_DANGER_PENALTY or (1 / 0.75)
    local _safe_token = (not e._safe_refuel)
      and string.format(" × danger{%.2f}", _danger_pen) or ""
    local _safe_detail = (not e._safe_refuel)
      and string.format(
        "|danger:danger_val>0 (exposed base) → multiply final cost by %.2f[REFUEL_DANGER_PENALTY]",
        _danger_pen)
      or ""
    local _mine_token = (e._mine_cost and e._mine_cost > 0)
      and string.format(" + mine{%.0f}", e._mine_cost) or ""
    local _mine_detail = (e._mine_cost and e._mine_cost > 0)
      and string.format(
        "|mine:hoard surcharge %.0f (mines past %d[REFUEL_MINE_FREE]) — applied ONLY at the base you're parked on, to push a mine-stuffed tank to dump",
        e._mine_cost, C.REFUEL_MINE_FREE)
      or ""
    local _hop_token = (e._hop and e._hop > 0)
      and string.format(" + hop{%.0f}", e._hop) or ""
    local _hop_detail = (e._hop and e._hop > 0)
      and string.format(
        "|hop:%.0f[REFUEL_BASE_HOP_PENALTY] — we're parked on ANOTHER base; switching to this one is wasteful churn, so it's penalised. Finish where you are (a depleted current base drops out, freeing the move).",
        e._hop)
      or ""
    local _d_astar = string.format(
      "danger-weighted Dijkstra-slate travel cost to base (%d,%d) = %.0f; path %s",
      e._mx or 0, e._my or 0, raw, e._path or "(not traced)")
    local _d_base = string.format(
      "%.0f[REFUEL_BASE_COST] flat floor so refuel-at-own-base isn't ~0",
      C.REFUEL_BASE_COST)
    f = string.format(
      "A*{%.0f}@(%d,%d) + base{%.0f} + danger{%.0f} + stale{%.0f} + contest{%.0f} + deplete{%.0f}%s%s%s%s"..
      "||A*:%s|base:%s|danger:%s|stale:%s|contest:%s|deplete:%s%s%s%s%s",
      raw, e._mx or 0, e._my or 0, C.REFUEL_BASE_COST, e._dang, e._stale, e._contest, e._dep, _shape_head, _safe_token, _mine_token, _hop_token,
      _d_astar, _d_base, _d_danger, _d_stale, _d_contest, _d_deplete, _shape_detail, _safe_detail, _mine_detail, _hop_detail)
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
    local _atk_tank_term = e._atk_tank_pen and string.format(" + atk_tank{%d}", e._atk_tank_pen) or ""
    local _d_atk_tank = e._atk_tank_pen
      and string.format("a non-rejected enemy tank is present this eval → flat +%d on every attack_pill (prefer fighting the tank over chipping pills)", e._atk_tank_pen)
      or "no live enemy tank → no cross-penalty"
    -- Spike shaping terms: multiplier on the combat block when THIS pill is
    -- spiking a friendly base, or the flat cross-penalty when a spike exists
    -- elsewhere. Detail row is always present (matches the intcpt pattern —
    -- an absent row read as "not part of the cost").
    local _spike_mult_term = e._spike_mult and string.format(" * spike{%.2f}", e._spike_mult) or ""
    local _spike_pen_term  = e._spike_pen and string.format(" * spike_pen{%.3f}", e._spike_pen) or ""
    local _d_spike = e._spike_mult
      and string.format(
        "SPIKING: this pill sits within PILL_FIRE_RANGE=%d of %d friendly base(s) (first @(%d,%d)); its least-contested base is covered by %d spike(s) → decisiveness 1/%d → combat × %.2f (SPIKE_PILL_DISCOUNT=%.2f at full decisiveness; each EXTRA denied base strengthens the pull by SPIKE_BASES_BONUS, floored at SPIKE_DISCOUNT_FLOOR; a co-spiked area is 'kinda lost' so clearing one of many pulls weakly)",
        C.PILL_FIRE_RANGE, e._spike_n or 1, e._spike_bmx or -1, e._spike_bmy or -1,
        e._spike_cover or 1, e._spike_cover or 1, e._spike_mult, C.SPIKE_PILL_DISCOUNT or 0.8)
      or "this pill is not in firing range of any friendly base → no spike discount"
    local _d_spike_pen = e._spike_pen
      and string.format(
        "a spiking pill exists elsewhere (@(%d,%d), in range of a friendly base) → WHOLE cost × %.3f [1 + (SPIKE_OTHER_PENALTY_MULT-1) × best decisiveness] on every non-spiking pill (clear the spike first; fades toward ×1.0 when every spike shares its base with others)",
        e._spike_ex_mx or -1, e._spike_ex_my or -1, e._spike_pen)
      or "no spiking pill elsewhere (or this IS the spike) → no cross-penalty"
    f = string.format(
      "(spot{%.0f}@(%d,%d) + pickup{%.0f}@(%d,%d)→(%d,%d)*wound_x2{%.2f} + (stale{%.0f} + diff{%.0f} + anger{%.0f} + xfire{%.0f} + intcpt{%.0f}) * hp{%.2f}%s%s + ammo{%s}%s)%s%s"..
      "||spot cost is offset-aware (target pill's danger contribution subtracted via load_danger_offset before A*); NOT scaled by hp or wound"..
      "|pickup:%s|hp:%s|anger:%s|stale:%s|finish_other:%s|ammo:%s|spot:%s|danger_nearby:%s|atk_tank:%s|spike:%s|spike_pen:%s",
      e._spot, e._spot_mx or 0, e._spot_my or 0,
      e._travel,
      e._spot_mx or 0, e._spot_my or 0, e._mx or 0, e._my or 0,
      _tw,
      e._stale, e._diff, e._anger, e._xfire, e._intcpt,
      e._hp, _wound_detail, _spike_mult_term, _ammo_str, _atk_tank_term, _spike_pen_term, _danger_term,
      -- (order: atk_tank inside the parens; spike_pen + danger_nearby are
      -- whole-cost multipliers, displayed trailing outside the parens)
      _d_pickup, _d_hp, _d_anger, _d_stale, _d_finish_other, _d_ammo, _d_spot, _d_danger, _d_atk_tank, _d_spike, _d_spike_pen)
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
      local _fd = e._free or 0
      local _free_mult = (_fd > 0.001) and (" − %.1f[FREE]"):format(_fd) or ""
      local _free_det = (_fd > 0.001)
        and string.format("|free:close/safe grab (≤ %.1f tiles, danger %.1f) → −%.1f (0=none .. %.1f=best, floor %.1f)",
              C.TANK_COMBAT_ENGAGE_RANGE * C.CAPTURE_FREE_PILL_RANGE_MULT, e._dv or 0, _fd, C.CAPTURE_FREE_PILL_VALUE, C.CAPTURE_FREE_PILL_MIN_COST)
        or  "|free:none (too far or too dangerous)"
      -- DIRECT-ROUTE probe verdict (capture_route_probe). Only the focus pill
      -- carries one; every other row is still a plain slate/A* distance. The
      -- "too far" case gets its own line so it can't be misread as "the direct
      -- route was tried and lost".
      local _route_det = ""
      if _dm_str == "astar_direct" then
        _route_det = string.format(
          "|route:DIRECT A* — danger_scale=0, the straight run at the body. The wsim drove it and we LIVE (%d dmg), so this route and its cost are what the pool ranks on",
          e._route_dmg or 0)
      elseif _dm_str == "astar_danger" then
        _route_det = string.format(
          "|route:DANGER A* — the direct (danger_scale=0) run KILLED us in the wsim (%d dmg), so we re-priced on the danger-weighted A* route instead",
          e._route_dmg or 0)
      elseif e._route_far then
        _route_det = string.format(
          "|route:NOT PROBED — pill is %d tiles away, past CAPTURE_ROUTE_MAX_TILES (%d). No direct-route A* and no wsim were run for it; the dist above is the plain Dijkstra/A* slate cost",
          e._route_far, C.CAPTURE_ROUTE_MAX_TILES or 10)
      end
      f = string.format(
        "(base{%d} + dist{%.1f}[%s]@(%d,%d) + danger{%.1f} + intcpt{%.0f})%s||dist:%.0f^1.5 × %.3f[DIST_SCALE] = %.1f [%s]|danger:%.1f × %.3f[DANGER_SCALE]%s = %.1f%s%s%s%s",
        C.CAPTURE_PILL_BASE_COST, e._ds, _dm_str, e._mx or 0, e._my or 0, _cpill_danger_score, _intcpt, _free_mult,
        raw, C.CAPTURE_PILL_DIST_SCALE, e._ds, _dm_str,
        e._dv, C.CAPTURE_PILL_DANGER_SCALE, _lgm_mult_str, _cpill_danger_score, _lgm_mult_det, intcpt_det, _free_det, _route_det)
    end
  elseif p == 3 then
    -- capture_base: the A* number IS the danger-weighted dijkstra travel cost —
    -- danger is baked into the per-tile path cost, NOT a separate additive term,
    -- so a CLOSE base behind enemy fire can out-cost a FAR safe one (by design:
    -- biases toward safer captures). Plus staleness for a neutral base unseen a
    -- while. Note: BASE_PILL_COVER_PEN is applied only on the finalize path
    -- (eval_capture_base), NOT this rolling cost, so it's not part of this total.
    local _cb_dv = threat.at(e._mx or 0, e._my or 0)
    f = string.format(
      "A*{%.0f}@(%d,%d) + stale{%.0f}||A*:danger-weighted dijkstra travel to base; danger at base tile=%.1f is BAKED INTO the path cost (not a separate term) — that's why a near dangerous base can cost more than a far safe one|stale:%s",
      raw, e._mx or 0, e._my or 0, e._stale or 0, _cb_dv, fmt_stale_detail(e._age, e._stale))
  else
    f = string.format("A*{%.0f}@(%d,%d) + stale{%.0f}||stale:%s",
      raw, e._mx or 0, e._my or 0, e._stale, fmt_stale_detail(e._age, e._stale))
  end
  e.formula = f
  return f
end

-- Combine a REJECT line with the candidate's normal score breakdown so the
-- detail panel shows the term math for rejected rows too (handy for debugging
-- "why was this rejected and what would it have cost"). Only when the cost was
-- actually computed (rejects that skip costing keep the bare reject line). The
-- recursion runs get_formula_inner with _reject cleared to get the real terms,
-- then we overwrite the cache with the combined string. pcall-guarded.
function reject_with_breakdown(e, reject_line, reject_desc)
  local combined = reject_line .. "||" .. reject_desc
  if e.cost and e.cost < 1e29 then
    local saved = e._reject
    e._reject  = nil
    e.formula  = nil
    local ok, normal = pcall(get_formula_inner, e)
    e._reject = saved
    if ok and type(normal) == "string" and normal ~= "" then
      local disp, comp = normal:match("^(.-)||(.*)$")
      if disp and comp then
        local _, full = disp:match("^(.-)!!(.*)$")
        combined = reject_line .. " !! " .. (full or disp) .. "||" .. comp .. " | " .. reject_desc
      else
        combined = reject_line .. " !! " .. normal .. "||" .. reject_desc
      end
    end
  end
  e.formula = combined   -- overwrite the recursion's cache with the combined string
  return combined
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
  -- Blitz join discount term. Appended before the ally branches below so it
  -- survives their early-returns. Shows "x blitz_discount{factor}" and the
  -- base→result in the map half (REF base when the entry was reject-sentinel'd).
  local bd = e._blitz_discount
  if bd then
    local disp = string.format(" x blitz_discount{%.2f}", bd.factor or 1.0)
    local map  = string.format("|blitz_discount:join C%s — %s%.0f x %.2f = %.0f",
      tostring(bd.cmdr), bd.sentinel and "REF " or "", bd.base or 0, bd.factor or 1.0, bd.now or 0)
    local sep = f:find("||", 1, true)
    if sep then f = f:sub(1, sep - 1) .. disp .. " " .. f:sub(sep) .. map
    else        f = f .. disp .. " ||" .. map:sub(2) end
  end
  -- REJECT row already formatted by inner — no trailing term to append.
  if e._reject == "ally_claimed"
     or e._reject == "armour_too_low"
     or e._reject == "ally_pill_take_priority" then
    return f
  end
  -- Cost-baked ally penalty (pool 1 soft +100/ally, pool 8 hard +10000).  Only
  -- emit the term when the penalty actually exists on this entry — EXCEPT
  -- refuel (pool 1), which ALWAYS shows ally_claimed (0 when no allies) so the
  -- Term Breakdown column stays stable.
  local pen = e.ally_claimed_pen or 0
  local term_disp, term_map
  if pen > 0 then
    term_disp = string.format(" + ally_claimed{%d}", pen)
    if e._p == 1 then
      -- Refuel (pool 1): soft, per-ally — NOT a yield. Show the count math.
      term_map = string.format(
        "|ally_claimed:%d ally%s targeting this base x %.0f[ALLY_CLAIMED_REFUEL_PENALTY] = %.0f (soft, still selectable; p%s first)",
        e._ally_n or 0, ((e._ally_n or 0) == 1) and "" or "s",
        C.ALLY_CLAIMED_REFUEL_PENALTY or 100, pen,
        tostring(e.ally_claimed_by or "-"))
    else
      term_map = string.format("|ally_claimed:yielding to ally (p%s)",
                               tostring(e.ally_claimed_by or "-"))
    end
  elseif e._p == 1 and not e._reject then
    -- Refuel with NO allies contesting: still emit the term at 0 so it's
    -- always present in the breakdown (becomes +N00 once allies pile on).
    term_disp = " + ally_claimed{0}"
    term_map  = string.format(
      "|ally_claimed:0 allies targeting this base -> 0 (soft +%.0f[ALLY_CLAIMED_REFUEL_PENALTY] per ally when contested)",
      C.ALLY_CLAIMED_REFUEL_PENALTY or 100)
  elseif e._ally_score then
    -- Hard-REJECT pool, ally is bidding, but we kept the candidate
    -- (either we're cheaper or we're within the steal band).  Display
    -- the bid + relationship so the band is visible without polluting
    -- the cost equation.
    local our   = e.cost or 0
    local their = e._ally_score
    -- capture_pill uses the near-zero steal threshold (any cost edge wins
    -- a drive-over grab); other pools the conservative default.
    local frac  = (e._p == 4) and (C.ALLY_CLAIMED_STEAL_FRAC_CAPTURE or 0.01)
                  or (C.ALLY_CLAIMED_STEAL_FRAC or 0.25)
    local rel
    if our < their * (1 - frac) then
      rel = string.format("we cheaper by %.0f%%", (1 - our / math.max(1, their)) * 100)
    elseif their < our * (1 - frac) then
      rel = string.format("they cheaper by %.0f%% (would REJECT)", (1 - their / math.max(1, our)) * 100)
    else
      rel = string.format("within steal band (need %.0f%% cheaper)", frac * 100)
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
    -- Whole queue processed at least once: mark "swept" so warm_ready can
    -- treat the warmup as done even on sparse maps with < WARMUP_MIN_REAL_GOALS
    -- reachable goals (we've costed everything there is — nothing more to wait
    -- for). Reset on queue rebuild / respawn.
    state._eval_swept = true
    if BRAIN_DEBUG_MODE then print2(string.format("eval_queue EXHAUSTED at pos=%d (queue had %d)", pos, #queue)) end
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

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local boat_flag = info.inboat and 1 or 0
  local shells = info.shells or 32
  local trees  = info.trees or 0
  local mines  = info.mines or 0
  local armour = info.armour or 40
  local now = state.tick or 0

  -- Are we currently part of a TRUE 2+ tank blitz (commander + >=1 committed
  -- soldier, or a soldier joining one)? If so the ally shares the fire and the
  -- "risky armour" attack_pill penalty below is waived. Computed once per call.
  local in_2plus_blitz = false
  if state.goal and state.goal._blitz and state.goal.kind == "attack_pill" then
    if state.squad_role == "s" then
      in_2plus_blitz = true   -- our commander + us
    else
      local _bt = squad.blitz_ready_status(state, now, info.player_number or -1, info)
      in_2plus_blitz = (_bt or 0) >= 1
    end
  end

  -- capture_pill DIRECT-ROUTE probe — see capture_route_probe's header for the
  -- decision and the cost bounds. It runs HERE (and nowhere else) for two
  -- reasons: step_eval_queue already refuses to run while the pathfinder has a
  -- live path_to search, which a one-shot A* would clobber; and it is the one
  -- place that is throttled by the eval_iv capacity tier. Exactly ONE pill is
  -- probed per call — the FOCUS pill:
  --   1. the pill we hold a capture_pill goal on (we are committed to it),
  --   2. else the body we have a fresh-kill claim on (we are about to be),
  --   3. else last tick's pool-4 leader (the one most likely to win).
  -- Everything else keeps the cheap Dijkstra lookup for the ranking pass.
  do
    local _fp, _fid
    local _g = state.goal
    if _g and _g.kind == "capture_pill" and _g.target_id then
      _fid = _g.target_id
    elseif state.kill_pickup and state.kill_pickup.id then
      _fid = state.kill_pickup.id
    else
      local _best = math.huge
      for _, ce in pairs(state.cost_cache or {}) do
        if ce._p == 4 and not ce._reject and (ce.cost or math.huge) < _best then
          _best = ce.cost; _fid = ce._id
        end
      end
    end
    _fp = _fid and world.pills and world.pills[_fid] or nil
    -- Only a real, takeable body is worth an A*: alive pills belong to
    -- attack_pill and a carried one has no meaningful (mx,my).
    if _fp and (_fp.health or 0) == 0 and not _fp.in_tank and not _fp.carrier then
      capture_route_probe(state, world, info, _fp, _fid, tmx, tmy)
    end
  end

  local partial = state.pool_partial
  if not partial then partial = {}; state.pool_partial = partial end

  -- Chunked pill-eval pre-probe (BRAIN_DEBUG_MODE only — the C fast path
  -- is plenty fast outside debug). Peek the next GOAL_CANDS_PER_TICK
  -- pool-6 candidates and advance one chunk on each whose diff_cache
  -- entry is stale.  If any is still in_progress, bail without draining
  -- the queue this tick so the bar can fill across ticks and the work
  -- is actually spread over time at lower capacity tiers.
  if BRAIN_DEBUG_MODE then
    local _probe_pos = pos
    local _probe_count = 0
    local _any_in_progress = false
    while _probe_pos <= #queue and _probe_count < C.GOAL_CANDS_PER_TICK do
      local _it = queue[_probe_pos]
      _probe_pos = _probe_pos + 1
      _probe_count = _probe_count + 1
      if _it.pool == 6 and not _it.reject then
        local _pid = _it.id
        local _pill = _it.obj
        if _pill and _pid and _pid >= 0 then
          local _dck = _pill.mx .. ":" .. _pill.my .. ":" .. (state.phase or "")
          local _dc = (state._pill_diff_cache or {})[_dck]
          local _dx_t = _pill.mx - tmx
          local _dy_t = _pill.my - tmy
          local _sqdist = _dx_t * _dx_t + _dy_t * _dy_t
          local _ttl
          if     _sqdist < 100 then _ttl = 50
          elseif _sqdist < 900 then _ttl = 150
          else                       _ttl = 500
          end
          local _ttl_mult = (state._capacity and state._capacity.ttl_mult) or 1.0
          if _ttl_mult ~= 1.0 then _ttl = math.floor(_ttl * _ttl_mult) end
          local _needs = (not _dc)
              or (_dc.hp ~= (_pill.health or 0))
              or (not _dc.spots)
              or ((now - _dc.tick) >= _ttl)
          if _needs then
            local _status = attack.advance_pill_eval_chunk(
              state, world, info, tmx, tmy, _pid, _pill)
            if _status == "in_progress" then _any_in_progress = true end
          end
        end
      end
    end
    if _any_in_progress then return end
  end

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
        -- Dead pill (health 0). Pool 5 (repair) rebuilds it IN PLACE — use the
        -- rebuild cost model (LGM walk sim; pill-fire ignored; math.huge if the
        -- builder can't reach). Other pools route to the pill tile as before.
        local _ts = clock_us()
        if pool_idx == 5 and C.REPAIR_FIX_ENABLED then
          raw_cost = compute_repair_dead_cost(state, world, info, obj, tmx, tmy)
        else
          raw_cost = cpf.smart_cost_dij_only(KIND_NORMAL, cost_dx, cost_dy, boat_flag)
        end
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
      -- Same contested shape as nearest_resupply_base (shared helper): moving
      -- enemies inside the range, proximity-scaled and summed, minus any tank
      -- an ally's attack_tank goal already owns.
      local contested_cost, _contest_n, _contest_h =
        contested_penalty(state, obj.mx, obj.my, info, now)
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
      -- Self-base hysteresis REMOVED: goal_selection already applies a
      -- GOAL_TARGET_SWITCH_PENALTY + commitment to any refuel base that isn't
      -- our current refuel goal, so a within-pool -REFUEL_SWITCH_THRESHOLD here
      -- double-damped the same switch. Let base choice be driven by
      -- travel/danger/stale/contest/depletion; the goal layer keeps us from
      -- flip-flopping between bases.
      local hysteresis_cost = 0
      -- Floor the TOTAL at REFUEL_BASE_COST so no discount can drive refuel
      -- negative and dominate cheap cross-pool goals (dead pills etc).
      local score = raw_cost + C.REFUEL_BASE_COST + danger_cost + stale_cost + contested_cost + hysteresis_cost + depletion_cost
      score = math.max(score, C.REFUEL_BASE_COST)
      -- Danger PENALTY (was a safe discount): an EXPOSED base (danger_val > 0)
      -- multiplies its cost so refueling out in the open is less attractive — a
      -- safe base keeps its raw cost. Same safe:unsafe ratio as the old 0.75
      -- discount but a higher absolute cost, so refuel doesn't out-compete real
      -- goals as easily. Compounds with everything above.
      local _safe_refuel = (danger_val == 0)
      if not _safe_refuel then
        score = score * (C.REFUEL_DANGER_PENALTY or (1 / 0.75))
      end

      -- Ally-claimed SOFT penalty: +ALLY_CLAIMED_REFUEL_PENALTY for EACH ally
      -- currently broadcasting refuel_at_base on THIS base. Refuel is NOT in
      -- _REJECT_POOLS — instead of a hard first-come-first-served reject, a base
      -- others are already heading to just gets pricier per ally. A closer / more
      -- urgent bot can still pick it (and then brakes beside it via the
      -- wait_for_ally substate in init.lua if an ally tank is parked on the
      -- tile); everyone else drifts to emptier bases. Skip allies whose tank is
      -- dead (a stale broadcast shouldn't price a base we can actually use).
      local ally_claimed_n  = 0
      local ally_claimed_by = nil
      for ally_pn, slot in ally_state.iter_active(now, 1750) do
        if ally_pn ~= info.player_number
           and not (state.tank_dead_at and state.tank_dead_at[ally_pn]
                    and state.tank_dead_at[ally_pn] > (slot.last_tick or 0)) then
          local h = slot.info
          if h and h.goal == "refuel_at_base" then
            local aid = tonumber(h.target)
            local matched
            if aid and id then
              matched = (aid == id)
            else
              local amx, amy = tonumber(h.mx), tonumber(h.my)
              if amx and amy then matched = (amx == obj.mx and amy == obj.my) end
            end
            if matched then
              ally_claimed_n  = ally_claimed_n + 1
              ally_claimed_by = ally_claimed_by or ally_pn
            end
          end
        end
      end
      local ally_claimed_cost = ally_claimed_n * (C.ALLY_CLAIMED_REFUEL_PENALTY or 100)
      score = score + ally_claimed_cost

      -- Anti-base-hop: while we're standing ON a refuel base, switching to a
      -- DIFFERENT base is wasteful churn — an ally claiming our base (the
      -- ally_claimed cost above) shouldn't bounce us off mid-refuel, or both of us
      -- thrash and neither finishes. Penalize every base except the one under us so
      -- we just finish here. (ally_claimed still RISES on our base — that's the
      -- "don't be greedy, take what you need" nudge — it just makes refuel lose to
      -- a COMBAT goal and leave, not hop to another base.) A depleted current base
      -- is rejected upstream and drops out of the queue, which still frees us to
      -- move — the only case we SHOULD switch bases.
      local _hop_cost = 0
      if info.base and info.base.x == tmx and info.base.y == tmy
         and not (obj.mx == tmx and obj.my == tmy) then
        _hop_cost = (C.REFUEL_BASE_HOP_PENALTY or 500)
        score = score + _hop_cost
      end

      -- Panel detail (debug only): walk the Dijkstra slate step-by-step from
      -- the tank to the costed destination (cost_dx,cost_dy — the base's
      -- cheapest adjacent tile) so the breakdown can show the actual route in
      -- (x,y),(x2,y2)... form instead of just the scalar travel cost.
      local _p1_path_str
      if BRAIN_POOL_VIZ then
        local px, py = tmx, tmy
        local parts = { string.format("(%d,%d)", px, py) }
        for _ = 1, 80 do
          if px == cost_dx and py == cost_dy then break end
          local nx, ny = cpf.dijkstra_next_step(KIND_NORMAL, px, py, cost_dx, cost_dy)
          if not nx or (nx == px and ny == py) then break end
          parts[#parts + 1] = string.format("(%d,%d)", nx, ny)
          px, py = nx, ny
        end
        _p1_path_str = table.concat(parts, ", ")
      end

      state.cost_cache[cache_key] = {
        cost = score, raw = raw_cost, tick = now, _p = 1,
        _id = id, _mx = obj.mx, _my = obj.my,
        _dv=danger_val, _dang=danger_cost, _age=_p1_age,
        _stale=stale_cost, _contest=contested_cost,
        _contest_n=_contest_n, _contest_h=_contest_h,
        _hyst=hysteresis_cost, _ratio=supply_ratio, _dep=depletion_cost,
        _lgm_mult = _lgm_mult,
        _safe_refuel = _safe_refuel or nil,
        _path = _p1_path_str,
        _ally_n = (ally_claimed_n > 0) and ally_claimed_n or nil,
        ally_claimed_pen = (ally_claimed_cost > 0) and ally_claimed_cost or nil,
        ally_claimed_by  = ally_claimed_by,
        _hop = (_hop_cost > 0) and _hop_cost or nil,
      }

      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, own = obj.owner,
        travel = raw_cost, danger = danger_val, score = score,
        -- rederive_pool_partial_best (run at finalize) re-derives the pool
        -- winner from cand.cost + cand.obj; without these it would reset
        -- best_obj to nil and refuel would never win after a replan.
        cost = score, obj = obj,
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
        -- Never request detailed spots here unless something actually draws
        -- them — detailed=true disables the C fast path (attack.lua's
        -- gh_attack branch is gated on `not detailed`), so asking for spots on
        -- every debug tick costs the full Lua sweep per candidate. The one
        -- consumer is the attack_scan_spots_all_pills overlay, which reads
        -- dc.spots in M.draw; gate on exactly that viz, not on debug mode.
        -- (In debug mode dc.spots is ALSO filled from the chunked pre-probe's
        -- _pill_eval_cache below, so the overlay still has data on the ticks
        -- this call takes the C path.)
        local force_detailed = BRAIN_DEBUG_MODE
                               and vizmod.is_on("attack_scan_spots_all_pills")
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
          if BRAIN_DEBUG_MODE and _pe and (now - _pe.tick) <= 250 then
            diff_score = _pe.best_score
            _spots     = _pe.spots
            best_spot  = _pe.best_spot
          else
            diff_score, _spots, best_spot =
              attack.evaluate_pill_difficulty(obj, world, force_detailed,
                                              _scan_step, state.phase, state, tmx, tmy)
          end
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
            local n = __idiv(#_path_tiles, 2)  -- waypoint count
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
          goal_spot_path_len = _path_tiles and __idiv(#_path_tiles, 2) or 0

          -- (self_dr removed: the offset-aware A* above bakes the
          -- as-if-pill-dead discount directly into spot_cost.)
        end
        if (_diff_us > 1000 or _spot_us > 1000) then
          if BRAIN_DEBUG_MODE then print2(string.format("  pool6 candidate id=%s diff=%.2fms spot=%.2fms",
                               tostring(id), _diff_us / 1000, _spot_us / 1000)) end
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
        -- Ammo-deprived helper: score the take as if it held a normal load
        -- (~20 shells), not its real 0. A decoy's job is to CHARGE and draw fire,
        -- not finish the pill, so it must not be priced out at COST_INF — with a
        -- normal finite cost attack_pill competes with explore (500) and wins when
        -- it should go help, no fragile join-discount needed.
        local _shells_for_cost = state.ammo_deprived and (C.AMMO_DEPRIVED_PHANTOM_SHELLS or 20) or info.shells
        if _shells_for_cost < pill_hp_now then
          ammo_cost = 1e30
        else
          local ending = _shells_for_cost - pill_hp_now
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
      -- Spike shaping (pool 6 only): a pill within firing range of a friendly
      -- base is "spiking" (denies refuel while it stands) → small combat
      -- discount. While ANY spike exists, every NON-spiking pill instead
      -- pays a small MULTIPLICATIVE penalty on its whole cost (applied once
      -- via the post-assembly multiply below, not per spike). BOTH scale
      -- with the spike's decisiveness (dec = 1/cover, see
      -- refresh_spike_detection): a lone spike gets the full discount and
      -- taxes other takes at full strength; one of 4 co-spiking pills barely
      -- registers (area's lost). Mild by design: the tilt is within the
      -- pool, so attack_tank / kill_lgm / refuel compete unchanged.
      -- Mirrored in attack_pill_adjustments (dead path).
      local spike_mult, spike_pen_mult = 1.0, 1.0
      local _spike_n, _spike_cover, _spike_bmx, _spike_bmy = 0, 1, nil, nil
      local _spike_ex_mx, _spike_ex_my = nil, nil
      if pool_idx == 6 then
        refresh_spike_detection(state, world)
        local sp = state._spike_pills and state._spike_pills[obj.my * 256 + obj.mx]
        if sp then
          spike_mult = spike_discount_mult(sp)
          _spike_n, _spike_cover = sp.n or 1, sp.cover or 1
          _spike_bmx, _spike_bmy = sp.bmx, sp.bmy
        elseif state._spike_present then
          spike_pen_mult = 1.0 + ((C.SPIKE_OTHER_PENALTY_MULT or 1.15) - 1.0) * (state._spike_pen_scale or 1.0)
          for _, esp in pairs(state._spike_pills) do
            _spike_ex_mx, _spike_ex_my = esp.pmx, esp.pmy
            break
          end
        end
      end
      local combat = (stale_cost + diff_cost + anger_cost + xfire_cost + intcpt_cost) * hp_mult * wound_mult * spike_mult
      -- attack_tank cross-penalty (pool 6 only): when a non-rejected enemy tank
      -- exists this eval (state._attack_tank_present, set by eval_attack_tank), add
      -- a flat penalty to EVERY attack_pill so the bot prefers dealing with the
      -- tank over chipping pills while one is live.
      local atk_tank_pen = (pool_idx == 6 and state._attack_tank_present) and (C.ATTACK_PILL_TANK_PRESENT_PENALTY or 30) or 0
      -- Risky-armour penalty (pool 6 only): below ATTACK_PILL_RISKY_ARMOUR we're
      -- not "unsafe" (that's the 20 floor that ABORTS) but exposed, so a SOLO take
      -- costs more — unless we're in a 2+ tank blitz, where the ally shares the
      -- fire. (Joinable-blitz pills still get this here, but the heavy join
      -- discount applied later multiplies it down, so joining stays attractive.)
      local risky_armour_pen = (pool_idx == 6 and not in_2plus_blitz
        and (info.armour or 40) < (C.ATTACK_PILL_RISKY_ARMOUR or 30))
        and (C.ATTACK_PILL_RISKY_PENALTY or 100) or 0
      -- Pool-6 harasser model: split the TRAVEL (distance) term from the
      -- ENGAGE/combat term. A harasser DISCOUNTS travel (HARASSER_TRAVEL_MULT)
      -- so it roams far to attack pills, and pays HARASSER_PILL_COST_MULT× on
      -- the engage portion only — keeping the distance de-emphasis from being
      -- cancelled by the engage multiplier. Non-harassers / non-pool-6 use the
      -- plain sum.
      local _travel_term = travel * travel_wound
      local c
      if pool_idx == 6 and state.is_harasser then
        local _engage_term = spot_cost + combat * capture_mult + base_extra + threat_cost + ammo_cost + atk_tank_pen + risky_armour_pen
        c = _travel_term * (C.HARASSER_TRAVEL_MULT or 1.0)
          + _engage_term * (C.HARASSER_PILL_COST_MULT or 1.0)
      else
        c = spot_cost + _travel_term + combat * capture_mult + base_extra + threat_cost + ammo_cost + atk_tank_pen + risky_armour_pen
      end
      -- Spike cross-penalty: multiply the WHOLE candidate cost (travel
      -- included — wandering off while a decisive spike stands is what
      -- this taxes). 1.0 when no spike / when this IS the spike.
      if spike_pen_mult ~= 1.0 then c = c * spike_pen_mult end

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
      local _cpill_free_disc = 0  -- value-bonus semantics: 0 = no free-grab bonus
      local _cpill_route_dmg = 0  -- wsim damage on the probed direct route
      local _cpill_route_far = nil -- set = beyond CAPTURE_ROUTE_MAX_TILES, never probed
      if pool_idx == 4 then
        c, _cpill_dist_raw, _cpill_dist_score, _cpill_danger_val, _cpill_intcpt, _cpill_lgm_mult, _cpill_dist_method, _cpill_free_disc, _cpill_route_dmg, _cpill_route_far =
          compute_pool4_cost(state, world, info, obj, tmx, tmy)
        -- Pool 4 skipped the smart_cost block (see above), so backfill
        -- raw_cost from compute_pool4_cost's distance — keeps the panel
        -- formula breakdown showing a meaningful raw value.
        raw_cost = _cpill_dist_raw
      end

      -- Pool 5 (repair_pill): the UNIFIED repair formula, per candidate.
      -- Historically the damage discount lived only in the finalize step
      -- (applied to the raw-path winner, so the pool picked by DISTANCE,
      -- not repair merit) and the base-cost + contested x3 terms only in
      -- the fallback evaluator — two drifting copies. One copy now, here;
      -- finalize passes best_cost straight through. Dead (0-HP) pills
      -- keep raw_cost: it is already the full rebuild-in-place model.
      local _rp_dmg, _rp_contested = 0, false
      if pool_idx == 5 and (obj.health or 0) > 0 then
        _rp_dmg = (C.PILLS_MAX_HEALTH or 15) - obj.health
        c = (C.REPAIR_BASE_COST or 30)
            + math.max(0, raw_cost - _rp_dmg * (C.REPAIR_DAMAGE_BONUS or 10))
            + stale_cost
        -- Contested: an enemy tank closer to the pill than we are, OR a
        -- fresh enemy sighting stamped at the pill (perception's
        -- _enemy_near_tick — the same evidence defend runs on, and via
        -- team pill view it works at any range). A contested repair sends
        -- the LGM into likely fire: price it x3, don't ban it.
        local _ets5 = state.perc and state.perc.enemy_tanks
        if _ets5 then
          local _our_d5 = U.mdist(tmx, tmy, obj.mx, obj.my)
          for _, _e5 in ipairs(_ets5) do
            if U.mdist(_e5.mx, _e5.my, obj.mx, obj.my) < _our_d5 then
              _rp_contested = true
              break
            end
          end
        end
        if not _rp_contested and obj._enemy_near_tick
           and (now - obj._enemy_near_tick) < (C.DEFEND_SIGHT_FRESH_TICKS or 600) then
          _rp_contested = true
        end
        if _rp_contested then c = c * (C.REPAIR_CONTESTED_MULT or 3.0) end
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
        entry._atk_tank_pen = (atk_tank_pen > 0) and atk_tank_pen or nil
        entry._spike_mult = (spike_mult ~= 1.0) and spike_mult or nil
        entry._spike_n    = (_spike_n > 0) and _spike_n or nil
        entry._spike_cover = (_spike_n > 0) and _spike_cover or nil
        entry._spike_bmx  = _spike_bmx
        entry._spike_bmy  = _spike_bmy
        entry._spike_pen  = (spike_pen_mult ~= 1.0) and spike_pen_mult or nil
        entry._spike_ex_mx = _spike_ex_mx
        entry._spike_ex_my = _spike_ex_my
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
        entry._free=_cpill_free_disc
        entry._route_dmg=_cpill_route_dmg
        entry._route_far=_cpill_route_far
      elseif pool_idx == 5 then
        entry._stale=stale_cost; entry._age=_gen_age
        entry._dmg=_rp_dmg
        entry._contested=_rp_contested or nil
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
    if BRAIN_DEBUG_MODE and _t_total > 2000 then
      print2(string.format(
        "  step_eval_queue cand: pool=%d id=%s total=%.2fms raw=%.2fms (dij=%s)",
        pool_idx, tostring(id), _t_total / 1000, _t_raw / 1000,
        tostring(_used_dij_for_raw)))
    end
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
-- This pass converts that into entry._reject = "ally_claimed" unless we're
-- cheaper than the holder by at least ALLY_CLAIMED_STEAL_FRAC (ratio), and clears
-- the reject when the ally drops the goal, the heartbeat expires, or
-- our raw cost drops below theirs.
--
-- Result: ally-claim REJECT freshness decouples from per-candidate eval
-- cadence — an ally yielding mid-cycle un-REJECTs us on the very next
-- tick instead of waiting for the slow attack_pill re-eval window.
-- =========================================================================
-- NOTE: pool 1 (refuel_at_base) is deliberately ABSENT — refuel uses a SOFT
-- per-ally +ALLY_CLAIMED_REFUEL_PENALTY cost bump baked at eval time (see the
-- pool-1 branch in step_eval_queue), not a hard ally_claimed reject. Two bots
-- may legitimately converge on one base; the wait_for_ally substate parks the
-- later arrival beside it.
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
-- prev}, ... }, capped to the most recent REJECT_HISTORY_MAX transitions
-- per key so it can't grow without bound over a long session.
local REJECT_HISTORY_MAX = 16
-- Debug-only telemetry for the pool grid. Wrapped in a BRAIN_DEBUG_MODE
-- block so lua_strip's --strip-block drops the whole body from opt/ —
-- only the forward declaration (nil) survives there, and the sole caller
-- is likewise BRAIN_DEBUG_MODE-gated, so it's never invoked in opt/.
local record_reject_history
if BRAIN_DEBUG_MODE then
function record_reject_history(state)
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
      if #list > REJECT_HISTORY_MAX then table.remove(list, 1) end
      e._last_reject_seen = cur
    end
  end
end
end

local function sync_ally_claimed_rejects(state, info)
  local cache = state.cost_cache
  if not cache then return end
  local now = state.tick or 0
  local self_pn = (_SELF_PN ~= -1) and _SELF_PN or (state.player_number or -1)
  local steal_frac = C.ALLY_CLAIMED_STEAL_FRAC or 0.25
  -- Prefer the live info passed by the caller; only fall back to the
  -- cached _last_info (which gets populated by get_pool_breakdown_json,
  -- not the per-tick brain loop) so the per-tick sync still sees the
  -- right armour on every tick, not just when the pool grid is open.
  local cur_armour = (info and info.armour)
                     or (state._last_info and state._last_info.armour) or 0
  -- Ammoless helpers (out of shells, or ammo-deprived) are DECOY bodies: their
  -- whole job in a blitz is to soak the pill's fire so the shooters get free
  -- shots. Low armour is the POINT, not a disqualifier — so they skip the
  -- armour_too_low pill precondition below (a normal tank still respects it).
  local _hshells = (info and info.shells)
                   or (state._last_info and state._last_info.shells) or 0
  local ammoless_helper = state.ammo_deprived or _hshells == 0

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
    if BRAIN_DEBUG_MODE and pool_idx == 6 then
      print2(string.format(
        "SYNC_P6 pid=%s mx=%s my=%s cost=%.0f _reject_in=%s in_REJECT_POOLS=%s",
        tostring(e._id), tostring(e._mx), tostring(e._my),
        e.cost or 0, tostring(e._reject),
        tostring(_REJECT_POOLS[pool_idx] ~= nil)))
    end
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
         and cur_armour < C.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR
         and not ammoless_helper
         -- A pill_suicider attacks regardless of armour — dying on the pill
         -- is an accepted outcome, so the safety gate does not apply. An
         -- existing reject clears via the elseif below the moment the role
         -- (or armour) makes this condition false.
         and not state.is_pill_suicider then
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
    -- Blitz exemption (pool 6 / attack_pill): the pill is NOT an ally_claimed
    -- lockout while a blitz on it is OPEN — either WE'RE a committed participant
    -- (squad_blitz_target) OR a live joinable call from an ally exists
    -- (blitz_calls). In both cases clear the reject and leave it a LIVE
    -- candidate: a committed bot takes it; a non-participant keeps RE-EVALUATING
    -- it every tick and JOINS via the squad layer (negotiate → commit, with the
    -- distance discount) if/when it picks it. We declined to join, not "can't
    -- have it". It only falls back to a real ally_claimed REJECT once the take is
    -- truly CLOSED — the ally is engaging or gone, so blitz_calls drops it (the
    -- substate/dead prune in squad.update) and the exemption no longer fires.
    if pool_idx == 6 and e._id then
      local blitz_open = state.squad_blitz_target == e._id
      if not blitz_open and state.blitz_calls then
        for _, c in pairs(state.blitz_calls) do
          if c.pill == e._id then blitz_open = true break end
        end
      end
      if blitz_open then
        -- Squad cap: a blitz is at most a commander + SQUAD_MAX_SIZE soldiers
        -- (2 tanks total). If the squad on this pill is already full and we're
        -- not part of it, REJECT (blitz_full) rather than offering it as a
        -- joinable candidate — stops a 3rd tank piling onto a full take. The
        -- count is role-agnostic (any ally broadcasting attack_pill/capture_pill
        -- on this pill, alive), so it holds even when the c/s roles get confused.
        local g = state.goal
        local we_on_pill = g and (g.kind == "attack_pill" or g.kind == "capture_pill")
                           and ((g.target_id and g.target_id == e._id)
                                or (g.mx == e._mx and g.my == e._my))
        local we_participate = we_on_pill or (state.squad_blitz_target == e._id)
        if not we_participate then
          local full_n = (C.SQUAD_MAX_SIZE or 1) + 1   -- commander + soldiers
          local tdead = state.tank_dead_at
          local n = 0
          for apn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
            if apn ~= self_pn and slot.info then
              local ag = slot.info.goal
              if (ag == "attack_pill" or ag == "capture_pill")
                 and tonumber(slot.info.target or "") == e._id
                 and not (tdead and tdead[apn] and tdead[apn] > (slot.last_tick or 0)) then
                n = n + 1
                if n >= full_n then break end
              end
            end
          end
          if n >= full_n then
            if e._reject ~= "blitz_full" then
              e._reject = "blitz_full"
              e._reject_remaining = 0
              e.formula = nil
            end
            e._blitz_joinable = nil
            goto continue_entry
          end
        end
        if e._reject == "ally_claimed" or e._reject == "blitz_full" then
          e._reject = nil
          e._reject_remaining = 0
          e._ally_by = nil
          e.formula = nil
        end
        e._blitz_joinable = true
        goto continue_entry
      elseif e._blitz_joinable then
        e._blitz_joinable = nil
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
          print2(string.format(
            "SYNC_P6 pid=%d MATCHED ally=p%d sub=%s force_engaging=%s ally_cost=%s our_cost=%.0f",
            e._id, match_pn, tostring(match_sub or "?"),
            tostring(force_engaging_reject),
            tostring(match_cost), e.cost or 0))
        elseif _diag_scanned > 0 then
          print2(string.format(
            "SYNC_P6 pid=%d NO_MATCH (%d allies scanned, none on attack_pill #%d)",
            e._id, _diag_scanned, e._id))
        end
      end

      if match_pn then
        local our_cost = e.cost
        -- Per-pool steal threshold: capture_pill grabs are cheap to re-route
        -- (drive-over, no shells invested), so essentially ANY cost edge wins
        -- the pickup (1%); other pools keep the conservative 25% band.
        local pool_steal_frac = steal_frac
        if pool_idx == 4 then
          pool_steal_frac = C.ALLY_CLAIMED_STEAL_FRAC_CAPTURE or 0.01
        elseif pool_idx == 2 then
          -- defend_pill: DISTANCE decides. Defend bids are base-dominated
          -- and flat-clamped, so a closer responder is only a sliver
          -- cheaper (floor/welldef carry a travel*0.01 tiebreaker) — a
          -- hair-trigger band lets the closer ally take over from a far
          -- one that merely re-scored first; ties still break by id.
          pool_steal_frac = C.ALLY_CLAIMED_STEAL_FRAC_DEFEND or 0.005
        end
        local we_keep
        local _reason  -- debug label, set at each decision site
        local kind = _REJECT_POOLS[pool_idx]
        local g    = state.goal
        local we_hold = g and g.kind == kind
                       and ((g.target_id and e._id and g.target_id == e._id)
                            or (g.mx == e._mx and g.my == e._my))
        if pool_idx == 6 and e._id then
          -- ── attack_pill: NEGOTIATED steal (stq/sta/str), no silent takeover ──
          -- The old silent cost-steal let a cheaper challenger just KEEP the
          -- pill; the pricier holder stamped REJECT on its own entry but the
          -- pool-6 mid-take lock kept re-selecting it — a zombie co-attacker
          -- (3 bots soloing pill #5, 20260713_010904_1 t=5022). Now:
          --   * a holder past approach is NEVER stealable (it's already
          --     getting into position),
          --   * a pre-commit holder yields only via an explicit stq→sta
          --     handshake (or a dual-hold race resolution), and the yield is
          --     REAL — state._steal_abandon makes init.lua clear the goal.
          local our_sub = g and g.substate or ""
          local we_pre_commit = (our_sub == "plan_position" or our_sub == "approach")
          if we_hold and not we_pre_commit then
            -- Committed (aim/charge/in_range/shoot/...): not stealable, and no
            -- ally cost can peel us off. Keep unconditionally.
            we_keep = true
            _reason = "we_hold committed (not stealable past approach)"
          elseif we_hold and force_engaging_reject then
            -- Dual-hold race, ally already ENGAGING while we're still
            -- pre-commit: their take is un-stealable — abandon ours for real.
            we_keep = false
            _reason = "dual_hold: ally engaged, we pre-commit -> abandon"
            state._steal_abandon = { pid = e._id, to = match_pn, tick = now, why = _reason }
          elseif we_hold then
            -- Dual-hold race, both pre-commit (picked within the same claim
            -- broadcast window): settle by cost, player id breaks ties — and
            -- the loser REALLY abandons instead of zombie-holding.
            if match_cost == nil then
              we_keep = (self_pn < match_pn)
              _reason = we_keep and "dual_hold no_cost: lower_id_keeps"
                                 or "dual_hold no_cost: higher_id_abandons"
            elseif our_cost < match_cost * (1 - pool_steal_frac) then
              we_keep = true
              _reason = "dual_hold: we_meaningfully_cheaper"
            elseif match_cost < our_cost * (1 - pool_steal_frac) then
              we_keep = false
              _reason = "dual_hold: they_meaningfully_cheaper -> abandon"
            else
              we_keep = (self_pn < match_pn)
              _reason = we_keep and "dual_hold steal_band: lower_id_keeps"
                                 or "dual_hold steal_band: higher_id_abandons"
            end
            if not we_keep then
              state._steal_abandon = { pid = e._id, to = match_pn, tick = now, why = _reason }
            end
          elseif force_engaging_reject then
            -- Ally past plan/approach: the take is in flight — no cost can
            -- justify peeling them off; arriving late as a co-attacker wastes
            -- our cycles since they finish the kill first.
            we_keep = false
            _reason = "force_engaging_reject (ally past plan)"
          elseif state.squad_joinable_pills and state.squad_joinable_pills[e._id] then
            -- Joinable blitz we are NOT part of: never steal it — the squad
            -- layer makes us JOIN instead of soloing alongside.
            we_keep = false
            _reason = "joinable_blitz: join via squad, never steal"
          else
            -- Challenger vs a pre-commit holder: NEVER take it silently.
            -- If we're meaningfully cheaper, ASK ("I want to steal this from
            -- you, my score is N") and keep yielding until they accept.
            local grant = state._steal_grants and state._steal_grants[e._id]
            if grant and grant.by == match_pn
               and (now - grant.tick) <= (C.STEAL_GRANT_TTL or 250) then
              we_keep = true
              _reason = "steal_granted by p" .. match_pn
            else
              we_keep = false
              if match_cost ~= nil and our_cost < match_cost * (1 - pool_steal_frac) then
                local sent = state._steal_req_sent and state._steal_req_sent[e._id]
                local rej  = state._steal_rejects and state._steal_rejects[e._id]
                local cd   = C.STEAL_REQ_COOLDOWN or 150
                if (not sent or (now - sent.tick) >= cd)
                   and (not rej or (now - rej.tick) >= cd) then
                  state._steal_req_sent = state._steal_req_sent or {}
                  state._steal_req_sent[e._id] = { to = match_pn, tick = now, cost = our_cost }
                  state._steal_outbox = state._steal_outbox or {}
                  state._steal_outbox[#state._steal_outbox + 1] =
                    string.format("/info stq %d %d %d", e._id, match_pn,
                                  math.floor(math.min(our_cost, 9999999) + 0.5))
                  _reason = "steal_requested (we_cheaper, asking p" .. match_pn .. ")"
                else
                  _reason = "steal_cooldown (we_cheaper, ask later)"
                end
              else
                _reason = "they_hold (not meaningfully cheaper)"
              end
            end
          end
        -- ── Non-pool-6 pools keep the legacy silent steal-band rules ──
        elseif force_engaging_reject and not we_hold then
          we_keep = false
        elseif match_cost == nil then
          -- Ally hasn't broadcast a cost yet (first frame post-pick) — no costs
          -- to compare. Settle deterministically by player id (see steal band).
          we_keep = (self_pn < match_pn)
        elseif our_cost < match_cost * (1 - pool_steal_frac) then
          we_keep = true  -- we're meaningfully cheaper (>=frac), keep
        elseif match_cost < our_cost * (1 - pool_steal_frac) then
          we_keep = false -- they're meaningfully cheaper, yield
        else
          -- Steal band (neither meaningfully cheaper): settle the tie by a
          -- CONSISTENT player order — identical on both bots — where the LOWER
          -- player id keeps and the higher yields. A genuinely cheaper bot still
          -- wins above; pn only settles ties.
          we_keep = (self_pn < match_pn)
        end
        if BRAIN_DEBUG_MODE and pool_idx == 6 and e._id then
          print2(string.format(
            "SYNC_P6 pid=%d DECISION ally=p%d ally_cost=%s our_cost=%.0f frac=%.2f " ..
            "we_hold=%s force_engaging=%s -> %s [%s]",
            e._id, match_pn, tostring(match_cost), e.cost or 0, pool_steal_frac,
            tostring(we_hold or false), tostring(force_engaging_reject),
            we_keep and "KEEP" or "REJECT(ally_claimed)", _reason or "?"))
        end
        if we_keep then
          if e._reject == "ally_claimed" then
            e._reject = nil
            e._reject_remaining = 0
            e.formula = nil  -- re-render
          end
          e._reject_joinable_blitz = nil
          e._ally_score     = match_cost
          e._ally_by        = match_pn
          e._ally_heartbeat = match_heartbeat
          -- Stealing: an ally is also bidding on this target, but we keep it
          -- by being meaningfully cheaper (not just first-claim hysteresis).
          -- Surfaced as a chip in the pool visualizer.
          e._stealing = (match_cost ~= nil)
            and (our_cost < match_cost * (1 - pool_steal_frac)) or false
        else
          e._reject           = "ally_claimed"
          e._reject_remaining = match_heartbeat
          e._ally_score       = match_cost
          e._ally_by          = match_pn
          e._ally_heartbeat   = match_heartbeat
          e._stealing         = false
          -- Open-blitz discriminator: is there a LIVE blitz call on this pill?
          -- If so the reject is "joinable blitz" (we just aren't joining yet),
          -- not a closed solo ally_claim. blitz_calls drops the entry on bcc /
          -- when the commander starts engaging, so a truly-closed take reads as
          -- ally_claimed again.
          e._reject_joinable_blitz = nil
          if pool_idx == 6 and e._id and state.blitz_calls then
            for _, c in pairs(state.blitz_calls) do
              if c.pill == e._id then e._reject_joinable_blitz = true break end
            end
          end
          e.formula           = nil  -- re-render with REJECT text
        end
      else
        -- No ally claiming this target any more. If WE just yielded it in a
        -- steal/dual-hold resolution, hold the reject through the gap until
        -- the winner's own claim broadcast lands — otherwise the next replan
        -- re-picks the pill we just gave away and the yield ping-pongs.
        local y = (pool_idx == 6) and e._id
                  and state._steal_yielded and state._steal_yielded[e._id]
        if y and (now - y.tick) <= (C.STEAL_YIELD_BLOCK or 300) then
          if e._reject ~= "ally_claimed" then
            e._reject = "ally_claimed"
            e.formula = nil
          end
          e._reject_remaining = (C.STEAL_YIELD_BLOCK or 300) - (now - y.tick)
          e._ally_by = y.to
        else
          if y then state._steal_yielded[e._id] = nil end
          -- Clear any prior reject.
          if e._reject == "ally_claimed" then
            e._reject = nil
            e._reject_remaining = 0
            e.formula = nil
          end
          e._reject_joinable_blitz = nil
          e._ally_score     = nil
          e._ally_by        = nil
          e._ally_heartbeat = nil
          e._stealing       = nil
        end
      end
    end
    ::continue_entry::
  end
  -- Snapshot any _reject transitions sync just made (or any made
  -- earlier this tick by build_eval_queue / pool finalizers) into
  -- state.reject_history so the pool-grid breakdown can surface the
  -- full per-entry timeline.  Debug-only: it's pure telemetry for the
  -- pool grid, and walks the whole cost_cache every tick — kept off the
  -- production (opt/) hot path. (One line so lua_strip removes it.)
  if BRAIN_DEBUG_MODE then record_reject_history(state) end
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
        -- Compare on the cost_cache cost, not cand.cost: the finalize passes above
        -- (blitz target / join discount / ally-claimed) adjust ce.cost in place but
        -- never touch cand.cost (the original eval score). Using cand.cost made a
        -- blitz-discounted pill lose its own pool to a pricier one — the pool grid
        -- showed the discounted winner while the competition used the stale score.
        local eff = (ce and ce.cost) or cand.cost or math.huge
        if ce and not ce._reject and eff < best_cost then
          best_cost = eff
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
-- Squad blitz adoption: a soldier in a squad forces its commander's blitz pill
-- to a flat very-low cost (SQUAD_BLITZ_COST) so it wins goal selection and the
-- soldier converges — but ONLY while the soldier's current goal is interruptible
-- (and it has ammo/armour), so a committed take/capture/etc. isn't yanked. Cost
-- 30 still loses to a cheaper attack_tank / flee / refuel, keeping it
-- interruptible. Clears any reject (armour_too_low / ally_claimed) on the pill
-- so the squad converges on the commander's target. Run AFTER area-lock.
local function apply_blitz_target(state, info)
  if not info or state.squad_role ~= "s" then return end
  local tgt = state.squad_blitz_target
  if not state.squad_cmdr or not tgt then return end
  if not squad.availability(state, info, tgt) then return end  -- current goal not interruptible
  local cache = state.cost_cache
  if not cache then return end
  for _, e in pairs(cache) do
    if e._p == 6 and e._id == tgt and e.cost then
      e.cost              = C.SQUAD_BLITZ_COST or 30
      e._reject           = nil   -- committed: converge on the commander's pill
      e._reject_remaining = 0
      e._blitz            = true
    end
  end
end

-- Distance→discount factor for joining a blitz (<= 1, so it only discounts).
-- d = dijkstra slate path cost to the pill. Single exponential: MIN at d=0,
-- rising to 1.0 (no discount) at FULL_TILES, clamped 1.0 beyond. Monotonic and
-- self-limiting — the discount fully vanishes by FULL, which is what lets the
-- score (not a hard range gate) decide who joins. Shape matches the old
-- piecewise values at the anchors: MIN@0, ~MIN^0.5 at FULL/2 (0.5 at d=10 for
-- MIN=0.25, FULL=20), 1.0@FULL.
local function blitz_join_factor(d)
  local minmul = C.SQUAD_BLITZ_JOIN_MIN_MULT  or 0.25
  local full   = C.SQUAD_BLITZ_JOIN_FULL_TILES or 20
  if d >= full then return 1.0 end
  if d <= 0     then return minmul end
  return minmul ^ (1.0 - d / full)
end

-- Pick-to-join blitz discount: give EVERY open blitz call's pill (state.blitz_calls)
-- a heavy distance-scaled discount on its pool-6 entry so goal_selection is pulled
-- toward joining ANY blitz, in ANY game phase — the discount (not a hard gate) is
-- what makes a second tank converge onto the take. Decoupled from negotiation:
-- the old version only discounted the single pill the squad layer had already
-- chosen to negotiate (squad_negotiate_pill, itself gated behind availability),
-- so the discount could never actually CAUSE the pick — chicken-and-egg.
--
-- Sane guards still skip a call: it's our OWN call (we're the commander), the
-- squad is already full, we're committed to a DIFFERENT blitz (handled by the
-- early return below — apply_blitz_target owns the accepted pill), the pill is in
-- the no-spot reject window, or we're mid-take on our own pill (only switch off
-- while still in `approach`). Distance keeps the nearest joiners cheapest.
local function apply_blitz_join_discount(state, info, world)
  -- info.tankx may be absent when the pool panel reads a partial _last_info stub
  -- before a full think populates it; the standoff estimate below needs the tank
  -- position, so bail (the real discount applies next think with full info).
  if not info or not info.tankx or state.squad_blitz_accepted then return end  -- accepted → apply_blitz_target owns it
  local calls = state.blitz_calls
  if not calls then return end
  local cache = state.cost_cache
  if not cache then return end
  local now     = state.tick or 0
  local self_pn = (_SELF_PN ~= -1) and _SELF_PN or (info.player_number or -1)
  local cap     = C.SQUAD_MAX_SIZE or 3
  local ref     = C.SQUAD_BLITZ_JOIN_REF_COST or 120
  -- Mid-take on our OWN pill: don't let a different blitz pull us off once we're
  -- past approach (planning/building/engaging). Our current pill itself stays
  -- eligible (keeps it cheap so we don't flap off it).
  local g = state.goal
  local own_locked = g and g.kind == "attack_pill" and g.substate ~= "approach"

  -- Lazy per-commander soldier tally (to skip FULL squads); built once if needed.
  local members = nil
  local function member_count(cmdr)
    if not members then
      members = {}
      local dead = state.tank_dead_at
      for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
        local is_dead = dead and dead[pn] and dead[pn] > (slot.last_tick or 0)
        if pn ~= self_pn and not is_dead and slot.info.role == "s" and slot.info.cmdr then
          local c = tonumber(slot.info.cmdr)
          if c then members[c] = (members[c] or 0) + 1 end
        end
      end
    end
    return members[cmdr] or 0
  end

  -- Lazy pn -> live tank tile map (from our perception), built once if needed.
  -- Lets the in-shooting-range bonus measure to the blitzing COMMANDER's tank,
  -- not just the target pill. nil for any commander we can't currently see.
  local tank_pos = nil
  local function cmdr_pos(pn)
    if not tank_pos then
      tank_pos = {}
      if info.objects then
        for _, ob in ipairs(info.objects) do
          if ob.type == OBJECT_TANK and (bit.band(ob.info, OBJECT_HOSTILE)) == 0 then
            tank_pos[ob.idnum] = { mx = bit.rshift(ob.x, 8), my = bit.rshift(ob.y, 8) }
          end
        end
      end
    end
    return tank_pos[pn]
  end

  for cmdr, call in pairs(calls) do
    local pid = call.pill
    -- Guards: not our own call; not in the no-spot reject window; squad not full;
    -- not being pulled off our own in-progress take.
    local rej = pid and state._blitz_pill_reject and state._blitz_pill_reject[pid]
    if pid and cmdr ~= self_pn
       and not (rej and now < rej)
       and not (own_locked and g.target_id ~= pid)
       and member_count(cmdr) < cap then
      local pill = world and world.pills and world.pills[pid] or nil
      if pill then
        -- Distance = dijkstra path cost (O(1)) to the STANDOFF, not the pill
        -- center (measuring to center overstates by ~the standoff radius). Prefer
        -- our negotiated offer for the call we're actually negotiating; otherwise
        -- estimate a standoff ATTACK_PILL_STANDOFF tiles out toward our tank.
        local s_mx, s_my
        if state.squad_negotiate_pill == pid and state.squad_blitz_engage_mx then
          s_mx, s_my = state.squad_blitz_engage_mx, state.squad_blitz_engage_my
        else
          local sr = C.ATTACK_PILL_STANDOFF or 7.4
          local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
          local ddx, ddy = tmx - pill.mx, tmy - pill.my
          local dd = math.sqrt(ddx * ddx + ddy * ddy)
          if dd > 0.5 then
            s_mx = math.floor(pill.mx + (ddx / dd) * sr + 0.5)
            s_my = math.floor(pill.my + (ddy / dd) * sr + 0.5)
          else
            s_mx, s_my = pill.mx, pill.my
          end
        end
        local dist = travel_cost_to_pill(s_mx, s_my, info.inboat)
        if not dist or dist >= 9999 then dist = travel_cost_to_pill(pill.mx, pill.my, info.inboat) end
        local factor = (dist and dist < 9999) and blitz_join_factor(dist) or 1.0
        -- In-shooting-range bonus: if our tank is within shooting distance
        -- (euclidean) of EITHER the target pill OR the blitzing commander's tank,
        -- we're right there to help, so stack an extra flat discount
        -- (SQUAD_BLITZ_INRANGE_MULT, default 0.5 = another -50%) on top of the
        -- distance curve. The not-full guard is already applied above.
        do
          local rng = C.SQUAD_BLITZ_INRANGE_TILES or 7
          local tmx2, tmy2 = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)
          local function within(ox, oy)
            local dx, dy = tmx2 - ox, tmy2 - oy
            return math.sqrt(dx * dx + dy * dy) <= rng
          end
          local in_range = within(pill.mx, pill.my)
          if not in_range then
            local cp = cmdr_pos(cmdr)
            if cp then in_range = within(cp.mx, cp.my) end
          end
          if in_range then factor = factor * (C.SQUAD_BLITZ_INRANGE_MULT or 0.5) end
        end
        if factor < 1.0 then  -- within discount range
          for _, e in pairs(cache) do
            if e._p == 6 and e._id == pid and e.cost then
              -- Anti-compounding: discounting e.cost every tick would spiral it to
              -- zero on a pill that ISN'T re-evaluated each tick (most aren't —
              -- only ~the queued few refresh). Use the entry's cost as last
              -- EVALUATED: if e.tick advanced since our last discount it's a fresh
              -- score (use it); otherwise reuse the stored pre-discount base.
              local prev = e._blitz_discount
              local fresh, base
              if prev and prev.eval_tick == e.tick then
                base = prev.base                              -- not re-evaluated → keep original base
                fresh = prev.sentinel
              else
                fresh = not (e.cost < 1e29)
                base  = fresh and ref or e.cost               -- reject/sentinel → reference base
              end
              e.cost              = base * factor
              e._reject           = nil
              e._reject_remaining = 0
              e._blitz            = true
              e._blitz_discount   = { factor = factor, base = base, now = e.cost, cmdr = cmdr, sentinel = fresh, eval_tick = e.tick }
              if BRAIN_DEBUG_MODE then print2(string.format("BLITZ_JOIN_DISCOUNT t=%d pill=%d C%s dist=%.1f factor=%.2f base=%.0f -> %.0f", now, pid, tostring(cmdr), dist or -1, factor, base, e.cost)) end
            end
          end
        end
      end
    end
  end
end

-- Post-blitz capture defer: after the squad kills the blitz pill, only the
-- commander (the initiator) captures it — soldiers reject their own
-- capture_pill candidate for the pill the commander is currently
-- attacking/capturing, so they don't race the captain for the dead pill.
local function apply_blitz_capture_defer(state, info)
  if not info or state.squad_role ~= "s" then return end
  local cmdr = state.squad_cmdr
  if not cmdr then return end
  local slot = ally_state.get(cmdr)
  if not (slot and slot.active) then return end
  local g   = slot.info.goal
  local pid = tonumber(slot.info.target or "")
  if not pid or (g ~= "attack_pill" and g ~= "capture_pill") then return end
  local cache = state.cost_cache
  if not cache then return end
  for _, e in pairs(cache) do
    if e._p == 11 and e._id == pid then          -- pool 11 = capture_pill
      e._reject           = "blitz_captain_captures"
      e._reject_remaining = C.ALLY_PILL_TAKE_PRIORITY_TICKS or 100
      e.formula           = nil
    end
  end
end

function M.finalize_pools(state, world, info)
  -- ── Ally-claimed REJECT sync (runs before partial → pool_cache) ──
  local _tpre = clock_us()
  _SELF_PN = info.player_number or _SELF_PN
  state.world = world  -- stash for the pool-panel breakdown builder (no world param there)
  sync_ally_claimed_rejects(state, info)
  local _t_sync = clock_us()
  apply_blitz_target(state, info)
  apply_blitz_join_discount(state, info, world)
  apply_blitz_capture_defer(state, info)
  rederive_pool_partial_best(state)
  local _t_apply = clock_us()
  if BRAIN_PROFILE then
    opt(string.format("    fp sync %.2f ms  apply+rederive %.2f ms",
        (_t_sync - _tpre) / 1000, (_t_apply - _t_sync) / 1000))
  end
  local _t0 = clock_us()
  local tmx  = bit.rshift(info.tankx, 8)
  local tmy  = bit.rshift(info.tanky, 8)
  local boat = info.inboat
  local ammo = (info.shells or 0) + (info.mines or 0)

  -- ── Fresh-kill "sweep the pill you killed" stamp ──
  -- Attribution: a pill that was OUR attack_pill target and just went dead is
  -- our kill. Stamp it (id/pos/tick) so its capture gets a windowed wsim-danger
  -- damp below — go grab the pill you killed even if a neighbour will tag you.
  do
    local prev_id = state._attack_pill_target_id
    if prev_id then
      local p = world.pills and world.pills[prev_id]
      if p and (p.health or 0) == 0 and not p.in_tank then
        state._swept_kill = { id = prev_id, mx = p.mx, my = p.my, tick = state.tick }
        state._attack_pill_target_id = nil   -- stamped once; don't keep refreshing the window
      elseif (not p) or p.in_tank then
        state._attack_pill_target_id = nil   -- captured/gone — nothing to sweep
      end
    end
    -- Remember the target only while it's still ALIVE, so its death is what
    -- triggers the stamp exactly once (above).
    if state.goal and state.goal.kind == "attack_pill" and state.goal.target_id then
      local tp = world.pills and world.pills[state.goal.target_id]
      if tp and (tp.health or 0) > 0 then
        state._attack_pill_target_id = state.goal.target_id
      end
    end
  end
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
  -- Decisive refuel diagnostic: distinguishes the three "no refuel winner"
  -- causes — (a) no partial at all (build_eval_queue didn't queue refuel this
  -- cycle, e.g. needs_refuel was false), (b) candidates existed but ALL got
  -- reject-flagged (lists each id:reason), (c) a winner was chosen.
  if BRAIN_DEBUG_MODE then
    if not pr1 then
      print2(string.format("REFUEL_FINALIZE t=%d NO PARTIAL — refuel not queued this cycle (needs_refuel false / queue not built)", state.tick or 0))
    else
      local nc = pr1.candidates and #pr1.candidates or 0
      local nrej, nscored, nunscored = 0, 0, 0
      local rej_list, sc_list, un_list = {}, {}, {}
      if pr1.candidates then
        for _, cand in ipairs(pr1.candidates) do
          local ce = state.cost_cache and state.cost_cache["1:" .. cand.id]
          if ce and ce._reject then
            nrej = nrej + 1; rej_list[#rej_list + 1] = string.format("#%s:%s", tostring(cand.id), tostring(ce._reject))
          elseif ce and ce.cost and ce.cost < 1e29 then
            nscored = nscored + 1; sc_list[#sc_list + 1] = string.format("#%s:%.0f", tostring(cand.id), ce.cost)
          else
            nunscored = nunscored + 1; un_list[#un_list + 1] = string.format("#%s:%s", tostring(cand.id), ce and (ce.cost and "cost=" .. tostring(ce.cost) or "no-cost") or "no-entry")
          end
        end
      end
      print2(string.format("REFUEL_FINALIZE t=%d candidates=%d scored=%d rejected=%d UNSCORED=%d winner=%s cost=%s | scored=[%s] unscored=[%s]%s", state.tick or 0, nc, nscored, nrej, nunscored, tostring(pr1.best_id or "NONE"), pr1.best_obj and string.format("%.0f", pr1.best_cost or -1) or "NONE", table.concat(sc_list, ","), table.concat(un_list, ","), (#rej_list > 0) and (" rejects=[" .. table.concat(rej_list, ",") .. "]") or ""))
    end
  end
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

    -- ── Critical-armour need floor (same rule as eval_refuel) ──
    -- Applied AFTER the combat look-ahead so it can only ever make refuel more
    -- urgent, never less. `urgency` is inverse need, hence the 1-x mapping:
    -- at/below ARMOUR_LOW, need is at least REFUEL_CRITICAL_NEED_MIN.
    if (info.armour or 99) <= C.ARMOUR_LOW then
      local need = math.max(1.0 - urgency, C.REFUEL_CRITICAL_NEED_MIN or 0)
      urgency = 1.0 - need
    end

    local cost = bscore * urgency
    -- Practical cost floor — same rule as eval_refuel (see comment there):
    -- routine refuel never dips into the <REFUEL_MIN_COST reserved band;
    -- flee-level armour keeps the raw (cheap) cost.
    if (info.armour or 0) > C.ARMOUR_CRITICAL then
      cost = math.max(cost, C.REFUEL_MIN_COST)
    end
    -- Pool-panel display: candidates carry the raw bscore (travel+danger),
    -- but the winner that flows to the cross-pool "winners pool" is
    -- bscore*urgency. Scale each candidate's displayed cost by the same
    -- urgency so the pool-1 candidate list matches the winners-pool number.
    -- urgency is a uniform multiplier → the internal refuel order is
    -- unchanged. Rejected candidates (non-numeric cost) pass through as-is.
    -- (The REFUEL_FINALIZE log above already printed the raw bscores.)
    local disp_cands = pr1.candidates
    if BRAIN_POOL_VIZ and urgency ~= 1.0 and pr1.candidates then
      disp_cands = {}
      for i, cand in ipairs(pr1.candidates) do
        local nc = {}
        for k, v in pairs(cand) do nc[k] = v end
        if type(nc.cost) == "number" and nc.cost < 1e29 then nc.cost = nc.cost * urgency end
        disp_cands[i] = nc
      end
    end
    pc[1] = {
      cost = cost,
      goal = { kind = "refuel_at_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = BRAIN_POOL_VIZ and string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d sh=%d",
             bid, base.mx, base.my, bscore, urgency, cost, info.armour, info.shells) or "",
      cands = disp_cands,
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
      imminent = imminent3,
      goal = { kind = "capture_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid,
               race_mode = race_mode3 },
      desc = desc3,
      cands = pr3.candidates,
    }
  elseif state.goal and state.goal.kind == "capture_base" and state.goal.target_id
         and world.bases and world.bases[state.goal.target_id] then
    -- Queue-gap bridge: the pool-3 queue for this cycle was built BEFORE our
    -- attack_base kill flipped the target to capturable, so the partial has
    -- no candidate — and with pc[3]=nil the imminent-capture floor never
    -- runs and refuel steals the goal 3 tiles from a FREE base
    -- (20260704_003141 t=7506). While the CURRENT goal is a capture_base
    -- whose base is still capturable, synthesize its candidate live so the
    -- commitment survives the one-cycle gap.
    local gb = world.bases[state.goal.target_id]
    if gb.owner == "neutral" or (gb.owner == "hostile" and (gb.health or 0) == 0) then
      local c3 = smart_cost(KIND_NORMAL, tmx, tmy, gb.mx, gb.my, boat and 1 or 0,
                            info.shells or 32, info.trees or 0, info.mines or 0,
                            info.armour or 40)
      if c3 and c3 < 1e8 then
        local imminent3 = false
        if (gb.health or 0) == 0 and c3 <= C.IMMINENT_CAPTURE_PATH_COST
           and (info.armour or 0) >= C.IMMINENT_CAPTURE_MIN_ARMOUR then
          c3 = math.min(c3, C.IMMINENT_CAPTURE_FLOOR)
          imminent3 = true
        end
        pc[3] = {
          cost = c3,
          imminent = imminent3,
          goal = { kind = "capture_base", mx = gb.mx, my = gb.my,
                   wx = U.m2w(gb.mx), wy = U.m2w(gb.my),
                   target_id = state.goal.target_id,
                   race_mode = C.CAPTURE_RACE_MODE_CAPTURE },
          desc = BRAIN_POOL_VIZ and string.format(
                 "capture_base#%d@(%d,%d) cost=%.0f GOAL-BRIDGE%s",
                 state.goal.target_id, gb.mx, gb.my, c3,
                 imminent3 and " IMMINENT" or "") or "",
        }
      else
        pc[3] = nil
      end
    else
      pc[3] = nil
    end
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
    local desc4 = ""  -- pool viz string; populated only when BRAIN_POOL_VIZ
    if BRAIN_POOL_VIZ then
      desc4 = string.format("capture_pill#%d@(%d,%d) cost=%.0f", pid, pill.mx, pill.my, raw_cost4)
      if imminent4 then desc4 = desc4 .. " IMMINENT" end
      -- Which route the DIRECT-ROUTE probe settled on for this body, and why.
      -- NOT-PROBED is called out separately from SAFE-ROUTE: one means the
      -- direct run lost, the other means we never asked.
      local _r4 = state.capture_route and state.capture_route[pill.my * 256 + pill.mx]
      local _ce4 = state.cost_cache and state.cost_cache["4:" .. pid]
      if _r4 and _r4.mode == "direct" then
        desc4 = desc4 .. string.format(" DIRECT(wsim %ddmg, lives)", _r4.damage or 0)
      elseif _r4 and _r4.mode == "danger" then
        desc4 = desc4 .. string.format(" SAFE-ROUTE(direct was lethal, %ddmg)", _r4.damage or 0)
      elseif _ce4 and _ce4._route_far then
        desc4 = desc4 .. string.format(" NOT-PROBED(%d tiles > %d)",
                _ce4._route_far, C.CAPTURE_ROUTE_MAX_TILES or 10)
      end
    end
    -- TODO: Phase 6 race-loss should clear race_mode on captures we've decided not to win.
    -- See note on race_mode above (line ~644). Same dead-branch.
    local race_mode4 = C.CAPTURE_RACE_MODE_CAPTURE
    pc[4] = {
      cost = raw_cost4,
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

  -- Pool 5: repair_pill. best_cost is ALREADY the unified per-candidate
  -- repair score (base + max(0, path - dmg x bonus), contested x3; dead
  -- pills carry the rebuild-in-place model) computed in step_eval_queue —
  -- the pool now picks its winner by repair MERIT, not raw distance, and
  -- there is exactly one copy of the formula. Pass it straight through.
  local pr5 = partial[5]
  if pr5 and pr5.best_obj then
    local pill = pr5.best_obj
    local pid = pr5.best_id
    local pcost = pr5.best_cost
    local desc5 = ""
    if BRAIN_POOL_VIZ then
      if pill.health == 0 then
        desc5 = string.format("repair_pill#%d@(%d,%d) cost=%.0f REBUILD(dead)",
                              pid, pill.mx, pill.my, pcost)
      else
        local ce5 = state.cost_cache and state.cost_cache["5:" .. pid]
        desc5 = string.format("repair_pill#%d@(%d,%d) cost=%.0f (base+path-dam=%d×%d)%s",
                pid, pill.mx, pill.my, pcost,
                ce5 and ce5._dmg or (C.PILLS_MAX_HEALTH - pill.health),
                C.REPAIR_DAMAGE_BONUS,
                (ce5 and ce5._contested) and " ×3 CONTESTED" or "")
      end
    end
    pc[5] = {
      cost = pcost,
      goal = { kind = "repair_pill", mx = pill.mx, my = pill.my,
               wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid },
      desc = desc5,
      cands = pr5.candidates,
    }
  else
    pc[5] = nil
  end

  local _t_p6start = clock_us()  -- pools 1-5 finalized above; pool 6 below
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
    pc[6] = {
      cost = pcost,
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

  local _t_p6end = clock_us()  -- pool 6 (attack_pill) done; pool 7 below
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

  if BRAIN_PROFILE then
    local _t_pf_end = clock_us()
    opt(string.format("    fp pool-finalizers %.2f ms  (pools1-5 %.2f, pool6_attack_pill %.2f, pool7 %.2f)",
        (_t_pf_end - _t1) / 1000,
        (_t_p6start - _t1) / 1000,
        (_t_p6end - _t_p6start) / 1000,
        (_t_pf_end - _t_p6end) / 1000))
  end
  local _t2 = clock_us()
  -- Run cheap evaluators directly
  local _ev_ms = {}
  for _, idx in ipairs(FINALIZE_POOLS) do
    local _te = clock_us()
    local evaluator = POOL_EVALUATORS[idx]
    if evaluator then
      pc[idx] = evaluator(state, world, info, tmx, tmy, boat, ammo)
    end
    local _dt = (clock_us() - _te) / 1000
    _ev_ms[idx] = _dt
    if BRAIN_PROFILE then opt(string.format("    fp eval[%d] %.2f ms", idx, _dt)) end
    -- PROFILING LITE: single-eval outlier (e.g. place_pill_strategic spiking
    -- to ~5 ms). Reuses _dt — no extra clock read — and formats nothing until
    -- an eval actually blows past 1.5 ms.
    if BRAIN_DEBUG_MODE and _dt > 1.5 then
      print2(string.format("EVAL_SLOW t=%d pool=%s ms=%.2f",
        state.tick or 0, tostring(POOL_NAMES[idx] or ("p" .. idx)), _dt))
    end
  end
  local _t3 = clock_us()

  -- Summary: which pools got finalized + a phase-by-phase time breakdown so
  -- a slow finalize_pools self-reports WHERE the time went (no need to enable
  -- BRAIN_PROFILE). FINALIZE_POOLS = {2,8,9,10}: 2=defend_pill,
  -- 8=place_pill_strategic, 9=attack_tank, 10=reposition.
  if BRAIN_DEBUG_MODE then
    do
      local names = {}
      local n = 0
      for i = 1, 12 do
        if pc[i] then
          n = n + 1
          names[#names + 1] = (POOL_NAMES[i] or ("p" .. i)) .. "(" ..
            string.format("%.0f", pc[i].cost or -1) .. ")"
        end
      end
      print2("finalize_pools: ", n, "/12 pools — ", table.concat(names, ", "))
      print2(string.format(
        "fp_time: total=%.2f sync=%.2f apply=%.2f backfill=%.2f pools1-5=%.2f pool6=%.2f pool7=%.2f evals=%.2f"
        .. " [eval2_defend=%.2f eval8_place=%.2f eval9_tank=%.2f eval10_repos=%.2f]",
        (_t3 - _tpre) / 1000,
        (_t_sync - _tpre) / 1000, (_t_apply - _t_sync) / 1000,
        (_t1 - _t0) / 1000,
        (_t_p6start - _t1) / 1000, (_t_p6end - _t_p6start) / 1000, (_t2 - _t_p6end) / 1000,
        (_t3 - _t2) / 1000,
        _ev_ms[2] or 0, _ev_ms[8] or 0, _ev_ms[9] or 0, _ev_ms[10] or 0))
    end
  end
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

  -- Deferred queue rebuild (C.INCREMENTAL_REPLAN): when the replan tick
  -- chose to defer build_eval_queue to spread its cost, run it here on the
  -- following tick — before step_eval_queue so the fresh queue is what we
  -- step through. update_pool_cache runs earlier in the tick than the
  -- replan block, so this never collides with the tick that set the flag.
  if state._deferred_build_eval then
    state._deferred_build_eval = nil
    M.build_eval_queue(state, world, info)
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
  if BRAIN_DEBUG_MODE and vizmod.is_on("attack_scan_spots_all_pills") and state._pill_diff_cache then
    local now = state.tick or 0
    for _, dc in pairs(state._pill_diff_cache) do
      if dc.spots and dc.mx then
        local age  = now - (dc.tick or 0)
        local mode = (age <= 0) and "all"
                  or (age == 1) and "bucket"
                  or "winner"
        local cdeg = dc.spot and dc.spot.deg or nil
        attack.draw_pill_eval_spots(dc.spots, dc.mx, dc.my,
                                    "attack_scan_spots_all_pills",
                                    mode, cdeg)
      end
    end
  end

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
    local tmx   = bit.rshift(info.tankx, 8)
    local tmy   = bit.rshift(info.tanky, 8)
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

      -- LOS fast-engage: LGM in range + clear LOS = cheap LOS branch.
      local los_range = R + C.TANK_COMBAT_LOS_EXTRA_RANGE
      local los_engage = (lgm.dist or 1e9) <= los_range
                     and PF.wall_hp_between(tmx, tmy, lgm.mx, lgm.my) == 0

      local cost, formula_str, shoot_mx, shoot_my
      local best_shoot_cost = math.huge

      -- Far-preempt guard (GLOBAL) — ALWAYS computed so it appears as a row in
      -- the cost breakdown table (the table parses name{value} terms out of the
      -- DISPLAY half of the formula). An LGM beyond shoot range gets an
      -- exponential euclidean-distance penalty on EVERY goal now (was gated on an
      -- attack_pill take; made global by design) so the bot won't chase a far LGM
      -- across the map; otherwise it's 0 and {value} carries the edist-vs-range
      -- reason. far_preempt_pen folds into cost.
      local far_preempt_pen = 0
      local far_val
      do
        local _shoot_r = C.ATTACK_FAR_PREEMPT_RANGE or C.TANK_COMBAT_ENGAGE_RANGE or 7
        local _ex, _ey = (lgm.mx - tmx), (lgm.my - tmy)
        local _edist = math.sqrt(_ex * _ex + _ey * _ey)
        if _edist > _shoot_r then
          far_preempt_pen = math.min(
            (C.ATTACK_FAR_PREEMPT_BASE or 1.7) ^ (_edist - _shoot_r) * (C.ATTACK_FAR_PREEMPT_K or 8),
            C.ATTACK_FAR_PREEMPT_CAP or 1e6)
          far_preempt_pen = far_preempt_pen * harass_dist_mult(state)
          far_val = string.format("%.0f", far_preempt_pen)
        else
          far_val = string.format("0 e%.1f<=%d", _edist, _shoot_r)
        end
      end

      if los_engage then
        -- (LOS_BASE + dist*LOS_PER_TILE + low_sh) * boat + threat
        local raw = C.TANK_COMBAT_LOS_BASE_COST
                  + (lgm.dist or 0) * C.TANK_COMBAT_LOS_COST_PER_TILE * harass_dist_mult(state)
                  + low_shells_penalty
        cost = raw * boat_mult + tank_tile_threat + far_preempt_pen
        -- LOS engage shoots from where we stand.
        shoot_mx, shoot_my = tmx, tmy
        formula_str = string.format(
          "kill_lgm@(%d,%d) LOS (los_base{%.0f} + dist{%.1f}*per_tile{%.0f} + low_sh{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%s} = %.0f"..
          "||dist=%.1f; aim_diff=%.1f; shells=%d; near_tank=%s",
          lgm.mx, lgm.my,
          C.TANK_COMBAT_LOS_BASE_COST, lgm.dist or 0,
          C.TANK_COMBAT_LOS_COST_PER_TILE,
          low_shells_penalty, boat_mult, tank_tile_threat, far_val, cost,
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

        -- Crossfire: NEW pill exposure at the shoot spot (shoot_mx,shoot_my) —
        -- pills covering it but not our current tile. Escalating per new pill.
        -- (The LOS branch shoots from where we stand, so it has none.)
        local crossfire = new_pill_crossfire(world, shoot_mx, shoot_my, tmx, tmy)
        -- (A* + base + low_sh - aim + xfire) * boat (if <1) + threat
        local raw = path_cost * harass_dist_mult(state) + KILL_LGM_BASE_COST + low_shells_penalty
                  - aim_bonus + crossfire
        if boat_mult < 1.0 then raw = raw * boat_mult end
        cost = raw + tank_tile_threat + far_preempt_pen
        local cold = best_shoot_cost >= 1e29
        formula_str = string.format(
          "kill_lgm@(%d,%d) standoff (A*{%.0f}%s + base{%.0f} + low_sh{%.0f} - aim{%.0f} + xfire{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%s} = %.0f"..
          "||shoot_from=(%d,%d); dist=%.1f; aim_diff=%.1f; shells=%d; near_tank=%s",
          lgm.mx, lgm.my,
          path_cost, cold and " (cold)" or "",
          KILL_LGM_BASE_COST, low_shells_penalty,
          aim_bonus, crossfire, boat_mult, tank_tile_threat, far_val, cost,
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
            local pmx = bit.rshift(math.floor(px), 8)
            local pmy = bit.rshift(math.floor(py), 8)
            if not U.in_map(pmx, pmy) then break end
            local plist = world.pill_at and world.pill_at[pmy * 256 + pmx]
            if plist then
              for _, e in ipairs(plist) do
                if e.pill and e.pill.health and e.pill.health > 0
                   and e.pill.health < C.PILLS_MAX_HEALTH
                   and (e.pill.owner == "hostile" or e.pill.owner == "neutral") then
                  -- Check if the trajectory aims near the pill center
                  local pcx = bit.bor((bit.lshift(pmx, 8)), 128)
                  local pcy = bit.bor((bit.lshift(pmy, 8)), 128)
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
                -- our_ticks is math.huge when our_cost is nil; %.0f (not %d) so
                -- inf formats as "inf" instead of crashing "no integer representation".
                string.format(" +REPAIR_FUTILE{%d}(pill@(%d,%d) hp=%d lgm_arr=%dt our=%.0ft)",
                  repair_penalty, pill_mx, pill_my,
                  repair_pill.health, lgm_arrive_ticks,
                  our_ticks)
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
  local tmx  = bit.rshift(info.tankx, 8)
  local tmy  = bit.rshift(info.tanky, 8)
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
  local tmx    = bit.rshift(info.tankx, 8)
  local tmy    = bit.rshift(info.tanky, 8)
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
    local lgm_mx = bit.rshift(info.man_x, 8)
    local lgm_my = bit.rshift(info.man_y, 8)

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
        -- Don't drive off to fetch the LGM while a hostile tank or
        -- pill is actively shooting at us: under_fire (shell trajectory
        -- over our tile / angry pill in range) or _shot_by_tank (took
        -- tank damage with no predicted shell — point-blank etc.). The
        -- stranded flag stays set, so the rescue fires as soon as the
        -- shooting stops; until then the normal pools deal with the
        -- attacker.
        local fire_suppress = ((state.perc and state.perc.under_fire)
                               or state._shot_by_tank) or false
        if state._lgm_stranded_factors then
          state._lgm_stranded_factors.fire_suppress = fire_suppress
        end
        if fire_suppress then
          log.reason("goal", {
            pick = "rescue_lgm-suppressed",
            why = "under fire (hostile tank/pill shooting at us)",
            mx = lgm_mx, my = lgm_my,
          })
        else
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
  -- CRITICAL_FLEE_ENABLED modes (see constants.lua): the carrying-pills mode
  -- scales flee eagerness by a haul-protection level — FULL when the builder is
  -- DEAD or out on a mission (pills can't be placed, pure liability), and a
  -- weaker, pill-count-scaled level when the builder is still in the tank.
  local _flee_mode = C.CRITICAL_FLEE_ENABLED
  local _do_flee = false
  if _flee_mode == "no_builder_and_carrying_only" then
    local _carry = info.carried_pills or 0
    local _man   = info.man_status
    -- Haul-protection LEVEL (0..1): how hard to bail to save the pills we carry.
    --   No builder (DEAD) or builder committed OUT on a mission → pills can't be
    --     placed soon and are pure liability → FULL protection at carry >= 1.
    --   Builder still in tank → we can place them ourselves, so protect only a
    --     STACK (carry >= 2), slightly weaker, ramping to full by FLEE_HAUL_FULL_PILLS.
    local _level = 0
    if _carry >= 1 and (_man == C.LGM_DEAD or _man == C.LGM_MOVING) then
      _level = 1.0
    elseif _carry >= 2 and _man == C.LGM_INTANK then
      local _full = (C.FLEE_HAUL_FULL_PILLS or 4)
      _level = math.min(1.0, 0.5 + 0.5 * (_carry - 2) / math.max(1, _full - 2))
    end
    if _level > 0 then
      -- Triggers scale with level: a stronger level reaches further for a
      -- threatening tank and bails at higher armour; only full strength (level 1)
      -- also bails on mere hostile-pill coverage. Critical armour always bails.
      local _range = (C.FLEE_HAUL_TANK_RANGE or 12) * _level
      local _tank_engaging = state.perc and state.perc.nearest_hostile_tank
                             and (state.perc.nearest_hostile_tank.dist or math.huge) <= _range
      local _pill_shooting = _level >= 1.0 and threat.pill_at(tmx, tmy) > 0
      local _arm_low = info.armour <= (C.ARMOUR_LOW * _level)
      _do_flee = _tank_engaging or _pill_shooting or _arm_low or critical
    end
  elseif _flee_mode then
    _do_flee = critical
  end
  if _do_flee then
    if critical and info.base then
      local base_useless = true
      if info.armour < C.TANK_FULL_ARMOUR and (info.base.armour or 0) > 0 then base_useless = false end
      if info.shells < C.TANK_FULL_SHELLS and (info.base.shells or 0) > 0 then base_useless = false end
      if base_useless then
        U.set_blocked(state, U.mkey(tmx, tmy), state.tick + 200, "refuel_base_useless")
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
      if BRAIN_DEBUG_MODE then print(string.format(TAG .. " CAPTURE: pill #%d captured!", co.id)) end
      state.command_reply     = string.format(C.BRAIN_NAME .. ": pill #%d captured!", co.id)
      state.capture_objective = nil
    elseif p.health == 0 then
      if not co.kill_tick then
        co.kill_tick = state.tick
        if BRAIN_DEBUG_MODE then
          print(string.format(TAG .. " CAPTURE: pill#%d killed at t=%d — holding %d ticks for shots to clear",
                co.id, co.kill_tick, C.POST_KILL_WAIT_TICKS))
        end
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
  -- Override 3b: Fresh-kill pickup (autonomous). WE (or our blitz) just
  -- dropped this pill to 0 — grab the body HARD, overriding refuel / flee /
  -- survival ENTIRELY (only an enemy/ally actually taking the pill stops us).
  -- A wasted kill hands the pill back to the enemy, so this is worth dying
  -- for. When several blitz members claim the same pill, the one with the
  -- LOWEST capture_pill score grabs (tie → lower player number); the rest
  -- stand down and the whole team treats the pill as claimed (the kg/_kill_-
  -- claimed reject in build_eval_queue). Mirrors Override 3 (cp command).
  -- ════════════════════════════════════════════════════════════════════
  if not result and C.KILL_PICKUP_ENABLED and state.kill_pickup then
    local kp  = state.kill_pickup
    local now = state.tick or 0
    local p   = world.pills[kp.id]
    -- Rolling TTL (since last kill/refresh) OR the absolute commitment cap
    -- (since first claim) — the cap stops a walled-in grabber refreshing forever.
    local ttl_lapsed = (now - (kp.kill_tick or now)) > (C.KILL_PICKUP_TTL or 250)
    local capped     = (now - (kp.created_tick or kp.kill_tick or now))
                       > (C.KILL_PICKUP_MAX_TICKS or 1000)
    -- Claim dies when: TTL lapsed, capped, pill gone, already scooped (in_tank),
    -- or (re)captured by the team (friendly & alive again). in_tank is the
    -- "enemy/ally took it" exit — the one interruption we honor.
    if ttl_lapsed or capped or not p or p.in_tank
       or (p.owner == "friendly" and (p.health or 0) > 0) then
      state.kill_pickup = nil
    elseif (p.health or 0) == 0 then
      -- Our own capture_pill score for the body — drives BOTH the handoff
      -- comparison and the unreachable WAY OUT below. compute_pool4_cost returns
      -- the INF sentinel (>=1e29) when there's no path within the A* budget
      -- (walled in, or simply too far).
      local my_cost   = compute_pool4_cost(state, world, info, p, tmx, tmy)
      local reachable = my_cost and my_cost < 1e29
      local mc        = reachable and my_cost or 1e9
      -- Handoff: defer to the blitz member with the LOWEST capture_pill score
      -- (the same balanced metric the goal selector uses) that also claims
      -- this kill; tie → lower player number. We broadcast our own score as kc.
      local yield_to, yield_cost = nil, nil
      if C.KILL_PICKUP_HANDOFF and ally_state.iter_active and info.player_number then
        local my_pn  = info.player_number
        local tdead  = state.tank_dead_at
        for apn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
          -- Don't yield to a DEAD claimer (its kg/kc broadcast lingers but it
          -- can't grab) — else a live bot stands down for a corpse and the pill
          -- goes unclaimed.
          local is_dead = tdead and tdead[apn] and tdead[apn] > (slot.last_tick or 0)
          if apn ~= my_pn and not is_dead then
            local si = slot.info
            if si and si.kg and tonumber(si.kg) == kp.id then
              local a_cost = tonumber(si.kc) or 1e9
              -- An ally beats us if its score is lower, or equal + lower pn.
              -- Track the BEST such ally so the viz names the real grabber.
              if a_cost < mc or (a_cost == mc and apn < my_pn) then
                if not yield_cost or a_cost < yield_cost
                   or (a_cost == yield_cost and apn < (yield_to or 1e9)) then
                  yield_to, yield_cost = apn, a_cost
                end
              end
            end
          end
        end
      end
      kp._yield_to = yield_to  -- for viz
      if not reachable then
        -- WAY OUT (unreachable): no path to the body within the A* budget —
        -- walled in or simply too far. Drop the claim so the pill reopens to
        -- ANYONE via the normal capture_pill pool, instead of us holding it
        -- hostage until the absolute timeout (the `capped` check at the top of
        -- this block) fires. Closer/abler bots take it.
        if BRAIN_DEBUG_MODE then print(string.format(TAG .. " KILLGRAB: pill#%d unreachable (cost INF) — dropping claim", kp.id)) end
        state.kill_pickup = nil
      elseif yield_to then
        -- A lower-score blitz member is grabbing. Stand DOWN AND OUT: drop our
        -- claim entirely. We do NOT keep a backup or watch for the grabber to
        -- fail — by then we've usually moved on (refueling, etc.), and holding
        -- extra priority for a pill we're nowhere near makes no sense. If the
        -- grabber fails, the dead pill is simply OPEN to anyone via the normal
        -- capture_pill pool. Clearing also stops us advertising kg, so nobody
        -- defers to a bot that isn't grabbing (saw bot1 yield to bot8 while bot8
        -- was off on refuel_at_base, leaving #2 unclaimed).
        state.kill_pickup = nil
        if BRAIN_DEBUG_MODE then print(string.format(TAG .. " KILLGRAB: pill#%d -> yielding to p%d (its score %.0f < ours), dropping claim", kp.id, yield_to, yield_cost or -1)) end
      else
        -- We're the grabber. Refresh the rolling TTL so the claim survives the
        -- whole drive-in (uninterruptible until the pill is taken/captured),
        -- bounded only by the absolute cap above. Hold briefly first so our
        -- in-flight shots clear, then force-win capture_pill. _grabbing=true →
        -- we advertise kg so others defer to us (we ARE going for it).
        kp.kill_tick = now
        kp._grabbing = true
        local waited = now - (kp.created_tick or now)
        if waited < (C.POST_KILL_WAIT_TICKS or 15) then
          result = { kind = "none", mx = tmx, my = tmy, wx = U.m2w(tmx), wy = U.m2w(tmy) }
          desc = string.format("killgrab#%d@(%d,%d) [clearing shots %d/%d]",
                 kp.id, p.mx, p.my, waited, C.POST_KILL_WAIT_TICKS)
        else
          result = { kind = "capture_pill", mx = p.mx, my = p.my,
                     wx = U.m2w(p.mx), wy = U.m2w(p.my), target_id = kp.id,
                     race_mode = true, kill_grab = true }
          desc = string.format("killgrab#%d@(%d,%d) [HARD pickup]", kp.id, p.mx, p.my)
        end
      end
    end
    -- p.health>0 & not friendly: enemy repaired it before we grabbed. Leave the
    -- claim; normal attack_pill re-engages and the TTL lapses if it stays alive.
  end

  -- Fresh-kill pickup overlay: yellow ring on the claimed pill + a line from
  -- the tank, magenta when we've yielded to a lower-score ally.
  if BRAIN_DEBUG_MODE and vizmod.is_on("kill_pickup") and state.kill_pickup then
    local kp = state.kill_pickup
    local p  = world.pills[kp.id]
    local kx, ky = (p and p.mx or kp.mx) + 0.5, (p and p.my or kp.my) + 0.5
    local yielded = kp._yield_to ~= nil
    local r, g, b = 255, 230, 40
    if yielded then r, g, b = 255, 60, 255 end
    vizmod.circle("kill_pickup", kx, ky, 0.6, r, g, b, 255)
    vizmod.circle("kill_pickup", kx, ky, 0.85, r, g, b, 140)
    vizmod.line("kill_pickup", (info.tankx / 256.0), (info.tanky / 256.0), kx, ky, r, g, b, 200)
    vizmod.text("kill_pickup", kx, ky - 1.0,
      yielded and string.format("KILLGRAB#%d -> p%d", kp.id, kp._yield_to)
              or  string.format("KILLGRAB#%d MINE", kp.id),
      "center", r, g, b, 255)
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
        if BRAIN_DEBUG_MODE then print(TAG .. " CAPTURE: all bases captured!") end
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
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " CAPTURE: base #%d captured, continuing cb:all", bco.id))
          end
          bco.id = nil; bco.mx = 0; bco.my = 0
          state.pf.status = "idle"
          state.stuck_for = 0
        else
          if BRAIN_DEBUG_MODE then
            print(string.format(TAG .. " CAPTURE: base #%d captured!", bco.id))
          end
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
      print2("goal_selection: pool_cache has ", pc_count, " entries: ", table.concat(pc_names, ","))
    end
    -- ── Refuel live cost-shape (pool 1) ─────────────────────────────────
    -- Compute once per tick. The same values feed (a) the pool-1 cost
    -- competition shaping below and (b) the Term Breakdown panel via a
    -- stash onto every per-base pool-1 cost_cache entry. eval_refuel
    -- already baked the multiplicative urgency into pool_cache[1].cost,
    -- so we recompute it here purely for the breakdown display.
    local _ref_bonus, _ref_mult, _ref_fill, _ref_scarcity, _ref_mine_cost,
          _ref_urgency, _ref_arm_def, _ref_sh_def = refuel_shape(info, state, now)
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
          e._scarcity       = _ref_scarcity
          -- Mine-hoard surcharge only applies to the base we're parked on (see
          -- the cost path below) — show 0 on every other base so the panel matches.
          e._mine_cost      = (e._mx == tmx and e._my == tmy) and _ref_mine_cost or 0
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
      if entry and not entry._reject then
        -- Skip entries whose destination is blocked (e.g. by goal lookahead)
        local gmx = entry.goal and entry.goal.mx
        local gmy = entry.goal and entry.goal.my
        if gmx and gmy and state.blocked then
          local bk = U.mkey(gmx, gmy)
          if state.blocked[bk] and now < state.blocked[bk] then
            goto continue_pool
          end
        end
        -- Skip goal shapes the tick-budget kill catch-all abandoned. Without
        -- this the forced goal=none right after a THINK_BLACKLIST just
        -- re-selects the same unfittable goal and the livelock resumes.
        -- Survival exemption (read side; the write side in init.lua's kill
        -- catch-all declines to add these in the first place): a bot at/below
        -- ARMOUR_LOW must never be denied the goal that restores armour, and
        -- the _critical_flee injection IS the survival goal by construction.
        -- Being budget-killed on the way to a base is a much smaller problem
        -- than being benched from resupply for GOAL_BLACKLIST_TICKS at 0 armour.
        local _bl_exempt = entry._critical_flee
          or ((entry.goal and (entry.goal.kind == "refuel_at_base"
                               or entry.goal.kind == "flee_to_base"))
              and (info.armour or 99) <= C.ARMOUR_LOW)
        if entry.goal and state._goal_blacklist and not _bl_exempt
           and U.goal_blacklisted(state, entry.goal, now) then
          goto continue_pool
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
            -- COMPLETION decline, not a mid-requeue gap. Flag it for the
            -- walkover guard below: this pool DID produce an entry this
            -- cycle and we are dropping it on purpose because the refuel
            -- is finished. Without the flag the guard reads "no entry for
            -- the current goal" and resurrects refuel_at_base from
            -- pool_cache at its last finalized cost, which then wins on
            -- stickiness forever — the livelock in 20260828_111758 bot2
            -- (114 straight ticks parked at arm=40 on base (143,115)).
            --
            -- Flagged by goal KIND, not by this entry's base: the test
            -- above is a property of the TANK (armour/shells vs target),
            -- so EVERY refuel candidate is declined for the same reason,
            -- including one at a base that isn't the one we're sitting on.
            -- Keyed on the tick so a stale flag can never suppress a
            -- legitimate carry-forward on a later cycle.
            if state.goal and state.goal.kind == "refuel_at_base" then
              state._pool_decline_complete = { tick = now, kind = "refuel_at_base" }
            end
            goto continue_pool   -- at dynamic target and no LGM waiting: don't compete
          end
          -- Reuse the live shape computed above (also stashed on cost_cache
          -- for the breakdown panel).
          local bonus = _ref_bonus
          local mult  = _ref_mult
          local base_cost = (entry.cost or 0) - bonus
          -- Scarcity-scaled top-off (mult) + exponential mine-hoard surcharge.
          -- The mine-hoard surcharge ONLY applies to the base we're parked on
          -- (at_this_base): its purpose is to make a mine-stuffed tank LEAVE
          -- instead of lingering to top off mines — NOT to price the tank out
          -- of TRAVELLING to a base to resupply armour/shells (that would make
          -- a mine-heavy tank refuse to refuel; the surcharge is exponential and
          -- uncapped, so on remote candidates it dwarfs the real cost).
          local final_cost = base_cost * mult + (at_this_base and _ref_mine_cost or 0)
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
        -- Apply phase-dependent weight as a multiplier on the full cost.
        local pool_name = POOL_NAMES[idx]
        local pw = phase_weights and pool_name and phase_weights[pool_name] or 1.0
        -- Distance-attenuate the phase bias toward neutral (1.0): the phase
        -- preference is a LOCAL strategy, so a discounted goal far across the map
        -- shouldn't get pulled in (and distance already dominates a far penalised
        -- one). Full weight at the tank, lerping to 1.0 by PHASE_WEIGHT_DIST_FALLOFF.
        if pw ~= 1.0 and entry.goal and entry.goal.mx then
          local _fot = C.PHASE_WEIGHT_DIST_FALLOFF
          local _fo  = (type(_fot) == "table" and ((pool_name and _fot[pool_name]) or _fot.default))
                       or (type(_fot) == "number" and _fot) or 40
          local _gd = U.mdist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8), entry.goal.mx, entry.goal.my)
          local _f  = math.min(1.0, _gd / _fo)
          pw = 1.0 + (pw - 1.0) * (1 - _f)
        end
        if cost and cost > 0 then
          cost = cost * pw
        end
        -- Mid-take engage-break: hard-cap under pool-6's mid-take floor
        -- (10) so phase weight can't push it back over.
        if entry._engage_break_lock then
          cost = math.min(cost, 9)
        end
        pool[#pool + 1] = {
          cost = cost, _base_cost = cost,  -- _base_cost preserved for breakdown display
          goal = entry.goal, desc = entry.desc,
          cands = entry.cands, _pill = entry._pill, _pill_id = entry._pill_id,
          phase_weight = pw,
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
      -- repair_pill has its own danger model (dead-pill: tank-snipe, advantage-
      -- scaled; pill-fire ignored) and should NOT be ×2'd for sitting in enemy
      -- influence — but only under the repair fix. Off → 1.90-beta1: subject to
      -- the influence ×0.5/×2 like any other goal.
      if C.REPAIR_FIX_ENABLED then INF_EXEMPT.repair_pill = true end
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

    -- ── Pillbox-suicider cost shaping ──
    -- THE choke point for the suicider role's goal preferences: one pass over
    -- the assembled pool, same layer as the influence scaling above (a whole-
    -- cost multiplier applied after the phase weight, before hysteresis).
    -- attack_pill and the refuel group ride at x1; defend_pill pays
    -- PILL_SUICIDER_DEFEND_MULT, everything else PILL_SUICIDER_OTHER_MULT.
    -- No-op for every bot that isn't a suicider (suicider_cost_mult -> 1.0),
    -- and unconditional on phase — unlike influence, this is who the bot IS,
    -- not where the goal sits. _suicider_mult is stashed for the WINNERS-row
    -- reconciliation exactly like _inf_mult.
    if state.is_pill_suicider then
      for _, c in ipairs(pool) do
        local sm = suicider_cost_mult(state, c.goal and c.goal.kind)
        if sm ~= 1.0 and c.cost and c.cost > 0 then
          c.cost = c.cost * sm
          c._suicider_mult = sm
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
    -- attack_pill's disengage / plan_position are non-committed substates
    -- (breaking off, or still choosing a standoff — no shells/position
    -- invested yet). In those, let attack_tank preempt on raw cost just as
    -- if we weren't mid-take, so a tank that wins the pool interrupts
    -- immediately instead of finishing the maneuver.
    local _cur_sub = state.goal and state.goal.substate
    local atk_pill_loose = cur_is_attack_pill
        and (_cur_sub == "disengage" or _cur_sub == "plan_position")
    -- kill_lgm: ALWAYS exempt.  Enemy LGMs are extremely time-critical
    -- (1-shot kill, ~10s of vulnerability before they return to tank)
    -- and the cost formula (20 + dist*1.5) is already very low — adding
    -- switch+commitment penalty on top reliably pushes it above any
    -- attack_pill incumbent, so the bot never actually picks the kill.
    local HYST_EXEMPT = { capture_pill = true, kill_lgm = true }
    -- attack_tank is ALWAYS exempt from cross-type hysteresis: a reactive
    -- self-defense response should never be held back by switch/commitment
    -- penalties (even when we're mid-pill-take). Local (same-group, target-to-
    -- target) hysteresis still applies via the HYST_EXEMPT branch below.
    HYST_EXEMPT.attack_tank = true
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
    local cur_is_attack_base = (state.goal.kind == "attack_base")
    local cur_is_capture_base = (state.goal.kind == "capture_base")
    for _, c in ipairs(pool) do
      -- defend_pill vs an in-progress attack_pill: tier the resistance by the
      -- take's real investment (see ATK_SHOOTING_SUBS / ATK_BUILD_SUBS).
      --   free     → skip hysteresis entirely (approach/planning — nothing lost)
      --   moderate → a small flat commitment (building the shield)
      --   full/nil → normal hysteresis (actually shooting)
      local _defend_tier = nil
      if cur_is_attack_pill and c.goal.kind == "defend_pill" then
        if ATK_SHOOTING_SUBS[cur_sub] then _defend_tier = "full"
        elseif ATK_BUILD_SUBS[cur_sub] then _defend_tier = "moderate"
        else _defend_tier = "free" end
      end
      -- Engage-break: an attack_tank goal that detected a mid-take
      -- threat (tank in range, further from pill than us) is exempt
      -- from both additive SW+CM penalty AND the multiplicative ratio
      -- gate (the gate only fires for entries with c.hysteresis set,
      -- which we leave nil here).
      if HYST_EXEMPT[c.goal.kind] or c.goal._place_forced or c._engage_break_lock
         or (state.ammo_deprived and c.goal.kind == "attack_pill")
         or _defend_tier == "free" then
        -- Ammo-deprived decoy: charging the pill to draw fire is a "drop
        -- everything and go" action like attack_tank, so switching TO it is
        -- free (skip the type-switch hysteresis that would otherwise price
        -- the finite base ~68 up past explore's ~500 and trap it exploring).
        -- Exempt goals skip the type-switch penalty (switching from
        -- another group is free). But within the same group, the
        -- target-switch penalty still applies to prevent spinning
        -- between targets (e.g. two capture_base candidates).
        -- _place_forced: the threat-reactive "build while fighting" drop.
        -- It exists precisely for the panic scenario, so it must be free to
        -- preempt kill_lgm/attack_tank instead of being buried under the
        -- +switch+commitment penalty (which it can never out-cost otherwise).
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
      -- Attack-base follow-through: once committed to grinding a base, stay on it
      -- hard. The ONLY non-urgent reason to break off is literally running dry
      -- (0 shells) — with any ammo left, finish the job. Critical-armour flee
      -- still preempts via init.lua's urgent goal-override (separate from this).
      if cur_is_attack_base and (info.shells or 0) > 0 then
        effective_commit = effective_commit + C.ATTACK_BASE_COMMITMENT_BONUS
      end
      -- Capture follow-through: once you've started capturing a base (driving
      -- onto a neutral / just-ground-down base), hold it as hard as the grind.
      -- No shell gate — capturing needs no ammo. Critical-armour flee still
      -- preempts via init.lua's urgent goal-override (separate from this).
      if cur_is_capture_base then
        effective_commit = effective_commit + C.CAPTURE_BASE_COMMITMENT_BONUS
      end
      -- defend_pill interrupting a wall-BUILDING attack: replace the full
      -- attack-pill commitment stack with a modest flat so a clearly cheaper
      -- defense can preempt without thrashing a recoverable build.
      if _defend_tier == "moderate" then
        effective_commit = C.DEFEND_ATTACK_BUILD_COMMITMENT or 40
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
        -- Cap the COMBINED sum, not each term separately. The two terms are a
        -- single "how often have I been here lately" signal read at two
        -- granularities and they are always added together below, so a cap on
        -- the sum is the only one that actually bounds what the pool sees;
        -- capping them individually would still allow 2x the ceiling.
        local hist_pen = target_pen + kind_pen
        if hist_pen > (C.GOAL_HISTORY_PEN_CAP or math.huge) then
          hist_pen = C.GOAL_HISTORY_PEN_CAP
        end
        -- Survival exemption: anti-thrash must never out-price staying alive.
        -- At/below ARMOUR_LOW (or out of shells) resupply carries no ratchet at
        -- all — the whole point of the history penalty is to break pointless
        -- loops, and "keep going back for armour while dying" is not one.
        if (c.goal.kind == "refuel_at_base" or c.goal.kind == "flee_to_base")
           and ((info.armour or 99) <= C.ARMOUR_LOW or (info.shells or 99) <= 0) then
          hist_pen = 0
        end
        if hist_pen > 0 then
          c.cost = c.cost + hist_pen
          c.hist_target = target_count
          c.hist_kind   = kind_count
        end
        ::continue_hist::
      end
    end

    if not quiet and BRAIN_DEBUG_MODE then
      print2("goal_selection: pool size=", #pool, " cur_group=", cur_group, " commitment=", commitment)
    end
    -- A NORMAL place_pill_strategic must never out-rank an attack_tank: fighting
    -- a tank beats casually dropping a pill. The EMERGENCY defensive build
    -- (goal._place_forced, set on the offensive_build path) is exempt — that's the
    -- "build now while fighting before I die" behavior and stays as-is. Done on
    -- the post-penalty pool costs so it's weight/penalty aware.
    do
      local at_cost, place_entry
      for _, e in ipairs(pool) do
        if e.goal then
          if e.goal.kind == "attack_tank" and e.cost
             and (not at_cost or e.cost < at_cost) then at_cost = e.cost end
          if e.goal.kind == "place_pill_strategic" and not e.goal._place_forced then
            place_entry = e
          end
        end
      end
      if at_cost and place_entry and place_entry.cost <= at_cost then
        place_entry.cost = at_cost + 1
      end
    end
    -- ── Sort by cost, pick winner ──
    table.sort(pool, function(a, b) return a.cost < b.cost end)
    -- Save the post-penalty competition for the pool breakdown display.
    -- Phase 0 scaffolding: loc_mult/density/pickup/wsim_add are carried
    -- through with safe defaults; Phases 1–4 will populate them on `c`
    -- before we reach this point. wsim_add is patched in after the wsim
    -- pass below since wsim runs later in the pipeline.
    -- Pool-grid panel data only — wrapped so lua_strip removes it from opt/.
    if BRAIN_DEBUG_MODE then
    state.goal_competition = {}
    for _, c in ipairs(pool) do
      local penalty = c.cost - (c._base_cost or c.cost)
      -- DIAGNOSTIC: dump everything we know when cost is suspiciously high
      if (c.cost or 0) > 99999 then
        print(string.format(
          "[HIGH-COST gc populate] kind=%s @(%d,%d) cost=%.1f base=%.1f penalty=%.1f hyst=%s switch_flat=%.1f commit_val=%.1f hist_t=%s hist_k=%s wsim_add=%.1f wsim_ran=%s wsim_killed=%s phase_weight=%s desc=%s",
          c.goal and c.goal.kind or "?",
          c.goal and c.goal.mx or -1, c.goal and c.goal.my or -1,
          c.cost or 0, c._base_cost or -1, penalty,
          tostring(c.hysteresis), c.switch_flat or 0, c.commit_val or 0,
          tostring(c.hist_target), tostring(c.hist_kind),
          c.wsim_add or 0, tostring(c.wsim_ran), tostring(c.wsim_killed),
          tostring(c.phase_weight), tostring(c.desc)))
      end
      state.goal_competition[#state.goal_competition + 1] = {
        kind    = c.goal and c.goal.kind,
        mx      = c.goal and c.goal.mx,
        my      = c.goal and c.goal.my,
        base    = c._base_cost or c.cost,
        penalty = penalty,
        -- APPLIED multipliers, for WINNERS-row reconciliation: the
        -- distance-attenuated phase weight actually used (NOT the raw
        -- table value) and the territory-influence scale.
        phase_weight = c.phase_weight or 1.0,
        inf_mult     = c._inf_mult or 1.0,
        suicider_mult = c._suicider_mult or 1.0,
        total   = c.cost,
        hyst       = c.hysteresis,
        hist_t     = c.hist_target,
        hist_k     = c.hist_kind,
        switch_flat = c.switch_flat or 0,
        commit_val  = c.commit_val or 0,
        ticks_on    = ticks_on_goal,
        density_mult = c.density_mult or 1.0,
        density_n    = c.density_n or 0,
        pickup_value = c.pickup_value or 0,
        wsim_add     = 0,
        ally_claimed_pen = c.ally_claimed_pen or 0,
        ally_claimed_by  = c.ally_claimed_by,
      }
    end
    end -- BRAIN_DEBUG_MODE
    if not quiet and BRAIN_DEBUG_MODE and #pool > 0 then
      for i = 1, math.min(5, #pool) do
        local c = pool[i]
        local hist_str = ""
        if c.hist_target or c.hist_kind then
          hist_str = string.format(" hist(t=%d k=%d)",
                                   c.hist_target or 0, c.hist_kind or 0)
        end
        print2("  pool[", i, "] ", c.goal.kind, "@", c.goal.mx or 0, ",", c.goal.my or 0,
               " cost=", string.format("%.0f", c.cost), " hyst=", c.hysteresis or "-",
               hist_str)
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
    if #pool >= 1 and state.goal and state.goal.kind ~= "none" then
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
        -- Walkover guard: incremental pools (repair/capture/attack_*)
        -- re-queue their candidates over many ticks, so the CURRENT
        -- goal's pool may simply not have bid this cycle. With no
        -- cur_entry the stickiness bar had nothing to hold onto and ANY
        -- challenger took the bot mid-goal by walkover (20260825_202948
        -- bot15 t=1511: dropped a close repair_pill for a cross-map
        -- defend purely because repair was mid-requeue). Fall back to
        -- the pool_cache's last finalized cost for the current goal and
        -- fabricate its entry so the ratio test still applies.
        --
        -- ...but a goal that FINISHED is not mid-requeue. Its pool ran and
        -- deliberately declined to bid (see the "COMPLETION decline" flag
        -- set in the pool-1 at-target skip above). Carrying it forward
        -- reinstalls a completed goal as the incumbent, and since the
        -- incumbent pays no switch/commitment penalty it then wins every
        -- tick until something external breaks the tie — the parked-at-
        -- full-armour livelock. The flag distinguishes the two cases
        -- without touching the mid-requeue protection: it is only set on
        -- ticks where the pool produced an entry and threw it away for a
        -- completion reason, which is exactly the case the carry-forward
        -- must NOT cover.
        local _dc = state._pool_decline_complete
        local _declined_complete = _dc and _dc.tick == now
                                   and _dc.kind == state.goal.kind
        if _declined_complete and not cur_entry and BRAIN_DEBUG_MODE then
          print2(string.format(
            "  hysteresis(carry): SKIPPED for %s@%d,%d — its pool declined for COMPLETION, not requeue",
            state.goal.kind, state.goal.mx or -1, state.goal.my or -1))
        end
        if not cur_entry and not _declined_complete and state.pool_cache then
          for pi = 0, 12 do
            local pce = state.pool_cache[pi]
            if pce and pce.goal and pce.cost
               and pce.goal.kind == state.goal.kind
               and pce.goal.mx == state.goal.mx
               and pce.goal.my == state.goal.my then
              -- Carried-forward costs come straight from pool_cache, which is
              -- PRE-selection — so the suicider multiplier (applied in the pool
              -- pass above, which this entry missed) has to be re-applied here
              -- or a suicider's stickiness bar would be 3-6x too low and every
              -- challenger would win by walkover.
              local _cf_sui = suicider_cost_mult(state, state.goal.kind)
              cur_entry = { cost = pce.cost * _cf_sui, goal = state.goal,
                            desc = "(carried forward: pool mid-requeue)",
                            _suicider_mult = (_cf_sui ~= 1.0) and _cf_sui or nil,
                            _carried_forward = true }
              pool[#pool + 1] = cur_entry
              if BRAIN_DEBUG_MODE then
                print2(string.format(
                  "  hysteresis(carry): current %s@%d,%d had no pool entry this cycle — carried at last cost %.0f",
                  state.goal.kind, state.goal.mx or -1, state.goal.my or -1, pce.cost))
              end
              break
            end
          end
        end
        if cur_entry and winner.cost > cur_entry.cost * C.GOAL_SWITCH_RATIO then
          if BRAIN_DEBUG_MODE then
            print2("  hysteresis(mult): winner ", winner.goal.kind,
                   " cost=", string.format("%.0f", winner.cost),
                   " > current ", cur_entry.goal.kind,
                   " cost=", string.format("%.0f", cur_entry.cost),
                   " * ", C.GOAL_SWITCH_RATIO,
                   " (", string.format("%.0f", cur_entry.cost * C.GOAL_SWITCH_RATIO),
                   ") — sticking with current")
          end
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
    -- Persisted wsim-KILL: re-apply a recent "you'd die" verdict (set within
    -- the last WSIM_KILL_PERSIST_TICKS) WITHOUT re-simming, so a lethal goal
    -- stays rejected on ticks where wsim is capped/disabled instead of flapping
    -- back to it (it's the cheapest base-cost candidate, so a one-pass +99999
    -- only sinks it for that pass). Gives refuel + hysteresis ~1s to take over.
    local _now = state.tick or 0
    local _wk = state.wsim_kill_until
    local _persist_applied = false
    if _wk then
      for k, until_t in pairs(_wk) do if _now >= until_t then _wk[k] = nil end end  -- prune expired
      for _, c in ipairs(pool) do
        local k = c.goal.kind .. ":" .. (c.goal.mx or 0) .. "," .. (c.goal.my or 0)
        if _wk[k] and not c.wsim_killed then
          c.cost = c.cost + 99999
          c.wsim_killed = true
          _persist_applied = true
        end
      end
    end
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
          -- Fresh-kill sweep: damp the wsim DANGER penalty (not the KILL reject)
          -- for capturing a pill WE just killed, within SWEEP_KILL_WINDOW, so a
          -- neighbouring pill's predicted fire doesn't scare us off the sweep.
          if extra > 0 and c.goal.kind == "capture_pill" and state._swept_kill
             and c.goal.target_id == state._swept_kill.id
             and (state.tick - (state._swept_kill.tick or 0)) <= (C.SWEEP_KILL_WINDOW or 300) then
            local _raw = extra
            extra = extra * (C.SWEEP_KILL_WSIM_MULT or 0.3)
            print2(string.format("SWEEP_KILL_DAMP t=%d pill#%d wsim %.0f->%.0f age=%d/%d",
              state.tick, c.goal.target_id or -1, _raw, extra,
              state.tick - (state._swept_kill.tick or 0), C.SWEEP_KILL_WINDOW or 300))
          end
          -- Debug print: every wsim run, even 0-damage survivors.
          if BRAIN_DEBUG_MODE then
            local shot_parts = {}
            if wsim_result and wsim_result.pills then
              for _, ps in ipairs(wsim_result.pills) do
                if ps.shots and ps.shots > 0 then
                  shot_parts[#shot_parts + 1] = string.format("p#%d:%d", ps.id, ps.shots)
                end
              end
            end
            local shot_str = #shot_parts > 0 and (" [" .. table.concat(shot_parts, ",") .. "]") or ""
            local dest_str = ""
            if c.goal.kind == "attack_pill" and spot_x then
              dest_str = string.format(" dest=spot(%d,%d)", spot_x, spot_y)
            else
              dest_str = string.format(" dest=goal(%d,%d)", c.goal.mx or 0, c.goal.my or 0)
            end
            print(string.format("[wsim] %s(%d,%d)%s +cost=%.0f %ddmg arm=%d->%d (%.1fs)%s%s",
              c.goal.kind, c.goal.mx or 0, c.goal.my or 0, dest_str,
              extra,
              wsim_result and wsim_result.damage or 0,
              info.armour,
              wsim_result and wsim_result.armour or info.armour,
              (wsim_result and wsim_result.ticks or 0) / 50.0,
              killed and " KILL+99999" or "",
              shot_str))
          end
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
            if not c.wsim_killed then c.cost = c.cost + 99999 end  -- effectively reject (skip if persist already added it)
            c.wsim_killed = true
            -- Persist the verdict so it survives capped/disabled-wsim ticks.
            state.wsim_kill_until = state.wsim_kill_until or {}
            state.wsim_kill_until[c.goal.kind .. ":" .. (c.goal.mx or 0) .. "," .. (c.goal.my or 0)] =
              _now + (C.WSIM_KILL_PERSIST_TICKS or 50)
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
    elseif _persist_applied then
      -- wsim didn't run this tick, but a persisted KILL changed costs — re-sort
      -- so the lethal goal can't win just because the live pass was skipped.
      table.sort(pool, function(a, b) return a.cost < b.cost end)
    end

    -- ── Ammoless: reject attack_pill #N unless #N is an ongoing blitz ──
    -- A tank with ZERO shells can't fire a single shot, so it must not pursue
    -- ANY attack_pill take #N — force it to COST_INF so the reject / explore
    -- fallback below drops it. The ONLY exception is a pill that already has an
    -- ONGOING blitz (our own committed target, or a live ally call on it): a
    -- real squad take it can HELP as a decoy body is kept. This stops a dry bot
    -- re-picking impossible SOLO takes every tick without breaking blitzes we
    -- want to keep (the pill's blitz pricing keeps its cost cheap either way).
    if (info.shells or 0) == 0 then
      for _, c in ipairs(pool) do
        if c.goal and c.goal.kind == "attack_pill" and (c.cost or 0) < 1e29 then
          local pid = c._pill_id or (c.goal and c.goal.target_id)
          local ongoing_blitz = false
          if pid then
            if state.squad_blitz_target == pid then
              ongoing_blitz = true
            elseif state.blitz_calls then
              for _, bc in pairs(state.blitz_calls) do
                if bc.pill == pid then ongoing_blitz = true; break end
              end
            end
          end
          if not ongoing_blitz then
            c.cost = 1e30
          end
        end
      end
    end

    -- ── Reject COST_INF winners (>= 1e29) ──
    -- A goal at/above the COST_INF sentinel is unreachable/unaffordable
    -- (attack_pill when shells < pill HP → 1e30, no standoff, etc.). It must
    -- never be SELECTED: goal-type hysteresis can otherwise glue the bot to it
    -- (it was promoted to pool[1] just above), and it then burns full planning
    -- — e.g. a ~50ms PPT shield scan — on a take it can't execute. Blitz
    -- commit/join already price their pill finite (apply_blitz_target ~30 /
    -- apply_blitz_join_discount's finite ref base), so a real helper's blitz
    -- survives here; only the futile solo take is dropped. Drop the leading
    -- sentinel entries so pool[1] becomes the cheapest AFFORDABLE goal
    -- (reposition/capture/explore). If NOTHING is affordable, leave the pool
    -- untouched so there's still a least-bad, non-nil winner.
    do
      local first_ok
      for i = 1, #pool do
        if (pool[i].cost or math.huge) < 1e29 then first_ok = i; break end
      end
      if first_ok and first_ok > 1 then
        if BRAIN_DEBUG_MODE then print2(string.format("  COST_INF reject: dropped %d leading sentinel goal(s); winner now %s cost=%.0f",
          first_ok - 1, pool[first_ok].goal.kind, pool[first_ok].cost)) end
        for _ = 1, first_ok - 1 do table.remove(pool, 1) end
      end
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
    if BRAIN_DEBUG_MODE then
    if state.goal_competition then
      for _, gc in ipairs(state.goal_competition) do
        for _, c in ipairs(pool) do
          if c.goal and c.goal.kind == gc.kind
             and c.goal.mx == gc.mx and c.goal.my == gc.my then
            local prev_total = gc.total
            gc.wsim_add = c.wsim_add or 0
            gc.wsim_killed = c.wsim_killed or false
            gc.wsim_desc = c.desc and c.desc:match("wsim:(.+)$") or nil
            gc.wsim_path = c.wsim_path  -- for visualization
            gc.wsim_ran = c.wsim_ran or false
            gc.wsim_damage = c.wsim_damage or 0
            gc.wsim_arm_before = c.wsim_arm_before
            gc.wsim_arm_after  = c.wsim_arm_after
            gc.wsim_ticks = c.wsim_ticks or 0
            gc.total    = c.cost
            -- DIAGNOSTIC: high-cost watch at post-wsim patch
            if (c.cost or 0) > 99999 then
              print(string.format(
                "[HIGH-COST post-wsim patch] kind=%s @(%d,%d) prev_gc_total=%.1f new_total=%.1f wsim_add=%.1f wsim_damage=%d wsim_arm=%s->%s wsim_killed=%s wsim_desc=%s",
                gc.kind, gc.mx, gc.my,
                prev_total or -1, c.cost, gc.wsim_add,
                gc.wsim_damage, tostring(gc.wsim_arm_before), tostring(gc.wsim_arm_after),
                tostring(gc.wsim_killed), tostring(gc.wsim_desc)))
            end
            break
          end
        end
      end
    end

    -- DIAGNOSTIC: also catch any goal_competition entries that end up with
    -- total > 99999 regardless of which pool they come from, so we see the
    -- mismatch if one exists across kinds at the same tile.
    if state.goal_competition then
      local high = {}
      for _, gc in ipairs(state.goal_competition) do
        if (gc.total or 0) > 99999 then
          high[#high + 1] = gc
        end
      end
      if #high > 0 then
        print(string.format("[HIGH-COST summary] %d entries with total>99999:", #high))
        for _, gc in ipairs(high) do
          print(string.format(
            "  kind=%s @(%d,%d) base=%.1f penalty=%.1f total=%.1f wsim_add=%.1f wsim_killed=%s hist_t=%s hist_k=%s",
            gc.kind, gc.mx, gc.my,
            gc.base or -1, gc.penalty or 0, gc.total or 0,
            gc.wsim_add or 0, tostring(gc.wsim_killed),
            tostring(gc.hist_t), tostring(gc.hist_k)))
        end
      end
    end

    -- Store wsim paths on state for persistent drawing every tick.
    -- Replaced each replan so stale paths don't linger.
    state._wsim_viz_paths = {}
    for _, c in ipairs(pool) do
      if c.wsim_path and #c.wsim_path > 2 then
        state._wsim_viz_paths[#state._wsim_viz_paths + 1] = {
          path = c.wsim_path,
          killed = c.wsim_killed or false,
          kind = c.goal and c.goal.kind or "?",
          mx = c.goal and c.goal.mx or 0,
          my = c.goal and c.goal.my or 0,
          detail = c.wsim_detail or "",
          hits = c.wsim_hits,
        }
      end
    end
    end -- BRAIN_DEBUG_MODE

    if #pool > 0 then
      -- Dump the FINAL post-everything scores for every candidate so a
      -- replan tick can be reconstructed after the fact. Stripped from
      -- opt/ via print2.
      --
      -- Tier-gated: one line per candidate is ~1 ms of string.format on the
      -- most expensive tick shape the brain has. The capacity controller drops
      -- tiers to survive the budget, but no tier lever reached the replan path
      -- — so below REPLAN_LOG_MIN_TIER the reconstruction detail is dropped and
      -- the controller's low tiers become measurably cheaper on replan ticks.
      if (state._capacity_tier or 10) >= (C.REPLAN_LOG_MIN_TIER or 3) then
      print2(string.format("FINAL_SCORES t=%d pool_size=%d cur=%s@%d,%d",
        state.tick or 0, #pool,
        state.goal and state.goal.kind or "none",
        state.goal and state.goal.mx or 0,
        state.goal and state.goal.my or 0))
      for i, c in ipairs(pool) do
        local base = c._base_cost or c.cost
        local penalty = (c.cost or 0) - base
        print2(string.format(
          "  [%d] %s@%d,%d total=%.1f base=%.1f pen=%.1f hyst=%s sw=%.1f cmt=%.1f histT=%s histK=%s wsim=%.1f dens=%.2f pickup=%.1f desc=%s",
          i,
          c.goal and c.goal.kind or "?",
          c.goal and c.goal.mx or 0, c.goal and c.goal.my or 0,
          c.cost or 0, base, penalty,
          tostring(c.hysteresis or "-"),
          c.switch_flat or 0, c.commit_val or 0,
          tostring(c.hist_target or 0), tostring(c.hist_kind or 0),
          c.wsim_add or 0,
          c.density_mult or 1.0,
          c.pickup_value or 0,
          tostring(c.desc or "")))
      end
      end -- REPLAN_LOG_MIN_TIER

      local winner = pool[1]

      -- Pool log + winner_cands + log.reason are debug/log-only — wrapped
      -- so lua_strip removes them from opt/.
      if BRAIN_DEBUG_MODE then
      -- Log all competing candidates for debugging
      local pool_log = nil
      if BRAIN_POOL_VIZ then
        pool_log = {}
        for i, c in ipairs(pool) do
          local d = c.desc
          if c.hysteresis then d = d .. " [" .. c.hysteresis .. "]" end
          if c.wsim_killed then d = d .. " [KILL]" end
          pool_log[i] = { desc = d, cost = c.cost, hysteresis = c.hysteresis,
                          winner = (i == 1), wsim_killed = c.wsim_killed,
                          phase_weight = c.phase_weight,
                          density_mult = c.density_mult or 1.0,
                          density_n    = c.density_n or 0,
                          pickup_value = c.pickup_value or 0,
                          wsim_add     = c.wsim_add or 0 }
        end
      else
        pool_log = { { desc = "[BRAIN_POOL_VIZ is off — set global to enable]" } }
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
      end -- BRAIN_DEBUG_MODE

      -- No AFFORDABLE goal: the winner is at/above the COST_INF sentinel
      -- (>= 1e29) — e.g. a dry bot whose only surviving pool entry is an
      -- unaffordable attack_pill (reposition/capture got filtered out by
      -- cooldown/blocked, so the reject had nothing cheaper to fall to). Don't
      -- commit to the sentinel — leave result nil so M.pick_goal drops through
      -- to the explore fallback, which picks a real frontier target. This is the
      -- guaranteed sub-1e29 escape hatch: without it the bot is glued to a take
      -- it can never complete (it just parks in plan_position re-planning).
      if (winner.cost or 0) >= 1e29 then
        result = nil
        desc = "no affordable goal (winner >=1e29) -> explore fallback"
        if BRAIN_DEBUG_MODE then print2(string.format("  COST_INF: winner %s cost=%.0f is unaffordable, no cheaper alt -> explore", winner.goal and winner.goal.kind or "?", winner.cost or 0)) end
      -- If attack pill won, resolve technique (standoff, wall-shield, etc.)
      elseif winner._pill then
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
-- Flag-gated mission override (R3-exec reinforcement + R4 harasser). Returns a
-- goal to preempt routine selection, or nil to fall through. By yielding (nil)
-- whenever refuel / attack_tank / kill_lgm is warranted, it implements the
-- "restricted goal set" — those three are the only things allowed to interrupt
-- the mission. All paths are off unless their flag is set.
function M.special_mode_goal(state, world, info)
  local role = state.squad_role   -- "c" / "s" / "h" (set by squad.update)
  -- Harassers are now plain GoalHunter bots, just biased OUT of the pill economy
  -- by a doubled attack_pill cost (HARASSER_PILL_COST_MULT, applied at the pool-6
  -- cost in update_pool_cache + attack_pill_adjustments). No special mission —
  -- they fall through to normal goal selection, so the doubled pill cost naturally
  -- pushes them toward bases / tanks / defense. Only R3 circle reinforcement
  -- remains as a flag-gated override here.
  local want_reinforce = C.CIRCLE_REINFORCE_ENABLED
  if not want_reinforce then return nil end
  if not world or not info then return nil end

  -- Restricted set: yield to refuel (charge up first), attack_tank, kill_lgm.
  if (info.armour or 0) <= (C.ARMOUR_LOW or 15)
     or (info.shells or 0) <= (C.SHELLS_LOW or 20) then return nil end
  local perc = state.perc
  if perc and perc.enemy_tanks and #perc.enemy_tanks > 0 then return nil end
  if perc and perc.enemy_lgms  and #perc.enemy_lgms  > 0 then return nil end
  if info.carried_pills and info.carried_pills > 0 then return nil end

  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)

  -- Reinforce: skip anyone mid hard-take (a commander) or already attacking a
  -- pill — only genuinely uncommitted bots get pulled.
  if role == "c" then return nil end
  if state.goal and state.goal.kind == "attack_pill" then return nil end
  local c = circles.nearest_losing(tmx, tmy)
  if c and c.safe_mx then
    state._reinforce_viz = { mx = c.safe_mx, my = c.safe_my, id = c.id }  -- reinforce_link overlay
    -- Drive to the safest (highest-influence) tile of the losing circle.
    return { kind = "explore", mx = c.safe_mx, my = c.safe_my,
             wx = U.m2w(c.safe_mx), wy = U.m2w(c.safe_my) }
  end
  state._reinforce_viz = nil
  return nil
end

function M.pick_goal(state, world, info, quiet)
  -- Cache the few info fields the breakdown viz needs (pool_breakdown has
  -- only `state` in scope).
  state._last_info = {
    tmx = bit.rshift(info.tankx, 8), tmy = bit.rshift(info.tanky, 8),
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
    local tmx = bit.rshift(info.tankx, 8)
    local tmy = bit.rshift(info.tanky, 8)

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
      if BRAIN_DEBUG_MODE then
        print(string.format(TAG .. " CMD: ARRIVED at %s #%d (%d,%d)",
              cg.kind, cg.id, cg.mx, cg.my))
      end
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

  -- Warmup gate: until the pools warm up (>= WARMUP_MIN_REAL_GOALS candidates
  -- costed below the wall cost) DON'T commit to a real goal — with only a
  -- handful evaluated we'd pick a poor one and then hysteresis/commitment would
  -- lock us onto it. Fall through to the explore path below; the rolling eval
  -- keeps warming regardless of the current goal, and warm_ready latches (incl.
  -- a sparse-map "queue fully swept" escape) so this can never stall. The
  -- command_goal path above is exempt (explicit orders always run).
  if M.warm_ready(state) then
    -- R4 harasser mission + R3 circle reinforcement (flag-gated). Preempts
    -- routine goals but yields to refuel/attack_tank/kill_lgm (restricted set)
    -- and to command_goal above. No-op unless a flag is set.
    do
      local ov = M.special_mode_goal(state, world, info)
      if ov then return ov end
    end

    -- Strategic goal selection (timing buf populated only when perf-log is active)
    state._pick_goal_timing = _G.BRAIN_PROFILE and {} or nil
    local strategic = goal_selection(state, world, info, quiet)
    if strategic then
      -- Blitz negotiation pause: while we're offering a standoff to a commander
      -- and not yet accepted, don't START a non-interruptible take — hold (idle)
      -- until the blitz question resolves (accept → commit to the blitz; decline /
      -- timeout → the negotiation clears and this gate lifts).
      if state.squad_negotiate_cmdr and not state.squad_blitz_accepted
         and state.squad_blitz_engage_mx   -- only pause while we have a live offer
         and (strategic.kind == "attack_pill" or strategic.kind == "capture_pill") then
        return { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
      end
      return strategic
    end
  end

  -- If exploration is disabled, just idle
  if not state.auto_explore then
    return { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
  end

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)

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

    -- Look up phase weight for this pool. `weighted` must match the number
    -- goal_selection actually competes, so a suicider's surcharge rides here
    -- alongside the phase weight (1.0 for everyone else).
    local pw = phase_weights and pname and phase_weights[pname] or 1.0
    local sui = suicider_mult_for_pool(state, pname)
    local weighted = cost_val >= 0 and (cost_val * pw * sui) or -1
    if sui ~= 1.0 and formula ~= "" then
      formula = formula .. string.format(" * suicider{%.1f}", sui)
    end

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
    local sui = suicider_mult_for_pool(state, pname)
    local weighted = cost >= 0 and (cost * pw * sui) or -1
    local fdesc = entry.desc or ""
    if sui ~= 1.0 then fdesc = fdesc .. string.format(" * suicider{%.1f}", sui) end
    entries[#entries+1] = {
      pool = idx, pname = pname, id = 0,
      mx = goal.mx or 0, my = goal.my or 0,
      cost = cost, weighted = weighted, raw = cost,
      pw = pw, age = 0, status = "done",
      formula = fdesc, flags = "",
    }
  end
  append_finalize(2)  -- defend_pill
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

-- Warmup gate: the per-tick rolling eval (update_pool_cache) costs candidates
-- a couple at a time, so for the first ~14-49 ticks most pools are still INF
-- and the bot rides the explore fallback. "Warm" = at least
-- WARMUP_MIN_REAL_GOALS finite-cost (pickable, non-rejected) candidates exist
-- in cost_cache. Latches once true (warmup only happens at the start of a life,
-- or briefly after a death-wipe). Stores state._warm_count for the panel row.
function M.warm_ready(state)
  if state._warm_ready then return true end
  -- Sparse-map escape: if the whole eval queue has been processed at least once
  -- and we STILL have fewer than the target, there simply aren't that many
  -- reachable goals — count what we have as warm rather than exploring forever.
  if state._eval_swept then state._warm_ready = true; return true end
  local need = C.WARMUP_MIN_REAL_GOALS or 10
  local n = 0
  local cc = state.cost_cache
  if cc then
    for _, e in pairs(cc) do
      -- A "real, reachable" candidate must cost LESS than the wall/impassable
      -- terrain cost (9999 per tile, brain_pathfinder.c). Real goal costs run
      -- well under ~2000; a cost >= 9999 means the only path crosses a wall /
      -- deepsea / unexpanded slate — i.e. not actually reachable yet. This is
      -- exactly the post-respawn state (new position, slate not grown), so
      -- counting those would falsely report "warm" and let the bot grab a
      -- ~9999-costed goal instead of exploring. (rejects carry _reject.)
      if e and not e._reject and e.cost and e.cost >= 0 and e.cost < 9999 then
        n = n + 1
        if n >= need then break end
      end
    end
  end
  state._warm_count = n
  if n >= need then state._warm_ready = true end
  return state._warm_ready == true
end

function M.get_pool_breakdown_json(state)
  -- Run the ally-claimed REJECT sync every tick the grid is read so
  -- the displayed _reject flags track live ally_state without waiting
  -- for the next replan cycle.
  if state.player_number then _SELF_PN = state.player_number end
  sync_ally_claimed_rejects(state)
  apply_blitz_target(state, state._last_info)
  apply_blitz_join_discount(state, state._last_info, state.world)
  apply_blitz_capture_defer(state, state._last_info)
  if not BRAIN_POOL_VIZ then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      state.phase or "?", state.tick or 0, state.player_number or 0)
  end
  local now = state.tick or 0
  local cache = state.cost_cache or {}
  local pc = state.pool_cache or {}
  if BRAIN_DEBUG_MODE then local _nc=0 for _ in pairs(cache) do _nc=_nc+1 end local _np=0 for _ in pairs(pc) do _np=_np+1 end print2(string.format("POOL_BREAKDOWN_ENTRY t=%d bot=%s cost_cache=%d pool_cache=%d pool_viz=%s", now, tostring(state.player_number), _nc, _np, tostring(BRAIN_POOL_VIZ))) end
  local phase_weights = C.PHASE_WEIGHTS[state.phase]

  -- Refuel live cost-shape (pool 1) for the term breakdown. goal_selection
  -- only stashes ur{}/def{}/fill{} inside its pool-competition block, which is
  -- skipped whenever an earlier override (command / critical flee / capture
  -- objective) already set the goal — leaving pool-1 rows with a stale,
  -- shapeless cached formula. Recompute it here (display-only; does NOT touch
  -- the real cost path) every time the grid is read so the shape always
  -- reflects current armour/shells regardless of how the goal was chosen.
  do
    local li = state._last_info
    if li and li.armour then
      -- Same shared shape the real cost path uses (scarcity + mine surcharge).
      local bonus, mult, fill, scarcity, mine_cost, urgency, arm_def, sh_def =
        refuel_shape(li, state, state.tick or 0)
      for _, e in pairs(cache) do
        if e._p == 1 then
          e._urgency     = urgency
          e._base_floor  = C.REFUEL_BASE_COST
          e._defic_bonus = bonus
          e._fill_mult   = mult
          e._fill        = fill
          e._scarcity    = scarcity
          e._mine_cost   = mine_cost
          e._arm         = li.armour
          e._sh          = li.shells or 0
          e._arm_def     = arm_def
          e._sh_def      = sh_def
          e.formula      = nil  -- force live re-render with shape tokens
        end
      end
    end
  end

  -- Is this pill a blitz (for the pool-viz BLITZ row style)? True when it's our
  -- own blitz lead (commander), the blitz we've committed to, the one we're
  -- negotiating to join, or any teammate's currently-open blitz call.
  local function is_blitz_pill(pid)
    if not pid or pid < 0 then return false end
    local g = state.goal
    if g and g.kind == "attack_pill" and g.target_id == pid and g._blitz then return true end
    if state.squad_blitz_target == pid then return true end
    if state.squad_negotiate_pill == pid then return true end
    if state.blitz_calls then
      for _, c in pairs(state.blitz_calls) do
        if c.pill == pid then return true end
      end
    end
    return false
  end

  local replan_left = 0
  if state.replan_offset then
    replan_left = C.GOAL_REPLAN_INTERVAL
        - ((now + state.replan_offset) % C.GOAL_REPLAN_INTERVAL)
    if replan_left == C.GOAL_REPLAN_INTERVAL then replan_left = 0 end
  end

  -- Fixed 2x5 layout matching the optimize-branch poolwindow.
  -- (Indexes 11/12 used by offensive_build/wait_for_lgm strips below the grid.)
  local LAYOUT_CELL = {
    [1] = {1,1}, [2] = {1,2}, [3] = {1,3}, [4] = {1,4}, [5] = {1,5},
    [6] = {2,1}, [7] = {2,2}, [8] = {2,3}, [9] = {2,4}, [10]= {2,5},
  }

  -- Active-goal lookup. The renderer marks the row whose (pool, id)
  -- matches the bot's currently-committed goal so the user can see at
  -- a glance which candidate is actually being acted on (vs. just the
  -- sort-winner). Map the ACTIVE goal to its pool via the module-level
  -- KIND_TO_POOL (goal-kind keyed). NOT a pool-NAME map: some goal kinds differ
  -- from their pool name (place_pill_strategic→8, refuel_at_base→1), and the old
  -- name map left active_pool nil for those, so the active goal vanished from
  -- WINNERS. Also capture the goal tile so we can match place_strategic (which
  -- has no target_id) by position.
  local active_pool, active_id, active_mx, active_my = nil, nil, nil, nil
  if state.goal and state.goal.kind then
    active_pool = KIND_TO_POOL[state.goal.kind]
    active_id   = state.goal.target_id
    active_mx, active_my = state.goal.mx, state.goal.my
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
        -- Show the reject chip honestly: an open/joinable blitz reads as "blitz",
        -- not "ally_claimed" (which is reserved for a truly-closed solo take).
        reject = cached and ((cached._reject == "ally_claimed" and cached._reject_joinable_blitz)
                             and "blitz" or cached._reject) or nil,
        reject_remaining = cached and cached._reject_remaining or 0,
        ally_score = cached and cached._ally_score or nil,
        ally_by    = cached and cached._ally_by    or nil,
        stealing   = (cached and cached._stealing) or false,
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
        "LOS (los_base{%.0f} + dist{%.1f}*per_tile{%.0f} + low_sh{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%.0f} = %.0f" ..
        "||dist=%.1f; shells_now=%d; aim_diff=%.1f; far_preempt=exp penalty when on an attack_pill + tank past shoot range",
        C.TANK_COMBAT_LOS_BASE_COST, b.dist or 0, C.TANK_COMBAT_LOS_COST_PER_TILE,
        b.low_shells_penalty or 0, b.boat_mult or 1.0,
        b.tank_tile_threat or 0, b.far_preempt_pen or 0, b.cost or 0,
        b.dist or 0, b.tank_shells or 0, b.aim_diff or 0)
    end
    return string.format(
      "standoff->(%s,%s) (A* to standoff{%.0f} + base{%.0f} + wall{%.0f} + low_sh{%.0f} - aim{%.0f} + xfire{%.0f}) * boat{%.2f} + threat{%.0f} + far_preempt{%.0f} = %.0f" ..
      "||A* is to the FIRING STANDOFF (%s,%s), NOT the enemy tile (%d,%d) — that's why it's cheaper than a test-click on the enemy. far_preempt=exp penalty when on an attack_pill + tank past shoot range. dist=%d; shells_now=%d; shells_arrival=%s; aim_diff=%.1f; wall_hp=%s; deg=%s",
      tostring(b.standoff_mx), tostring(b.standoff_my),
      b.path_cost or 0, b.base or 0, b.wall_penalty or 0,
      b.low_shells_penalty or 0, b.aim_bonus or 0, b.crossfire or 0,
      b.boat_mult or 1.0, b.tank_tile_threat or 0, b.far_preempt_pen or 0, b.cost or 0,
      tostring(b.standoff_mx), tostring(b.standoff_my), b.mx or 0, b.my or 0,
      b.dist or 0, b.tank_shells or 0, tostring(b.shells_on_arrival),
      b.aim_diff or 0, tostring(b.wall_hp), tostring(b.standoff_deg))
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
          local allied = (bit.band(allies, (bit.lshift(1, pn)))) ~= 0
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
      id = -1, mx = 0, my = 0,
      cost = -1,
      formula = "SKIP " .. reason .. "||" .. reason,
      stale = 0,
      reject = reason,
      reject_remaining = 0,
    }}
  end

  -- Inject defend_pill (pool 2) rows. eval_defend_pill is a finalize pool —
  -- it never enqueues eval_queue items — and scores ALL owned pills itself,
  -- leaving the full candidate list in state.defend_breakdown every replan
  -- (including replans where nothing is defendable), so the cell always
  -- lists every owned pill with its score or reject reason.
  do
    local db = state.defend_breakdown
    if db and db.rows and #db.rows > 0 then
      local age = now - (db.tick or now)
      for _, r in ipairs(db.rows) do r.stale = age end
      by_pool[2] = db.rows
    end
  end

  -- Build a normal pool section. Used for indexes 1..9 and the
  -- offensive_build (11) / wait_for_lgm (12) strips below the main grid.
  local function build_section(idx)
    local pname = POOL_NAMES[idx] or ("p"..idx)
    local pw = (phase_weights and phase_weights[pname]) or 1.0  -- PHASE_WEIGHTS is name-keyed, not idx-keyed
    -- Suicider surcharge shares the phase weight's role here: it is a
    -- selection-layer multiplier on this whole pool, so it belongs in
    -- `weighted` (and therefore in the row ordering). 1.0 for non-suiciders.
    local sui = suicider_mult_for_pool(state, pname)
    local pwx = pw * sui
    local rows_raw = by_pool[idx] or {}
    table.sort(rows_raw, function(a, b)
      local ac = (a.cost >= 0) and a.cost * pwx or math.huge
      local bc = (b.cost >= 0) and b.cost * pwx or math.huge
      return ac < bc
    end)
    local rows = {}
    for i, r in ipairs(rows_raw) do
      rows[i] = {
        id = r.id, mx = r.mx, my = r.my,
        cost = r.cost,
        weighted = (r.cost >= 0) and (r.cost * pwx) or -1,
        is_winner = false,   -- assigned below from pool_cache (the real winner)
        active_goal = (active_pool == idx
                       and ((active_id and active_id >= 0 and active_id == r.id)
                            or (active_mx ~= nil and r.mx == active_mx and r.my == active_my))),
        stale = r.stale,
        formula = (sui ~= 1.0 and r.formula)
                  and (r.formula .. string.format(" * suicider{%.1f}", sui))
                  or r.formula,
        reject = r.reject,
        reject_remaining = r.reject_remaining,
        stealing = r.stealing or false,
        ally_by = r.ally_by,
        imminent = r.imminent or false,
        blitz = (pname == "attack_pill") and is_blitz_pill(r.id) or false,
      }
    end
    -- Winner = the pool's ACTUAL selected candidate, not merely the raw-cheapest
    -- rolling row — otherwise a sibling that's raw-cheaper but loses on
    -- penalties / current-goal commitment takes the WINNERS slot and the real
    -- (active) goal vanishes from the strip. Priority:
    --   1. If this is the ACTIVE goal's pool, use the active-goal row. The
    --      committed goal is sticky (commitment discount) and may not be the
    --      pool's raw-cheapest or even the current pool_cache winner, so this
    --      guarantees it is always present in WINNERS.
    --   2. Else the pool_cache winner (post-penalty selection) matched to a row.
    --   3. Else the raw-cheapest (rows[1]).
    local win = nil
    if active_pool == idx then
      for _, rr in ipairs(rows) do
        if rr.active_goal then win = rr; break end
      end
    end
    if not win then
      local pcw = pc[idx]
      if pcw and pcw.goal then
        local gid = pcw.goal.target_id
        local gmx, gmy = pcw.goal.mx, pcw.goal.my
        for _, rr in ipairs(rows) do
          if (gid and gid >= 0 and rr.id == gid)
             or (gmx and rr.mx == gmx and rr.my == gmy) then
            win = rr; break
          end
        end
      end
    end
    if not win then win = rows[1] end
    if win and win.cost >= 0 and win.cost < 1e29 and not win.reject then
      win.is_winner = true
    end
    local winner_id = (win and win.is_winner) and win.id or -1
    return {
      idx = idx, name = pname, weight = pw, winner_id = winner_id,
      layout_cell = LAYOUT_CELL[idx], rows = rows,
    }, win
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
      -- PHASE_WEIGHTS is keyed by pool NAME (matching goal_selection at the
      -- base computation), NOT by numeric pool index — looking it up by idx
      -- always missed and fell back to 1.0, so the row showed "@1.00" while
      -- the base already had the real weight (e.g. opening attack_pill 3.0)
      -- baked in. Use the name so the displayed multiplier matches reality.
      local pw = (phase_weights and phase_weights[pname]) or 1.0

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
      -- gc.base is raw_pool_cost * phase_weight. w.cost is the raw pre-phase
      -- cost shown in the per-pool section / detail popup.

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
      local detail_formula = string.format("%s(x%.2f): %s", pname,
        (gc and gc.phase_weight) or pw, w.formula)
      local detail_map = {}

      detail_map[#detail_map + 1] = string.format("base:%.0f (raw=%.0f x pw=%.1f, %s phase)",
        base_cost, w.cost or 0, pw,
        state.phase or "unknown")

      -- Pillbox-suicider surcharge, applied at selection alongside the phase
      -- weight / influence scale. Rendered whenever it isn't 1.0 so the row's
      -- numbers still reconcile (base x pw x inf x suicider + penalties = total).
      local suicider_mult = (gc and gc.suicider_mult) or 1.0
      if suicider_mult ~= 1.0 then
        detail_formula = string.format("%s * suicider{%.1f}", detail_formula, suicider_mult)
        detail_map[#detail_map + 1] = string.format(
          "suicider:pill_suicider role -> this goal kind (%s) costs x%.1f (attack_pill and the refuel group are exempt; defend_pill x%.1f, everything else x%.1f)",
          tostring(w.kind or (gc and gc.kind) or pname), suicider_mult,
          C.PILL_SUICIDER_DEFEND_MULT or 1.0, C.PILL_SUICIDER_OTHER_MULT or 1.0)
      end

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
      -- Show the TRUE raw cost (w.cost, same number the per-pool section and
      -- detail popup show) times the phase multiplier, so the line reads
      -- "raw x PHASE@pw +penalties = total" and every number reconciles with
      -- the other views.
      local raw_base = w.cost or (pw ~= 0 and base_cost / pw) or base_cost
      -- Reconciling multipliers: show the APPLIED phase weight
      -- (distance-attenuated, recorded by goal_selection) and the
      -- influence scale — the raw table weight (e.g. "MD@0.50") never
      -- balanced against =total (defend_pill 100 x MD@0.50 +38 "=75"
      -- actually meant 100 x 0.74 x inf0.5 + 38 = 75).
      local eff_pw   = (gc and gc.phase_weight) or pw
      local inf_mult = (gc and gc.inf_mult) or 1.0
      local row_summary = string.format("%s %.0f x %s@%.2f",
        pname, raw_base, phase_abbrev, eff_pw)
      if inf_mult ~= 1.0 then
        row_summary = row_summary .. string.format(" x inf@%.1f", inf_mult)
      end
      if suicider_mult ~= 1.0 then
        row_summary = row_summary .. string.format(" x suicider{%.1f}", suicider_mult)
      end

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
      local synthetic_id = bit.bor((bit.lshift(idx, 16)), (w.id or 0))

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
        blitz = (pname == "attack_pill") and is_blitz_pill(w.id) or false,
      }
    end
  end

  -- Strip pools (11/12/13) — injected directly into pool_cache by
  -- their respective evaluators, not routed through eval_queue or
  -- goal_competition.  We synthesize a WINNERS row from pool_cache so
  -- they compete on the same cost axis as the regular pools.  Note:
  -- 11 (offensive_build) is reserved but currently unused — the entry will
  -- show up here as soon as something writes pool_cache[11].
  --
  -- Strip rows get the same "<pool_name> <cost> <phase>@<pw>" row
  -- summary as main pools so the WINNERS column is uniformly readable.
  -- Their detail formula comes from cost_cache (richer breakdown) when
  -- available, falling back to sw.desc (the short tagline).
  for _, idx in ipairs({10, 11, 12, 13}) do
    local sw = pc[idx]
    if sw and sw.goal and sw.cost and sw.cost >= 0 and sw.cost < 1e29 then
      local synthetic_id = bit.bor((bit.lshift(idx, 16)), (sw.goal.target_id or 0))
      local pname = POOL_NAMES[idx] or ("p"..idx)
      local pw   = (phase_weights and phase_weights[pname]) or 1.0  -- name-keyed, not idx
      local phase_abbrev = ({
        opening = "OP", early = "EA", middle = "MD", late = "LT",
        endgame = "EG", unknown = "??"
      })[(state.phase or "unknown")] or (state.phase or "??"):sub(1,2):upper()
      local raw_base = (pw and pw ~= 0) and (sw.cost / pw) or sw.cost
      local row_summary = string.format("%s %.0f x %s@%.2f",
        pname, raw_base, phase_abbrev, pw)
      -- Suicider surcharge (reposition / offensive_build / wait_for_lgm / kill_lgm
      -- are all non-exempt kinds, so these strips DO pay it).
      local _strip_sui = suicider_mult_for_pool(state, pname)
      if _strip_sui ~= 1.0 then
        row_summary = row_summary .. string.format(" x suicider{%.1f}", _strip_sui)
      end
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
        reject = sw._reject,
        active_goal = (active_pool == idx
                       and ((active_id and active_id >= 0 and active_id == (sw.goal.target_id or -1))
                            or (active_mx ~= nil and sw.goal.mx == active_mx and sw.goal.my == active_my))),
        stale = 0,
        formula = final_formula,
      }
    end
  end

  -- Cell 10 = cross-pool WINNERS table, ranked ascending by FINAL cost
  -- (`weighted` = the yellow "cost= C" the renderer shows = post-phase-weight,
  -- post-penalty total). Sorting by the raw per-pool `cost` instead would put
  -- rows out of order vs. the number displayed on them.
  table.sort(winners, function(a, b) return (a.weighted or a.cost) < (b.weighted or b.cost) end)
  -- The winner is the cheapest NON-rejected row (rejected rows, e.g. a
  -- reposition with no team pills, are shown for visibility but can't win).
  local first_winner = nil
  for _, w in ipairs(winners) do
    if not w.reject then w.is_winner = true; first_winner = w; break end
  end
  -- Warmup: while the pools are still warming (fewer than WARMUP_MIN_REAL_GOALS
  -- candidates costed below the wall cost) surface a reject row at the TOP of
  -- WINNERS so the panel shows the warmup state — even if the bot has grabbed a
  -- single reachable goal (e.g. attack_tank against the tank that just killed
  -- it) and so isn't on the explore fallback this tick. Shown whenever not
  -- warm, regardless of current goal; marked active only when actually
  -- exploring. warm_ready latches once warm so this clears in steady state.
  if state.goal and not M.warm_ready(state) then
    local need = C.WARMUP_MIN_REAL_GOALS or 10
    local n = state._warm_count or 0
    local gmx, gmy = state.goal.mx or 0, state.goal.my or 0
    table.insert(winners, 1, {
      id = 0, src_pool = 0,
      mx = gmx, my = gmy,
      cost = -1e9, weighted = -1e9,   -- renders as "cost= ?"
      is_winner = false,
      active_goal = (state.goal.kind == "explore"),
      reject = "warmup", reject_remaining = math.max(0, need - n),
      formula = string.format(
        "REJECT warmup %d/%d @(%d,%d)||reject:warmup — only %d of %d goals costed below the wall cost (9999); pools still warming from this position",
        n, need, gmx, gmy, n, need),
    })
  end
  sections[#sections + 1] = {
    idx = 10, name = "WINNERS", weight = 1.0,
    winner_id = (first_winner and first_winner.id) or -1,
    layout_cell = LAYOUT_CELL[10], rows = winners,
  }

  -- Strips below the grid: offensive_build (11), wait_for_lgm (12),
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

  if BRAIN_DEBUG_MODE then local _nr=0 for _,s in ipairs(sections) do _nr=_nr+(s.rows and #s.rows or 0) end print2(string.format("POOL_BREAKDOWN_EXIT t=%d bot=%s sections=%d rows=%d", now, tostring(state.player_number), #sections, _nr)) end
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
    local nwp = __idiv(#vp.path, 2)
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

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
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
        label = label .. string.format(" XF=%.0f", b.crossfire)
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
