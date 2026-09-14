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
local danger = require("danger")   -- the two build scores + the panic trigger
local vizmod = require("viz")
local json   = require("json")
local ally_state = require("ally_state")
local circles    = require("circles")
local squad  = require("squad")
local builder = require("builder")   -- shared panic guard-spot search (M.guard_build_spot)
local bpool  = require("builder_pool")  -- the repair_pill split + the BUILDER panel strip
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

-- =========================================================================
-- Low-stock markup for refuel candidates (author, 2026-09-03).
--
-- The low_stock REJECT (in both refuel scoring paths below) only drops a base
-- that can supply NOTHING we need. A base that is full of armour but down to
-- four shells survives it and was then priced exactly like one holding ninety.
-- So: for each resource we ACTUALLY NEED, a base whose observed stock is below
-- REFUEL_MIN_STOCK marks its whole cost up:
--   need armour (armour < armour_target) and obs_armour < REFUEL_MIN_STOCK
--                                              -> cost x REFUEL_LOW_ARMOUR_MULT
--   need shells (shells < shell_target)  and obs_shells < REFUEL_MIN_STOCK
--                                              -> cost x REFUEL_LOW_SHELLS_MULT
-- Both at once compounds (1.10 x 1.10 = 1.21). A multiplier, not an additive
-- charge, so a short base under the tank's nose stays cheap to top up at while
-- a short base across the map is priced out.
--
-- FRESHNESS: only a live observation (obs_tick within REFUEL_OBS_STALE) marks
-- anything up. Absent or stale pays nothing -- bases regenerate, and the
-- low_stock/depleted rejects plus STALE_PENALTY_* already own that case.
--
-- UNITS: obs_armour is compared against REFUEL_MIN_STOCK exactly the way the
-- low_stock reject compares it, deliberately. Base armour reaches the brain
-- through TWO engine paths that disagree by 5x -- EVENT_BASE_STOCK carries it
-- raw (world.lua writes 90) while the per-tick "closest base" item divides by
-- five (bases.c basesGetBrainBaseItem: armour/5, which perception.lua writes
-- into the same obs_armour field, 90 -> 18) -- so the field's units depend on
-- which source wrote last. Reading it the same way as the reject means the
-- markup can never contradict the reject, whichever number is in there.
--
-- Returns mult, detail (detail carries the chip inputs for the panels).
-- =========================================================================
local function refuel_low_stock_mult(b, info, state, now)
  local d = {
    fresh = false, low_arm = false, low_sh = false,
    need_arm = false, need_sh = false,
    obs_sh = b.obs_shells, obs_arm = b.obs_armour,
    age = b.obs_tick and (now - b.obs_tick) or nil,
  }
  if not d.age or d.age >= (C.REFUEL_OBS_STALE or 500) then
    return 1.0, d
  end
  d.fresh = true
  local sh_t  = (state and state.shell_target)  or C.TANK_FULL_SHELLS
  local arm_t = (state and state.armour_target) or C.TANK_FULL_ARMOUR
  d.need_arm = (info.armour or 0) < arm_t
  d.need_sh  = (info.shells or 0) < sh_t
  local mult = 1.0
  if d.need_arm and (b.obs_armour or 0) < C.REFUEL_MIN_STOCK then
    d.low_arm = true
    mult = mult * (C.REFUEL_LOW_ARMOUR_MULT or 1.0)
  end
  if d.need_sh and (b.obs_shells or 0) < C.REFUEL_MIN_STOCK then
    d.low_sh = true
    mult = mult * (C.REFUEL_LOW_SHELLS_MULT or 1.0)
  end
  return mult, d
end

-- One chip per markup so every log line / panel says the same thing: which
-- resource the base is short of and what that multiplied the cost by.
local function refuel_low_stock_chip(d)
  if not d then return "" end
  local s = ""
  if d.low_arm then
    s = s .. string.format(" lowarm{x%.2f}", C.REFUEL_LOW_ARMOUR_MULT or 1.0)
  end
  if d.low_sh then
    s = s .. string.format(" lowsh{x%.2f}", C.REFUEL_LOW_SHELLS_MULT or 1.0)
  end
  return s
end

-- =========================================================================
-- REFUEL-PAD REACH — can any live pill actually shoot a tank that is DOCKED
-- on the base tile (mx,my)?
--
-- The danger grid a pill stamps is deliberately one tile wider than its gun:
-- PILL_RANGE_MAP is 9 against a real PILL_FIRE_RANGE of 8, so the DRIVE keeps
-- a margin. That pad is right for steering and wrong for pricing a refuel
-- stop, where the tank does not pass through the tile, it PARKS on it.
--
-- The engine's test (pillbox.c pillsUpdate -> util.c utilIsItemInRange) is the
-- euclidean distance from the PILL's tile centre (x + MAP_SQUARE_MIDDLE) to
-- the TANK's world position, against PILLBOX_RANGE 2048 wu = 8.0 tiles. A tank
-- sitting on a tile is at most half a tile diagonal -- REFUEL_PAD_TANK_OFFSET,
-- 0.7071 tiles -- from that tile's centre. So a pill can touch a docked tank
-- only when the tile-centre distance is <= PILL_FIRE_RANGE + that offset
-- (8.7071); at 9.0 it cannot, from ANY point on the tile.
--
-- Incident 20260903_193428 bot2 t=1563: base#0 @(138,112) priced
-- raw 9 + base 45 + danger 427 = 640.9 and lost to a base 12 tiles away at
-- 301, while the tank had 10 armour two tiles from base#0. The 427 was
-- threat.at(base) x REFUEL_DANGER_WEIGHT, and the whole of it came from pill#2
-- at (138,121) -- exactly 9.0 tiles from the base centre, sitting on the RIM of
-- the stamp and angry, so the rim still read 21.4. It could not have fired a
-- single shell at that base.
--
-- Returns reachable, nearest_id, nearest_dist:
--   reachable    -- true if ANY live, deployed hostile/neutral pill is within
--                   PILL_FIRE_RANGE + REFUEL_PAD_TANK_OFFSET of the tile. No
--                   fractional scaling: the tank does not choose where on the
--                   tile it stops, so one pill reaching part of it is a hit.
--   nearest_id   -- the closest such pill (whichever verdict), nil if there is
--                   no live hostile/neutral pill on the map at all.
--   nearest_dist -- its tile-centre euclidean distance, in tiles.
-- Ties go to the lower id so the verdict is the same in every replay.
-- Cheap by construction: one pass over world.pills per refuel candidate per
-- replan (a handful of pills, a handful of bases).
-- =========================================================================
local function refuel_pad_pill_reach(world, mx, my)
  local thresh = (C.PILL_FIRE_RANGE or 8) + (C.REFUEL_PAD_TANK_OFFSET or 0.7071)
  local best_id, best_d = nil, math.huge
  local pills = world and world.pills
  if pills then
    for id, p in pairs(pills) do
      if (p.owner == "hostile" or p.owner == "neutral")
         and (p.health or 0) > 0
         and not p.in_tank and not p.carrier and not p._synth_carry then
        local d = U.edist(p.mx, p.my, mx, my)
        if d < best_d or (d == best_d and best_id ~= nil and id < best_id) then
          best_d, best_id = d, id
        end
      end
    end
  end
  if best_id == nil then return false, nil, nil end
  return (best_d <= thresh), best_id, best_d
end

-- The chip both refuel paths print, and the panel repeats: which verdict the
-- pad reach test reached and the two numbers it compared.
local function refuel_pad_chip(pad_safe, pad_id, pad_dist)
  local thresh = (C.PILL_FIRE_RANGE or 8) + (C.REFUEL_PAD_TANK_OFFSET or 0.7071)
  if pad_safe then
    if pad_id == nil then
      return " padsafe{no live hostile pill on the map}"
    end
    return string.format(" padsafe{no pill reaches the pad; nearest #%s at %.1f > %.2f}",
                         tostring(pad_id), pad_dist or 0, thresh)
  end
  return string.format(" padhit{#%s reaches: %.1f <= %.2f}",
                       tostring(pad_id), pad_dist or 0, thresh)
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
        -- REFUEL-PAD READ (see refuel_pad_pill_reach). The PILL layer of the
        -- danger grid is one tile wider than a pill's gun; a tank that PARKS
        -- on this tile is either inside somebody's fire circle or it is not.
        -- When no live pill reaches the pad, drop the pill layer entirely and
        -- keep only the enemy-tank layer. When one does, the stamped value
        -- stands as it is -- no fractional scaling, the tank does not pick
        -- which corner of the tile it stops on.
        local _pad_reach, _pad_id, _pad_dist = refuel_pad_pill_reach(world, b.mx, b.my)
        local _pad_safe = not _pad_reach
        local _pad_removed = 0
        if _pad_safe then
          _pad_removed = threat.pill_at(b.mx, b.my) or 0
          danger = math.max(0, danger - _pad_removed)
        end
        -- When fleeing at critical armour, hard-reject bases that are too
        -- dangerous to sit at.  No point driving to a base where you'll die
        -- before the refuel completes.
        if danger_reject and danger > danger_reject then
          candidates[#candidates + 1] = {
            id = id, mx = b.mx, my = b.my, own = b.owner,
            travel = travel, danger = danger, dw = danger_weight,
            score = -1,
            pad_safe = _pad_safe, pad_pill = _pad_id, pad_dist = _pad_dist,
            reject = string.format("danger %.1f > reject %.1f%s", danger, danger_reject,
                                   refuel_pad_chip(_pad_safe, _pad_id, _pad_dist)),
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
           and not (b.mx == tmx and b.my == tmy)
           -- Yield waiver: we're topping off and a starved ally claimed the
           -- base under us (state._refuel_yield_tick, stamped by init.lua's
           -- target cap) -- the +500 must not pin us here.
           and (now - (state._refuel_yield_tick or -1e9)) > 25 then
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
        -- Low-stock markup: x1.10 per resource we need that this base is
        -- observed short of (see refuel_low_stock_mult). Multiplies the whole
        -- score above, so it scales with the trip.
        local low_mult, low_d = refuel_low_stock_mult(b, info, state, now)
        score = score * low_mult
        candidates[#candidates + 1] = {
          id = id, mx = b.mx, my = b.my, own = b.owner,
          travel = travel, danger = danger, dw = danger_weight,
          score = score, hyst = hysteresis, contested = contested,
          low_mult = low_mult, low_d = low_d,
          pad_safe = _pad_safe, pad_pill = _pad_id, pad_dist = _pad_dist,
          pad_removed = (_pad_removed > 0) and _pad_removed or nil,
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
  -- take_cover is its own group on purpose: switching TO it from anything
  -- else pays the "type" hysteresis tier (like a flee), which is exactly the
  -- resistance we want on a goal that abandons whatever we were doing.
  take_cover = "take_cover",
  none = "none",
}

local function goal_group(kind)
  return GOAL_GROUPS[kind] or kind
end

-- Rows the LOADED, BUILDER-LESS state multiplies by C.ATTACK_NO_BUILDER_MULT.
-- The four "drive at a fight" kinds: three attacks and the defend_pill ALARM
-- row (which is itself "a hostile tank is on our pill, go and meet it").
-- kill_lgm is deliberately NOT here -- it is a one-shot on a walking man, not
-- a tank duel, and it already prices itself very low for that reason.
-- KM: the whole "loaded, builder-less" toolbox in ONE chunk-level local.
-- goals.lua sits right on Lua's 200-locals-per-chunk ceiling, so a feature
-- that needs five helpers has to spend one name, not five.
local KM = {}
KM.ATTACK_KINDS = {
  attack_tank = true, attack_pill = true,
  attack_base = true, defend_pill = true,
}

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
  take_cover     = true,   -- survival, like offensive_build: a suicider that
                           -- cannot take cover just dies earlier for less
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
  -- ── Mine-hoard staying-cost (additive, at-this-base only) ──────────────
  -- The exponential itself is unchanged: WEIGHT x (BASE^(mines-FREE) - 1).
  --
  -- What is new (2026-09-06, C.REFUEL_MINE_HOARD_NEEDS_SUPPLY) is WHEN it is
  -- charged. The term is an EVICTION lever — "don't sit on a pad just to load
  -- mines" — so it is waived while the tank still has a real reason to be on
  -- the pad: below one of its targets AND parked on a base that still holds
  -- REFUEL_MIN_STOCK of that same supply. At both targets, or on a base that
  -- has run dry of everything we still need (nothing left but mines), it is
  -- charged in full and still evicts, exactly as before.
  --
  -- UNITS: info.base.armour is the engine's per-tick "closest base" item,
  -- which is the raw base armour DIVIDED BY FIVE (bases.c
  -- basesGetBrainBaseItem: armour/5, so a full 90 reads 18); shells are raw.
  -- perception.lua writes those same two numbers into obs_armour/obs_shells
  -- for the base we're standing on, so comparing them against REFUEL_MIN_STOCK
  -- here reads the base exactly the way nearest_resupply_base's low_stock
  -- reject reads it, and the waiver can never contradict the reject.
  local mines_carried = info.mines or 0
  local mine_free  = C.REFUEL_MINE_FREE or 5
  local mines_over = math.max(0, mines_carried - mine_free)
  local mine_raw = 0.0
  if mines_over > 0 then
    mine_raw = (C.REFUEL_MINE_HOARD_WEIGHT or 0)
               * ((C.REFUEL_MINE_HOARD_BASE or 1.3) ^ mines_over - 1.0)
  end
  local _b        = info.base
  local b_arm     = _b and (_b.armour or 0) or 0
  local b_sh      = _b and (_b.shells or 0) or 0
  local min_stock = C.REFUEL_MIN_STOCK or 5
  local need_arm  = arm < arm_target
  local need_sh   = sh  < sh_target
  local mine_waived = false
  if C.REFUEL_MINE_HOARD_NEEDS_SUPPLY and mines_over > 0 then
    mine_waived = (need_arm and b_arm >= min_stock)
               or (need_sh  and b_sh  >= min_stock)
  end
  local mine_cost = mine_waived and 0.0 or mine_raw
  -- Every input to the decision, carried out for the debug line and the panel.
  -- Plain numbers only: the strings are built at the print sites so nothing
  -- formats on a tick nobody is looking.
  local mine_d = {
    carried = mines_carried, free = mine_free, over = mines_over,
    raw = mine_raw, waived = mine_waived,
    knob = C.REFUEL_MINE_HOARD_NEEDS_SUPPLY and true or false,
    weight = C.REFUEL_MINE_HOARD_WEIGHT or 0,
    expbase = C.REFUEL_MINE_HOARD_BASE or 1.3,
    arm = arm, arm_target = arm_target, need_arm = need_arm,
    sh = sh, sh_target = sh_target, need_sh = need_sh,
    has_base = _b and true or false, b_arm = b_arm, b_sh = b_sh,
    min_stock = min_stock,
  }
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
  return bonus, mult, fill, scarcity, mine_cost, urgency, arm_def, sh_def, mine_d
end

-- Mine-hoard chip text, shared by the REFUEL_SHAPE debug line and the pool-viz
-- term breakdown so the two can never disagree. `md` is refuel_shape's
-- mine-detail table. Returns:
--   head — the short value that goes inside mines{...}. The panel's formula
--          parser DROPS a term whose braced value reaches 32 characters, so
--          this stays tiny and the derivation lives in `why`.
--   why  — the whole thing, hand-computable: the exponential written out with
--          its constants, whether it was charged, and every number the
--          waive decision looked at.
local function mine_chip(md)
  local head = md.waived
    and string.format("0 of %.1f waived", md.raw)
    or  string.format("%.1f", md.raw)
  local why = string.format(
    "%.1f = %g[MINE_HOARD_WEIGHT] x (%g[MINE_HOARD_BASE]^(%d mines - %d[MINE_FREE]) - 1)"
    .. "; %s: arm %d/%d (%s), sh %d/%d (%s); base under us arm=%s sh=%s,"
    .. " MIN_STOCK=%d",
    md.raw, md.weight, md.expbase, md.carried, md.free,
    (not md.knob) and "CHARGED (NEEDS_SUPPLY off)"
      or (md.waived and "WAIVED (below a target this base can still supply)"
                    or "CHARGED (this base supplies nothing we still need)"),
    md.arm, md.arm_target, md.need_arm and "below" or "at",
    md.sh, md.sh_target, md.need_sh and "below" or "at",
    md.has_base and tostring(md.b_arm) or "none",
    md.has_base and tostring(md.b_sh) or "none",
    md.min_stock)
  return head, why
end

-- Is refuel a CANDIDATE this tick? Two ways in, and they mean different things:
--
--   LOW      — at/below either watermark (ARMOUR_LOW 15 / SHELLS_LOW 20).
--              The original rule, and the only one before 2026-09-06.
--   TOP-OFF  — C.REFUEL_TOPOFF_CANDIDATE: anywhere BELOW the dynamic full
--              targets (state.armour_target / state.shell_target, plus
--              REFUEL_MIN_MINES). The author's rule: "20 shells is a good
--              number to be 'you're full enough, go do stuff unless it's
--              worth the cost to keep recharging'" — i.e. topping off past
--              the low line is a real option that has to WIN, not an option
--              that does not exist. The existing quadratic ramp in
--              refuel_shape prices it (fill 0 at the low line, 1 at target),
--              so off the pad a 30/40-shell tank prices refuel at base x2.75
--              and normally loses. Nothing about the PRICING changes: below
--              the low lines fill is 0 and the mult is 1.0, exactly as before.
--
-- Only CANDIDACY moves. has_shells (the pool gate for pill takes), the
-- critical-armour flee injection (gated on `critical`, an emergency) and the
-- refuel goal's own completion (armour_target/shell_target in init.lua) are
-- all untouched.
--
-- Sets state._refuel_topoff_only so the debug lines can say `topoff=on` for a
-- row that exists ONLY because of the flag.
local function refuel_need(state, info)
  local low = (info.armour or 99) <= C.ARMOUR_LOW
           or (info.shells or 99) <= C.SHELLS_LOW
  local topoff = false
  if C.REFUEL_TOPOFF_CANDIDATE and not low then
    topoff = (info.armour or 99) < (state.armour_target or C.TANK_FULL_ARMOUR)
          or (info.shells or 99) < (state.shell_target or C.TANK_FULL_SHELLS)
          or (info.mines or 99) < (C.REFUEL_MIN_MINES or 0)
  end
  state._refuel_topoff_only = topoff
  return (low or topoff), low, topoff
end

-- =========================================================================
-- Pool evaluators — each evaluates one category of goal candidates.
-- Called one-per-tick by update_pool_cache() to spread the cost.
-- Each returns a pool entry table or nil.
-- =========================================================================

local function eval_refuel(state, world, info, tmx, tmy, boat, ammo)
  local needs_resupply = refuel_need(state, info)
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
    desc = BRAIN_POOL_VIZ and string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d/%d sh=%d/%d%s%s",
           bid, base.mx, base.my, bscore, urgency, cost,
           info.armour, state.armour_target or C.TANK_FULL_ARMOUR,
           info.shells, state.shell_target or C.TANK_FULL_SHELLS,
           state._refuel_topoff_only and " topoff=on" or "", hyst_str) or "",
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
  -- HOISTED out of the `else` block below: the desc at the bottom of this
  -- function referenced `contested` while it was a local scoped INSIDE that
  -- block, so on the dead-pill path (and, in Lua, on every path as far as the
  -- desc was concerned) it read a nil GLOBAL. Declared here, it means what the
  -- desc says it means on both paths.
  local rp_uf, rp_hit_age = false, nil
  local rp_soft, rp_soft_id, rp_soft_d, rp_soft_why = false, nil, nil, nil
  if pill.health == 0 then
    -- Dead pill → rebuild-in-place model (own terrain + tank-snipe cost; the
    -- contest multipliers below are skipped — snipe already prices in nearby
    -- enemy tanks).
    adj_cost = compute_repair_dead_cost(state, world, info, pill, tmx, tmy)
  else
    adj_cost = (C.REPAIR_BASE_COST or 30) + math.max(0, pcost - damage * C.REPAIR_DAMAGE_BONUS)
    -- Same two contest terms as the live pool-5 copy in step_eval_queue (see
    -- the long note there and REPAIR_QUIET_TICKS in constants.lua):
    --   UNDER_FIRE      x3    the pill took a hit less than REPAIR_QUIET_TICKS
    --                         ago -- shells are still landing on it.
    --   ENEMY_IN_RANGE  x1.5  a hostile tank visible THIS tick within
    --                         PILL_FIRE_RANGE + PILL_REPOSITION_ENEMY_TANK_PAD
    --                         of the pill, or closer to it than we are and
    --                         inside DEFEND_ENEMY_NEAR_RADIUS.
    local now_t = state.tick or 0
    local lh = pill.last_hit_tick
    if lh and lh > 0 then
      rp_hit_age = now_t - lh
      if rp_hit_age < (C.REPAIR_QUIET_TICKS or 75) then rp_uf = true end
    end
    local enemy_tanks = state.perc and state.perc.enemy_tanks
    if not rp_uf and enemy_tanks then
      local soft_r = (C.PILL_FIRE_RANGE or 8)
                     + (C.PILL_REPOSITION_ENEMY_TANK_PAD or 5)
      local near_r = C.DEFEND_ENEMY_NEAR_RADIUS or 10
      local our_d = U.mdist(tmx, tmy, pill.mx, pill.my)
      for _, e in ipairs(enemy_tanks) do
        local ed = U.edist(e.mx, e.my, pill.mx, pill.my)
        local md = U.mdist(e.mx, e.my, pill.mx, pill.my)
        if ed <= soft_r then
          rp_soft, rp_soft_id, rp_soft_d, rp_soft_why = true, e.id, ed, "in_range"
          break
        elseif md < our_d and md <= near_r then
          rp_soft, rp_soft_id, rp_soft_d, rp_soft_why = true, e.id, ed, "closer_than_us"
          break
        end
      end
    end
    if rp_uf then
      adj_cost = adj_cost * (C.REPAIR_UNDER_FIRE_MULT or C.REPAIR_CONTESTED_MULT or 3.0)
    elseif rp_soft then
      adj_cost = adj_cost * (C.REPAIR_ENEMY_IN_RANGE_MULT or 1.5)
    end
  end
  local rp_chip = ""
  if rp_uf then
    rp_chip = string.format(" x%.1f UNDER_FIRE(hit %st ago < quiet %d)",
      C.REPAIR_UNDER_FIRE_MULT or 3.0,
      rp_hit_age and string.format("%d", rp_hit_age) or "?",
      C.REPAIR_QUIET_TICKS or 75)
  elseif rp_soft then
    rp_chip = string.format(" x%.1f ENEMY_IN_RANGE(#%s %.1ft %s)",
      C.REPAIR_ENEMY_IN_RANGE_MULT or 1.5,
      tostring(rp_soft_id), rp_soft_d or -1, tostring(rp_soft_why))
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
           rp_chip) or "",
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

  -- ── "KILL ME" RESPONDER ROWS ──────────────────────────────────────────
  -- Every ally advertising a kill_me token becomes an extra pool-9 candidate
  -- whose SUBJECT IS THAT ALLY. It rides pool 9 because it is the same
  -- errand mechanically -- drive into gun range of a tank and shoot it until
  -- it dies -- and because that is where the fight loop already lives.
  --
  -- Priced KILL_ME_RESPONDER_BASE + the Dijkstra travel cost to the ally's
  -- tile, and NOTHING else: no aim bonus, no crossfire term, no
  -- low-shells ramp. A delivery is a delivery.
  --
  -- Two REJECTS, both priced INF with the row still shown so the panel says
  -- why we are not answering:
  --   no_lgm      our OWN man is not in the tank. Killing an ally for pills
  --               we also cannot place moves the problem, it does not solve
  --               it.
  --   low_shells  ceil(ally armour / TANK_SHELL_DAMAGE) + KILL_ME_SHELL_MARGIN,
  --               and never below TANK_COMBAT_MIN_SHELLS. Under-gunned help
  --               is worse than none: it wakes the ally's armour up and
  --               leaves it standing in the open with a stack aboard.
  --   ally_claimed  another ally already claimed this request and beat our
  --               bid by more than ALLY_CLAIMED_STEAL_FRAC_KILLME.
  --
  -- These rows are exempt from the influence x0.5/x2 rule and from
  -- ATTACK_NO_BUILDER_MULT (both keyed off goal.km_ally_pn in
  -- goal_competition) -- our own builder is aboard by construction, and the
  -- errand is the cure for the state, not an instance of it.
  do
    local reqs = M.kill_me_requests(state, info, state.tick or 0)
    if #reqs > 0 then
      local km = state.km
      if not km then km = {}; state.km = km end
      local best_claim = nil
      for _, rq in ipairs(reqs) do
        local reject, why = nil, nil
        if info.man_status ~= C.LGM_INTANK then
          reject = "no_lgm"
          why = string.format("our own man is %s -- we could not place the pills either",
            (info.man_status == C.LGM_DEAD) and "DEAD" or "OUT")
        end
        local need = math.ceil(rq.armour / (C.TANK_SHELL_DAMAGE or 5))
                     + (C.KILL_ME_SHELL_MARGIN or 2)
        if need < (C.TANK_COMBAT_MIN_SHELLS or 10) then
          need = C.TANK_COMBAT_MIN_SHELLS or 10
        end
        if not reject and (info.shells or 0) < need then
          reject = "low_shells"
          why = string.format("%d shells, need ceil(%d armour / %d) + margin %d = %d (floor TANK_COMBAT_MIN_SHELLS %d)",
            info.shells or 0, rq.armour, C.TANK_SHELL_DAMAGE or 5,
            C.KILL_ME_SHELL_MARGIN or 2, need, C.TANK_COMBAT_MIN_SHELLS or 10)
        end
        local travel = smart_cost(KIND_NORMAL, tmx, tmy, rq.mx, rq.my,
                                  info.inboat and 1 or 0, info.shells or 32,
                                  info.trees or 0, info.mines or 0,
                                  info.armour or 40)
        if (not travel) or travel >= 1e8 then
          if not reject then
            reject = "unreachable"
            why = string.format("no route from (%d,%d) to the ally's tile (%d,%d)",
                                tmx, tmy, rq.mx, rq.my)
          end
          travel = 0
        end
        local cost = (C.KILL_ME_RESPONDER_BASE or 20) + travel
        if not reject then
          local ac = KM.ally_claim(state, info, rq.pn, cost, state.tick or 0)
          if ac then
            reject = "ally_claimed"
            why = string.format("p%d already claimed p%d's request at %.0f; ours is %.0f and the steal band is %.0f%%",
              ac.pn, rq.pn, ac.cost or -1, cost,
              100 * (C.ALLY_CLAIMED_STEAL_FRAC_KILLME or 0.10))
          end
        end
        breakdown[#breakdown + 1] = {
          id = rq.pn, mx = rq.mx, my = rq.my,
          dist = U.mdist(tmx, tmy, rq.mx, rq.my), speed = 0,
          path_cost = travel, base = C.KILL_ME_RESPONDER_BASE or 20,
          aim_bonus = 0, aim_diff = 0, crossfire = 0, wall_penalty = 0,
          low_shells_penalty = 0, tank_shells = info.shells,
          shells_on_arrival = 0,
          cost = reject and 1e30 or cost,
          skipped = reject, kill_me_why = why,
          kill_me = true, km_ally_pn = rq.pn, km_ally_armour = rq.armour,
          km_need_shells = need,
          player_name = "ally p" .. tostring(rq.pn),
        }
        if not reject and cost < best_cost then
          best_cost = cost
          best_tank = { mx = rq.mx, my = rq.my, dist = U.mdist(tmx, tmy, rq.mx, rq.my),
                        id = rq.pn, speed = 0, km_ally_pn = rq.pn }
          best_claim = { pn = rq.pn, cost = cost }
        end
        -- One line per request per eval: the row, its price and its verdict.
        -- The pool grid shows the same thing, but a headless run has no panel.
      end
      -- What we advertise as OUR claim this tick. Read by every ally's
      -- KM.ally_claim, and by the INITIATOR so it knows whom not to shoot.
      km.claim = best_claim
    else
      if state.km then state.km.claim = nil end
    end
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
    return nil
  end
  -- A non-rejected attack_tank candidate exists. Flag it so attack_pill gets a
  -- flat cross-penalty (prefer dealing with the tank) and the panic_build viz
  -- shows. Stash the threat tile for the viz.
  state._attack_tank_present = true
  state._attack_tank_threat  = { mx = best_tank.mx, my = best_tank.my, dist = best_tank.dist,
                                 have_pill = (info.carried_pills or 0) >= 1 }

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
     and not best_tank.km_ally_pn
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
             -- "KILL ME" delivery: the subject of this attack_tank goal is an
             -- ALLY who asked to be killed for its cargo. The flag is what
             -- makes goal_competition skip the influence scale and
             -- ATTACK_NO_BUILDER_MULT, what makes init.lua's goal-validity
             -- check stop demanding a visible ENEMY, and what makes
             -- steering's fight loop target a friendly tank at all.
             km_ally_pn = best_tank.km_ally_pn,
             tank_scan_spots = win_entry and win_entry.scan_spots or nil,
             tank_standoff_deg = win_entry and win_entry.standoff_deg or nil,
             tank_standoff_mx = win_entry and win_entry.standoff_mx or nil,
             tank_standoff_my = win_entry and win_entry.standoff_my or nil, },
    desc = BRAIN_POOL_VIZ and string.format("attack_tank@(%d,%d) cost=%.0f dist=%d spd=%.1f%s%s",
           best_tank.mx, best_tank.my, best_cost, best_tank.dist, best_tank.speed,
           best_tank.km_ally_pn
             and string.format(" subject=ally p%d [KILL_ME]", best_tank.km_ally_pn) or "",
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
-- Returns fx, fy, danger_of_chosen_tile, fell_back
--   fell_back = true when EVERY forest on the map is inside hostile pill fire
--   and we picked the least-dangerous one anyway (better than deadlocking on
--   an unpayable build) — the caller says so in the pool desc.
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

  -- "Safe" has to MEAN something: a forest tile inside a hostile/neutral
  -- pill's fire is not a place to send a builder, whatever the Dijkstra slate
  -- says (the slate's danger weighting is a soft preference, not a veto).
  -- Same line the rest of the brain calls bad ground:
  -- TAKE_COVER_BAD_GROUND_PILL_AT == TANK_COMBAT_DEFENDED_DANGER.
  local BAD = C.TAKE_COVER_BAD_GROUND_PILL_AT or C.TANK_COMBAT_DEFENDED_DANGER or 30
  local best_x, best_y, best_cost = nil, nil, math.huge
  local near_x, near_y, near_d    = nil, nil, math.huge
  local safe_x, safe_y, safe_cost = nil, nil, math.huge
  local calm_x, calm_y, calm_dgr  = nil, nil, math.huge
  for k = 1, nf do
    local x, y = fx[k], fy[k]
    local cost = cpf.smart_cost_dij_only(KIND_NORMAL, x, y, 0)
    local dgr  = threat.pill_at(x, y) or 0
    if cost < best_cost then best_cost = cost; best_x = x; best_y = y end
    if dgr < BAD and cost < safe_cost then
      safe_cost = cost; safe_x = x; safe_y = y
    end
    if dgr < calm_dgr then calm_dgr = dgr; calm_x = x; calm_y = y end
    local d = U.mdist(tmx, tmy, x, y)
    if d < near_d then near_d = d; near_x = x; near_y = y end
  end
  -- cheapest reachable forest that is NOT under pill fire
  if safe_x then return safe_x, safe_y, threat.pill_at(safe_x, safe_y) or 0, false end
  -- every forest is covered: take the least-dangerous one and say so
  if calm_x then return calm_x, calm_y, calm_dgr, true end
  if best_x then return best_x, best_y, threat.pill_at(best_x, best_y) or 0, true end
  return near_x, near_y, near_x and (threat.pill_at(near_x, near_y) or 0) or 0, true
end

-- `only` (optional): { mx, my, sc7 } -- re-score exactly ONE tile with the
-- scan's context rebuilt fresh, and return its score (nil if the tile no longer
-- qualifies: surplus category, category drift under STRICT_NEED, blocked,
-- unplaceable). Used by the harvest resume (init.lua) to ask "is the tile the
-- builder just cleared still worth the pill?" without the pool. sc7 (distance
-- from the tank) is pinned to its dispatch value because the tank has moved on
-- by design; every other term reads the live world. Skips the paths that are
-- about choosing WHETHER to place at all (seek-trees redirect, offensive/panic
-- drop, combat-goal interrupt, util-reserve hold): that decision was made at
-- dispatch.
--
-- `tmx, tmy` on a re-score (`only`) are the TANK POSITION THE SCORE IS ASKED
-- FROM, which the harvest resume pins to the dispatch position -- see
-- M.score_place_tile.
local defend_hold_tile   -- forward decl (defined with the defend evaluators
                         -- below; the follow-through bid uses the same
                         -- "where do I stand while I wait" answer defend does)
local function eval_place_pill_strategic(state, world, info, tmx, tmy, boat, ammo, only)
  if not C.STRATEGIC_PLACE_ENABLED then return nil end

  -- ── Follow-through: a HARVEST trip is live ──────────────────────────────
  -- The builder is out chopping the very tile we chose for the pill, so the
  -- `actionable` gate below (man must be IN the tank) is about to return nil
  -- and delete this whole row from the pool for the length of the walk. That
  -- is what let refuel drag the tank 22 tiles away mid-harvest and collapse the
  -- resume re-score (20260902_030233 bot2 t=36981-37445; see
  -- PLACE_FOLLOW_THROUGH_COST in constants.lua).
  --
  -- So keep bidding, at a flat hold price, for one thing only: STAY IN RANGE.
  -- Deliberately ABOVE the seek-trees redirect: during a harvest trip the trip
  -- IS the tree plan (LGM_GATHER_TREE 4 == PILL_PLACE_TREE_COST 4 -- the chop
  -- funds the placement exactly), so a low tree count must not send the tank
  -- off to a different forest and abandon the man mid-walk.
  --
  -- Interplay with eval_wait_for_lgm: that evaluator returns nil for the whole
  -- trip (`if state._place_trip then return nil end`) because parking the tank
  -- was the wrong answer for a PLAIN placement -- the pill leaves with the man
  -- and the tank is free. It stays suppressed; this row is now the thing that
  -- holds the tank, and it does it only for harvest trips, only near the trip
  -- tile, and at a price a real fight or a flee still beats.
  if not only and state._place_trip
     and state._place_trip.harvest
     and info.man_status == C.LGM_MOVING
     and (info.carried_pills or 0) >= 1
     and not info.inboat then
    local ft = state._place_trip
    local org = ft.origin
    if org == "strategic" or org == "forced" or org == "guard" then
      local now = state.tick or 0
      local hold_r = C.PLACE_FOLLOW_THROUGH_HOLD_DIST or C.REPAIR_DISPATCH_DIST_BASE or 5
      -- Where to stand. Same question defend answers when it has ARRIVED at a
      -- pill and must wait somewhere sane, so it uses the same answer: the
      -- take_cover pick when one is nearby, else the safest of the tile's 8
      -- neighbours. Then clamped to the hold radius -- a cover tile 9 tiles off
      -- is not a hold -- preferring "stand exactly where you are" when that is
      -- already in range, since not moving is the whole point.
      local hmx, hmy, hsrc = defend_hold_tile(state, info, { mx = ft.mx, my = ft.my })
      if U.mdist(hmx, hmy, ft.mx, ft.my) > hold_r then
        if U.mdist(tmx, tmy, ft.mx, ft.my) <= hold_r then
          hmx, hmy, hsrc = tmx, tmy, "stand_fast"
        else
          hmx, hmy, hsrc = ft.mx, ft.my, "trip_tile"
        end
      end
      -- OFF_BUILD's "not actionable" skip line never prints here (we return
      -- before that gate), so say the same thing in the same grep-able place,
      -- once per trip rather than once per evaluation.
      local fkey = string.format("%d:%d:%d", ft.mx, ft.my, ft.tick or 0)
      if state._ft_log_key ~= fkey then
        state._ft_log_key = fkey
      end
      local cost = C.PLACE_FOLLOW_THROUGH_COST or 25
      return {
        cost = cost,
        goal = { kind = "place_pill_strategic", mx = hmx, my = hmy,
                 wx = U.m2w(hmx), wy = U.m2w(hmy),
                 -- follow_through is read by: goal_selection (pin the cost past
                 -- the phase weight, exempt from the influence multiplier),
                 -- builder.set_mode (skip the dispatch branch -- the man is
                 -- already out) and viz's harvest_trip overlay.
                 follow_through = true,
                 trip_mx = ft.mx, trip_my = ft.my,
                 _spot_score = ft.score_at_dispatch,
                 _spot_sc7   = ft.sc7_at_dispatch },
        desc = BRAIN_POOL_VIZ and string.format(
               "FOLLOW_THROUGH harvest@(%d,%d) score_at_dispatch=%s lgm=out cost{%.0f} hold=(%d,%d)[%s] d=%d/%d age=%dt origin=%s",
               ft.mx, ft.my,
               ft.score_at_dispatch and string.format("%.0f", ft.score_at_dispatch) or "none",
               cost, hmx, hmy, hsrc,
               U.mdist(tmx, tmy, ft.mx, ft.my), hold_r,
               now - (ft.tick or now), org) or "",
        cands = {
          { id = ft.my * 256 + ft.mx, mx = hmx, my = hmy, cost = cost,
            own = "self", hp = 0, stale = 0 },
        },
      }
    end
  end

  -- Seek-trees redirect (BEFORE the LGM-in-tank actionable gate below, so it
  -- persists while the LGM is out harvesting): carrying pills we can't afford to
  -- place (each needs PILL_PLACE_TREE_COST wood) and out of trees -> travel to a
  -- SAFE forest and gather rather than deadlocking on an unpayable build
  -- (20260707_044217 t=127262: carry=6, tr=0, frozen 764 ticks re-issuing
  -- BUILDMODE_PBOX). Pressure scales with pills carried; distance barely dents it.
  -- LGM_DEAD blocks the redirect entirely: nobody can harvest, so driving to
  -- a forest is pure motion. (LGM_MOVING is deliberately still allowed — that
  -- IS the builder out gathering, and the tank should follow along.) Without
  -- this, seek_trees@(144,118) won the pool at cost 34 with a dead builder and
  -- parked the tank inside an angry pill's range for 130 ticks
  -- (20260901_000042_1_loss_b6 bot3 t=16625).
  if not only and (info.carried_pills or 0) >= 1 and not info.inboat
     and info.man_status ~= C.LGM_DEAD
     and (info.trees or 0) < (C.PILL_PLACE_TREE_COST or 4) then
    -- Cache the chosen forest (static terrain) so the expensive map-wide ring
    -- scan runs rarely — not on every pool re-eval (it was spiking pool_cache to
    -- ~9ms). Re-search when the cache is stale or the tile got harvested to grass.
    local now = state.tick or 0
    local sf  = state._seek_forest
    local fresh = sf and (now - (sf.tick or 0)) < (C.SEEK_TREES_CACHE_TICKS or 150)
    local ok    = fresh and sf.mx and U.in_map(sf.mx, sf.my)
                        and U.ttype(sf.mx, sf.my) == C.T_FOREST
    local fx, fy, fdgr, ffell
    if fresh and (ok or not sf.mx) then
      fx, fy = sf.mx, sf.my                       -- reuse (valid forest, or cached "none")
      fdgr, ffell = sf.dgr, sf.fell_back
    else
      -- Arm the cache cooldown BEFORE the scan as a cheap safety net: if
      -- find_safe_forest (Lua) ever overruns the per-tick budget it's killed
      -- MID-LOOP, before the post-scan cache write below. Pre-stamping `now`
      -- bounds re-scans to once per SEEK_TREES_CACHE_TICKS even on overrun, so a
      -- killed scan can't re-fire every tick (the seek_trees t=525 spiral). The
      -- scan itself is now near-instant (bare terrain reads + O(1) Dijkstra
      -- ranking), so this is belt-and-suspenders rather than load-bearing.
      state._seek_forest = { mx = sf and sf.mx or nil, my = sf and sf.my or nil, tick = now }
      fx, fy, fdgr, ffell = find_safe_forest(tmx, tmy)
      state._seek_forest = { mx = fx, my = fy, tick = now, dgr = fdgr, fell_back = ffell }
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
               "seek_trees@(%d,%d) cost=%.0f d=%d carry=%d tr=%d pill_at=%.0f%s",
               fx, fy, cost, d,
               info.carried_pills or 0, info.trees or 0, fdgr or 0,
               ffell and " LEAST-BAD (every forest is under hostile pill fire)"
                     or "") or "",
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
  --
  -- BOTH those overlays MUST declare `default_on = false` in viz.lua. An
  -- unattended -braindebug host (winbolods recording) pushes no _BT_VIZ_*
  -- toggles, so viz.is_on() falls back to the declared default and a toggle
  -- with no default reads as ON — which ran this scan in every recorded game
  -- and in no production game. That is a different brain, and it broke
  -- production-vs-recorded identity from tick 17701 on the seed-1 Oil Rig
  -- 2v2 (found 2026-09-06; tests/prod_recorded_identity_test.py guards it).
  local viz_only = BRAIN_DEBUG_MODE
                   and (vizmod.is_on("pill_best_spots_back") or vizmod.is_on("pill_best_spots_aggro"))
  if only then
    actionable = true   -- re-score: the LGM is back aboard by definition
  elseif not actionable and not viz_only then
    return nil
  end

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
  -- (Whole offensive/panic block skipped on a single-tile re-score: it decides
  -- whether to DROP a pill somewhere else, which is not the question asked.)
  if not only then
  local _db_et = (state.perc and state.perc.enemy_tanks) and #state.perc.enemy_tanks or 0
  -- Defensive build DROPS a carried pill — so it must only fire when we actually
  -- hold one. Without it the goal wins the pool on a cheap score, then dead-ends
  -- at PLACE_PILL_SETMODE "no-dispatch" (carried=0, man=0), stealing a goal cycle
  -- from attack_tank and flip-flopping the aim. Gate on carried_pills > 0.
  local _db_carrying = (info.carried_pills or 0) > 0
  -- Panic build: bank a carried pill NOW, because we are about to lose it.
  -- Driven by the two scores rather than a bare armour threshold -- armour
  -- alone said nothing about whether anything was actually threatening us,
  -- which is how a bot at armour 10 with the nearest enemy 15 tiles away and
  -- not even visible dumped four pills in 200 ticks. See danger.lua.
  local _panic, _panic_thresh, _panic_why = danger.should_panic_build(state, info)
  local _db_skip = ((not _db_carrying) and " -> SKIP(not carrying a pill)")
                or ((not actionable) and " -> SKIP(not actionable: builder not in tank / in boat)")
                or ((_db_et == 0 and not _panic) and " -> SKIP(no visible enemy tank, armour ok)") or ""
  -- `actionable` is re-checked HERE, not just at the gate above: in a debug
  -- run `viz_only` lets us fall through that gate to feed the best-spot
  -- overlays, and this block would then return a real cost-1 goal with a DEAD
  -- builder — a goal the live game could never produce and PLACE_PILL_SETMODE
  -- can never dispatch (20260901_000042_1_loss_b6 bot3 t=11075: winner
  -- offensive_build@(133,121) cost=1, tank motionless 55 ticks, dead at
  -- 11191). An overlay flag must never change what the bot DOES.
  if (_db_et > 0 or _panic) and _db_carrying and actionable then
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
        closest_et = nil
      end
    end
    -- The DESPERATE override used to sit here: armour <= DEATH_BUILD_ARMOUR
    -- (30) plus a hit within DEATH_BUILD_HIT_WINDOW (50 ticks) plus a threat in
    -- range, bypassing the support veto and forcing cost 1. Deleted. Its own
    -- comment gave the game away -- "Re-fires each cycle while carried>0, so it
    -- plants BOTH pills" -- which is the four-pills-in-200-ticks complaint
    -- written down as intent. It was a third armour-threshold trigger of
    -- exactly the kind removed with the antitank drop.
    --
    -- The scores deliberately do NOT reproduce it: at armour 30 carrying one
    -- pill, vulnerability is 62.5 -- above the <=50 gate, so panic cannot fire;
    -- with three pills it is 45.83 against a threshold of 13.33, and an enemy
    -- within 8 gives imdanger 15, still no panic. That band -- 75% health and
    -- one recent hit -- is not panic-worthy under this design. Its cover-bypass
    -- purpose is separately absorbed by the narrowed support veto, which no
    -- longer blocks on a pill that fails to cover the enemy.
    --
    -- What this gives up: a bot at armour 30 taking fire with a covering pill
    -- nearby used to plant anyway; now it holds until the scores say otherwise.
    -- If dying with cargo proves the larger cost, revisit this first.

    -- Support veto: a healthy friendly pill already within fire range is the
    -- guard this build would provide — don't drop a second beside it. A pill at
    -- or below SUPPORT_PILL_MIN_HP is nearly dead and doesn't count; build its
    -- replacement. Skipped on panic: banking the pill is the point, and a panic
    -- can fire with no enemy at all, so there is no "this fight" to cover.
    if closest_et and not _panic then
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
        -- support_veto overlay: both 8-tile circles and the pill in their overlap
        closest_et = nil
      end
    end
    -- When BOTH builds fire, the OFFENSIVE one takes priority. Its trigger
    -- requires an enemy within OFF_BUILD_THREAT_RANGE, so whenever both are
    -- true there IS a nearby enemy and the ±45° spot rule is well defined -- a
    -- tactically placed pill beats a merely safe one. Panic stays the fallback
    -- for everything offensive cannot express, including the cases with no
    -- enemy tank at all. Without this rule both produce a place_pill_strategic
    -- goal at cost 1 with different spot rules, and pool ordering decides by
    -- accident.
    local _kind = closest_et and "offensive" or (_panic and "panic" or nil)
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
      if best_cx then
        local path_cost = smart_cost(KIND_NORMAL, tmx, tmy, best_cx, best_cy, 0,
                           info.shells or 32, info.trees or 0, info.mines or 0, info.armour or 40)
        local raw_cost = path_cost + C.STRATEGIC_PLACE_BASE_COST - carry_discount
        local cost = math.max(1, raw_cost * C.STRATEGIC_PLACE_COST_MULT)
        -- Either trigger floors the cost so the plant decisively wins the pool.
        if _kind then cost = 1 end
        local cands = {}
        if _kind == "offensive" then
        else
        end
        -- Build-gate urgency for the panic drop. This return happens BEFORE the
        -- portfolio block below runs, so there is no pf_max_deficit to pass —
        -- deficit 0 is the honest answer here, and the flat emergency term is
        -- what actually buys the raise. Without this the path that most needs a
        -- raised gate was the one path getting urgency 0: in the 9k-tick check,
        -- 24 of 25 PLACE_PILL_GATE lines came from here, all reading urgency{0}
        -- while the tank sat at 10 armour holding 2 pills.
        return {
          cost = cost,
          -- _place_forced: this is the threat-reactive "build while fighting"
          -- drop — exempt from the "place must lose to attack_tank" rule, and
          -- the flag builder.set_mode reads for the PLACE_EMERGENCY_MAX_DIST
          -- dispatch relaxation.
          goal = { kind = "place_pill_strategic", mx = best_cx, my = best_cy,
                   wx = U.m2w(best_cx), wy = U.m2w(best_cy), _place_forced = true },
          desc = BRAIN_POOL_VIZ and string.format("offensive_build@(%d,%d) cost=%.0f thr@(%d,%d) (A*{%.0f}+base{%.0f}-carry{%.0f})*%.2f",
                 best_cx, best_cy, cost, _thr_mx, _thr_my,
                 path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_discount, C.STRATEGIC_PLACE_COST_MULT) or "",
          cands = cands,
        }
      end
    end
  end

  -- Don't interrupt active combat goals (normal strategic only — defensive
  -- build above is allowed to preempt since it's directly threat-reactive).
  if gk == "attack_pill" or gk == "pill_place" then return nil end
  end  -- if not only

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
  -- the deficit role (default 20% back / 45% front / 20% aggressive / 15% util;
  -- read live from PP.TARGET_*, which a per-bot "portfolio=" init arg may replace).
  local pf_counts  = PP.counts(world, state.tick)
  local pf_total   = pf_counts.back + pf_counts.front + pf_counts.aggro
  -- Project the back/front/aggro targets over the pills we actually have to
  -- place (built + THIS tank's carried hoard), not just +1. A category is
  -- buildable while its built count is under its projected target (N < T), so a
  -- hoard can fill back/front/aggro up to the live target ratio instead of being
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
  if not only and util_surplus <= 0 and (info.carried_pills or 0) < 2 then
    state._place_need_cat = "util_reserve"   -- viz hint
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

  -- Per-tile scoring. One closure over the scan context above (pf_counts /
  -- pf_targets / pf_need_cat / unguarded_bases / search center / spike / war
  -- zone / blocked tiles), so the (2R+1)^2 scan and the harvest resume's
  -- single-tile re-score run the SAME thirteen terms. Appends to all_cands and
  -- updates best_*; a bare `return` is the old `return`.
  local function score_cell(cx, cy)
      if U.is_placeable(cx, cy, world) then
        if near_repos_origin(cx, cy) then return end
        if blocked_tiles then
          local _blk_until = blocked_tiles[cy * MAPW + cx]
          if _blk_until and blk_now < _blk_until then return end
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
        if _ctgt and (pf_counts[cell_cat] or 0) >= _ctgt then return end
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
          return
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
          -- FRONT_ZERO_IS_FRONT=false: an unclaimed 0 tile off the line is
          -- beyond our claim and pays the same penalty as enemy ground.
          if influence < 0
             or (influence == 0 and C.FRONT_ZERO_IS_FRONT == false
                 and not PP.near_front(cx, cy, C.FRONT_NEAR_RADIUS_PLACE or 0)) then
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

        -- 7. Distance from tank (pinned to the dispatch value on a harvest
        --    re-score: the tank moved on by design, so live distance would
        --    say nothing about the tile)
        sc7 = (only and only.sc7) or (-U.mdist(tmx, tmy, cx, cy) * 0.5)
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
  end  -- score_cell

  if only then
    -- Harvest re-score: one tile, fresh context. nil = the tile no longer
    -- qualifies at all (category drift / surplus / blocked / occupied).
    score_cell(only.mx, only.my)
    if best_mx then return best_score end
    return nil
  end

  for dy = -R, R do
    for dx = -R, R do
      -- TANK-centric: scan around our position
      score_cell(U.mclamp(tmx + dx), U.mclamp(tmy + dy))
    end
  end

  if not best_mx then
    -- No placeable, non-surplus spot anywhere in range. The right move is to
    -- KEEP CARRYING (return nil) until we're somewhere a needed pill belongs —
    -- there is intentionally no fallback that would dump a surplus pill nearby.
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
  -- Armour multiplier: the lower our armour, the cheaper it is to field what we
  -- are carrying. ARMOUR ONLY, not full vulnerability -- cargo is already
  -- discounted twice above (carry_discount by time held, multi_carry_mult by
  -- count), and a vulnerability multiplier would make it three times. Armour is
  -- the piece nothing currently prices: today a bot at 5 and one at 40 pay
  -- identically for the same spot.
  local armour_mult = 0.5 + (math.min(info.armour or 40, C.VULN_ARMOUR_CAP)
                             / C.VULN_ARMOUR_CAP) * 0.5
  cost = math.max(1, cost * armour_mult)
  -- Combat-zone penalty: enemy tank near the chosen spot (flat add, shown as the
  -- tankpen term). Emergency offensive_build is exempt — it returns earlier.
  local tank_pen = place_near_tank_penalty(state, best_mx, best_my)
  cost = cost + tank_pen

  -- Build pool-grid candidate list: winner gets actual cost, others get cost + score delta.
  -- Pool-grid panel data only — wrapped so lua_strip removes it from opt/.
  local cands = {}

  -- viz_only (debug overlay): scan + spot stash done above; emit no goal.
  if not actionable then return nil end

  return {
    cost = cost,
    goal = { kind = "place_pill_strategic", mx = best_mx, my = best_my,
             wx = U.m2w(best_mx), wy = U.m2w(best_my),
             -- Spot score + its tank-distance term, carried on the goal so the
             -- dispatch can stamp them on state._place_trip: the harvest resume
             -- compares a fresh re-score (sc7 pinned to _spot_sc7) against
             -- _spot_score.
             _spot_score = best_score,
             _spot_sc7   = -U.mdist(tmx, tmy, best_mx, best_my) * 0.5 },
    -- Every multiplier that actually shapes `cost` has to appear here — this
    -- desc is what FINAL_SCORES prints, and lastpill/surplus/multi were missing,
    -- so the printed formula did not reproduce the printed number. Order matches
    -- the code above: (path + base + carry_pen - carry) x mult x lastpill, then
    -- x bal x surplus x multi, then + tankpen.
    desc = BRAIN_POOL_VIZ and string.format("(A*{%.0f}+base{%.0f}+carry_pen{%.0f}-carry{%.0f})*mult{%.2f}*lastpill{%.2f}*bal{%.2f}*surplus{%.2f}*multi{%.2f}*armour{%.2f}+tankpen{%.0f} = cost{%.1f} center=%s score=%.0f | balance back %d/%d front %d/%d aggro %d/%d util %d/%d unguarded=%d | %s",
           path_cost, C.STRATEGIC_PLACE_BASE_COST, carry_value_penalty, carry_discount,
           C.STRATEGIC_PLACE_COST_MULT, last_pill_mult, imbalance_mult, surplus_mult,
           multi_carry_mult, armour_mult, tank_pen, cost, search_reason, best_score,
           pf_counts.back, pf_targets.back, pf_counts.front, pf_targets.front,
           pf_counts.aggro, pf_targets.aggro, pf_counts.utility or 0, util_reserve,
           #unguarded_bases, PP.targets_label()) or "",
    cands = cands,
  }
end

-- Harvest resume: re-score ONE tile with the placement scan's context rebuilt
-- fresh (portfolio counts, unguarded bases, strategic center...), sc7 pinned to
-- the value it had at dispatch. Returns the score, or nil when the tile no
-- longer qualifies (category drift under STRICT_NEED, surplus role, blocked,
-- occupied, unplaceable). See init.lua's trip lifecycle.
--
-- tank_mx/tank_my (optional): ask the question FROM a tank position other than
-- the live one. sc7 was never the only travel-shaped term -- the strategic
-- centre chain (nearest friendly base, nearest hostile pill, the offensive
-- spike base) is all measured from the tank too, and at 20260902_030233 bot2
-- t=37445 driving 22 tiles off to refuel mid-harvest is what turned a 311 tile
-- into a 182 one and threw the harvest away. The resume pins these to the
-- position the tank was standing in when it dispatched the builder, so the
-- comparison is "is the TILE still good", not "did I wander off".
function M.score_place_tile(state, world, info, mx, my, sc7_pinned, tank_mx, tank_my)
  local tmx = tank_mx or bit.rshift(info.tankx, 8)
  local tmy = tank_my or bit.rshift(info.tanky, 8)
  return eval_place_pill_strategic(state, world, info, tmx, tmy,
                                   info.inboat, info.shells,
                                   { mx = mx, my = my, sc7 = sc7_pinned or 0 })
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
      end
    end
  end

  -- Spiking pills: hostile/neutral pill within firing range of a friendly
  -- base (denies refuel). Magenta square on the pill, a line to EACH denied
  -- base, and a "SPIKE n=N" label. Data from refresh_spike_detection (goal
  -- scoring shares the same table, so what you see is what the cost used).
  if viz.is_on("spike_pills") and state._spike_pills then
    for _, sp in pairs(state._spike_pills) do
      if viz.line and sp.bases then
        for _, b in ipairs(sp.bases) do
        end
      end
      if viz.text then
        local lbl = (sp.cover or 1) > 1
          and string.format("SPIKE n=%d (shared x%d)", sp.n or 1, sp.cover)
          or  string.format("SPIKE n=%d", sp.n or 1)
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
        -- Colour by SPACING CLASS (builder.spacing_class): the rule ranks
        --   terrain tier > spacing class > distance
        -- so a valid tile shows what class it fell in: clear = yellow,
        -- diagonal = orange, orthogonal = deep orange. Chosen = green,
        -- rejected = red with the reason. Class letter C/D/O on valid tiles.
        local r, g, b = 230, 70, 70                      -- red = rejected
        local lbl = c.rej
        if is_win then r, g, b = 60, 230, 60             -- green = chosen
        elseif c.tier then
          if c.space == 1 then r, g, b = 230, 220, 70     -- clear
          elseif c.space == 2 then r, g, b = 240, 160, 50 -- diagonal
          else r, g, b = 240, 110, 40 end                 -- orthogonal
        end
        if c.space and not c.rej then
          lbl = ({ "C", "D", "O" })[c.space] .. (c.tier == 2 and "2" or "")
        end
        if lbl and viz.text then
        end
      end
      if viz.line then viz.line("panic_build", v.tank_mx + 0.5, v.tank_my + 0.5, v.threat_mx + 0.5, v.threat_my + 0.5, 255, 80, 80, 200) end
      -- Closeness gauge: how near the threat tank is (proxy for panic urgency).
      -- A 5-segment bar above the tank fills as the enemy closes; label shows the
      -- tile distance + percent. Uses the threat position already stashed above.
      local td    = U.mdist(v.tank_mx, v.tank_my, v.threat_mx, v.threat_my)
      local near  = C.PANIC_BUILD_NEAR_TILES or 12
      local close = math.max(0, math.min(1, 1 - td / near))
      local segs  = math.floor(close * 5 + 0.5)
      for i = 0, 4 do
        local on = i < segs
      end
      if viz.text then viz.text("panic_build", v.tank_mx + 0.5, v.tank_my - 2.0, string.format("PANIC BUILD  %dt %d%%", td, math.floor(close * 100)), "center", 255, 80, 80, 255) end
    elseif state._attack_tank_present and state._attack_tank_threat and viz.text then
      local t = state._attack_tank_threat
      -- This marker means "an enemy tank is present but offensive_build produced no
      -- plan this tick" — which is NOT necessarily "no pill". Only say no-pill
      -- when we actually have none; otherwise it's threat-out-of-panic-range (the
      -- offensive_build distance gate) or the panic eval just wasn't the active one.
      local _plabel = t.have_pill
        and string.format("THREAT d=%d (no panic build)", t.dist or -1)
        or "PANIC (no pill)"
    end
  end
end

-- Build-design overlays (VULNERABILITY_AND_BUILDS_PLAN.md "Overlays"). Drawn
-- from the SAME term tables danger.scores produced this tick (state.vuln_terms
-- / state.imdanger_terms), so what is on the map is what the number was made
-- from — never a recompute.
function M.draw_build_viz(viz, state, info)
  if not viz or not viz.is_on or not state or not info then return end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local now = state.tick or 0
  local it  = state.imdanger_terms
  local vt  = state.vuln_terms

  -- build_scores: the 8/15-tile odds rings with every counted tank labelled by
  -- its weight (red enemy / blue ally, excluded ones grey with who engages
  -- them), and the 4/8/9 cover rings with each contributing pill's units.
  if viz.is_on("build_scores") and it then
    local cx, cy = tmx + 0.5, tmy + 0.5
    for _, t in ipairs(it.counted or {}) do
      local r, g, b = 255, 90, 90
      if not t.foe then r, g, b = 90, 150, 255 end
    end
    for _, s in ipairs(it.skipped or {}) do
    end
    for _, p in ipairs(it.cover_pills or {}) do
    end
  end

  -- hud_scores: both scores, every term, the threshold and the panic verdict.
  if viz.is_on("hud_scores") and it and vt then
    local v, i = state.vuln or 0, state.imdanger or 0
    local thr  = danger.panic_threshold(v)
    local _panic, _, why = danger.should_panic_build(state, info)
    local pr, pg, pb = 160, 220, 160
    if _panic then pr, pg, pb = 255, 70, 70 elseif v <= 50 then pr, pg, pb = 255, 200, 80 end
  end

  -- support_veto: the two 8-tile support circles (ours and the enemy's) and
  -- the pill that sat in their overlap and vetoed the offensive build.
  if viz.is_on("support_veto") then
    local bv = state._build_veto_viz
    if bv and (now - bv.tick) <= 2 then
      local r = C.SUPPORT_PILL_RADIUS or 8
    end
  end

  -- influence_tail: cells the tail claimed (not stamped), +-16 around the tank.
  if viz.is_on("influence_tail") then
    for dy = -16, 16 do
      for dx = -16, 16 do
        local x, y = tmx + dx, tmy + dy
        if x >= 0 and x <= 255 and y >= 0 and y <= 255 then
          local t = cpf.influence_tail_at(x, y)
          if t ~= 0 then
            local a = 40 + math.min(15, math.abs(t)) * 8
            if t > 0 then viz.rect("influence_tail", x, y, x + 1, y + 1, 80, 140, 255, a, true)
            else          viz.rect("influence_tail", x, y, x + 1, y + 1, 255, 80, 80, a, true) end
          end
        end
      end
    end
  end

  -- harvest_trip: the pending placement/harvest tile with its stored score, a
  -- line from the tank, and the resume request when one is waiting.
  if viz.is_on("harvest_trip") then
    local trip = state._place_trip
    if trip then
      local r, g, b = 80, 220, 120
      if trip.harvest then r, g, b = 60, 180, 60 end
      -- Follow-through hold tile: where pool 8's FOLLOW_THROUGH row is parking
      -- the tank for the length of the harvest, with the hold radius it was
      -- clamped to. Only drawn while that row is actually the live bid (it is
      -- stamped each time the row is built), so a stale marker can't imply a
      -- hold that isn't happening.
      local h = state._ft_hold
      if h and (now - (h.tick or 0)) <= 2 then
        local hr = C.PLACE_FOLLOW_THROUGH_HOLD_DIST or C.REPAIR_DISPATCH_DIST_BASE or 5
      end
    end
    local rs = state._place_resume
    if rs then
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
  -- hoard can fill back/front/aggro up to the live target ratio instead of being
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
          -- FRONT_ZERO_IS_FRONT=false: an unclaimed 0 tile off the line is
          -- beyond our claim and pays the same penalty as enemy ground.
          if influence < 0
             or (influence == 0 and C.FRONT_ZERO_IS_FRONT == false
                 and not PP.near_front(cx, cy, C.FRONT_NEAR_RADIUS_PLACE or 0)) then
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

-- =========================================================================
-- defend_travel_cost — travel price for the DEFEND evaluation only.
--
-- Defending is not "drive onto the pill", it is "get into the standoff
-- around it": defend_pill_score's own arrival handoff below calls the tank
-- ARRIVED anywhere inside DEFEND_ARRIVE_RADIUS Euclidean tiles. Pricing the
-- trip to the pill's own 8 neighbours (travel_cost_to_pill) charges for the
-- last ten tiles we never have to drive, and because the KIND_NORMAL slate is
-- danger-weighted those are exactly the tiles under enemy pill fire — so the
-- threat gets counted twice (the tier multiplier already prices it) and the
-- bid ignores that a standoff on OUR side of the pill is cheap.
--
-- Incident 20260831_113629 bot3 t=19709: pill #14 at hp 2 priced dij 359 —
-- hostile pills #1 (Chebyshev 4 away) and #4 (7 away) put their 8-tile fire
-- fields over its approach — where ~13 tiles of bare terrain is about 30.
-- Full-health pill #0 won the pool on that inflation alone.
--
-- So: cheapest reachable slate cost over a sampled Chebyshev disc of radius
-- DEFEND_ARRIVE_RADIUS around the pill — the 8 adjacent tiles plus a stride-2
-- grid across the disc. The near side of that disc sits on friendly turf, so
-- the number reflects the standoff we actually drive to. ~130 O(1) lookups
-- per pill row. math.huge if nothing in the disc is reachable, as before.
--
-- The ETA in the late tier (travel * DEFEND_ETA_PER_COST) rides on this too,
-- which is right: reaching the standoff IS arriving, per the same radius.
-- =========================================================================
local function defend_travel_cost(pmx, pmy, boat)
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
  -- Stride-2 grid over the disc. Chebyshev <= R holds by construction, so
  -- every sampled tile is one the arrival handoff would call ARRIVED on.
  -- (0,0) is the pill tile itself: impassable, so it reads back as
  -- unreachable and the 1e29 guard drops it like any other blocked tile.
  local R = C.DEFEND_ARRIVE_RADIUS or 10
  for dy = -R, R, 2 do
    for dx = -R, R, 2 do
      local c = cpf.smart_cost_dij_only(KIND_NORMAL, pmx + dx, pmy + dy, bf)
      if c and c < 1e29 and c < best then best = c end
    end
  end
  return best
end

-- defend_pill_score — the defend formula for one built team pill.
--
--   live tier : cost = max(FLOOR, (base + travel) * threat_mult * wear
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
--   wear  : on the sight/setup tiers only, standing damage (hits taken)
--           discounts the cost by up to DEFEND_WEAR_WEIGHT (~33% at
--           nearly dead) — a chewed-up pill dies to the next few shells,
--           so guard it before an untouched one at the same tier. Left
--           off siege (its savability scaling deliberately runs the
--           other way) and off quiet (the damage curve IS the price).
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
-- Feasibility (siege only): MEASURED damage rate — >= 2 hits inside the
-- last DEFEND_ACTIVE_WINDOW ticks give ticks-per-hit, TTL = hp x rate vs
-- ETA = travel x DEFEND_ETA_PER_COST. Arriving late scales cost up toward
-- DEFEND_FUTILITY_MAX (never INF — a late arrival still degrades into
-- rebuild/capture recovery). No recent hits = shooter paused = no penalty.
--
-- TRAVEL is the STANDOFF price, from defend_travel_cost above: the cheapest
-- reachable tile in the DEFEND_ARRIVE_RADIUS disc around the pill, not the
-- cost of driving up to its edge. That matches the arrival handoff below —
-- inside the radius the trip is already over — and stops the danger-weighted
-- slate charging us for the last ten tiles through enemy pill fire, which the
-- tier multiplier has already priced once.
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

-- How scary is whatever last hit this pill? perception.lua attributes every
-- fresh hp drop to a source class (see its shell_source_class + the
-- attribution pass); this turns that class into the factor that scales the
-- siege tier's DISCOUNT.
--
--   tank  x1.00  aimed fire — a tank is finishing the pill off, unchanged
--   epill x0.50  an enemy pillbox's stray, aimed at some tank near it
--   npill x0.25  a neutral pillbox's stray, same but nobody even chose it
--
-- The factor multiplies the DISCOUNT, not the multiplier: the tiers here are
-- multipliers BELOW 1.0 and smaller means MORE urgent, so scaling them
-- directly by 0.25 would make a stray four times as alarming as a tank. The
-- correct wiring is  m' = 1 - (1 - m) * f  — f=1 leaves the tier exactly as
-- it was, f=0.25 keeps a quarter of its pull, f=0 would erase the tier.
-- Returns (factor, class-or-nil); nil class = never attributed, treat as tank.
local function defend_src_factor(p)
  local src = p and p.last_hit_src
  if src == "npill" then return (C.DEFEND_SRC_NPILL_MULT or 0.25), "npill" end
  if src == "epill" then return (C.DEFEND_SRC_EPILL_MULT or 0.50), "epill" end
  if src == "tank"  then return (C.DEFEND_SRC_TANK_MULT  or 1.00), "tank"  end
  return (C.DEFEND_SRC_TANK_MULT or 1.00), nil
end
local function defend_src_scale(m, f)
  return 1 - (1 - m) * f
end

-- Is an ALLY's repair going to land on this pill inside our own action window?
-- Extracted verbatim from the heat ladder's last `else` branch so the new
-- defend->repair handoff can ask the SAME question the heat gate asks -- two
-- defenders must not both dispatch, and we must not heat a pill an ally is
-- about to patch. Memoised per (pill, tick) on state so pulling it forward
-- doesn't turn one ally sweep into two.
--   * lgmd advert (/info extra): ally LGM DISPATCHED to this tile, hex XXYY
--     EEEE -- real walk-sim ETA at send time, aged by the heartbeat gap.
--     "-" = no dispatch.
--   * goal advert: ally repair_pill CLAIM on this tile; arrival estimated from
--     their broadcast pool cost (cost x DEFEND_ETA_PER_COST). No cost seen yet
--     -> assume imminent.
local function defend_ally_repair_inbound(state, info, p, now)
  local key = (p.my * 256 + p.mx)
  local memo = state._def_ally_rep
  if memo and memo.tick == now and memo.key == key then return memo.blocked end
  local our_window = (C.HEAT_SEQUENCE_TICKS or 150)
                     + (C.HEAT_REPAIR_OVERLAP_MARGIN or 100)
  local blocked = false
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
          blocked = true
          break
        else
        end
      end
    end
  end
  state._def_ally_rep = { tick = now, key = key, blocked = blocked }
  return blocked
end

-- Where to STAND while working a pill we have arrived at (watch, or repair).
-- The take_cover pick when it is inside the arrival radius (so the two agree
-- about where "sane" is), else the safest of the pill's 8 neighbours by
-- threat.at. Returns mx, my, src, threat_of_pick.
-- (forward-declared above eval_place_pill_strategic, which uses it for the
-- follow-through hold tile -- keep the `local` on the declaration, not here.)
function defend_hold_tile(state, info, p)
  local cs = state._cover_spot
  if cs and U.edist(cs.mx, cs.my, p.mx, p.my) <= (C.DEFEND_ARRIVE_RADIUS or 10) then
    return cs.mx, cs.my, "take_cover_pick", nil
  end
  local bd_thr, wmx, wmy = math.huge, nil, nil
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        local nx, ny = U.mclamp(p.mx + dx), U.mclamp(p.my + dy)
        local tt = U.ttype(nx, ny)
        if tt ~= C.T_DEEPSEA and tt ~= C.T_BUILDING and tt ~= C.T_HALFBUILD then
          local th = threat.at(nx, ny) or 0
          -- Deterministic tie-break on tile key: the 3x3 walk is already in a
          -- fixed order, but say so explicitly.
          if th < bd_thr
             or (th == bd_thr and wmx and (ny * 256 + nx) < (wmy * 256 + wmx)) then
            bd_thr = th; wmx, wmy = nx, ny
          end
        end
      end
    end
  end
  if not wmx then return p.mx, p.my, "pill_tile", nil end
  return wmx, wmy, "safest_neighbour", (bd_thr < math.huge) and bd_thr or nil
end

-- One phrasing of "where is the tank actually driving" for the REPAIR rung, so
-- the print2, the candidate row and the winner desc cannot drift apart.
local function repair_dest_str(bd, p)
  if bd.repair_dest == "hold" then
    return string.format("hold=(%d,%d) [%s]", bd.watch_mx or p.mx,
                         bd.watch_my or p.my, tostring(bd.watch_src or "?"))
  end
  return string.format(
    "hold=pill_tile(out of dispatch range: hold (%d,%d) is %dt > %d)",
    bd.watch_mx or p.mx, bd.watch_my or p.my, bd.repair_hold_dist or -1,
    C.REPAIR_DISPATCH_DIST_BASE or 5)
end

-- defend_well_defended — "is this pill already held?", the ONE counting pass
-- behind both the KEEL evaluator's well-defended clamp (defend_pill_score
-- below) and alarm mode's fourth reject condition (M.defend_alarm_status).
-- Factored out so the two can never count differently.
--
-- Allies ALREADY at the pill (fresh /info positions, the bidder excluded)
-- covering the enemies there in team-ratio proportion means this pill doesn't
-- need US too — the whole team swarming one threatened pill strips every other
-- front.  Coverage rounds in the defenders' favour:
--   R = ceil(their_team / our_team); well-defended when
--   foes_near <= allies_near * R  (no visible foes + any ally = held).
-- Both counts are taken within DEFEND_WELL_DEFENDED_RADIUS euclidean tiles of
-- the pill.
--
-- Defenders come from two live signals:
--   (a) VISIBLE allied tanks parked within the radius (info.objects hostility
--       bit — real presence, only when we can see them);
--   (b) allies whose broadcast goal is a defense RESPONSE targeting this pill
--       area (defend_pill / repair_pill claims) — covers the fog case AND bots
--       still en route, which is exactly the everyone-swarms window.
-- max() of the two, since a visible defender usually also claims.
--
-- Determinism: both loops only COUNT, and ally_state.iter_active walks player
-- numbers in ascending order, so no result here depends on hash order.
--
-- Returns (held, allies_near, foes_near, ratio).
local function defend_well_defended(state, info, p, now)
  local wr  = C.DEFEND_WELL_DEFENDED_RADIUS or 10
  local wr2 = wr * wr
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
  local held = (allies_near > 0) and (foes_near <= allies_near * ratio)
  return held, allies_near, foes_near, ratio
end

local function defend_pill_score(state, world, info, p, travel, now, tmx, tmy)
  local bd = { travel = travel }
  local hp  = p.health or 0
  local dmg = p.attack_damage or 0

  -- Damage-source scariness (Part 1). Recorded UNCONDITIONALLY so the src=
  -- chip is visible on every row that has ever been hit, even when the tier
  -- it scales is not the live one.
  local src_f, src_tag = defend_src_factor(p)
  bd.src_f = src_f
  bd.src   = src_tag

  local hit_age   = (p.last_hit_tick and p.last_hit_tick > 0)
                    and (now - p.last_hit_tick) or math.huge
  local sight_age = p._enemy_near_tick and (now - p._enemy_near_tick) or math.huge
  local setup_age = p._lgm_near_tick and (now - p._lgm_near_tick) or math.huge
  -- Recorded so the FINAL_SCORES winner row can print the AGE behind whichever
  -- tier produced m{}: a bare m{0.36} is unattributable, and the per-candidate
  -- row builder already prints setup/sight/cover chips the winner line did not.
  bd.hit_age   = (hit_age   < math.huge) and hit_age   or nil
  bd.sight_age = (sight_age < math.huge) and sight_age or nil
  bd.setup_age = (setup_age < math.huge) and setup_age or nil

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

    -- ── REPAIR handoff (before the heat/watch ladder) ─────────────────
    -- The hole this fills: `block = "taking_damage"` holds for
    -- DEFEND_DMG_FRESH_TICKS (400 = 8 s) after the last hit, so a defender
    -- that has arrived at its own chewed-up pill drops to the WATCH bid and
    -- then sits there with the LGM aboard and a pocketful of trees, watching.
    -- repair_ready (LGM in tank + trees) was computed for a DISPLAY string and
    -- nothing else. If the shelling has actually stopped (hit_age >=
    -- REPAIR_QUIET_TICKS -- the same quiet window the pool-5 UNDER_FIRE term
    -- and the builder's LGM interlock use) then fixing the pill beats both
    -- watching it and heating it, and it is priced between the two.
    -- Guarded by the SAME ally-repair-inbound question the heat gate asks, so
    -- two defenders never both dispatch.
    if hp < (C.PILLS_MAX_HEALTH or 15)
       and hit_age >= (C.REPAIR_QUIET_TICKS or 75)
       and info.man_status == C.LGM_INTANK
       and (info.trees or 0) > 0
       and not defend_ally_repair_inbound(state, info, p, now) then
      bd.repair = true
      bd.repair_quiet = (hit_age < math.huge) and hit_age or nil
      bd.repair_trees = info.trees or 0
      -- Nothing is hitting the pill right now, but a hostile tank standing at
      -- it may start. Same SOFT term pool-5 charges for exactly this evidence
      -- (REPAIR_ENEMY_IN_RANGE_MULT): a price, never a gate -- the repair still
      -- goes, it just stops out-bidding work that matters more while a tank is
      -- sitting on top of us. live_enemy was computed and printed here and
      -- never priced.
      bd.repair_enemy = live_enemy or nil
      bd.repair_mult  = live_enemy and (C.REPAIR_ENEMY_IN_RANGE_MULT or 1.5) or 1.0
      bd.cost = (C.DEFEND_REPAIR_COST or 40) * bd.repair_mult
      -- WHERE the tank drives. A defend win that means "fix this" must not
      -- steer at the pill's own tile -- a live pill is impassable, so the tank
      -- grinds against it while the row claims it is holding at (x,y). Drive to
      -- the hold tile instead, but only if the LGM could actually be dispatched
      -- from there: builder.decide insists on REPAIR_DISPATCH_DIST_BASE tiles
      -- while the tank is calm, and the hold tile is picked for survivability,
      -- not for range. Too far -> keep the pill as the destination (generic
      -- navigation brakes beside it) and SAY so on the row.
      local rmx, rmy, rsrc = defend_hold_tile(state, info, p)
      bd.watch_mx, bd.watch_my, bd.watch_src = rmx, rmy, rsrc
      bd.repair_hold_dist = U.mdist(rmx, rmy, p.mx, p.my)
      bd.repair_dest = (bd.repair_hold_dist <= (C.REPAIR_DISPATCH_DIST_BASE or 5))
                       and "hold" or "pill_tile"
      return bd.cost, bd
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
      -- AFTER our volley finishes is no reason to hold fire. The scan itself
      -- lives in defend_ally_repair_inbound (memoised per pill per tick) so
      -- the REPAIR handoff above asks exactly the same question.
      if defend_ally_repair_inbound(state, info, p, now) then
        block = "ally_repair"
      end
    end
    if block then
      bd.heat_block = block
      -- Is anything actually HAPPENING at this pill? The watch bid is a
      -- response to a live threat, not a standing order to babysit every pill
      -- we happen to be parked near: a quiet pill with no enemy in sight
      -- blocks heat with "no_live_enemy" every single tick, and bidding 30 on
      -- that would park every bot beside its own pillbox forever. Evidence =
      -- fresh damage (siege) or an enemy tank visible at the pill right now —
      -- exactly incident B's situation. ally_repair is excluded: an ally is
      -- already handling it, so camping adds nothing.
      --
      -- SOURCE GATE (Part 1). Fresh damage only counts as watch-worthy
      -- evidence when a TANK is doing it, or when the pill is already chewed
      -- below DEFEND_WATCH_MIN_HP_FRAC of full. A healthy pill catching stray
      -- shells from a pillbox that is really shooting at a tank is not under
      -- siege and must not park us: par2 bot3 t=18080, pill #4 at 13/15 hp
      -- taking neutral-pill strays, watch bid 30 (weighted 93) preempting a
      -- six-pill free capture 50 ticks after it finally won. The live_enemy
      -- path is untouched — a hostile tank standing at the pill right now is
      -- its own evidence regardless of who fired the last shell.
      local dmg_fresh   = hit_age < (C.DEFEND_DMG_FRESH_TICKS or 400)
      local hp_frac     = hp / (C.PILLS_MAX_HEALTH or 15)
      local hp_low      = hp_frac < (C.DEFEND_WATCH_MIN_HP_FRAC or (2/3))
      local src_is_tank = (bd.src == nil or bd.src == "tank")
      local dmg_watch_ok = dmg_fresh and (src_is_tank or hp_low)
      if dmg_fresh and not dmg_watch_ok then
        bd.watch_src_denied = string.format("%s strays, hp %d/%d",
          tostring(bd.src), hp, C.PILLS_MAX_HEALTH or 15)
      end
      local watch_ok = (dmg_watch_ok or live_enemy)
                       and block ~= "ally_repair"
      if not watch_ok then
        bd.cost = math.huge
        return math.huge, bd
      end
      -- WATCH bid (was: no bid at all).  Arrived at the pill, heat is not
      -- available -- but "no bid" meant defend silently handed the tick to
      -- whatever else happened to be cheap, which in
      -- 20260901_000042_1_loss_b6 bot3 t=16625 was seek_trees at cost 34,
      -- parking the tank one tile from a forest inside an angry pill's range
      -- while our pill #13 was shot from 15 down to 1.  Instead bid a flat
      -- DEFEND_WATCH_COST to STAND somewhere sane near the pill: the
      -- take_cover pick when it is inside the arrival radius (so the two
      -- agree about where "sane" is), else the safest of the pill's 8
      -- neighbours by threat.at.
      --
      -- Priced (30) to LOSE to a real attack_tank (~20-30 engage band) and
      -- to take_cover's haul floor (10), and to BEAT seek_trees (40 - carry*6
      -- = 34 at carry 1).  It is a holding action, not a mission.
      local wmx, wmy, wsrc, wthr = defend_hold_tile(state, info, p)
      bd.watch_threat = wthr
      bd.watch = true
      bd.watch_mx, bd.watch_my, bd.watch_src = wmx, wmy, wsrc
      bd.cost = C.DEFEND_WATCH_COST or 30
      return bd.cost, bd
    end
    bd.heat = true
    bd.cost = C.DEFEND_HEAT_COST or 200
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
    bd.siege_m_raw = 1 - (1 - (C.DEFEND_SIEGE_MULT or 0.30))
                     * (hp / (C.PILLS_MAX_HEALTH or 15))
    -- ...then scaled by WHO is shooting (Part 1). defend_src_scale shrinks the
    -- discount, never the multiplier: pillbox strays leave a shallower tier,
    -- aimed tank fire leaves it exactly as it was.
    bd.siege_m = defend_src_scale(bd.siege_m_raw, src_f)
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

    -- Late arrival, priced off the MEASURED damage rate. The old TTL was
    -- hp x 80 ticks, a constant: it read a pill being shelled every 12
    -- ticks as "in time" from 19 tiles away, and a pill the shooter had
    -- stopped hitting 60 ticks ago as "futile x3" (full2 bot2, pill 5).
    -- Now: hits inside the last DEFEND_ACTIVE_WINDOW ticks (~1 s) say the
    -- enemy is killing it RIGHT NOW -- two or more give a rate, TTL =
    -- hp x rate, and ETA beyond that is genuinely late. One hit in the
    -- window (rate unknown) or none (shooter paused; TTL not shrinking)
    -- -> no late penalty: the siege tier still pulls, arrival is not
    -- futile.
    local feas = 1.0
    bd.eta = (travel < math.huge) and travel * (C.DEFEND_ETA_PER_COST or 6) or nil
    do
      local log = p.hit_log
      local win = C.DEFEND_ACTIVE_WINDOW or 50
      local n_recent, first_t, last_t = 0, nil, nil
      if log then
        for i = #log, 1, -1 do
          local t = log[i]
          if now - t <= win then
            n_recent = n_recent + 1
            first_t  = t
            last_t   = last_t or t
          else
            break
          end
        end
      end
      bd.hits_win = n_recent
      if siege and bd.eta and n_recent >= 2 then
        bd.rate = (last_t - first_t) / (n_recent - 1)   -- ticks per hit, measured
        if bd.rate < 1 then bd.rate = 1 end
        bd.ttl  = hp * bd.rate
        bd.active = true
        if bd.ttl > 0 and bd.eta > bd.ttl then
          feas = math.min(bd.eta / bd.ttl, C.DEFEND_FUTILITY_MAX or 3.0)
        end
      end
    end
    bd.feas = feas

    -- Wear: standing damage makes a pill worth guarding harder. Straight
    -- linear discount on the sight/setup tiers by hits taken — a chewed-up
    -- pill dies to the next few shells, so among equal-tier candidates the
    -- worn one should pull first. Applied ONLY to sight/setup: the siege
    -- tier prices damage the other way on purpose (savability — an
    -- almost-dead pill under fire is mostly lost), and the quiet tier's
    -- DEFEND_QUIET_DMG_COST curve already prices wear directly.
    -- Incident: full-hp #0 outbid hp-2 #14, both sight-tier, at session
    -- 20260831_113629 bot3 t=19709.
    if bd.tier == "sight" or bd.tier == "setup" then
      local hits = (C.PILLS_MAX_HEALTH or 15) - hp
      if hits < 0 then hits = 0 end
      bd.wear_hits = hits
      bd.wear = 1 - (C.DEFEND_WEAR_WEIGHT or 0.3333)
                    * (hits / (C.PILLS_MAX_HEALTH or 15))
    end

    cost = ((C.DEFEND_PILL_BASE_COST or 250) + travel) * mult
           * (bd.wear or 1.0) * feas * ready
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
    local held, allies_near, foes_near, ratio =
          defend_well_defended(state, info, p, now)
    if held then
      bd.welldef = { ours = allies_near, foes = foes_near, ratio = ratio }
      cost = wd_cost
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

-- =========================================================================
-- ALARM MODE (C.DEFEND_ALARM_MODE, 2026-09-06) — Andrew's redesign of the
-- MAIN defend_pill evaluator.  Everything above (the siege/setup/sight/quiet
-- tier ladder, coverage, lateness, readiness, the well-defended clamp, the
-- two-tier floor, the flat-cost tiebreaker AND the ARRIVED heat / watch /
-- repair rungs) is the KEEL path and is untouched: with DEFEND_ALARM_MODE
-- false not one line below here runs, no new terrain is read and no new state
-- is written, so `preset=keel` reproduces today's evaluator exactly.
--
-- THE RULE (Andrew, verbatim intent).  defend_pill stays VISIBLE in the goal
-- pool but is REJECTED, with the failing condition as the reject reason,
-- unless an ALARM holds for that pill.  The alarm is on ONLY while ALL of:
--
--   1. an enemy is within DEFEND_ALARM_ENEMY_TILES (11) of the pill.  A LIVE
--      sighting: a hostile tank the engine is showing us THIS TICK
--      (state.perc.enemy_tanks, which holds real sightings only — ghosts are
--      a separate list and deliberately do not count).  Not a remembered
--      sighting, not a windowed one.  The tick nothing hostile is visible in
--      the ring, the alarm is off.
--   2. AND one of
--      (a) the pill took damage FROM AN ENEMY within DEFEND_ALARM_WINDOW_TICKS
--          (250 = 5 s at 50 brain ticks/s).  Counted from perception's
--          per-pill enemy-hit ring (_alarm_hits): every hit its attribution
--          pass classified `tank` (hostile tank) or `epill` (hostile pillbox).
--          A `npill` stray is a NEUTRAL pillbox and is not an enemy.
--      (b) OR an enemy pill or a wall was built inside
--          DEFEND_ALARM_BUILD_RADIUS (4) of the pill inside the same window,
--          with a HOSTILE LGM seen inside that same stamp in the window.  The
--          OBJECT_HOSTILE bit is the whole attribution — Andrew, 2026-09-06:
--          "If we know the LGM is an enemy, that's sufficient."  No ally
--          build-claim lookup gates it (the debug line still prints what the
--          allies were claiming, for the reader).
--          The watch list and the stamp are perception.lua's (see its
--          ALARM_STAMP block): the offsets are computed ONCE at module load,
--          never per tick.
--   3. AND we are MORE than DEFEND_ALARM_MIN_DIST (9) tiles from the pill.
--   4. AND the pill is not already WELL DEFENDED (C.DEFEND_ALARM_WELL_DEFENDED,
--      2026-09-07).  Andrew: "Let's just reject the ones that are
--      well_defended / No need to raise that alarm if it's well_defended."
--      The KEEL evaluator's own well-defended gate, asked through the shared
--      defend_well_defended() above: R = ceil(their_team/our_team), held when
--      foes_near <= allies_near * R, both counted within
--      DEFEND_WELL_DEFENDED_RADIUS (10) euclidean tiles of the pill, this bot
--      excluded, allies = allied tanks at the pill PLUS allies whose broadcast
--      goal is a defend/repair response aimed there.  A held pill is somebody
--      else's alarm, so the row is rejected and the per-pill loop moves on to
--      the next alarmed pill.
--
-- No hysteresis and no commitment may keep an alarm alive: the moment a
-- condition stops holding the row goes back to REJECTED, and a bot standing on
-- that defend goal DROPS it — init.lua's goal-validity hook re-asks
-- M.defend_alarm_status every tick and invalidates the goal, which forces the
-- immediate replan.  (The pool's normal hysteresis for switching TO the goal
-- is untouched: that is about picking it, not about holding it past its
-- precondition.)
--
-- COST while the alarm holds:
--     max(DEFEND_ALARM_MIN_COST,
--         DEFEND_ALARM_BASE_COST - hits x DEFEND_ALARM_HIT_DISCOUNT)
--   + the Dijkstra cost along the path to the pill, STOPPED once the path is
--     within DEFEND_ALARM_DIJ_STOP_TILES (9) tiles of it.  No path at all ->
--     the row rejects `no_path`.
--
-- WHAT NO LONGER HAPPENS.  Alarm mode has no ARRIVED branch, so defend_pill
-- never sets goal.heat, goal.watch or goal.repair.  The heat-up action, the
-- watch hold and the defend->repair handoff are all gone with the ladder:
-- builder_pool.repair_feeder's `defend_repair` seed (builder_pool.lua ~1682,
-- which fires on `g.kind == "defend_pill" and g.repair`) can therefore never
-- fire in alarm mode.  Nothing there needed changing — the flag is simply
-- never set — and repair_pill still seeds the man exactly as before.  This is
-- consistent by construction: condition 3 drops the alarm at 9 tiles, so an
-- alarm-mode defender never arrives at the pill in the first place.
-- =========================================================================
local ALARM_R_NO_ENEMY  = "no_enemy_near"
local ALARM_R_NO_TRIG   = "no_trigger"
local ALARM_R_TOO_CLOSE = "too_close"
local ALARM_R_NO_PATH   = "no_path"
local ALARM_R_WELL_DEF  = "well_defended"

-- Enemy-attributed hits on this pill inside the window, from perception's
-- ring.  Returns (count, newest_tick, newest_src).
local function defend_alarm_hits(p, now)
  local win  = C.DEFEND_ALARM_WINDOW_TICKS or 250
  local ring = p._alarm_hits
  local n, newest, src = 0, nil, nil
  if ring then
    for i = 1, #ring do
      local e = ring[i]
      local age = now - (e.t or 0)
      if age >= 0 and age <= win then
        n = n + 1
        if newest == nil or e.t > newest then newest, src = e.t, e.src end
      end
    end
  end
  return n, newest, src
end

-- Condition 1: the nearest hostile tank VISIBLE THIS TICK inside the ring.
-- Euclidean, the metric the rest of this evaluator uses for its own radii.
-- Deterministic: nearest wins, ties break to the lower tank id.
local function defend_alarm_enemy(state, p)
  local r  = C.DEFEND_ALARM_ENEMY_TILES or 11
  local r2 = r * r
  local best, best_d2 = nil, nil
  for _, et in ipairs((state.perc and state.perc.enemy_tanks) or {}) do
    local dx, dy = (et.mx or 0) - p.mx, (et.my or 0) - p.my
    local d2 = dx * dx + dy * dy
    if d2 <= r2 then
      if best_d2 == nil or d2 < best_d2
         or (d2 == best_d2 and (et.id or 0) < (best.id or 0)) then
        best, best_d2 = et, d2
      end
    end
  end
  if not best then return nil, nil end
  return best, math.sqrt(best_d2)
end

-- M.defend_alarm_status — is the alarm ON for this pill, and if not, which
-- condition failed?  ONE implementation, called from two places: this pool's
-- evaluator (at replan cadence) and init.lua's goal-validity hook (every
-- tick), so the row the panel shows and the drop the bot performs can never
-- disagree.
--
-- Returns (on, reason, reason_key, a):
--   reason      full text for the row, e.g. "too_close(6.1<=9)"
--   reason_key  the bare condition name for the reject chip (<= 24 chars once
--               prefixed "alarm_off:", which is the pool grid's chip cap)
--   a           the evidence table every chip and print2 below reads
function M.defend_alarm_status(state, world, info, p, now, tmx, tmy)
  local win = C.DEFEND_ALARM_WINDOW_TICKS or 250
  local a = { win = win }
  -- Distance is computed UP FRONT even though it is condition 3: every row,
  -- including the ones rejected on condition 1 or 2, prints an al_dist chip,
  -- and a chip that appears only on some rejects is a chip the reader cannot
  -- trust.  Computing it early changes no outcome — the reasons are still
  -- returned in condition order.
  a.min_dist = C.DEFEND_ALARM_MIN_DIST or 9
  a.dist = U.edist(tmx or 0, tmy or 0, p.mx, p.my)
  -- Condition 4's evidence, computed here for the same reason the distance is:
  -- the al_wd chip has to be on EVERY row, including the ones that never reach
  -- condition 4, or it is a chip the reader cannot trust.  Counting only; the
  -- verdict is still applied in condition order, below.  With the knob off
  -- nothing is counted at all and the chip says so.
  if C.DEFEND_ALARM_WELL_DEFENDED ~= false then
    a.wd_held, a.wd_allies, a.wd_foes, a.wd_ratio =
      defend_well_defended(state, info, p, now)
  end
  -- Condition 1 — live sighting, this tick.
  local et, ed = defend_alarm_enemy(state, p)
  a.enemy, a.enemy_d = et, ed
  if not et then
    return false, ALARM_R_NO_ENEMY, ALARM_R_NO_ENEMY, a
  end
  -- Condition 2 — a trigger inside the window.
  local nh, hit_t, hit_src = defend_alarm_hits(p, now)
  a.hits, a.hit_tick, a.hit_src = nh, hit_t, hit_src
  a.hit_age = hit_t and (now - hit_t) or nil
  local b = p._alarm_build
  if b and (now - (b.t or 0)) <= win then
    a.build = b
    a.build_age = now - (b.t or 0)
  end
  a.lgm_tick = p._alarm_lgm_tick
  a.lgm_age  = a.lgm_tick and (now - a.lgm_tick) or nil
  a.lgm_fresh = (a.lgm_age ~= nil) and (a.lgm_age <= win) or false
  a.trig_damage = nh > 0
  a.trig_build  = (a.build ~= nil) and a.lgm_fresh
  if not (a.trig_damage or a.trig_build) then
    return false, ALARM_R_NO_TRIG, ALARM_R_NO_TRIG, a
  end
  -- Condition 3 — we must be MORE than MIN_DIST tiles away.
  if a.dist <= a.min_dist then
    return false, string.format("too_close(%.1f<=%d)", a.dist, a.min_dist),
           ALARM_R_TOO_CLOSE, a
  end
  -- Condition 4 — the pill is not ALREADY HELD.  Andrew, 2026-09-07: "Let's
  -- just reject the ones that are well_defended / No need to raise that alarm
  -- if it's well_defended."  Same rule the KEEL evaluator clamps on (the
  -- shared defend_well_defended above): allies already covering the foes at
  -- the pill in team-ratio proportion means the alarm is somebody else's, and
  -- the per-pill loop simply moves on to the next alarmed pill.
  if a.wd_held then
    return false, string.format("well_defended(foes %d <= allies %d x %d)",
                                a.wd_foes or 0, a.wd_allies or 0, a.wd_ratio or 1),
           ALARM_R_WELL_DEF, a
  end
  return true, nil, nil, a
end

-- Cheapest reachable tile inside the stop disc — the FALLBACK for the partial
-- Dijkstra below, used only when the traced path is unusable.  Same stride-2
-- sample of the disc that defend_travel_cost uses for the keel path, so the
-- two numbers are comparable when a bench puts them side by side.
local function defend_alarm_disc_min(p, bf, stop)
  local best, bx, by = math.huge, nil, nil
  for dy = -stop, stop, 2 do
    for dx = -stop, stop, 2 do
      if dx * dx + dy * dy <= stop * stop then
        local c = cpf.smart_cost_dij_only(KIND_NORMAL, p.mx + dx, p.my + dy, bf)
        if c and c < 1e29 and c < best then
          best, bx, by = c, p.mx + dx, p.my + dy
        end
      end
    end
  end
  if bx == nil then return nil end
  return best, bx, by
end

-- The PARTIAL Dijkstra: walk the traced path from the tank toward the pill,
-- summing per-tile costs, and STOP at the first tile whose distance to the
-- pill is <= DEFEND_ALARM_DIJ_STOP_TILES.  Because a Dijkstra slate's cost IS
-- the running sum from the source, "the sum up to tile T" is exactly the
-- slate's cost AT T — no per-step arithmetic is needed or possible.
--
-- The pill's own tile is impassable (a live pillbox blocks the cost surface),
-- so the trace destination is its cheapest reachable neighbour: that is "the
-- path to the pill" as far as any path can be, and the stop rule cuts it 9
-- tiles out anyway.
--
-- The C trace caps at 64 waypoints (braincore.c l_cpf_dijkstra_trace_path*)
-- and keeps the SOURCE end, so a very long path can be truncated before it
-- ever enters the stop disc.  That case falls back to the cheapest reachable
-- tile in the same disc (the stride-2 sample defend_travel_cost uses) and SAYS
-- so in the chip, rather than silently pricing the whole trip.
--
-- Returns (cost, method, stop_i, nsteps, smx, smy) or nil when nothing in the
-- disc is reachable.
local function defend_alarm_travel(p, boat)
  local bf    = boat and 1 or 0
  local stop  = C.DEFEND_ALARM_DIJ_STOP_TILES or 9
  local stop2 = stop * stop
  -- Trace destination: cheapest reachable neighbour of the pill.
  local nb_c, nb_x, nb_y = math.huge, nil, nil
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        local c = cpf.smart_cost_dij_only(KIND_NORMAL, p.mx + dx, p.my + dy, bf)
        if c and c < nb_c then nb_c, nb_x, nb_y = c, p.mx + dx, p.my + dy end
      end
    end
  end
  if nb_x and nb_c < 1e29 then
    local path = cpf.dijkstra_trace_path_by_kind(KIND_NORMAL, nb_x, nb_y)
    if path and #path >= 2 then
      local nsteps = math.floor(#path / 2)
      for i = 1, nsteps do
        local tx, ty = path[2 * i - 1], path[2 * i]
        local dx, dy = tx - p.mx, ty - p.my
        if dx * dx + dy * dy <= stop2 then
          local c = cpf.smart_cost_dij_only(KIND_NORMAL, tx, ty, bf)
          if c and c < 1e29 then
            return c, "path", i, nsteps, tx, ty
          end
          -- The traced tile is on the slate by construction, so this is a
          -- boat/land-node mismatch; fall through to the disc sample.
          break
        end
      end
      -- Path never entered the disc: the 64-waypoint cap cut it short.
      local c, cx, cy = defend_alarm_disc_min(p, bf, stop)
      if c then return c, "trunc_disc", nil, nsteps, cx, cy end
      return nil
    end
  end
  local c, cx, cy = defend_alarm_disc_min(p, bf, stop)
  if c then return c, "disc", nil, nil, cx, cy end
  return nil
end

-- Selection-layer preview for the alarm rows.  Same arithmetic as
-- eval_defend_pill's own sel_preview closure (phase weight with its distance
-- falloff, then the influence multiplier), lifted to a function because alarm
-- mode is a separate evaluator; a clicked alarm row must reconcile with the
-- WINNERS view exactly as a keel row does.
local function defend_alarm_sel_preview(state, tmx, tmy, mx, my, cost)
  if not cost or cost >= math.huge then return "" end
  local pw = (C.PHASE_WEIGHTS and C.PHASE_WEIGHTS[state.phase]
              and C.PHASE_WEIGHTS[state.phase].defend_pill) or 1.0
  local _fot = C.PHASE_WEIGHT_DIST_FALLOFF
  local pw_falloff = (type(_fot) == "table" and (_fot.defend_pill or _fot.default))
                     or (type(_fot) == "number" and _fot) or 40
  if pw ~= 1.0 then
    local gd = U.mdist(tmx or 0, tmy or 0, mx, my)
    pw = 1.0 + (pw - 1.0) * (1 - math.min(1.0, gd / pw_falloff))
  end
  local im = 1.0
  if state.phase ~= "opening" then
    local inf = cpf.influence_at(mx, my) or 0
    if inf < -50 then im = 2.0 elseif inf > 50 then im = 0.5 end
  end
  return string.format(
    " ->sel{%.0f xph%.2f xinf%.1f} (+switch/commit at selection)",
    cost * pw * im, pw, im)
end

-- The EVIDENCE chips, shared by the alarmed row, every rejected row and the
-- winner desc so the three can never drift apart.
local function defend_alarm_evidence(a)
  local en = a.enemy
    and string.format(" al_enemy{#%s @%.1ft}", tostring(a.enemy.id or "?"),
                      a.enemy_d or 0)
    or  string.format(" al_enemy{none in %dt}", C.DEFEND_ALARM_ENEMY_TILES or 11)
  local tr
  if a.trig_build then
    tr = string.format(" al_trig{bld (%d,%d) %s %dt}",
                       a.build.mx, a.build.my, a.build.what, a.lgm_age or -1)
  elseif a.trig_damage then
    tr = string.format(" al_trig{dmg %dx %s %dt}", a.hits or 0,
                       tostring(a.hit_src or "?"), a.hit_age or -1)
  else
    tr = string.format(" al_trig{none dmg%d bld%s}", a.hits or 0,
                       a.build and (a.lgm_fresh and "Y" or "nolgm") or "N")
  end
  local wd
  if a.wd_ratio == nil then
    wd = " al_wd{off}"
  else
    wd = string.format(" al_wd{%d/%dx%d%s}", a.wd_foes or 0, a.wd_allies or 0,
                       a.wd_ratio, a.wd_held and " HELD" or "")
  end
  return en .. tr .. string.format(" al_dist{%.1f>%d}", a.dist or 0,
                                   a.min_dist or 9) .. wd
end

-- The per-term "How computed" segments.  Andrew's standing rule: EVERY factor
-- in the score has to appear, so the final number is hand-computable from the
-- panel alone.
local function defend_alarm_detail(a, bd, p, hp)
  local segs = {
    string.format("al_base:DEFEND_ALARM_BASE_COST — the flat price of an alarmed defend trip, before the damage discount"),
    string.format("al_hits:%d enemy-attributed hit(s) on this pill inside the last %d ticks (%d s), from perception's per-pill ring: hits classified `tank` (hostile tank fire) or `epill` (hostile pillbox stray). A `npill` stray is a NEUTRAL pillbox and is not counted.",
                  a.hits or 0, a.win or 250, math.floor((a.win or 250) / 50)),
    string.format("al_disc:DEFEND_ALARM_HIT_DISCOUNT — subtracted once per counted hit: %d - %d x %d = %d",
                  C.DEFEND_ALARM_BASE_COST or 100,
                  a.hits or 0, C.DEFEND_ALARM_HIT_DISCOUNT or 10,
                  (bd and bd.net_raw) or 0),
    string.format("al_net:base - hits x discount, before the floor"),
    string.format("al_floor:DEFEND_ALARM_MIN_COST — the discount can never take the trip below this. %s",
                  (bd and bd.floored) and "IT BIT: the raw net was lower and was clamped up."
                                       or "Not binding here (the raw net is at or above it)."),
    string.format("al_dij:%s", (bd and bd.dij_why) or "-"),
    string.format("al_enemy:condition 1 — a hostile tank the engine is showing us THIS TICK within %d euclidean tiles of the pill. Not a remembered sighting and not a ghost. %s",
                  C.DEFEND_ALARM_ENEMY_TILES or 11,
                  a.enemy and string.format("Nearest is tank #%s at %.1f tiles.",
                                            tostring(a.enemy.id or "?"), a.enemy_d or 0)
                          or "Nothing hostile is visible in the ring, so the alarm is OFF."),
    string.format("al_trig:condition 2 — (a) an enemy hit the pill inside the last %d ticks [%d hit(s)%s], OR (b) a wall/hostile pill went up inside %d tiles of it in that window WITH a hostile LGM seen in the same stamp [%s]. The OBJECT_HOSTILE bit on the LGM is the whole attribution; no ally build claim is consulted.",
                  a.win or 250, a.hits or 0,
                  a.hit_age and string.format(", newest %dt ago (%s)", a.hit_age,
                                              tostring(a.hit_src or "?")) or "",
                  C.DEFEND_ALARM_BUILD_RADIUS or 4,
                  a.build and string.format("build (%d,%d) %s %dt ago, hostile LGM %s",
                                            a.build.mx, a.build.my, a.build.what,
                                            a.build_age or -1,
                                            a.lgm_fresh
                                              and string.format("seen %dt ago", a.lgm_age or -1)
                                              or "NOT seen in the window")
                          or "no build seen in the stamp"),
    string.format("al_dist:condition 3 — we must be MORE than DEFEND_ALARM_MIN_DIST (%d) tiles from the pill. We are %.1f. Inside that radius there is nothing left to travel to, so the alarm drops and the goal with it.",
                  a.min_dist or 9, a.dist or 0),
    string.format("al_wd:condition 4 — the pill must not already be HELD. Allies within DEFEND_WELL_DEFENDED_RADIUS (%d tiles) of it — visible allied tanks, or allies whose broadcast goal is a defend_pill/repair_pill response aimed here, whichever count is larger, this bot excluded — versus enemy tanks in the same radius. R = ceil(their_team/our_team) rounds the coverage requirement in the defenders' favour, so it is held when foes <= allies x R (and no visible foes plus any ally is held). %s",
                  C.DEFEND_WELL_DEFENDED_RADIUS or 10,
                  (a.wd_ratio == nil)
                    and "DEFEND_ALARM_WELL_DEFENDED is off — nothing is counted and this condition never rejects."
                    or string.format("Here: foes %d, allies %d, R %d -> %s.",
                                     a.wd_foes or 0, a.wd_allies or 0, a.wd_ratio,
                                     a.wd_held
                                       and "HELD, so no alarm is raised and the evaluator moves on to the next pill"
                                       or "not held, so this condition passes")),
    string.format("state:pill hp=%d/%d, hits taken %d; watch=%s",
                  hp, C.PILLS_MAX_HEALTH or 15,
                  math.max(0, (C.PILLS_MAX_HEALTH or 15) - hp),
                  p._alarm_watch_tick and "on the alarm watch list" or "not watched"),
  }
  return table.concat(segs, "|")
end

-- eval_defend_pill_alarm — the ALARM MODE pool.  Every built team pill still
-- gets a row (so the panel never goes blank and a reject is visible with its
-- reason); only alarmed pills bid.
local function eval_defend_pill_alarm(state, world, info, tmx, tmy, boat, ammo)
  local now  = state.tick or 0
  local rows = BRAIN_POOL_VIZ and {} or nil
  local best, best_id, best_cost, best_bd, best_a = nil, nil, math.huge, nil, nil
  -- BY ID, not pairs order: the pool must be identical on every machine and
  -- every replay, and a tie between two equally-priced pills must not depend
  -- on the hash order of world.pills.
  local ids = {}
  for id in pairs(world.pills) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local p = world.pills[id]
    if p and (p.owner == "friendly" or p.owner == "allied")
       and not (p.in_tank or p.carrier or p._synth_carry) then
      local hp = p.health or 0
      local reject_key, reject_txt, a = nil, nil, nil
      if hp == 0 then
        reject_key, reject_txt = "dead", "dead"
        a = { dist = U.edist(tmx or 0, tmy or 0, p.mx, p.my),
              min_dist = C.DEFEND_ALARM_MIN_DIST or 9,
              win = C.DEFEND_ALARM_WINDOW_TICKS or 250, hits = 0 }
      else
        local on, why, key
        on, why, key, a = M.defend_alarm_status(state, world, info, p, now, tmx, tmy)
        if not on then
          reject_key = "alarm_off:" .. key
          reject_txt = "alarm_off:" .. why
        end
      end
      local cost, bd = nil, nil
      if not reject_key then
        bd = {}
        local base = C.DEFEND_ALARM_BASE_COST or 100
        local disc = C.DEFEND_ALARM_HIT_DISCOUNT or 10
        local flr  = C.DEFEND_ALARM_MIN_COST or 50
        bd.net_raw = base - (a.hits or 0) * disc
        bd.net = bd.net_raw
        if bd.net < flr then bd.net = flr; bd.floored = true end
        local tcost, method, stop_i, nsteps, smx, smy =
              defend_alarm_travel(p, boat)
        if tcost == nil then
          reject_key, reject_txt = "alarm_off:" .. ALARM_R_NO_PATH,
                                   "alarm_off:" .. ALARM_R_NO_PATH
          bd = nil
        else
          bd.dij, bd.dij_method = tcost, method
          bd.dij_stop_i, bd.dij_steps = stop_i, nsteps
          bd.dij_mx, bd.dij_my = smx, smy
          if method == "path" then
            bd.dij_chip = string.format("%.0f stop%dt %d/%d", tcost,
                                        C.DEFEND_ALARM_DIJ_STOP_TILES or 9,
                                        stop_i or 0, nsteps or 0)
            -- The long "How computed" prose is for the panel only; building it
            -- on a production tick that will never render a row is pure waste.
            bd.dij_why = rows and string.format(
              "the Dijkstra path to the pill, priced only as far as the stop ring: the slate's running cost AT the first traced tile (%d,%d) whose distance to the pill is <= DEFEND_ALARM_DIJ_STOP_TILES (%d). That is step %d of the %d-step trace; the remaining %d step(s) are NOT charged. A slate cost IS the running sum from the tank, so no per-step arithmetic is possible or needed.",
              smx or 0, smy or 0, C.DEFEND_ALARM_DIJ_STOP_TILES or 9,
              stop_i or 0, nsteps or 0, math.max(0, (nsteps or 0) - (stop_i or 0)))
          else
            bd.dij_chip = string.format("%.0f disc%dt", tcost,
                                        C.DEFEND_ALARM_DIJ_STOP_TILES or 9)
            bd.dij_why = rows and string.format(
              "FALLBACK (%s): the cheapest reachable tile in the %d-tile stop disc around the pill (stride-2 sample, at (%d,%d)). Used because %s.",
              method, C.DEFEND_ALARM_DIJ_STOP_TILES or 9, smx or 0, smy or 0,
              (method == "trunc_disc")
                and string.format("the C trace caps at 64 waypoints and this %d-step path was cut off before it reached the stop ring", nsteps or 0)
                or "no Dijkstra slate has traced a path to the pill's neighbours yet")
          end
          cost = bd.net + tcost
          bd.cost = cost
          if cost < best_cost then
            best, best_id, best_cost, best_bd, best_a = p, id, cost, bd, a
          end
        end
      end
      -- Every REJECTED row says so out loud, once per replan per pill, with
      -- the failing condition and the numbers behind all three.  The
      -- goal-validity hook in init.lua prints its own DEFEND_ALARM_OFF when it
      -- DROPS a live goal; this one is the pool's side of the same statement,
      -- and it is what makes "the row was rejected for reason X" checkable
      -- from the log instead of only from the panel.
      if reject_key and hp > 0 then
      end
      if rows then
        local formula
        if reject_key then
          formula = string.format("REJECT %s%s||%s", reject_txt,
                                  (hp > 0) and defend_alarm_evidence(a) or "",
                                  (hp > 0) and defend_alarm_detail(a, nil, p, hp)
                                    or "state:hp=0 — rebuild/capture territory, not defend")
        else
          formula = string.format(
            "ALARM al_base{%d} - al_hits{%d}*al_disc{%d} = al_net{%.0f}%s + al_dij{%s} = %.0f%s%s||%s",
            C.DEFEND_ALARM_BASE_COST or 100, a.hits or 0,
            C.DEFEND_ALARM_HIT_DISCOUNT or 10, bd.net_raw,
            bd.floored and string.format(" al_floor{%d}", C.DEFEND_ALARM_MIN_COST or 50) or "",
            bd.dij_chip, cost,
            defend_alarm_evidence(a),
            defend_alarm_sel_preview(state, tmx, tmy, p.mx, p.my, cost),
            defend_alarm_detail(a, bd, p, hp))
        end
        rows[#rows + 1] = {
          id = id, mx = p.mx, my = p.my,
          cost = reject_key and 1e30 or cost,
          formula = formula,
          stale = 0,
          -- The chip is capped at 24 chars by the renderer, which every
          -- reason key here fits inside once prefixed.
          reject = reject_key,
          reject_remaining = 0,
          tier = reject_key and ((hp == 0) and "dead" or "alarm_off") or "alarm",
        }
      end
    end
  end

  state.defend_breakdown = rows and { tick = now, rows = rows } or nil
  if not best then return nil end

  local best_desc = ""
  if BRAIN_POOL_VIZ then
    best_desc = string.format(
      "defend#%s@(%d,%d) ALARM al_base{%d} - al_hits{%d}*al_disc{%d} = al_net{%.0f}%s + al_dij{%s} = %.0f%s",
      tostring(best_id), best.mx, best.my,
      C.DEFEND_ALARM_BASE_COST or 100, best_a.hits or 0,
      C.DEFEND_ALARM_HIT_DISCOUNT or 10, best_bd.net_raw,
      best_bd.floored and string.format(" al_floor{%d}", C.DEFEND_ALARM_MIN_COST or 50) or "",
      best_bd.dij_chip, best_cost, defend_alarm_evidence(best_a))
  end
  -- Alarm mode is a TRAVEL-AND-FIGHT goal and nothing else: no heat, no watch,
  -- no repair handoff (see the block comment above).  The destination is the
  -- pill itself; condition 3 drops the alarm at MIN_DIST, so the tank is
  -- released before it ever grinds against the impassable pill tile.
  return {
    cost = best_cost,
    goal = { kind = "defend_pill", mx = best.mx, my = best.my,
             wx = U.m2w(best.mx), wy = U.m2w(best.my),
             target_id = best_id,
             alarm = true,
             pill_mx = best.mx, pill_my = best.my },
    desc = best_desc,
    cands = rows,
  }
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
-- travel is O(1) dij-slate lookups per pill (defend_travel_cost — the
-- standoff-disc price, not the walk-right-up-to-it price).
local function eval_defend_pill(state, world, info, tmx, tmy, boat, ammo)
  -- ALARM MODE (2026-09-06) replaces this whole evaluator.  One branch, taken
  -- before anything below runs, so with the knob false (PRESETS.keel) the keel
  -- path is bit-for-bit what it was.
  if C.DEFEND_ALARM_MODE then
    return eval_defend_pill_alarm(state, world, info, tmx, tmy, boat, ammo)
  end
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
      -- Standoff price, not neighbour price: defending means getting inside
      -- DEFEND_ARRIVE_RADIUS, so charge for the cheapest tile in that disc.
      -- (reposition still uses travel_cost_to_pill — it really does drive
      -- onto the pill's edge.)
      local travel = defend_travel_cost(p.mx, p.my, boat)
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
            if bd.repair then
              formula = string.format(
                "ARRIVED base{%.0f}%s = repair{%.0f} quiet=%st lgm=in_tank trees=%d %s%s"
                .. "||within %d tiles:"
                .. " the pill is damaged, nothing has hit it for %s tick(s) (>= REPAIR_QUIET_TICKS %d),"
                .. " the LGM is aboard with wood and no ally repair is inbound -- so FIX it."
                .. " Priced between watch (%.0f) and heat (%.0f)%s; %s",
                C.DEFEND_REPAIR_COST or 40,
                bd.repair_enemy
                  and string.format("*x%.2f{ENEMY_IN_RANGE: a hostile tank is visible within %d tiles of the pill}",
                                    bd.repair_mult or 1.0, C.HEAT_REQUIRE_ENEMY_RANGE or 10)
                  or "",
                cost,
                bd.repair_quiet and string.format("%d", bd.repair_quiet) or "never",
                bd.repair_trees or 0, repair_dest_str(bd, p),
                sel_preview(p.mx, p.my, cost),
                C.DEFEND_ARRIVE_RADIUS or 10,
                bd.repair_quiet and string.format("%d", bd.repair_quiet) or "never",
                C.REPAIR_QUIET_TICKS or 75,
                C.DEFEND_WATCH_COST or 30, C.DEFEND_HEAT_COST or 200,
                bd.repair_enemy
                  and string.format(", then x%.2f because a hostile tank is standing at the pill (same soft term pool 5 charges)",
                                    bd.repair_mult or 1.0)
                  or "",
                detail)
            elseif bd.heat then
              formula = string.format(
                "ARRIVED heat{%.0f}%s||within %d tiles: travel phase done; bidding the heat-up action only (%d shells to anger the pill); %s",
                cost, sel_preview(p.mx, p.my, cost),
                C.DEFEND_ARRIVE_RADIUS or 10, C.HEAT_PILL_SHOTS or 3, detail)
            elseif bd.watch then
              formula = string.format(
                "ARRIVED watch{%.0f} src=%s%s||within %d tiles: travel phase done; heat blocked by %s,"
                .. " so bid the flat DEFEND_WATCH_COST to HOLD at (%d,%d) [%s]%s —"
                .. " loses to attack_tank and to take_cover's haul floor, beats seek_trees; %s",
                cost, tostring(bd.src or "-"), sel_preview(p.mx, p.my, cost),
                C.DEFEND_ARRIVE_RADIUS or 10, bd.heat_block,
                bd.watch_mx or p.mx, bd.watch_my or p.my, bd.watch_src or "?",
                bd.watch_threat and string.format(" threat.at=%.0f", bd.watch_threat) or "",
                detail)
            elseif bd.watch_src_denied then
              -- Fresh damage, but it is pillbox spray on a still-healthy pill:
              -- the watch bid is refused ON THE SOURCE, which is a different
              -- statement from "nothing is happening here" and gets its own row.
              formula = string.format(
                "ARRIVED no-watch (%s)||within %d tiles: travel phase done and the pill IS taking damage,"
                .. " but perception attributed it to %s fire (src=%s, x%.2f) and the pill is at %d/%d hp,"
                .. " at or above DEFEND_WATCH_MIN_HP_FRAC (%.2f). Pillboxes only ever shoot at TANKS, so"
                .. " these are strays aimed at somebody else — no watch bid, defend yields; %s",
                bd.watch_src_denied, C.DEFEND_ARRIVE_RADIUS or 10,
                tostring(bd.src), tostring(bd.src), bd.src_f or 1.0,
                hp, C.PILLS_MAX_HEALTH or 15,
                C.DEFEND_WATCH_MIN_HP_FRAC or (2/3), detail)
            else
              formula = string.format(
                "ARRIVED no-bid (%s)||within %d tiles: travel phase done; heat blocked by %s -> defend yields to attack_tank / repair_pill / whatever else bids; %s",
                tostring(bd.heat_block), C.DEFEND_ARRIVE_RADIUS or 10,
                tostring(bd.heat_block), detail)
            end
          elseif cost >= 1e29 or travel >= math.huge then
            formula = string.format("base{%.0f}+dij{unreachable} = INF||%s",
              C.DEFEND_PILL_BASE_COST, detail)
          else
            -- Threat-tier chips, only the live ones (strongest wins).
            local u = ""
            if bd.worn then u = " WORN(quiet + damaged: curve-priced)" end
            if bd.siege_m then
              u = u .. string.format(" siege{%.2f}", bd.siege_m)
              -- Where the siege tier came from: the savability multiplier, and
              -- then the damage-source factor applied to its DISCOUNT.
              -- 1-(1-raw)*f reproduces the printed siege{} exactly.
              u = u .. string.format(" src=%s{x%.2f: 1-(1-%.2f)*%.2f}",
                                     tostring(bd.src or "unknown"), bd.src_f or 1.0,
                                     bd.siege_m_raw or bd.siege_m, bd.src_f or 1.0)
            end
            if bd.setup_m then
              u = u .. string.format(" setup{%.2f age=%st/%d}", bd.setup_m,
                    bd.setup_age and string.format("%d", bd.setup_age) or "-",
                    C.DEFEND_SIGHT_FRESH_TICKS or 600)
            end
            if bd.sight_m then
              u = u .. string.format(" sight{%.2f age=%st/%d}", bd.sight_m,
                    bd.sight_age and string.format("%d", bd.sight_age) or "-",
                    C.DEFEND_SIGHT_FRESH_TICKS or 600)
            end
            if bd.cover_m then u = u .. string.format(" cover{%.2fx%d}", bd.cover_m, bd.cover_n) end
            u = u .. string.format(" tier=%s", tostring(bd.tier or "quiet"))
            -- FULL VISIBILITY: every factor that touches the number gets a
            -- chip UNCONDITIONALLY (rdy/late even at 1.00), and each clamp
            -- prints the value it clamps TO, so reading the chain left to
            -- right reproduces the printed total exactly.
            --   live tier : (base+dij)*m [*wear] *rdy *late [floor]
            --                                        [WELLDEF] [+tb]
            --   quiet     : (quiet_dmg+dij)  *rdy   [WELLDEF] [+tb]
            -- wear is the one conditional factor: it prints only on the
            -- sight/setup tiers, where it is the only place it applies.
            local f = bd.quiet_curve and ""
              or string.format("*late{%.2f eta=%.0f ttl=%s rate=%s hits%ds=%d}",
                               bd.feas or 1.0, bd.eta or 0,
                               bd.ttl and string.format("%.0f", bd.ttl) or "-",
                               bd.rate and string.format("%.0f", bd.rate) or "-",
                               math.floor((C.DEFEND_ACTIVE_WINDOW or 50) / 50),
                               bd.hits_win or 0)
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
            -- Wear discount, sight/setup tiers only — absent chip means the
            -- factor was not applied (siege/quiet), so the chain still
            -- multiplies out exactly either way.
            local wr = bd.wear
              and string.format("*wear{%.2f hits=%d}", bd.wear, bd.wear_hits or 0)
              or ""
            local head = bd.quiet_curve
              and string.format("(quiet_dmg{%d hits -> %.0f}+dij{%.0f})",
                                bd.quiet_hits or 0, bd.quiet_curve, travel)
              or string.format("(base{%.0f}+dij{%.0f})*m{%.2f}",
                               C.DEFEND_PILL_BASE_COST, travel, bd.mult)
            formula = string.format(
              "%s%s%s%s%s%s%s%s = %.0f%s||%s",
              head, wr, rd, f, fl, wd, tb, u, cost,
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
                 or (bd.repair and "repair")
                 or (bd.heat and "heat") or (bd.watch and "watch")
                 or (bd.heat_block and "no_heat")
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
  local is_watch = best_bd and best_bd.watch or nil
  local is_repair = best_bd and best_bd.repair or nil
  -- Winner desc carries the SAME full chip chain as the candidate rows:
  -- the WINNERS strip must reproduce the final pool cost from what it
  -- shows alone (head, wear, readiness, lateness, floor, WELLDEF,
  -- tiebreaker).
  local best_desc = ""
  if BRAIN_POOL_VIZ then
    local b = best_bd or {}
    if is_repair then
      best_desc = string.format(
        "defend#%d@(%d,%d) ARRIVED base{%.0f}%s = repair{%.0f} quiet=%st lgm=in_tank trees=%d %s",
        best_id, best.mx, best.my, C.DEFEND_REPAIR_COST or 40,
        b.repair_enemy and string.format("*x%.2f ENEMY_IN_RANGE", b.repair_mult or 1.0) or "",
        best_cost,
        b.repair_quiet and string.format("%d", b.repair_quiet) or "never",
        b.repair_trees or 0, repair_dest_str(b, best))
    elseif is_heat then
      best_desc = string.format("defend#%d@(%d,%d) ARRIVED heat{%.0f}",
                                best_id, best.mx, best.my, best_cost)
    elseif is_watch then
      best_desc = string.format(
        "defend#%d@(%d,%d) ARRIVED watch{%.0f} src=%s hold=(%d,%d) [%s] blocked=%s",
        best_id, best.mx, best.my, best_cost, tostring(b.src or "-"),
        b.watch_mx or best.mx, b.watch_my or best.my,
        b.watch_src or "?", tostring(b.heat_block))
    else
      -- Wear discount, sight/setup tiers only; nothing printed when it was
      -- not applied, so the chain multiplies out exactly either way.
      local wr = b.wear
        and string.format("*wear{%.2f hits=%d}", b.wear, b.wear_hits or 0)
        or ""
      local head = b.quiet_curve
        and string.format("(quiet_dmg{%d hits -> %.0f}+dij{%.0f})",
                          b.quiet_hits or 0, b.quiet_curve, best_travel)
        or string.format("(base{%.0f}+dij{%.0f})*m{%.2f}%s*late{%.2f eta=%.0f ttl=%s rate=%s hits1s=%d}",
                         C.DEFEND_PILL_BASE_COST, best_travel,
                         b.mult or 1.0, wr, b.feas or 1.0, b.eta or 0,
                         b.ttl and string.format("%.0f", b.ttl) or "-",
                         b.rate and string.format("%.0f", b.rate) or "-",
                         b.hits_win or 0)
      -- FULL tier chain, same chips the per-candidate rows print. m{} is a
      -- product of whichever tiers were live times cover; printing only siege
      -- left m{0.36} unattributable on the winner line (20260902_000405 bot2
      -- t=22561: it was the SETUP tier -- an enemy LGM seen 6 tiles from the
      -- pill 91 ticks earlier -- and nothing on the row said so).
      local sr = ""
      if b.siege_m then
        sr = sr .. string.format(" siege{%.2f age=%st/%d} src=%s{x%.2f}",
               b.siege_m,
               b.hit_age and string.format("%d", b.hit_age) or "-",
               C.DEFEND_DMG_FRESH_TICKS or 400,
               tostring(b.src or "unknown"), b.src_f or 1.0)
      end
      if b.setup_m then
        sr = sr .. string.format(" setup{%.2f age=%st/%d}", b.setup_m,
               b.setup_age and string.format("%d", b.setup_age) or "-",
               C.DEFEND_SIGHT_FRESH_TICKS or 600)
      end
      if b.sight_m then
        sr = sr .. string.format(" sight{%.2f age=%st/%d}", b.sight_m,
               b.sight_age and string.format("%d", b.sight_age) or "-",
               C.DEFEND_SIGHT_FRESH_TICKS or 600)
      end
      if b.cover_m then
        sr = sr .. string.format(" cover{%.2fx%d}", b.cover_m, b.cover_n or 0)
      end
      sr = sr .. string.format(" tier=%s", tostring(b.tier or "quiet"))
      best_desc = string.format(
        "defend#%d@(%d,%d) %s*rdy{%.2f}%s%s%s".. sr .." = %.0f hits=%d dmg=%d",
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
  -- A WATCH win drives to the hold tile, not onto the pill: the whole point
  -- is to be somewhere survivable next to it. goal.heat stays nil so
  -- defend_pill_steer declines and generic navigation takes us there and
  -- brakes, which is exactly the behaviour we want.
  -- WATCH drives to its hold tile; REPAIR drives to its hold tile only when
  -- that tile is inside the builder's calm dispatch cap (see the REPAIR rung),
  -- otherwise to the pill so the tank keeps closing until the LGM can go.
  -- Never AT the pill tile for a watch: the point is to stand somewhere
  -- survivable next to it.
  local gmx, gmy = best.mx, best.my
  if is_watch or (is_repair and best_bd.repair_dest == "hold") then
    gmx, gmy = best_bd.watch_mx or best.mx, best_bd.watch_my or best.my
  end
  return {
    cost = best_cost,
    goal = { kind = "defend_pill", mx = gmx, my = gmy,
             wx = U.m2w(gmx), wy = U.m2w(gmy),
             target_id = best_id,
             -- Arrival-phase win: the bid is the heat-up action, not a
             -- drive. Phase-3 heat substates key off this flag.
             heat = is_heat,
             watch = is_watch,
             -- REPAIR handoff: builder.lua's repair dispatch (Priority 0.4)
             -- accepts this exactly as it accepts a repair_pill goal, and
             -- targets pill_mx/pill_my.
             repair = is_repair,
             pill_mx = best.mx, pill_my = best.my },
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
  -- category (back/front/aggressive) that's OVER its live allotment gets a
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
             "REJECT{%s} | balance back %d/%d front %d/%d aggro %d/%d util %d/%d | %s",
             reject, counts.back, targets.back, counts.front, targets.front,
             counts.aggro, targets.aggro, counts.utility or 0, targets.utility or 0,
             PP.targets_label()) or "",
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
           "||list EVERY friendly pill, only BACK eligible; lower=more worth moving. if this BID wins the pool a team vote opens; the shoot is gated on approval. surp comes from balance back %d/%d front %d/%d aggro %d/%d util %d/%d @ %s",
           (repos_locked and "LOCK" or (approved and "APPROVED" or "BID")),
           cost, best_pid, best_pill.mx, best_pill.my, tostring(bc.cat),
           C.PILL_REPOSITION_BASE_COST or 500, -(bc.surp or 0), -(bc.card or 0),
           (bc.base or 0), (bc.act or 0), (bc.tank or 0), (bc.travel or 0),
           counts.back, targets.back, counts.front, targets.front, counts.aggro, targets.aggro,
           counts.utility or 0, targets.utility or 0, PP.targets_label()) or "",
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
  [14] = "take_cover",
  -- 15 = kill_me_wait: the INITIATOR half of the "kill me" hand-off. Like
  -- 12/13/14 it is written straight into pool_cache by its own evaluator and
  -- renders as a WINNERS strip row, not a main-grid cell.
  [15] = "kill_me_wait",
}

-- Reverse map: actual goal.kind → pool index, for looking up cost_cache
-- entries by candidate.  Note pool 1 (refuel) and pool 8 (place_strategic)
-- have different UI labels than their goal.kind values.
-- Pool 10 in the JSON is the WINNERS section, 11 is offensive_build, 12 is
-- wait_for_lgm, 13 is kill_lgm, 14 is take_cover — those render as strips
-- below the main 2x5 grid.
local KIND_TO_POOL = {
  refuel_at_base = 1, defend_pill = 2, capture_base = 3, capture_pill = 4,
  repair_pill = 5, attack_pill = 6, attack_base = 7,
  place_pill_strategic = 8, attack_tank = 9, wait_for_lgm = 12,
  kill_lgm = 13, take_cover = 14, kill_me_wait = 15,
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

-- ── Refuel cost multiplier (per bot) ──────────────────────────────────────
-- A whole-cost multiplier on the "refuel" GOAL_GROUP (refuel_at_base +
-- flee_to_base). Module-level, and each bot has its own lua_State, so it is per
-- bot; init.lua's "refuel=X" BRAIN_INIT_ARG token calls set_refuel_mult.
-- Applied at ONE place: the selection-layer pass in goal_selection, right after
-- the pill-suicider surcharge — NOT per candidate base. Refuel's pool cost is
-- assembled in three different spots (eval_refuel, finalize_partial's pool 1,
-- and the critical-armour flee injection that OVERWRITES pool 1), so the only
-- point that is genuinely single is where the assembled pool is shaped.
local REFUEL_MULT = C.REFUEL_COST_MULT or 1.0
M.refuel_mult_source = "default"
function M.refuel_mult() return REFUEL_MULT end
function M.set_refuel_mult(x, source)
  REFUEL_MULT = (type(x) == "number" and x > 0) and x or REFUEL_MULT
  M.refuel_mult_source = source or "init_arg"
end
function M.refuel_mult_label()
  return string.format("refuelmult{x%.2f (%s)}", REFUEL_MULT, M.refuel_mult_source)
end
-- Same question the display renderers ask for the suicider surcharge: is THIS
-- pool part of the refuel group, and if so what multiplier did selection apply?
local function refuel_mult_for_pool(pname)
  local kind = POOL_NAME_TO_KIND[pname] or pname
  return (GOAL_GROUPS[kind] == "refuel") and REFUEL_MULT or 1.0
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
  -- Placement trip in progress (state._place_trip, set at every placement
  -- dispatch, cleared on the builder's return): the tank is meant to move on,
  -- not park. Without this the carrying case costs 20 and wins the pool on a
  -- harvest trip (the pill is still aboard), and a plain placement still costs
  -- 50 -- cheap enough to re-park the tank next to the spot it just stood off
  -- from. Keyed off the flag, not the goal, because the goal changes while the
  -- builder walks.
  --
  -- This stays suppressed for HARVEST trips too, even though a harvest trip is
  -- exactly the case where the tank SHOULD hold: pool 8's follow-through row
  -- (eval_place_pill_strategic, PLACE_FOLLOW_THROUGH_COST) is what holds it
  -- now, and it holds it in the right PLACE (within the builder's dispatch
  -- range of the trip tile) rather than wherever the tank happened to be, at a
  -- price tuned against the live band. Two rows both saying "park" would just
  -- race each other -- and the wait_for_lgm carrying price (20) is below a real
  -- attack_tank engage, which is precisely the danger the follow-through row is
  -- priced to lose to.
  if state._place_trip then return nil end
  -- Builder-pool side-quest in flight. The rule from the plan's collision
  -- section is that a side-quest must NEVER delay a goal transition -- the
  -- tank carries on and the stranded/rescue machinery is the backstop, which
  -- is what keeps the leash cheap. So no park, with ONE exception: on the
  -- RETURN leg with the tank under fire, the point of bidding is not to wait
  -- longer, it is to move the RENDEZVOUS out of the shell zone
  -- (pick_wait_spot's danger-aware tile) instead of dragging the man home
  -- through it. Outbound / working / quiet: no bid at all.
  local bpj = state._bp_job
  if bpj then
    local _fa = danger.tank_fire_age(state, state.tick or 0)
    local hot = _fa ~= nil and _fa < (C.BUILDER_POOL_UNDER_FIRE_TICKS or 100)
    if not (hot and bpj.phase == "returning") then return nil end
  end
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
-- take_cover — pool 14
-- =========================================================================
-- "Stand somewhere less lethal."  The brain used to have exactly one
-- get-out-of-here reaction (danger.should_panic_build -> drop a pill), and it
-- is unavailable in precisely the situations that need it most: builder dead,
-- nothing carried, or already in a boat.  Two deaths in
-- 20260901_000042_1_loss_b6 bot3 came straight out of that hole (see the
-- take_cover block in constants.lua for the incident notes).
--
-- Design rules, in the order they matter:
--   1. eval_take_cover ALWAYS returns a pool entry.  A goal you cannot see
--      the score of is a goal you cannot tune, so the row exists on every
--      replan; when it genuinely shouldn't be considered the row carries a
--      `reject` reason and the sentinel cost instead of disappearing.
--   2. ONE scoring function (tc_safety) prices every tile, "here" included,
--      so `margin` is literally best - here and the panel arithmetic closes.
--   3. The TRIGGERS choose the COST, never whether the row exists.
-- =========================================================================

-- Sentinel for a rejected pool-14 entry (see constants). Mirrors
-- REPOSITION_REJECT_COST: large enough never to win, small enough that the
-- WINNERS strip still renders the row.
local TAKE_COVER_REJECT_COST = C.TAKE_COVER_REJECT_COST or 1e8

-- =========================================================================
-- LOADED, BUILDER-LESS  (state.loaded_no_lgm)
--
-- ONE definition, recomputed EVERY TICK from engine truth and never latched.
-- init.lua calls this from its per-tick prelude (beside cautious_mode) and
-- stashes the answer on state; every consumer reads the FIELD, so there is
-- exactly one place the question is asked.
--
--   loaded_no_lgm = carried_pills >= C.LOADED_NO_LGM_MIN_PILLS
--                   and ( man_status == LGM_DEAD
--                         or ( man out walking
--                              and his return ETA > C.LOADED_NO_LGM_ETA_TICKS ) )
--
-- The ETA is the SAME walk sim the builder pool prices its trips with
-- (cpf_lgm_travel_ticks, the engine's straight-line lgmReturn walk with a
-- per-axis slide), and it is BUDGETED -- so like the stranded check in
-- goal_selection it is only re-run every LOADED_NO_LGM_ETA_RECHECK_TICKS and
-- cached in between.  A sim that returns -1 (STUCK: the stranded man) is not
-- "unknown", it is "he is not walking home at all", so it counts as longer
-- than any threshold.
--
-- The DEAD half costs nothing and is therefore read fresh every tick: a man
-- who dies flips the state on that tick, not up to 50 ticks later.  Factors
-- are stashed for the HUD / pool breakdowns; nothing reads them for logic.
function M.loaded_no_lgm_eval(state, info, now)
  local f = state._loaded_no_lgm_factors
  if not f then f = {}; state._loaded_no_lgm_factors = f end
  local carry = (info and info.carried_pills) or 0
  local man   = (info and info.man_status) or C.LGM_INTANK
  f.carry     = carry
  f.min_pills = C.LOADED_NO_LGM_MIN_PILLS or 3
  f.man       = man
  f.eta_thresh = C.LOADED_NO_LGM_ETA_TICKS or 1000
  if carry < f.min_pills then
    f.why = "carry"
    f.eta = nil
    return false
  end
  if man == C.LGM_DEAD then
    f.why = "dead"
    f.eta = nil
    return true
  end
  if man ~= C.LGM_MOVING then
    f.why = "intank"
    f.eta = nil
    return false
  end
  -- Man is out walking: how long until he is back? Budgeted sim, so cache.
  local recheck = C.LOADED_NO_LGM_ETA_RECHECK_TICKS or 50
  if not f.eta_tick or (now - f.eta_tick) >= recheck then
    f.eta_tick = now
    local ticks = cpf_lgm_travel_ticks(
      info.man_x or 0, info.man_y or 0, info.tankx or 0, info.tanky or 0,
      0, 0, 2000, 150)
    f.eta_raw = ticks
    -- -1 == STUCK: he is not coming home on this ground at all.
    f.eta = (ticks == -1) and math.huge or ticks
  end
  f.why = (f.eta or 0) > f.eta_thresh and "eta" or "eta_soon"
  return (f.eta or 0) > f.eta_thresh
end

-- Human-readable one-liner for the HUD / breakdown chips.
function M.loaded_no_lgm_label(state)
  local f = state and state._loaded_no_lgm_factors
  if not f then return "loaded_no_lgm{unknown}" end
  local eta
  if f.eta == nil then eta = "n/a"
  elseif f.eta == math.huge then eta = "STUCK"
  else eta = string.format("%.0ft", f.eta) end
  return string.format("loaded_no_lgm{carry %d>=%d, man=%s, eta %s>%d, %s}",
    f.carry or 0, f.min_pills or 0,
    (f.man == C.LGM_DEAD) and "DEAD"
      or (f.man == C.LGM_MOVING) and "OUT" or "INTANK",
    eta, f.eta_thresh or 0, tostring(f.why))
end

-- Haul-protection trigger, SHARED by goal_selection's critical-flee injection
-- and eval_take_cover.  Two copies of this ladder would drift instantly, and
-- then "the flee fired but take_cover didn't" would be unexplainable from the
-- logs. Returns a table; `level` 0 means haul protection is not in play at all.
--   level 1.0  -- carrying with the builder DEAD or OUT: the pills cannot be
--                placed soon, so they are pure liability.
--   level 0.5+ -- carrying a STACK with the builder aboard: we can place them
--                ourselves, so protect a bit less, ramping to full by
--                FLEE_HAUL_FULL_PILLS.
-- `mode_ok` reports whether C.CRITICAL_FLEE_ENABLED selects the carrying mode;
-- the flee injection honours it, take_cover does not (it is not a flee).
local function haul_flee_eval(state, info, tmx, tmy)
  local h = {
    level = 0,
    mode_ok = (C.CRITICAL_FLEE_ENABLED == "no_builder_and_carrying_only"),
    carry = info.carried_pills or 0,
    man = info.man_status,
    pill_at = 0,
  }
  local man = h.man
  if h.carry >= 1 and (man == C.LGM_DEAD or man == C.LGM_MOVING) then
    h.level = 1.0
  elseif h.carry >= 2 and man == C.LGM_INTANK then
    local full = (C.FLEE_HAUL_FULL_PILLS or 4)
    h.level = math.min(1.0, 0.5 + 0.5 * (h.carry - 2) / math.max(1, full - 2))
  end
  if h.level > 0 then
    -- Triggers scale with level: a stronger level reaches further for a
    -- threatening tank and bails at higher armour; only full strength also
    -- bails on mere hostile-pill coverage.
    h.range = (C.FLEE_HAUL_TANK_RANGE or 12) * h.level
    local nh = state.perc and state.perc.nearest_hostile_tank
    h.tank_dist = nh and nh.dist or nil
    h.tank_engaging = (nh ~= nil and (nh.dist or math.huge) <= h.range) or false
    h.pill_at = threat.pill_at(tmx, tmy) or 0
    h.pill_shooting = (h.level >= 1.0 and h.pill_at > 0) or false
    h.arm_thresh = C.ARMOUR_LOW * h.level
    h.arm_low = (info.armour or 0) <= h.arm_thresh
    -- "KILL ME" in progress: the ally we asked is shooting us on purpose, so
    -- the armour coming off is the plan, not a reason to run. Waived only
    -- while state.km.executing holds (claimant within KILL_ME_EXECUTE_TILES,
    -- no enemy tank inside the cancel radius -- see init.lua).
    if h.arm_low and state.km and state.km.executing then
      h.arm_low = false
      h.km_exec = true
    end
    h.triggered = h.tank_engaging or h.pill_shooting or h.arm_low
  end
  return h
end

-- Standable for a TANK: the terrain a take_cover tile is allowed to be.
-- Mirrors the pathfinder's wall/deepsea table (brain_pathfinder.c) plus the
-- one rule smart_cost cannot express for us: a river tile is only a place to
-- STAND when we are already afloat.
local function tc_standable(mx, my, boat)
  if not U.in_map(mx, my) then return false, "offmap" end
  local tt = U.ttype(mx, my)
  if tt == C.T_DEEPSEA then return false, "deepsea" end
  if tt == C.T_BUILDING or tt == C.T_HALFBUILD then return false, "wall" end
  if tt == C.T_RIVER and not boat then return false, "river" end
  -- A pillbox tile is not a parking space: a LIVE one blocks the tank
  -- outright, and a dead one is a pickup (capture_pill's job), not cover.
  -- The pathfinder prices T_PILLBOX as grass-like, so without this the scan
  -- would happily "pick" our own pill's tile and then never arrive.
  if tt == C.T_PILLBOX then return false, "pillbox" end
  return true, nil
end

-- tc_safety(state, world, info, mx, my, tmx, tmy) -> safety, terms
--
-- The ONE tile scorer. Every term below appears in the printed row, and the
-- printed row adds out to `safety` exactly:
--
--   safety = W_COVER*cover - W_EXPO*expo - W_ENEMY*enemy + W_ALLY*ally
--
--   cover -- our own / allied DEPLOYED pills that can shoot back at whoever is
--            shooting us, in danger.cover_weight units (heated pills count
--            IMD_COVER_HEATED_MULT times, exactly as imdanger counts them),
--            capped at IMD_COVER_MAX/IMD_COVER_PER_UNIT.
--   expo  -- threat.pill_at: hostile/neutral pill fire covering the tile.
--   enemy -- sum of danger.odds_weight over VISIBLE enemy tanks.
--   ally  -- same over visible allied tanks.
--
-- `closer` is set when the tile is euclidean-nearer to some visible enemy tank
-- than the tank is right now. Taking cover means moving AWAY; the caller hard-
-- rejects those candidates rather than pricing them.
local function tc_safety(state, world, info, mx, my, tmx, tmy)
  local t = { mx = mx, my = my }

  -- cover: friendly + allied deployed pills, sorted-id iteration so the
  -- float summation order is fixed run to run.
  local units, n_cover, n_hot = 0, 0, 0
  local ids = state._tc_pill_ids or {}
  local wp = world.pills or {}
  for i = 1, #ids do
    local p = wp[ids[i]]
    if p then
      local w = danger.cover_weight(U.edist(mx, my, p.mx, p.my))
      if w > 0 then
        local hot = (p.anger or 0) >= C.HEATED_ANGER
        units = units + w * (hot and C.IMD_COVER_HEATED_MULT or 1)
        n_cover = n_cover + 1
        if hot then n_hot = n_hot + 1 end
      end
    end
  end
  local cap = (C.IMD_COVER_MAX or 30) / (C.IMD_COVER_PER_UNIT or 10)
  if units > cap then units = cap; t.cover_capped = cap end
  t.cover = units
  t.cover_n = n_cover
  t.cover_hot = n_hot

  -- exposure: hostile/neutral pill fire on this tile.
  t.expo = threat.pill_at(mx, my) or 0

  -- odds: visible tanks, ours and theirs, weighted by the same distance
  -- ladder imdanger uses.
  local en, al, n_en, n_al = 0, 0, 0, 0
  local closer = false
  for _, et in ipairs((state.perc and state.perc.enemy_tanks) or {}) do
    local d = U.edist(mx, my, et.mx, et.my)
    local w = danger.odds_weight(d)
    if w > 0 then en = en + w; n_en = n_en + 1 end
    -- "AWAY from the enemy" is a HARD rule, not a weight: a tile that walks
    -- us into a tank we are trying to escape is never cover, however much
    -- pillbox shade it has.
    if d < U.edist(tmx, tmy, et.mx, et.my) then closer = true end
  end
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OBJECT_TANK and bit.band(ob.info, OBJECT_HOSTILE) == 0 then
      local w = danger.odds_weight(
        U.edist(mx, my, bit.rshift(ob.x, 8), bit.rshift(ob.y, 8)))
      if w > 0 then al = al + w; n_al = n_al + 1 end
    end
  end
  t.enemy = en; t.n_enemy = n_en
  t.ally  = al; t.n_ally  = n_al
  t.closer = closer

  t.safety = (C.TAKE_COVER_W_COVER or 8) * t.cover
           - (C.TAKE_COVER_W_EXPO  or 0.15) * t.expo
           - (C.TAKE_COVER_W_ENEMY or 20) * t.enemy
           + (C.TAKE_COVER_W_ALLY  or 4) * t.ally
  return t.safety, t
end

-- Compact one-line term dump so every printed number is hand-checkable.
local function tc_terms_str(t)
  return string.format("cov %.2f(%dp %dhot) expo %.0f en %.2f(%d) al %.2f(%d) = %.1f",
    t.cover or 0, t.cover_n or 0, t.cover_hot or 0, t.expo or 0,
    t.enemy or 0, t.n_enemy or 0, t.ally or 0, t.n_ally or 0, t.safety or 0)
end

local function tc_formula(t, travel, adj)
  local trav_part = ""
  if travel then
    trav_part = string.format(" - travel{%.0f}*%.2f = adj{%.1f}",
                              travel, C.TAKE_COVER_W_TRAVEL or 0.6, adj or 0)
  end
  return string.format(
    "cover{%.2f}*%.0f - expo{%.0f}*%.2f - enemy{%.2f}*%.0f + ally{%.2f}*%.0f = safety{%.1f}%s"
    .. "||%d own pill(s) cover this tile (%d heated); threat.pill_at=%.0f;"
    .. " %d enemy / %d allied tank(s) in odds range",
    t.cover or 0, C.TAKE_COVER_W_COVER or 8,
    t.expo or 0, C.TAKE_COVER_W_EXPO or 0.15,
    t.enemy or 0, C.TAKE_COVER_W_ENEMY or 20,
    t.ally or 0, C.TAKE_COVER_W_ALLY or 4,
    t.safety or 0, trav_part,
    t.cover_n or 0, t.cover_hot or 0, t.expo or 0,
    t.n_enemy or 0, t.n_ally or 0)
end

-- M.find_cover_tile — the scan.  Rings 3/6/9/12 x 8 directions around the
-- tank, plus the 8 neighbours of every own/allied deployed pill in range and
-- every friendly base tile in range.  Returns:
--   here_terms, best_terms, best_travel, cands, reject
-- `reject` is "unreachable" when nothing survived the filters.
--
-- Cached for TAKE_COVER_SCAN_TICKS (state._cover_scan) so a 40-candidate
-- smart_cost sweep doesn't run on every tick of a replan cycle.
function M.find_cover_tile(state, world, info, tmx, tmy)
  local now  = state.tick or 0
  local boat = info.inboat and true or false
  local sc   = state._cover_scan
  if sc and sc.tick and (now - sc.tick) < (C.TAKE_COVER_SCAN_TICKS or 10)
     and sc.tmx == tmx and sc.tmy == tmy and sc.boat == boat then
    -- Restore the id list too: eval_take_cover's sticky re-score calls
    -- tc_safety after this returns, and tc_safety walks state._tc_pill_ids.
    state._tc_pill_ids = sc.ids
    return sc.here, sc.best, sc.best_travel, sc.cands, sc.reject
  end

  -- Sorted pill-id list, rebuilt per scan: tc_safety walks it with a numeric
  -- for so the cover sum never depends on pairs() order.
  local ids = {}
  for id, p in pairs(world.pills or {}) do
    if (p.owner == "friendly" or p.owner == "allied")
       and not (p.in_tank or p.carrier or p._synth_carry)
       and (p.health or 0) > 0 then
      ids[#ids + 1] = id
    end
  end
  table.sort(ids)
  state._tc_pill_ids = ids

  local boat_flag = info.inboat and 1 or 0
  local _, here = tc_safety(state, world, info, tmx, tmy, tmx, tmy)
  here.here = true

  local seen  = {}
  local cands = {}
  local n_rej = 0
  local function consider(cx, cy, src)
    cx, cy = U.mclamp(cx), U.mclamp(cy)
    if cx == tmx and cy == tmy then return end
    local k = cy * 256 + cx
    if seen[k] then return end
    seen[k] = true
    local ok, why = tc_standable(cx, cy, boat)
    if not ok then
      n_rej = n_rej + 1
      cands[#cands + 1] = { mx = cx, my = cy, src = src, reject = why }
      return
    end
    local trav = smart_cost(KIND_NORMAL, tmx, tmy, cx, cy, boat_flag,
                            info.shells or 32, info.trees or 0,
                            info.mines or 0, info.armour or 40)
    if not trav or trav >= 1e8 then
      n_rej = n_rej + 1
      cands[#cands + 1] = { mx = cx, my = cy, src = src, reject = "unreachable" }
      return
    end
    local s, t = tc_safety(state, world, info, cx, cy, tmx, tmy)
    t.travel = trav
    t.src = src
    t.adj = s - (C.TAKE_COVER_W_TRAVEL or 0.6) * trav
    if t.closer then
      n_rej = n_rej + 1
      t.reject = "toward_enemy"
    end
    cands[#cands + 1] = t
  end

  -- Rings around the tank (fixed radius x direction order = deterministic).
  for _, r in ipairs({ 3, 6, 9, 12 }) do
    for i = 0, 7 do
      local ang = i * math.pi / 4
      consider(tmx + math.floor(r * math.sin(ang) + 0.5),
               tmy - math.floor(r * math.cos(ang) + 0.5), "ring" .. r)
    end
  end
  -- The 8 neighbours of every own/allied deployed pill in range: standing
  -- beside our own pill is the cheapest cover on the map.
  local pr = C.TAKE_COVER_PILL_NEIGHBOUR_RANGE or 15
  local wp = world.pills or {}
  for i = 1, #ids do
    local p = wp[ids[i]]
    if p and U.mdist(tmx, tmy, p.mx, p.my) <= pr then
      for dy = -1, 1 do
        for dx = -1, 1 do
          if dx ~= 0 or dy ~= 0 then
            consider(p.mx + dx, p.my + dy, "pill" .. tostring(ids[i]))
          end
        end
      end
    end
  end
  -- Friendly base tiles in range: armour and shells are there if we need them
  -- later, and a base tile is by definition ground we hold.
  local bids = {}
  local wb = world.bases or {}
  for id, b in pairs(wb) do
    if b.owner == "friendly"
       and U.mdist(tmx, tmy, b.mx, b.my) <= (C.TAKE_COVER_BASE_TILE_RANGE or 15) then
      bids[#bids + 1] = id
    end
  end
  table.sort(bids)
  for i = 1, #bids do
    local b = wb[bids[i]]
    consider(b.mx, b.my, "base" .. tostring(bids[i]))
  end

  -- Pick: highest safety-minus-travel among the survivors. Deterministic
  -- tie-break: lower travel first, then lower my*256+mx.
  local best = nil
  for i = 1, #cands do
    local c = cands[i]
    if not c.reject then
      local better = false
      if best == nil then
        better = true
      elseif c.adj > best.adj then
        better = true
      elseif c.adj == best.adj then
        if c.travel < best.travel then
          better = true
        elseif c.travel == best.travel
               and (c.my * 256 + c.mx) < (best.my * 256 + best.mx) then
          better = true
        end
      end
      if better then best = c end
    end
  end
  local reject = (best == nil) and "unreachable" or nil
  if best then best.pick = true end

  state._cover_scan = {
    tick = now, tmx = tmx, tmy = tmy, boat = boat, ids = ids,
    here = here, best = best, best_travel = best and best.travel or nil,
    cands = cands, reject = reject, n_rej = n_rej,
  }
  return here, best, best and best.travel or nil, cands, reject
end

-- eval_take_cover — pool 14.  ALWAYS returns an entry (never nil).
local function eval_take_cover(state, world, info, tmx, tmy, boat, ammo)
  local now = state.tick or 0

  -- A row with nothing behind it still has to say WHY. These are the only
  -- states where asking "where is it safer?" is meaningless.
  local function dead_row(reason, why)
    return {
      cost = TAKE_COVER_REJECT_COST,
      _reject = reason,
      goal = { kind = "take_cover", mx = tmx, my = tmy,
               wx = U.m2w(tmx), wy = U.m2w(tmy) },
      desc = BRAIN_POOL_VIZ and ("take_cover REJECT " .. reason) or "",
      cands = { { id = 0, mx = tmx, my = tmy, cost = 1e30,
                  formula = string.format("REJECT %s||reject:%s", reason, why),
                  stale = 0, reject = reason, reject_remaining = 0 } },
    }
  end
  -- info.dead is the engine's flag (init.lua bails out of think() on it, so
  -- this is belt-and-braces for the fill_pool_cache path).
  if info.dead then
    return dead_row("dead", "tank is dead — nowhere to stand")
  end
  if info.inboat then
    -- A boat has no cover to take: the terrain under it is water, our own
    -- pills do not shade it, and beaching to hide loses the boat.
    -- escape_water / normal navigation own this case.
    return dead_row("in_boat",
      "afloat — cover is a LAND concept; boat handling belongs to escape_water/nav")
  end

  local here, best, travel, cands, scan_reject =
    M.find_cover_tile(state, world, info, tmx, tmy)
  if scan_reject or not best then
    local row = dead_row("unreachable",
      "no reachable, standable tile that is not closer to a visible enemy")
    if BRAIN_POOL_VIZ then
      row.cands = {}
      row.cands[#row.cands + 1] = {
        id = -1, mx = tmx, my = tmy, cost = 1e30,
        formula = "HERE " .. tc_formula(here)
                  .. " — the baseline every margin is measured from",
        stale = 0, reject = "here", reject_remaining = 0,
      }
      for i = 1, #cands do
        local c = cands[i]
        row.cands[#row.cands + 1] = {
          id = c.my * 256 + c.mx, mx = c.mx, my = c.my, cost = 1e30,
          formula = string.format("REJECT %s (%s)||reject:%s",
                                  tostring(c.reject), tostring(c.src),
                                  tostring(c.reject)),
          stale = 0, reject = c.reject or "unreachable", reject_remaining = 0,
        }
      end
    end
    return row
  end

  -- ── Sticky pick ────────────────────────────────────────────────────
  -- Same shape as pick_wait_spot's _wait_spot: hold the tile we already
  -- chose while it is still valid, so driving there doesn't churn the
  -- destination out from under the pathfinder every replan.
  local cs = state._cover_spot
  if cs and (now - (cs.tick or 0)) <= (C.TAKE_COVER_STICKY_TICKS or 500)
     and U.mdist(tmx, tmy, cs.mx, cs.my) <= (C.TAKE_COVER_STICKY_DIST or 12)
     and tc_standable(cs.mx, cs.my, false)
     and not (cs.mx == best.mx and cs.my == best.my) then
    local s, t = tc_safety(state, world, info, cs.mx, cs.my, tmx, tmy)
    if not t.closer
       and s >= (best.safety or 0) - (C.TAKE_COVER_HOLD_RELEASE_MARGIN or 4) then
      local tv = smart_cost(KIND_NORMAL, tmx, tmy, cs.mx, cs.my,
                            info.inboat and 1 or 0, info.shells or 32,
                            info.trees or 0, info.mines or 0, info.armour or 40)
      if tv and tv < 1e8 then
        t.travel = tv
        t.adj = s - (C.TAKE_COVER_W_TRAVEL or 0.6) * tv
        t.pick = true
        t.sticky = true
        best = t
        travel = tv
      end
    end
  end
  local pick_ref_safety = (cs and cs.mx == best.mx and cs.my == best.my)
                          and (cs.safety or best.safety) or best.safety
  state._cover_spot = { mx = best.mx, my = best.my, tick = now,
                        safety = pick_ref_safety }

  local margin = (best.safety or 0) - (here.safety or 0)
  -- Split the margin into the harm it AVOIDS and the comfort it merely adds.
  -- safety = W_COVER*cover - W_EXPO*expo - W_ENEMY*enemy + W_ALLY*ally, so the
  -- danger half is the expo + enemy terms and the comfort half is cover + ally.
  -- The CALM branch below gates on the danger half alone: moving to a tile that
  -- has one more of our pillboxes overhead, with the same zero incoming fire and
  -- the same zero enemy tanks as where we stand, is not taking cover from
  -- anything. (20260902_000405 bot2 t=22561: margin 12.0 = cover*8 + ally*4,
  -- dexpo = denemy = 0, and that bid preempted a live defend.) The FULL margin
  -- still sets the discount -- a safer tile that is also better shaded is
  -- genuinely better once we have decided to move.
  local danger_margin =
      (C.TAKE_COVER_W_EXPO  or 0.15) * ((here.expo  or 0) - (best.expo  or 0))
    + (C.TAKE_COVER_W_ENEMY or 20)   * ((here.enemy or 0) - (best.enemy or 0))
  local comfort_margin = margin - danger_margin
  local holding = (tmx == best.mx and tmy == best.my)

  -- ── Triggers: they set the COST, never whether the row exists ──────
  local haul = haul_flee_eval(state, info, tmx, tmy)
  -- Resupply actually needed? Then the EXISTING critical-flee injection (which
  -- drives to a base and refills) is the better answer and stays in charge;
  -- take_cover bids at its ordinary cost so it can still win on merit.
  local need_resupply = (info.armour or 99) <= (C.ARMOUR_LOW or 15)
                        or (info.shells or 99) <= (C.SHELLS_LOW or 20)
  local haul_fires = (haul.level > 0) and haul.triggered and not need_resupply

  local panic_ok, panic_thresh, _panic_why = danger.panic_scores(state)
  -- The hole incident A fell into: the SCORES say panic, but the panic
  -- REACTION (a build) is impossible. Ask should_panic_build which gate it
  -- tripped so we only claim this trigger for the cases a build can't cover.
  local _, _, build_why = danger.should_panic_build(state, info)
  local panic_no_build = panic_ok
    and (build_why == "lgm_out" or build_why == "not_carrying"
         or build_why == "inboat")

  local bad_pill = (here.expo or 0) >= (C.TAKE_COVER_BAD_GROUND_PILL_AT or 30)
  local bad_score = (state.imdanger ~= nil and state.vuln ~= nil)
                    and (state.imdanger <= danger.panic_threshold(state.vuln))
  local bad_ground = bad_pill or bad_score

  local trig, cost, cost_str
  if haul_fires then
    trig = "haul"
    cost = C.TAKE_COVER_HAUL_FLOOR or 10
    cost_str = string.format(
      "HAUL_FLOOR{%.0f} (carry=%d man=%s level=%.2f trigger=%s)",
      cost, haul.carry, tostring(haul.man), haul.level,
      haul.tank_engaging and string.format("tank_engaging d=%.1f<=%.1f",
                                           haul.tank_dist or -1, haul.range or 0)
      or haul.pill_shooting and string.format("pill_shooting pill_at=%.0f>0", haul.pill_at)
      or string.format("arm_low %d<=%.1f", info.armour or -1, haul.arm_thresh or 0))
  elseif panic_no_build then
    trig = "panic_no_build"
    cost = C.TAKE_COVER_HAUL_FLOOR or 10
    cost_str = string.format(
      "HAUL_FLOOR{%.0f} (panic scores fire: vuln %.1f <= 50 and imdanger %.1f <= thresh %.1f;"
      .. " the panic BUILD is blocked by %s, so moving is the only reaction left)",
      cost, state.vuln or -1, state.imdanger or -1, panic_thresh or -1,
      tostring(build_why))
  elseif bad_ground then
    trig = "bad_ground"
    cost = math.max(1, (C.TAKE_COVER_BASE_COST or 60)
                       - (C.TAKE_COVER_K or 1.0) * margin)
    cost_str = string.format(
      "max(1, base{%.0f} - K{%.2f} * margin{%.1f}) = %.0f (%s)",
      C.TAKE_COVER_BASE_COST or 60, C.TAKE_COVER_K or 1.0, margin, cost,
      bad_pill and string.format("pill_at{%.0f} >= BAD_GROUND{%.0f}", here.expo or 0,
                                 C.TAKE_COVER_BAD_GROUND_PILL_AT or 30)
      or string.format("imdanger{%.1f} <= panic_thresh{%.1f}",
                       state.imdanger or -1,
                       danger.panic_threshold(state.vuln or 50)))
  elseif danger_margin >= (C.TAKE_COVER_MIN_DANGER_MARGIN
                           or C.TAKE_COVER_MIN_MARGIN or 8) then
    -- CALM: nothing is happening, but the pick genuinely gets us out from under
    -- measurable harm (pill fire and/or enemy-tank odds). Floored at
    -- TAKE_COVER_CALM_MIN so an untriggered move can only ever beat filler
    -- (seek_trees / explore) and never a live defend or attack.
    trig = "calm"
    cost = math.max(C.TAKE_COVER_CALM_MIN or 120,
                    (C.TAKE_COVER_BASE_COST or 60)
                    - (C.TAKE_COVER_K or 1.0) * margin)
    cost_str = string.format(
      "max(CALM_MIN{%.0f}, base{%.0f} - K{%.2f} * margin{%.1f}) = %.0f"
      .. " (no trigger; danger_margin{%.1f} = expo{%.1f}+enemy{%.1f} clears"
      .. " MIN_DANGER_MARGIN{%.0f}; comfort{%.1f} = cover+ally rides the discount only)",
      C.TAKE_COVER_CALM_MIN or 120,
      C.TAKE_COVER_BASE_COST or 60, C.TAKE_COVER_K or 1.0, margin, cost,
      danger_margin,
      (C.TAKE_COVER_W_EXPO or 0.15) * ((here.expo or 0) - (best.expo or 0)),
      (C.TAKE_COVER_W_ENEMY or 20) * ((here.enemy or 0) - (best.enemy or 0)),
      C.TAKE_COVER_MIN_DANGER_MARGIN or C.TAKE_COVER_MIN_MARGIN or 8,
      comfort_margin)
  else
    trig = "none"
    cost = nil
    cost_str = string.format(
      "REJECT no_safer_tile (danger margin %.1f < MIN_DANGER_MARGIN{%.0f}):"
      .. " total margin{%.1f} = danger{%.1f} + comfort{%.1f}, and no trigger fired"
      .. " (haul level %.2f, panic %s, pill_at %.0f)."
      .. " A tile that is only better SHADED is not cover from anything",
      danger_margin, C.TAKE_COVER_MIN_DANGER_MARGIN or C.TAKE_COVER_MIN_MARGIN or 8,
      margin, danger_margin, comfort_margin,
      haul.level, tostring(panic_ok), here.expo or 0)
  end

  -- ── Hold release ───────────────────────────────────────────────────
  -- Standing ON the pick: keep the goal while here is still meaningfully
  -- worse than the pick looked when we chose it. Once here has caught up
  -- (it IS the pick, so this is really "the world calmed down"), stop
  -- holding and let the pool move on. Survival triggers never release.
  if holding and trig ~= "haul" and trig ~= "panic_no_build" then
    local ref = pick_ref_safety or best.safety or 0
    if (here.safety or 0) >= ref - (C.TAKE_COVER_HOLD_RELEASE_MARGIN or 4) then
      trig = "released"
      cost = nil
      cost_str = string.format(
        "REJECT released: standing on the pick and here{%.1f} >= pick_at_choice{%.1f}"
        .. " - hold_margin{%.0f}",
        here.safety or 0, ref, C.TAKE_COVER_HOLD_RELEASE_MARGIN or 4)
    end
  elseif holding then
    trig = trig .. "_holding"
  end

  -- Every candidate becomes a row so the panel shows the whole scan.
  local rows = nil
  if BRAIN_POOL_VIZ then
    rows = {}
    rows[#rows + 1] = {
      id = -1, mx = tmx, my = tmy, cost = 1e30,
      formula = "HERE " .. tc_formula(here)
                .. string.format("; margin = best{%.1f} - here{%.1f} = %.1f"
                                 .. " = danger{%.1f: expo+enemy, what the move AVOIDS}"
                                 .. " + comfort{%.1f: cover+ally}. The calm branch"
                                 .. " gates on danger alone (MIN_DANGER_MARGIN %.0f);"
                                 .. " the discount uses the full margin",
                                 best.safety or 0, here.safety or 0, margin,
                                 danger_margin, comfort_margin,
                                 C.TAKE_COVER_MIN_DANGER_MARGIN or C.TAKE_COVER_MIN_MARGIN or 8),
      stale = 0, reject = "here", reject_remaining = 0,
    }
    for i = 1, #cands do
      local c = cands[i]
      local is_pick = (c.mx == best.mx and c.my == best.my)
      local f
      if c.reject then
        f = string.format("REJECT %s (%s)||reject:%s — %s", tostring(c.reject),
                          tostring(c.src), tostring(c.reject),
                          (c.reject == "toward_enemy")
                            and "closer to a visible enemy tank than we are now"
                            or "not standable / not reachable")
      elseif is_pick then
        f = "PICK " .. tc_formula(c, c.travel, c.adj) .. " -> cost " .. cost_str
      else
        f = tc_formula(c, c.travel, c.adj)
      end
      rows[#rows + 1] = {
        id = c.my * 256 + c.mx, mx = c.mx, my = c.my,
        cost = (is_pick and cost) and cost or 1e30,
        formula = f, stale = 0,
        reject = c.reject, reject_remaining = 0,
      }
    end
  end

  -- print2: once per SCAN, not per tick (the scan cache gates it), plus a
  -- rate-limited line for the rejected case. The `if BRAIN_DEBUG_MODE then`
  -- has to be a SINGLE line — that is the token strip.bat's --strip-block
  -- matches on, and a wrapped condition leaves an orphan `end` behind in opt/.

  -- Overlay payload (take_cover_viz, default OFF).

  local desc = BRAIN_POOL_VIZ and string.format(
    "take_cover@(%d,%d) cost=%s [%s] here{%s} best{%s} margin %.1f travel %.0f",
    best.mx, best.my, cost and string.format("%.0f", cost) or "REJECT", trig,
    tc_terms_str(here), tc_terms_str(best), margin, travel or 0) or ""

  local goal = { kind = "take_cover", mx = best.mx, my = best.my,
                 wx = U.m2w(best.mx), wy = U.m2w(best.my),
                 target_id = -1,
                 _tc_trigger = trig, _tc_margin = margin }

  -- Seed cost_cache so the WINNERS strip finds the full formula (same
  -- contract the wait_for_lgm / kill_lgm strips use: "<pool>:<target_id>").
  if BRAIN_POOL_VIZ then
    if not state.cost_cache then state.cost_cache = {} end
    state.cost_cache["14:-1"] = {
      cost = cost or TAKE_COVER_REJECT_COST, raw = travel or 0,
      tick = now, _p = 14, _mx = best.mx, _my = best.my,
      formula = string.format(
        "take_cover@(%d,%d) [%s] %s||here{%s}; best{%s}; margin = best - here = %.1f;"
        .. " travel{%.0f} x W_TRAVEL{%.2f} shaped the PICK only, not the cost",
        best.mx, best.my, trig, cost_str,
        tc_terms_str(here), tc_terms_str(best), margin,
        travel or 0, C.TAKE_COVER_W_TRAVEL or 0.6),
    }
  end

  if not cost then
    return {
      cost = TAKE_COVER_REJECT_COST,
      _reject = (trig == "released") and "released" or "no_safer_tile",
      goal = goal, desc = desc, cands = rows,
    }
  end
  return { cost = cost, goal = goal, desc = desc, cands = rows }
end

-- =========================================================================
-- "KILL ME" — handing a stack off a builder-less tank
--
-- Engine facts this is built on:
--   * ALLIED shells damage allied tanks. tank.c tankIsTankHit only ignores
--     the SHOOTER's own shells, so a team-mate can kill us on purpose.
--   * a killed tank's carried pills fall on the ground as corpses that keep
--     the DEAD PLAYER's ownership -- and capture_pill already scoops an
--     ally-owned corpse, so the responder pockets the stack by driving over
--     it, with no new pickup code at all.
--   * a killed LGM is choppered in from a RANDOM start tile at 3 wu/engine
--     tick and then walks; dying does NOT bring him back (lgm.c lgmTankDied
--     only clears nextAction). So "wait for the builder" is not a plan.
--
-- INITIATOR side is this file (eval_kill_me_wait, pool 15) plus the token on
-- the /info extra slate in init.lua. RESPONDER side is an extra candidate in
-- eval_attack_tank (pool 9) and the target override in steering.lua.
-- =========================================================================

-- Every ally currently advertising a kill_me request, sorted by player number
-- so two bots reading the same slate see the same list in the same order.
--   km  = "XXYYAA"  (hex: request tile x, tile y, the requester's armour)
--   kmc = "PPCCCC"  (hex: the initiator this ally is answering, and the cost
--                    it bid) -- the CLAIM, read by claim_on_kill_me below.
function KM.parse(s)
  if not s or s == "-" or #s < 6 then return nil end
  local mx = tonumber(string.sub(s, 1, 2), 16)
  local my = tonumber(string.sub(s, 3, 4), 16)
  local ar = tonumber(string.sub(s, 5, 6), 16)
  if not (mx and my and ar) then return nil end
  return mx, my, ar
end

function KM.parse_claim(s)
  if not s or s == "-" or #s < 6 then return nil end
  local pn = tonumber(string.sub(s, 1, 2), 16)
  local ct = tonumber(string.sub(s, 3, 6), 16)
  if not (pn and ct) then return nil end
  return pn, ct
end

function M.kill_me_requests(state, info, now)
  local out = {}
  if not C.KILL_ME_ENABLED then return out end
  local self_pn = info and info.player_number or -1
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn then
      local h = slot.info
      local mx, my, ar = KM.parse(h and h.km)
      if mx then
        -- A slot whose tank we have SEEN die since its last slate is stale:
        -- the request died with it. Same guard the blitz commander uses.
        local dead = state.tank_dead_at and state.tank_dead_at[pn]
                     and state.tank_dead_at[pn] > (slot.last_tick or 0)
        if not dead then
          out[#out + 1] = { pn = pn, mx = mx, my = my, armour = ar }
        end
      end
    end
  end
  table.sort(out, function(a, b) return a.pn < b.pn end)
  return out
end

-- Is another ALLY already claiming this initiator, and does their claim beat
-- ours? Same shape as builder_pool.ally_claim_on: a cheaper bid wins, but only
-- if it is cheaper by ALLY_CLAIMED_STEAL_FRAC_KILLME; a tie breaks to the
-- LOWER player number, the rule every other claim in the brain uses.
-- Returns nil when the request is ours to take, else { pn, cost }.
function KM.ally_claim(state, info, target_pn, our_cost, now)
  local self_pn = info and info.player_number or -1
  local frac = C.ALLY_CLAIMED_STEAL_FRAC_KILLME or 0.10
  local best = nil
  for pn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= self_pn and pn ~= target_pn then
      local cpn, ccost = KM.parse_claim(slot.info and slot.info.kmc)
      if cpn == target_pn then
        local theirs_wins
        if our_cost == nil then
          theirs_wins = true
        elseif math.abs(ccost - our_cost) < 0.5 then
          theirs_wins = pn < self_pn                       -- tie: lower pn
        else
          theirs_wins = not (our_cost < ccost * (1 - frac))  -- steal band
        end
        if theirs_wins and (not best or pn < best.pn) then
          best = { pn = pn, cost = ccost }
        end
      end
    end
  end
  return best
end

-- eval_kill_me_wait — pool 15, the INITIATOR row.
--
-- Bids only while the tank is loaded and builder-less AND nothing is actually
-- happening: the escape rows own the situation the moment they trigger, and
-- an enemy tank inside KILL_ME_CANCEL_ENEMY_TILES cancels the request
-- outright (a hand-off in front of the enemy just gives them the corpses).
-- Priced at KILL_ME_WAIT_COST -- above nothing, below every escape.
function KM.eval_wait(state, world, info, tmx, tmy)
  if not C.KILL_ME_ENABLED then return nil end
  if not state.loaded_no_lgm then return nil end
  if info.dead or info.inboat then return nil end
  local now = state.tick or 0
  local km = state.km
  if not km then km = {}; state.km = km end
  if km.cooldown_until and now < km.cooldown_until then return nil end
  -- The escape rows are in charge whenever they fire. haul_flee_eval is the
  -- SHARED ladder (flee injection + take_cover both read it), so asking it
  -- here is asking the same question they answer, not a third copy.
  local haul = haul_flee_eval(state, info, tmx, tmy)
  if haul.triggered then return nil end
  -- Our armour floor is waived while the claimant is executing: those last
  -- points ARE the hand-off. (The haul ladder above already waived its own
  -- armour trigger for the same reason.)
  if not km.executing
     and (info.armour or 0) <= (C.ARMOUR_CRITICAL or 8) then return nil end
  -- An enemy tank in sight of the meeting point cancels the whole idea.
  local nh = state.perc and state.perc.nearest_hostile_tank
  local enemy_d = nh and nh.dist or nil
  if enemy_d and enemy_d <= (C.KILL_ME_CANCEL_ENEMY_TILES or 10) then
    return nil
  end
  -- Somebody has to be able to answer.
  local n_allies = 0
  for pn in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if pn ~= (info.player_number or -1) then n_allies = n_allies + 1 end
  end
  if n_allies == 0 then return nil end
  -- Park somewhere the enemy is least likely to interrupt: the take_cover
  -- picker already answers "where is it safer than here", so use it rather
  -- than inventing a second notion of cover.
  local _here, best = M.find_cover_tile(state, world, info, tmx, tmy)
  local wmx, wmy = tmx, tmy
  local where = "here"
  if best and best.mx then
    wmx, wmy = best.mx, best.my
    where = "cover"
  end
  local cost = C.KILL_ME_WAIT_COST or 15
  local claimed = km.claimed_by
  local desc = BRAIN_POOL_VIZ and string.format(
    "kill_me_wait@(%d,%d) cost=%.0f [%s] %s claim=%s",
    wmx, wmy, cost, where, M.loaded_no_lgm_label(state),
    claimed and ("p" .. tostring(claimed)) or "none") or ""
  if BRAIN_POOL_VIZ then
    if not state.cost_cache then state.cost_cache = {} end
    state.cost_cache["15:-1"] = {
      cost = cost, raw = 0, tick = now, _p = 15, _mx = wmx, _my = wmy,
      formula = string.format(
        "kill_me_wait{%.0f}@(%d,%d)||kill_me:%s. Park on the %s tile and advertise a kill_me token"
        .. " (tile + armour %d) on the /info extra slate so an ally with its builder aboard can"
        .. " shoot us and pocket the %d pill(s) we cannot place. Priced KILL_ME_WAIT_COST{%.0f}:"
        .. " above routine work, below flee_to_base (~40) and take_cover (~%d), so a real escape"
        .. " always wins|cancel:the row disappears the moment an enemy tank comes within"
        .. " KILL_ME_CANCEL_ENEMY_TILES{%d} (nearest right now: %s), the escape triggers fire,"
        .. " armour drops to ARMOUR_CRITICAL{%d}, or the builder is back|claim:%s",
        cost, wmx, wmy, M.loaded_no_lgm_label(state), where,
        info.armour or 0, info.carried_pills or 0, cost,
        C.TAKE_COVER_BASE_COST or 60, C.KILL_ME_CANCEL_ENEMY_TILES or 10,
        enemy_d and string.format("%d tiles", enemy_d) or "none visible",
        C.ARMOUR_CRITICAL or 8,
        claimed and string.format("p%d has claimed the request -- we stop targeting THAT ally"
                                  .. " (enemies are still fair game) until it lands",
                                  claimed)
                or "nobody has claimed it yet"),
    }
  end
  return {
    cost = cost,
    goal = { kind = "kill_me_wait", mx = wmx, my = wmy,
             wx = U.m2w(wmx), wy = U.m2w(wmy), target_id = -1,
             km_claimed_by = claimed },
    desc = desc,
  }
end

-- Map overlay for the take_cover scan (viz id "take_cover_viz", default OFF).
-- Candidate tiles tinted by safety (green = safer than here, red = worse),
-- rejects greyed with their reason, the PICK ringed, hostile-pill range rings
-- in red and our own cover rings in blue.
function M.draw_take_cover(viz, state)
  if not viz or not viz.is_on or not viz.is_on("take_cover_viz") then return end
  local v = state._cover_viz
  if not v then return end
  if (state.tick or 0) - (v.tick or 0) > 120 then return end
  local hs = (v.here and v.here.safety) or 0
  for _, c in ipairs(v.cands or {}) do
    if c.reject then
      if viz.text then
      end
    else
      local d = (c.safety or 0) - hs
      local r, g = 220, 220
      if d > 0 then r = math.max(40, 220 - d * 8) else g = math.max(40, 220 + d * 8) end
      if viz.text then
      end
    end
  end
  if v.here then
    if viz.text then
    end
  end
  if v.pick then
    if viz.text then
    end
  end
  for _, p in ipairs(v.rings or {}) do
    if p.own then
    else
    end
  end
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
  -- A live ALLY builder-pool claim covers the OUTCOME, not just the trip
  -- (BUILDER_POOL_PLAN section 6): if p2's man is walking to this pill, it is
  -- going to be repaired, so it should not appear in our "damaged friendly
  -- pill needing attention" discovery at all -- not as a cheap row we then
  -- reject, which would still let it drag our replan around. Arbitration
  -- (earlier claim tick, then lower player number) lives in
  -- builder_pool.ally_claim_on; here we simply honour whatever it says beats
  -- us, asking as of NOW because we hold no claim on this tile.
  if C.BUILDER_POOL_ENABLED and state then
    local _now_c = state.tick or 0
    if bpool.ally_claim_on(state, info, obj.mx, obj.my, _now_c, _now_c) then
      return false
    end
  end
  -- The OTHER ally interest in a corpse -- "I am driving over to scoop it" --
  -- is NOT filtered out here, deliberately. It is priced at INF with the reason
  -- on the row instead (search ally_capturing in the pool-5 block below), for
  -- the always-show rule the sibling builder_can split states in full: a row
  -- that reads `REJECT ally_capturing (p3, 42t)` is how the guard is checked
  -- against the BUILDER strip's matching row. INF cannot win a competition, so
  -- the refusal is just as hard as a `return false` here would be.
  if C.REPAIR_FIX_ENABLED and obj.health == 0 then
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
  -- LOADED, BUILDER-LESS: the bar for "the direct route is fine" moves from
  -- "the sim says we SURVIVE it" to "the sim says nothing touches us". With a
  -- stack aboard and nobody to place it, armour is the only thing standing
  -- between the enemy and five free pillboxes, so ANY predicted damage sends
  -- the decision to the danger-weighted A* instead of the danger-free one.
  local _strict = state.loaded_no_lgm and C.CAPTURE_NO_LGM_ROUTE_STRICT or false
  local _refuse = _strict and (killed or damage > 0) or killed
  ent.strict = _strict or nil
  if _refuse then
    -- Lethal direct run: hand the decision to the danger-weighted A* next tick.
    ent.mode = "pending_danger"
  else
    ent.mode = "direct"
    ent.cost = cost
  end
  state.capture_route[key] = ent
end

-- =========================================================================
-- PILL CLUSTERING — shared by the sea harvest and the land capture discount.
--
-- Greedy single-linkage over SORTED ids: a pill joins the first existing
-- cluster that already holds a tile within R, otherwise it starts its own.
-- Sorting first is what makes it deterministic — with pairs() order the same
-- three pills could produce one cluster or two depending on hash layout, and
-- every cluster id in the log and the panel would drift run to run.
--
-- ids : an ARRAY of pill ids (this function sorts a copy, callers need not)
-- Returns clusters = { {id=, ids={...}, tiles={{mx,my},...}, n=}, ... },
--         by_pill  = { [pill_id] = cluster index }
local function build_pill_clusters(world, ids, R)
  local sorted = {}
  for i = 1, #ids do sorted[i] = ids[i] end
  table.sort(sorted)
  local clusters, by_pill = {}, {}
  for _, pid in ipairs(sorted) do
    local p = world.pills[pid]
    local home = nil
    for ci = 1, #clusters do
      for _, t in ipairs(clusters[ci].tiles) do
        if U.mdist(p.mx, p.my, t[1], t[2]) <= R then home = ci break end
      end
      if home then break end
    end
    if not home then
      clusters[#clusters + 1] = { id = #clusters + 1, ids = {}, tiles = {} }
      home = #clusters
    end
    local cl = clusters[home]
    cl.ids[#cl.ids + 1] = pid
    cl.tiles[#cl.tiles + 1] = { p.mx, p.my }
    by_pill[pid] = home
  end
  for _, cl in ipairs(clusters) do cl.n = #cl.ids end
  return clusters, by_pill
end

-- =========================================================================
-- LAND DEAD-PILL CLUSTERS (Part 2 + Part 3 of the par2 fix)
--
-- Incident 20260901_160325_1_par2 bot3, t=17930-18600: SIX dead pills sat in
-- a heap at (114-117,130-131) — no reject on any of them — and capture#2
-- still priced 234 because each one was quoted the full ~11-tile trip through
-- a contested zone. Six free pills in one heap is ONE errand: the tank is
-- already there for the first, and the rest are a few tiles of walking. So
-- price each member as its SHARE of the trip, exactly as the sea harvest
-- already does for a raft (sea_plan_cluster splits the boat cost by cl.n).
--
--   discount : cost / min(n, CAPTURE_CLUSTER_DIVISOR_MAX), floored at
--              CAPTURE_CLUSTER_MIN_COST
--   guard    : x (1 + CAPTURE_CLUSTER_GUARD_MULT * k) capped at
--              CAPTURE_CLUSTER_GUARD_MAX, where k = live hostile/neutral
--              pillboxes that can actually put a shell on a cluster tile
--
-- The guard term is the user's own condition on the discount: "we should
-- still penalize the cost of the capture_pills even on a cluster though if
-- it's in really hostile territory (protected by a lot of pills)". It uses
-- the SAME test as the sea coverage veto — PILLBOX_RANGE plus the heated
-- margin AND cpf.simulate_shot actually reaching the tile — so a pillbox
-- walled off from the heap does not count, and one that can shell it does.
--
-- Only the CLUSTER TILES are tested. The route is already danger-weighted by
-- the Dijkstra/A* slate that produced dist_raw; charging for it twice would
-- price the same pillboxes into the number two different ways.
--
-- DEEP-SEA pills are excluded outright: sea_plan_cluster already splits their
-- cost by cluster size, and a second divisor here would double-discount them.
--
-- Cached like the sea scan (CAPTURE_CLUSTER_SCAN_TICKS): the guard test is
-- ~5 shell sims per guard pill, so it runs on a cadence, not per candidate
-- per tick. The cache also rebuilds immediately when the dead-pill set
-- changes (a pill collected or a fresh kill), so a stale n can never price a
-- heap that is no longer there.
local function capture_cluster_signature(world, deepsea)
  local ids = {}
  for pid, p in pairs(world.pills) do
    if (p.health or 0) == 0 and not p.in_tank and not p.carrier
       and not p._synth_carry and not (deepsea and deepsea[pid]) then
      ids[#ids + 1] = pid
    end
  end
  table.sort(ids)
  local sig = {}
  for i = 1, #ids do
    local p = world.pills[ids[i]]
    sig[i] = string.format("%d@%d,%d", ids[i], p.mx, p.my)
  end
  return ids, table.concat(sig, ";")
end

local function capture_cluster_refresh(state, world, now)
  local cache = state._cap_clusters
  -- compute_pool4_cost calls this once PER CANDIDATE; the world does not
  -- change inside a tick, so the signature scan runs at most once per tick.
  if cache and cache.check_tick == now then return cache end
  local deepsea = state.perc and state.perc.deepsea_pill_ids
  local ids, sig = capture_cluster_signature(world, deepsea)
  local period = C.CAPTURE_CLUSTER_SCAN_TICKS or 50
  if cache and cache.sig == sig and (now - (cache.tick or 0)) < period then
    cache.check_tick = now
    return cache
  end
  local clusters = build_pill_clusters(world, ids,
                                       C.CAPTURE_CLUSTER_RADIUS or 3)
  local by_tile = {}
  local guard_range = C.SEA_PILL_PILL_SAFE_RANGE or 9
  for _, cl in ipairs(clusters) do
    cl.div = math.min(cl.n, C.CAPTURE_CLUSTER_DIVISOR_MAX or 6)
    cl.guard_ids = {}
    if cl.n > 1 then
      -- Only a cluster that will actually get a discount pays for the scan.
      local threats = M.sea_threat_pills(world, cl.tiles, guard_range)
      for _, tp in ipairs(threats) do
        local covers = false
        for _, t in ipairs(cl.tiles) do
          if M.sea_pill_covers(world, tp, t[1], t[2], nil) then
            covers = true
            break
          end
        end
        if covers then cl.guard_ids[#cl.guard_ids + 1] = tp.id end
      end
    end
    cl.guard_n = #cl.guard_ids
    cl.guard_m = math.min(C.CAPTURE_CLUSTER_GUARD_MAX or 4.0,
                          1.0 + (C.CAPTURE_CLUSTER_GUARD_MULT or 0.75)
                                * cl.guard_n)
    for _, t in ipairs(cl.tiles) do
      by_tile[t[2] * 256 + t[1]] = cl
    end
  end
  cache = { tick = now, check_tick = now, sig = sig,
            clusters = clusters, by_tile = by_tile }
  state._cap_clusters = cache
  return cache
end
M.capture_cluster_refresh = capture_cluster_refresh

-- The land cluster covering a tile, or nil. nil for a lone dead pill too:
-- a cluster of one is not a cluster and must price exactly as it did before.
local function capture_cluster_at(state, world, now, mx, my)
  local cache = capture_cluster_refresh(state, world, now)
  local cl = cache.by_tile[my * 256 + mx]
  if cl and cl.n > 1 then return cl end
  return nil
end
M.capture_cluster_at = capture_cluster_at

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
            -- Cheapest arrival in EITHER layer. This used to read only the
            -- land layer (boat=0): a dead pill surrounded by water has only
            -- water neighbours, whose land-layer nodes never exist, so every
            -- open-water pill priced as unreachable even from a tank sitting
            -- in a boat (20260831_173448 bot3 t=6819: two dead team pills 13
            -- tiles away by sea, pool never offered them). A shore-touching
            -- pill worked only because one neighbour was land.
            local c0 = cpf.dijkstra_cost_at(idx, ax, ay, 0)
            local c1 = cpf.dijkstra_cost_at(idx, ax, ay, 1)
            local c = (c1 and c1 < c0) and c1 or c0
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
  -- LOADED, BUILDER-LESS: the cautious-mode x5 is replaced by
  -- CAPTURE_NO_LGM_DANGER_MULT. A corpse is only worth a trip if we live to
  -- PLACE it, and with no builder we cannot place anything at all -- so the
  -- danger on the way is the whole story and it is priced like it.
  -- (The state requires >= LOADED_NO_LGM_MIN_PILLS aboard, so cautious_mode
  -- is always true inside it; the keel value of the knob is CAUTIOUS_MODE_MULT
  -- and swapping it in reproduces today's number exactly.)
  local _lgm_mult_src = state.cautious_mode and "cautious" or "none"
  if state.loaded_no_lgm and C.CAPTURE_NO_LGM_DANGER_MULT then
    _lgm_mult = C.CAPTURE_NO_LGM_DANGER_MULT
    _lgm_mult_src = "no_builder"
  end
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
  -- LOADED, BUILDER-LESS: the "free pill" bonus is what makes a grab
  -- outrank routine goals, and it is the wrong incentive here -- the pill is
  -- not free, it costs a drive we may not survive, and it cannot be placed
  -- when we get there. CAPTURE_NO_LGM_FREE_BONUS_MULT scales the whole ramp
  -- (0 = no bonus at all; keel 1 = the full CAPTURE_FREE_PILL_VALUE curve).
  local _free_mult = 1
  if state.loaded_no_lgm and C.CAPTURE_NO_LGM_FREE_BONUS_MULT then
    _free_mult = C.CAPTURE_NO_LGM_FREE_BONUS_MULT
  end
  local free_bonus = C.CAPTURE_FREE_PILL_VALUE * (1 - badness) * _free_mult
  if free_bonus > 0 then
    c = math.max(C.CAPTURE_FREE_PILL_MIN_COST, c - free_bonus)
  end
  -- ── Cluster discount + hostile-territory guard (Parts 2 and 3) ────────
  -- Dead pills only, and never deep-sea ones (capture_cluster_refresh drops
  -- those — the sea plan already splits their cost by cluster size).
  local _cl_n, _cl_div, _cl_guard_n, _cl_guard_m, _cl_ids = 1, 1, 0, 1.0, nil
  -- LOADED, BUILDER-LESS: the cluster DISCOUNT is off. "Three corpses in one
  -- trip" is only a bargain if the trip is the cheap part, and here the trip
  -- is the thing that kills us -- so a pile of bodies must not divide the
  -- price of walking into it. The hostile-territory GUARD multiplier is a
  -- SURCHARGE and stays on. _cl_off records the suppression for the chip.
  local _cl_off = state.loaded_no_lgm
                  and (C.CAPTURE_NO_LGM_CLUSTER_DISCOUNT == false) or false
  if (obj.health or 0) == 0 and not obj.in_tank and not obj.carrier
     and not obj._synth_carry then
    local cl = capture_cluster_at(state, world, state.tick or 0, obj.mx, obj.my)
    if cl then
      _cl_n, _cl_div = cl.n, cl.div
      _cl_guard_n, _cl_guard_m = cl.guard_n, cl.guard_m
      _cl_ids = cl.guard_ids
      if _cl_off then
        _cl_div = 1
      else
        c = c / _cl_div
        local floor_c = C.CAPTURE_CLUSTER_MIN_COST or 5
        if c < floor_c then c = floor_c end
      end
      c = c * _cl_guard_m
    end
  end
  -- One line per TICK (not per candidate) while the state holds, so a headless
  -- run can see all four capture changes without the pool panel. The four
  -- numbers are exactly the four knobs, in the order the formula applies them.
  if state.loaded_no_lgm and state._cap_nb_log ~= (state.tick or 0) then
    state._cap_nb_log = state.tick or 0
  end
  return c, dist_raw, dist_score, danger_val, intercept, _lgm_mult, dist_method, free_bonus, _route_dmg, _route_far,
         _cl_n, _cl_div, _cl_guard_n, _cl_guard_m, _cl_ids,
         _lgm_mult_src, _free_mult, _cl_off
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

-- =========================================================================
-- SEA-PILL HARVEST — the DEEP-SEA branch of capture_pill
--
-- A dead pill lying in open deep sea used to be a flat `deepsea_no_boat`
-- reject: the tank would drown walking to it and nothing in the brain knew
-- how to get afloat (20260901_000042 bot2, three dead pills off the east
-- shore of DH-Oil Rig rejected every 50 ticks for 5000 ticks).
--
-- The engine already gives us the tools:
--   * a mine explodes into a CRATER (minesexp.c),
--   * a crater with a CARDINAL deep-sea/river neighbour floods to RIVER
--     (floodfill.c) — diagonal is not enough,
--   * BUILDMODE_BUILD (a wall) on RIVER builds a BOAT for LGM_COST_BOAT (20)
--     trees (lgm.c),
--   * and once we are afloat compute_pool4_cost already prices the pill
--     through the boat Dijkstra layer and the ordinary drive-over pickup
--     takes it (fixed 2026-08-31, tests/water_pills_test.py).
--
-- So this is not a new goal. It is capture_pill on the same target_id with a
-- substate chain in FRONT of the pickup:
--
--   entrance_plan -> (refuel_mines) -> (seek_trees) -> approach_F
--                 -> lay_mine -> detonate -> build_boat -> board -> pickup
--
-- The whole plan is priced ONCE per cluster (dead sea pills within
-- SEA_PILL_CLUSTER_RADIUS of each other — one boat trip takes them all) and
-- every member pill's pool row shows the same plan and the same cluster id.
-- Rejections are named, never silent: a row always exists.
--
-- The make-or-break question is line of fire. One shell sinks a boat, so a
-- hostile pill that can put a shell on the pills or on any tile of the boat
-- path rejects the whole cluster — STRICTLY, calm pills included.
-- =========================================================================

-- Terrain that stops a shell dead (mapIsPassable == FALSE with onBoat FALSE,
-- bolo_map.c:921). Water is PASSABLE: shells fly over river and deep sea.
local SEA_SHOT_STOPPERS = {
  [C.T_BUILDING]  = true,
  [C.T_HALFBUILD] = true,
  [C.T_FOREST]    = true,
  [C.T_BOAT]      = true,
}

-- Terrain an LGM may drop a mine on (lgm.c LGM_REQUEST_MINE refuses
-- DEEP_SEA / RIVER / BUILDING / BOAT / HALFBUILDING and any pill/base tile).
local SEA_MINABLE = {
  [C.T_GRASS]  = true,
  [C.T_ROAD]   = true,
  [C.T_FOREST] = true,
  [C.T_SWAMP]  = true,
  [C.T_RUBBLE] = true,
  [C.T_CRATER] = true,
}

-- Terrain a tank can park on (the firing spot F).
local SEA_STANDABLE = {
  [C.T_GRASS]  = true,
  [C.T_ROAD]   = true,
  [C.T_FOREST] = true,
  [C.T_SWAMP]  = true,
  [C.T_RUBBLE] = true,
  [C.T_CRATER] = true,
}

local SEA_CARD = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
local SEA_D8   = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
                   { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } }

-- Substates of capture_pill that belong to the sea chain. Exposed so
-- init.lua / steering / the hysteresis block can recognise them without
-- duplicating the list.
local SEA_SUBS = {
  entrance_plan = true, refuel_mines = true, seek_trees = true,
  approach_F = true, lay_mine = true, detonate = true,
  build_boat = true, board = true,
}
M.SEA_SUBS = SEA_SUBS
-- From lay_mine until the boat exists we are COMMITTED: there is a live mine
-- on our own shore and half-done is worse than not started.
local SEA_COMMITTED = { lay_mine = true, detonate = true, build_boat = true }
M.SEA_COMMITTED = SEA_COMMITTED
-- Every substate of a live plan. These get a SMALLER surcharge than the
-- committed ones, but they need one: a plan with resource legs prices at ~25,
-- which loses to routine goals, and the bot then wanders off mid-leg and comes
-- back to start again (tests/sea_pills_D milled around a base for 2000 ticks
-- with the refuel leg nominally active). Once a harvest has started, finishing
-- it is worth more than re-deciding it every replan.
local SEA_ENGAGED = {
  refuel_mines = true, seek_trees = true, approach_F = true,
  lay_mine = true, detonate = true, build_boat = true,
  board = true, collect = true,
}
M.SEA_ENGAGED = SEA_ENGAGED

-- Rate-limited SEA_* logging (one line per (key, message) per
-- SEA_PILL_LOG_PERIOD ticks) so a stable reject doesn't print 50x/second.
local function sea_log(state, key, msg)
  local now = state.tick or 0
  state._sea_log = state._sea_log or {}
  local last = state._sea_log[key]
  if last and (now - last) < (C.SEA_PILL_LOG_PERIOD or 50) then return end
  state._sea_log[key] = now
end

-- Does a shell fired from (owx,owy) world units at tile (tmx,tmy) actually
-- REACH that tile? cpf.simulate_shot walks the real shell physics but is
-- purely geometric — it reports the tiles crossed and stops for nothing — so
-- terrain and object collision are applied here, exactly as shells.c does:
-- the shell dies on the first impassable tile (building / half-building /
-- forest / boat) or the first LIVE pill or base. A DEAD pill does not block
-- (it is rubble the shell flies over), which is what lets us test a line onto
-- the dead sea pills at all.
-- Returns reached (bool), stop_mx, stop_my (the tile that ate it, or nil when
-- the shell simply ran out of range).
local function sea_shot_reaches(world, owx, owy, tmx, tmy, shooter)
  if owx == U.m2w(tmx) and owy == U.m2w(tmy) then return true, tmx, tmy end
  local ok, tiles = pcall(cpf.simulate_shot, owx, owy, U.m2w(tmx), U.m2w(tmy),
                          shooter or cpf.SHOT_PILL, 0)
  if not ok or not tiles then return false, nil, nil end
  local omx = bit.rshift(owx, 8)
  local omy = bit.rshift(owy, 8)
  for i = 1, #tiles do
    local st = tiles[i]
    if st.mx == tmx and st.my == tmy then return true, tmx, tmy end
    if st.mx ~= omx or st.my ~= omy then
      local tt = U.ttype(st.mx, st.my)
      if SEA_SHOT_STOPPERS[tt] then return false, st.mx, st.my end
      local plist = world.pill_at and world.pill_at[st.my * 256 + st.mx]
      if plist then
        for _, pe in ipairs(plist) do
          if pe.pill and (pe.pill.health or 0) > 0 and not pe.pill.in_tank then
            return false, st.mx, st.my
          end
        end
      end
      if world.base_at and world.base_at[st.my * 256 + st.mx] then
        return false, st.mx, st.my
      end
    end
  end
  return false, nil, nil   -- ran out of range without reaching
end
M.sea_shot_reaches = sea_shot_reaches

-- Every LIVE hostile/neutral pill within `range` tiles of ANY tile in
-- `tiles` ({{mx,my},...}). Sorted by pill id — this list decides rejections,
-- so its order must not depend on pairs().
local function sea_threat_pills(world, tiles, range)
  local ids = {}
  for pid, p in pairs(world.pills) do
    if (p.owner == "hostile" or p.owner == "neutral")
       and (p.health or 0) > 0 and not p.in_tank then
      for _, t in ipairs(tiles) do
        -- EUCLIDEAN. A pillbox's 8-tile reach is a circle; Manhattan calls a
        -- diagonal neighbour distance 2, so an mdist test silently drops the
        -- whole diagonal band a pill really can shoot. On the DH-Oil Rig
        -- incident that is the difference between seeing hostile pill #10 at
        -- (141,114) — 7.8 tiles from the cluster, inside its range — and not
        -- seeing it at all (Manhattan 11).
        if U.edist(p.mx, p.my, t[1], t[2]) <= range then
          ids[#ids + 1] = pid
          break
        end
      end
    end
  end
  table.sort(ids)
  local out = {}
  for i = 1, #ids do out[i] = { id = ids[i], pill = world.pills[ids[i]] } end
  return out
end
-- Exported so the LAND capture-cluster guard scan (capture_cluster_refresh,
-- defined earlier in this file) can run the IDENTICAL coverage test the sea
-- harvest uses instead of keeping a second copy of it.
M.sea_threat_pills = sea_threat_pills

-- First threatening pill with a clear line onto (mx,my), or nil.
-- `rays` (optional) collects every ray for the overlay.
-- Can pill P actually SHOOT the boat on tile (mx,my)? Two independent facts,
-- cheapest first:
--   1. TARGETING RANGE. pillbox.c only fires at what utilIsItemInRange accepts:
--      euclidean world-unit distance <= PILLBOX_RANGE (2048), inclusive
--      (util.c: `distance >= 0 && distance <= range`). That is 8 tiles, and it
--      is a CIRCLE — the old code skipped this test entirely and treated
--      anything the shell geometry could reach as covered, which condemned a
--      whole raft for one member that happened to sit on the 8-tile line.
--      Measured to the tile CENTRE, with a margin that depends on how HOT the
--      pill is. A calm pill reloads slowly and has to notice us first, so we
--      will sail right up to its 8-tile line: margin 0. A heated one is already
--      firing on a short reload, so the boat keeps a tile of buffer:
--      SEA_COVER_MARGIN_HOT_WU. (The tile's near corner is 181 wu closer than
--      its centre, so even the calm rule is not quite exact — that slack is
--      deliberate: a boat one shell from death should not be priced on a
--      knife edge, and the heated case is where it actually matters.)
--   2. LINE OF FIRE: the shell actually arrives (sea_shot_reaches).
-- Returns covered (bool), and the limit + hot flag used, so the row and the
-- overlay can say WHICH rule applied to WHICH pill.
local function sea_cover_limit(p)
  local hot = (p.anger or 0) >= (C.HEATED_ANGER or 0.6)
  local base = C.PILLBOX_RANGE_WU or 2048
  return base + (hot and (C.SEA_COVER_MARGIN_HOT_WU or 256)
                     or (C.SEA_COVER_MARGIN_CALM_WU or 0)), hot
end
M.sea_cover_limit = sea_cover_limit

local function sea_pill_covers(world, tp, mx, my, rays)
  local p = tp.pill
  local dwx = U.m2w(p.mx) - U.m2w(mx)
  local dwy = U.m2w(p.my) - U.m2w(my)
  local d = math.sqrt(dwx * dwx + dwy * dwy)
  local lim, hot = sea_cover_limit(p)
  if d > lim then
    if rays then
      rays[#rays + 1] = { pmx = p.mx, pmy = p.my, tmx = mx, tmy = my,
                          reached = false, out_of_range = true,
                          dist_wu = d, lim_wu = lim, hot = hot, id = tp.id }
    end
    return false
  end
  local reached, sx, sy = sea_shot_reaches(world, U.m2w(p.mx), U.m2w(p.my),
                                           mx, my, cpf.SHOT_PILL)
  if rays then
    rays[#rays + 1] = { pmx = p.mx, pmy = p.my, tmx = mx, tmy = my,
                        reached = reached, smx = sx, smy = sy,
                        dist_wu = d, lim_wu = lim, hot = hot, id = tp.id }
  end
  return reached
end
M.sea_pill_covers = sea_pill_covers

local function sea_tile_covered(world, threats, mx, my, rays)
  for _, tp in ipairs(threats) do
    if sea_pill_covers(world, tp, mx, my, rays) then return tp.id end
  end
  return nil
end

-- =========================================================================
-- ARMOUR-AWARE BASE PRICING + IMMINENT BASE STEAL
--
-- See the constants.lua block of the same name for the engine facts these
-- rest on.  The short version:
--   * a base runs 0..90 armour and is capturable at <= 9; one shell is 5,
--     so a full base is 17 shells from falling;
--   * world.bases[id].health is FOGGED for a hostile base -- it is 1 (alive)
--     or 0 (capturable), never an armour number;
--   * the real armour arrives as EVENT_BASE_STOCK, which bots receive for
--     every base on the map, and world.lua parks it in obs_armour/obs_tick.
-- Everything below reads obs_armour/obs_tick, never health.
-- =========================================================================

-- Shells still needed to drive `armour` down to capturable.
local function base_shells_to_kill(armour)
  local over = (armour or C.BASE_FULL_ARMOUR) - C.BASE_CAPTURE_ARMOUR
  if over <= 0 then return 0 end
  return math.ceil(over / C.BASE_SHELL_DAMAGE)
end

-- Last observed engine armour for a base + how old that reading is.
-- Returns nil, math.huge when the base has never reported its stock.
local function base_armour_obs(b, now)
  local a, t = b.obs_armour, b.obs_tick
  if a == nil or t == nil or (now or 0) <= 0 then return nil, math.huge end
  local age = now - t
  if age < 0 then age = 0 end
  return a, age
end

-- The armour-aware attack_base markup.  Returns
--   markup   the number that replaces the flat ATTACK_BASE_EXTRA_COST
--   n        shells still needed (nil when never observed)
--   n_full   shells needed from FULL armour (the denominator, 17)
--   frac     the fraction actually charged, AFTER the staleness decay
--   armour   the observed engine armour (nil when never observed)
--   age      ticks since that observation
--   decay    0 = reading is this tick, 1 = fully aged out to the flat price
-- A never-observed base pays the full flat price, and no decay can price a
-- base BELOW what its fresh reading would have cost.
local function base_markup(b, now)
  local full   = C.ATTACK_BASE_EXTRA_COST
  local n_full = C.BASE_FULL_SHELLS_TO_KILL
  local a, age = base_armour_obs(b, now)
  if a == nil or (a <= 0 and (b.health or 0) > 0) then
    return full, nil, n_full, 1.0, nil, math.huge, 1.0
  end
  local n = base_shells_to_kill(a)
  local frac = n / n_full
  if frac > 1 then frac = 1 end
  local win = C.BASE_MARKUP_STALE or 500
  local decay = (win > 0) and math.min(1.0, age / win) or 1.0
  local eff = frac + (1.0 - frac) * decay
  if eff > 1 then eff = 1 end
  if eff < frac then eff = frac end
  return full * eff, n, n_full, eff, a, age, decay
end

-- The tiles of the straight tank->base approach, endpoints included.  Used by
-- the steal's coverage guard: a base we can only reach by driving down a
-- pillbox's line of fire is not a free base.
local function base_approach_tiles(tmx, tmy, bmx, bmy)
  local dx, dy = bmx - tmx, bmy - tmy
  local n = math.max(math.abs(dx), math.abs(dy))
  if n < 1 then return { { tmx, tmy } } end
  local tiles = {}
  for i = 0, n do
    tiles[#tiles + 1] = { tmx + math.floor(dx * i / n + 0.5),
                          tmy + math.floor(dy * i / n + 0.5) }
  end
  return tiles
end

-- base_friendly_cover -- the FRIENDLY mirror of the steal's coverage guard.
-- refresh_base_steal rejects a base we could only reach down a hostile pill's
-- line of fire; nothing did the opposite sum. Our own (or an ally's) LIVE
-- deployed pill within PILL_FIRE_RANGE of the base tile, or of any tile on the
-- straight approach we would drive down, is already doing part of this errand:
-- it shells the base, and it shoots the tanks that come to defend it. Each one
-- multiplies the ENGAGE half of the candidate cost (markup + threat) by
-- ATTACK_BASE_FRIENDLY_COVER_MULT, floored at ATTACK_BASE_FRIENDLY_COVER_FLOOR.
-- TRAVEL is deliberately untouched -- a friendly pill does not shorten the drive.
--
-- Range test only, no line-of-fire shot sim (which is what the hostile guard
-- spends its time on): this is a discount, not a veto, so over-counting is the
-- safe direction. Euclidean, like every other pillbox-reach test in this file --
-- PILLBOX_RANGE is a circle and mdist drops the whole diagonal band.
-- Returns (mult, n_covering).
local function base_friendly_cover(world, base, tmx, tmy)
  if not (world and world.pills and base) then return 1.0, 0 end
  local rng = C.PILL_FIRE_RANGE or 8
  local tiles = (tmx and tmy)
                and base_approach_tiles(tmx, tmy, base.mx, base.my)
                or { { base.mx, base.my } }
  local n = 0
  for _, q in pairs(world.pills) do
    if (q.owner == "friendly" or q.owner == "allied") and (q.health or 0) > 0
       and not (q.in_tank or q.carrier or q._synth_carry) then
      local hit = U.edist(q.mx, q.my, base.mx, base.my) <= rng
      if not hit then
        for i = 1, #tiles do
          local t = tiles[i]
          if U.edist(q.mx, q.my, t[1], t[2]) <= rng then hit = true break end
        end
      end
      if hit then n = n + 1 end
    end
  end
  if n == 0 then return 1.0, 0 end
  local m = (C.ATTACK_BASE_FRIENDLY_COVER_MULT or 0.8) ^ n
  local fl = C.ATTACK_BASE_FRIENDLY_COVER_FLOOR or 0.5
  if m < fl then m = fl end
  return m, n
end

-- refresh_base_steal -- pick at most ONE hostile base to steal, and record why
-- every other hostile base was passed over so the pool grid can say so.
--
-- Gates, in the order they are reported:
--   stale_obs       no armour reading, or one older than BASE_STEAL_OBS_STALE
--   not_capturable  fresh armour above BASE_STEAL_MAX_ARMOUR (more than three
--                   shells from falling) -- a normal attack_base errand
--   out_of_range    further than BASE_STEAL_RANGE tiles away
--   no_shells       shells <= SHELL_RESERVE + shells-still-needed
--   covered         a live hostile/neutral pillbox can put a shell on the base
--                   tile or on our straight approach to it
-- Whole-tank gates (inboat / tank armour) reject every candidate at once.
-- Deterministic: bases are walked in sorted-id order and the pick is the
-- NEAREST survivor, ties going to the lower id (the sorted walk gets there
-- first and the strict `<` keeps it).
local function refresh_base_steal(state, world, info)
  if not (world and world.bases and info and info.tankx) then return end
  -- Both call sites (build_eval_queue at the start of the cycle, finalize_pools
  -- at the decision tick) can land on the SAME tick when the eval cycle is
  -- short. The coverage guard runs shot sims, so only do the work once.
  local _now = state.tick or 0
  if state._base_steal_tick == _now then return end
  state._base_steal_tick = _now
  state.base_steal = nil
  state.base_steal_rejects = {}
  state.base_steal_why = nil
  local rej = state.base_steal_rejects
  local now = state.tick or 0
  local tmx, tmy = bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8)

  local ids = {}
  for id, b in pairs(world.bases) do
    if b.owner == "hostile" and (b.health or 0) > 0 then ids[#ids + 1] = id end
  end
  table.sort(ids)

  -- Whole-tank vetoes.  Still walk the bases so every row gets a reason.
  local veto = nil
  if info.inboat then
    veto = "inboat"
  elseif (info.armour or 0) < (C.BASE_STEAL_MIN_ARMOUR or 8) then
    veto = "tank_armour"
  end
  state.base_steal_why = veto

  local best, best_d = nil, nil
  local rows = BRAIN_DEBUG_MODE and {} or nil
  for _, id in ipairs(ids) do
    local b = world.bases[id]
    local a, age = base_armour_obs(b, now)
    local d = U.mdist(tmx, tmy, b.mx, b.my)
    -- n comes back from base_markup so the row's `n` and its `markup` can never
    -- disagree -- base_markup is the one place that decides a reading is not
    -- usable (never observed, or the EVENT_BASE_UPDATE armour-0 artefact) and
    -- returns n = nil for it.
    local mk, n = base_markup(b, now)
    local reason, cover_id = veto, nil
    if not reason then
      if a == nil or age > (C.BASE_STEAL_OBS_STALE or 1200)
         or a <= 0 then
        -- a <= 0 while the object scan still calls the base ALIVE is the
        -- EVENT_BASE_UPDATE artefact described above base_markup, not a real
        -- reading. (This branch only sees standing bases: the id list above
        -- filters on health > 0.)
        reason = "stale_obs"
      elseif d > (C.BASE_STEAL_RANGE or 6) then
        reason = "out_of_range"
      elseif a > (C.BASE_STEAL_MAX_ARMOUR or 24) then
        reason = "not_capturable"
      elseif (info.shells or 0) <= (C.SHELL_RESERVE or 0) + n then
        reason = "no_shells"
      else
        -- Coverage guard last: it is the only expensive test, and by here at
        -- most a base or two can still reach it.
        local tiles = base_approach_tiles(tmx, tmy, b.mx, b.my)
        local threats = sea_threat_pills(world, tiles, C.BASE_STEAL_COVER_RANGE or 9)
        for _, tp in ipairs(threats) do
          local hit = nil
          for _, t in ipairs(tiles) do
            if sea_pill_covers(world, tp, t[1], t[2], nil) then hit = tp.id break end
          end
          if hit then cover_id = hit break end
        end
        if cover_id then reason = "covered" end
      end
    end
    rej[id] = reason and { reason = reason, armour = a, age = age, dist = d,
                           need = n, markup = mk, pill = cover_id } or nil
    if not reason then
      if (not best) or d < best_d then
        best, best_d = { id = id, mx = b.mx, my = b.my, armour = a, age = age,
                         need = n, dist = d, markup = mk }, d
      end
    end
    if rows then
      rows[#rows + 1] = string.format(
        "base#%s@(%d,%d) arm=%s(age=%s) n=%s/%d markup=%.1f d=%d %s",
        tostring(id), b.mx, b.my,
        a and tostring(a) or "?", (age == math.huge) and "never" or tostring(age),
        n and tostring(n) or "?", C.BASE_FULL_SHELLS_TO_KILL, mk, d,
        reason and ("REJECT " .. reason .. (cover_id and ("(pill#" .. tostring(cover_id) .. ")") or ""))
                or "STEAL")
    end
  end
  if rows and #rows > 0 then
  end
  state.base_steal = best
end
M.refresh_base_steal = refresh_base_steal

-- A visible enemy tank within `range` of any tile in `tiles`.
local function sea_enemy_tank_near(state, tiles, range)
  local ets = state.perc and state.perc.enemy_tanks
  if not ets then return nil end
  for _, et in ipairs(ets) do
    for _, t in ipairs(tiles) do
      -- Euclidean for the same reason as sea_threat_pills: this is a physical
      -- reach, not a walking distance.
      if U.edist(et.mx, et.my, t[1], t[2]) <= range then return et end
    end
  end
  return nil
end

-- Is (mx,my) free of live pills and of any base?
local function sea_tile_object_free(world, mx, my)
  local plist = world.pill_at and world.pill_at[my * 256 + mx]
  if plist then
    for _, pe in ipairs(plist) do
      if pe.pill and (pe.pill.health or 0) > 0 and not pe.pill.in_tank then
        return false
      end
    end
  end
  if world.base_at and world.base_at[my * 256 + mx] then return false end
  return true
end

-- Firing spot F for a mine at S: a standable tile exactly SEA_PILL_FIRE_DIST
-- (2) tiles away on a straight cardinal or diagonal line, so the centre-to-
-- centre distance is 512 wu on at least one axis and the 384 wu mine blast
-- box (tank.c tankMineDamage: |dx| < 384 AND |dy| < 384) cannot reach us.
-- A BUILDING between eats the shell for good; a FOREST between only costs one
-- extra shell, so it is allowed. Forest UNDER F is preferred: pillboxes skip
-- a tank hidden in trees (pillbox.c:388), and we are standing still to shoot.
local function sea_pick_F(world, smx, smy, threats)
  local d = C.SEA_PILL_FIRE_DIST or 2
  local best, best_key = nil, nil
  for i = 1, 8 do
    local fx = smx + SEA_D8[i][1] * d
    local fy = smy + SEA_D8[i][2] * d
    if U.in_map(fx, fy) then
      local tt = U.ttype(fx, fy)
      if SEA_STANDABLE[tt] and sea_tile_object_free(world, fx, fy)
         and bit.band(U.traw(fx, fy), TERRAIN_MINE_FLAG) == 0 then
        -- Nothing solid in the lane. Only tiles strictly between count.
        local blocked = false
        for step = 1, d - 1 do
          local bx = smx + SEA_D8[i][1] * step
          local by = smy + SEA_D8[i][2] * step
          local btt = U.ttype(bx, by)
          if btt == C.T_BUILDING or btt == C.T_HALFBUILD
             or not sea_tile_object_free(world, bx, by) then
            blocked = true
            break
          end
        end
        if not blocked then
          local hot = threat.pill_at(fx, fy) or 0
          -- Lower is better. Forest is worth a big discount (it hides us);
          -- pill heat on the spot is the main cost. The tile key makes ties
          -- a total order so the pick is deterministic.
          local key = hot - (tt == C.T_FOREST and 100 or 0)
          local exposed = sea_tile_covered(world, threats, fx, fy, nil)
          if exposed then key = key + 200 end
          if best_key == nil or key < best_key
             or (key == best_key and (fy * 256 + fx) < (best.my * 256 + best.mx)) then
            best_key = key
            best = { mx = fx, my = fy, tt = tt, hot = hot, exposed = exposed }
          end
        end
      end
    end
  end
  return best
end

-- Count forest tiles we could actually harvest for the boat. Gates mirror
-- find_safe_forest (the seek_trees scan): pill heat below the bad-ground line,
-- threat.at within SEEK_TREES_MAX_THREAT, NOT COVERED BY AN ENEMY PILLBOX —
-- plus the tank being able to get there at all. Scanned in a box around S; a
-- tile also qualifies when it is close to the TANK, because the harvest
-- happens on the way. Scan order is a plain row-major sweep, so the count is
-- deterministic. Returns (count, nearest_mx, nearest_my, covered_count) —
-- "nearest" measured from the TANK, with the tile key as a deterministic
-- tie-break. The whole box is scanned (no early exit) so the nearest tile is
-- really the nearest, which is what the seek_trees substate parks on; the box
-- is at most 25x25 and sea_refresh only runs it every SEA_PILL_SCAN_PERIOD
-- ticks.
--
-- COVERAGE, NOT INFLUENCE (2026-09-01). This used to require
-- cpf.influence_at > SEA_TREES_MIN_INFLUENCE — "our side of the front by the
-- numbers". par2 bot3 t=23410: pill #9 lay dead in the water at (112,142) with
-- the tank right beside it at (114,139) holding 11/21 trees, and forest was
-- everywhere — the shore strip at x~108-110 and a whole block at y~142-145 —
-- but two hostile BASES at (109,137) and (115,143) owned that corner by
-- influence, so every tile was rejected and the row read
-- no_trees_in_territory for the rest of the game. We were in fact raiding
-- that corner with four pills aboard. Influence is a bookkeeping fact about
-- who owns the neighbourhood; what actually kills an LGM walking out for wood
-- is a PILLBOX with a line on the tile. So the gate is now exactly that, and
-- it is the same per-tile rule the boat already uses for water:
-- sea_pill_covers — within PILLBOX_RANGE (inclusive, euclidean, plus the
-- heated margin) AND the shell actually arrives.
local function sea_count_safe_forest(state, world, info, smx, smy, tmx, tmy)
  local R    = C.SEA_TREES_RADIUS or 12
  local BAD  = C.TAKE_COVER_BAD_GROUND_PILL_AT or C.TANK_COMBAT_DEFENDED_DANGER or 30
  local MAXT = C.SEEK_TREES_MAX_THREAT or 8
  -- Every live hostile/neutral pill that could cover ANY tile of the box: a
  -- qualifying tile is within R of S or of the tank, and a pill covering that
  -- tile is within SEA_PILL_PILL_SAFE_RANGE of it, so R + that reach around
  -- either centre is a superset. Sorted by id inside sea_threat_pills.
  local threats = sea_threat_pills(world, { { smx, smy }, { tmx, tmy } },
                                   R + (C.SEA_PILL_PILL_SAFE_RANGE or 9))
  -- Per-tile coverage memo, keyed on the threat set itself (id, tile and hot
  -- flag — the hot flag moves the margin). Anything that changes the answer
  -- changes the key, so a stale verdict cannot survive a pill dying, moving or
  -- heating up. Within one refresh the memo also stops two clusters that share
  -- a shore from paying for the same shot sims twice.
  local sigp = {}
  for i = 1, #threats do
    local tp = threats[i]
    sigp[i] = string.format("%d:%d,%d,%d", tp.id, tp.pill.mx, tp.pill.my,
      ((tp.pill.anger or 0) >= (C.HEATED_ANGER or 0.6)) and 1 or 0)
  end
  local sig = table.concat(sigp, ";")
  local memo = state._sea_forest_cover
  if not memo or memo.sig ~= sig then
    memo = { sig = sig, c = {} }
    state._sea_forest_cover = memo
  end
  local n, covered_n = 0, 0
  local bx, by, bd = nil, nil, 1e9
  for my = math.max(0, smy - R), math.min(255, smy + R) do
    for mx = math.max(0, smx - R), math.min(255, smx + R) do
      if U.mdist(mx, my, smx, smy) <= R or U.mdist(mx, my, tmx, tmy) <= R then
        if U.ttype(mx, my) == C.T_FOREST then
          -- COVERAGE FIRST, then the heat/threat gates. Order matters only for
          -- ATTRIBUTION, not for the verdict: the two sets a covered tile could
          -- fall in overlap almost completely (a pillbox close enough to shoot
          -- a tile has usually already pushed threat.at past
          -- SEEK_TREES_MAX_THREAT). Asking the coverage question first is what
          -- lets the reject say "found 8 covered_by_pill" instead of the
          -- uninformative "found 0 forest tiles".
          local k = my * 256 + mx
          local cov = memo.c[k]
          if cov == nil then
            -- sea_pill_covers range-tests before it simulates, so a tile with
            -- no pill anywhere near it costs a few subtractions.
            cov = sea_tile_covered(world, threats, mx, my, nil) or false
            memo.c[k] = cov
          end
          if cov then
            covered_n = covered_n + 1
          else
            local hot = threat.pill_at(mx, my) or 0
            if hot < BAD and (threat.at(mx, my) or 0) <= MAXT then
              local tc = cpf.smart_cost_dij_only(KIND_NORMAL, mx, my, 0)
              -- INF only means "the slate has not expanded here yet"; a forest
              -- tile this close to a shore we can already reach is not really
              -- unreachable, so fall back to the geometric test.
              if (tc and tc < 1e29) or U.mdist(mx, my, tmx, tmy) <= R then
                n = n + 1
                local d = U.mdist(mx, my, tmx, tmy)
                if d < bd or (d == bd and (my * 256 + mx) < ((by or 0) * 256 + (bx or 0))) then
                  bd = d; bx = mx; by = my
                end
              end
            end
          end
        end
      end
    end
  end
  -- One line per scan (the scan itself is on sea_refresh's 50-tick cadence).
  -- infl is REPORTED, never read by the decision: it is the number the retired
  -- SEA_TREES_MIN_INFLUENCE gate used to test, kept in the log so a run can
  -- show a harvest going ahead on ground the old rule called enemy territory.
  return n, bx, by, covered_n
end

-- The connected body of water the cluster floats in. BFS out from the pill
-- tiles over water only (DEEP_SEA / RIVER / BOAT), 4-connected — the same
-- adjacency floodfill.c uses when it decides whether a crater becomes river.
-- The pill tiles themselves read T_PILLBOX in the brain map (the overlay hides
-- the terrain under a pill), so they are seeded as water explicitly.
-- Bounded by a box around the cluster and a hard tile cap so an ocean map
-- cannot turn this into a map-wide flood every rescan.
-- Returns a set keyed by my*256+mx, and the tile count.
local function sea_water_component(cl)
  local R   = C.SEA_COMPONENT_BOX or 40
  local CAP = C.SEA_COMPONENT_MAX_TILES or 4000
  local x0, x1, y0, y1 = 255, 0, 255, 0
  for _, t in ipairs(cl.tiles) do
    if t[1] < x0 then x0 = t[1] end
    if t[1] > x1 then x1 = t[1] end
    if t[2] < y0 then y0 = t[2] end
    if t[2] > y1 then y1 = t[2] end
  end
  x0 = math.max(0, x0 - R); x1 = math.min(255, x1 + R)
  y0 = math.max(0, y0 - R); y1 = math.min(255, y1 + R)
  local seen, n = {}, 0
  local q, qh = {}, 1
  for _, t in ipairs(cl.tiles) do
    local k = t[2] * 256 + t[1]
    if not seen[k] then seen[k] = true; n = n + 1; q[#q + 1] = { t[1], t[2] } end
  end
  while qh <= #q and n < CAP do
    local cur = q[qh]; qh = qh + 1
    for _, d in ipairs(SEA_CARD) do
      local nx2, ny2 = cur[1] + d[1], cur[2] + d[2]
      if nx2 >= x0 and nx2 <= x1 and ny2 >= y0 and ny2 <= y1 then
        local k = ny2 * 256 + nx2
        if not seen[k] then
          local tt = U.ttype(nx2, ny2)
          if tt == C.T_DEEPSEA or tt == C.T_RIVER or tt == C.T_BOAT then
            seen[k] = true
            n = n + 1
            q[#q + 1] = { nx2, ny2 }
          end
        end
      end
    end
  end
  return seen, n
end

-- Every tile of the cluster's water a threatening pill can actually shoot.
-- Walked per THREAT rather than per tile: each pill only reaches a disc of
-- radius PILLBOX_RANGE, so stamping outward from the pills costs a few hundred
-- shot sims instead of one per tile of a 1200-tile component.
-- Returns { [key] = pill_id } and the count.
local function sea_covered_water(world, comp, threats)
  local cov, n = {}, 0
  for _, tp in ipairs(threats) do
    local p = tp.pill
    local R = math.ceil(select(1, sea_cover_limit(p)) / 256)
    for my = math.max(0, p.my - R), math.min(255, p.my + R) do
      for mx = math.max(0, p.mx - R), math.min(255, p.mx + R) do
        local k = my * 256 + mx
        if comp[k] and not cov[k] and sea_pill_covers(world, tp, mx, my, nil) then
          cov[k] = tp.id
          n = n + 1
        end
      end
    end
  end
  return cov, n
end

-- Shortest boat route from (sx,sy) to (dx,dy) across the cluster's water,
-- treating covered tiles as IMPASSABLE. 8-connected: a boat moves diagonally,
-- and the route is what the collect drive will follow, so a path that clips a
-- covered tile is a path that gets us shot. Returns a tile list (source first,
-- destination last) or nil when every route is covered.
-- BFS, not Dijkstra: every water step costs the same here, and the search is
-- bounded by the component.
-- Is this tile navigable by boat RIGHT NOW? Live terrain, not the plan-time
-- component: the entrance was dry land when the component was built, so the
-- tile the tank is floating on after boarding is not in it. A dead pill reads
-- T_PILLBOX (the overlay hides the water under it) and is exactly where we are
-- driving, so it counts as water too.
local function sea_is_water(world, mx, my)
  if not U.in_map(mx, my) then return false end
  local tt = U.ttype(mx, my)
  if tt == C.T_DEEPSEA or tt == C.T_RIVER or tt == C.T_BOAT then return true end
  if tt == C.T_PILLBOX then
    local lst = world.pill_at and world.pill_at[my * 256 + mx]
    if lst then
      for _, e in ipairs(lst) do
        if e.pill and (e.pill.health or 0) == 0 then return true end
      end
    end
  end
  return false
end
M.sea_is_water = sea_is_water

-- Returns the tile list, and on failure a reason plus the number of nodes
-- expanded, so a refusal can be logged instead of silently dropping a member.
local function sea_water_route(world, comp, cov, sx, sy, dx, dy)
  local skey, dkey = sy * 256 + sx, dy * 256 + dx
  -- The DESTINATION must be water and uncovered. The START is wherever the
  -- tank already is: if it is somehow standing on a covered tile, the answer is
  -- to route OUT of it, not to refuse to move.
  if not sea_is_water(world, dx, dy) then return nil, "dest_not_water", 0 end
  if cov[dkey] then return nil, "dest_covered", 0 end
  if skey == dkey then return { { sx, sy } }, nil, 0 end
  -- Box the search: the raft and its entrance are close together, and an
  -- unbounded BFS over open ocean is not worth the ticks.
  local R = C.SEA_COMPONENT_BOX or 16
  local x0, x1 = math.min(sx, dx) - R, math.max(sx, dx) + R
  local y0, y1 = math.min(sy, dy) - R, math.max(sy, dy) + R
  local expanded = 0
  local prev = { [skey] = false }
  local q, qh = { { sx, sy } }, 1
  while qh <= #q do
    local cur = q[qh]; qh = qh + 1
    expanded = expanded + 1
    for i = 1, 8 do
      local nx2 = cur[1] + SEA_D8[i][1]
      local ny2 = cur[2] + SEA_D8[i][2]
      local k = ny2 * 256 + nx2
      if nx2 >= x0 and nx2 <= x1 and ny2 >= y0 and ny2 <= y1
         and not cov[k] and prev[k] == nil
         and sea_is_water(world, nx2, ny2) then
        prev[k] = cur[2] * 256 + cur[1]
        if k == dkey then
          local out, ck = {}, dkey
          while ck do
            out[#out + 1] = { ck % 256, __idiv(ck, 256) }
            ck = prev[ck]
          end
          for a = 1, __idiv(#out, 2) do
            out[a], out[#out + 1 - a] = out[#out + 1 - a], out[a]
          end
          return out, nil, expanded
        end
        q[#q + 1] = { nx2, ny2 }
      end
    end
  end
  return nil, "no_open_route", expanded
end
M.sea_water_route = sea_water_route

-- The land tile a tank drives to in order to step onto a river/boat entrance,
-- and that the LGM builds the boat from. Cheapest-looking standable neighbour
-- of the water tile; ties broken by tile key so the pick is deterministic.
local function sea_pick_board_from(world, wmx, wmy)
  local best, best_key = nil, nil
  for i = 1, 8 do
    local nx2, ny2 = wmx + SEA_D8[i][1], wmy + SEA_D8[i][2]
    if U.in_map(nx2, ny2) then
      local tt = U.ttype(nx2, ny2)
      if SEA_STANDABLE[tt] and sea_tile_object_free(world, nx2, ny2)
         and bit.band(U.traw(nx2, ny2), TERRAIN_MINE_FLAG) == 0 then
        local key = threat.pill_at(nx2, ny2) or 0
        if best_key == nil or key < best_key
           or (key == best_key and (ny2 * 256 + nx2) < (best.my * 256 + best.mx)) then
          best_key = key
          best = { mx = nx2, my = ny2, tt = tt, hot = key }
        end
      end
    end
  end
  return best
end

-- Is this dead sea pill inside a live hostile pill's firing circle, with a
-- clear shell line? Standalone (no plan needed) because the answer matters
-- whether or not we are already afloat: a boat that sails into a pillbox's
-- circle dies exactly the same either way, and the harvest plan only exists
-- while the tank is ashore. Returns the covering pill's id, or nil.
local function sea_dead_pill_covered(world, p)
  local threats = sea_threat_pills(world, { { p.mx, p.my } },
                                   C.SEA_PILL_PILL_SAFE_RANGE or 9)
  if #threats == 0 then return nil end
  return sea_tile_covered(world, threats, p.mx, p.my, nil)
end
M.sea_dead_pill_covered = sea_dead_pill_covered

-- Cluster the dead deep-sea pills. Greedy over SORTED ids so the clustering
-- (and therefore every cluster id in the log and the panel) is deterministic.
local function sea_build_clusters(state, world)
  local perc = state.perc
  if not perc or not perc.deepsea_pill_ids then return {}, {} end
  local ids = {}
  for pid in pairs(perc.deepsea_pill_ids) do
    local p = world.pills[pid]
    if p and (p.health or 0) == 0 and not p.in_tank and not p.carrier
       and not p._synth_carry then
      ids[#ids + 1] = pid
    end
  end
  -- Same greedy-over-sorted-ids union rule the LAND capture discount uses;
  -- one copy, in build_pill_clusters above.
  local clusters, by_pill = build_pill_clusters(world, ids,
                                                C.SEA_PILL_CLUSTER_RADIUS or 3)
  for _, cl in ipairs(clusters) do
    -- LEAD pill: the lowest id in the cluster. One boat trip takes the whole
    -- cluster, so the cluster is ONE goal identity — only the lead is priced
    -- and can win; the others ride along as visible `cluster_member_of` rows.
    -- Without this the three members priced identically, the pool tie-break
    -- flipped the target every replan, and each flip restarted the plan at
    -- entrance_plan (~850 ticks lost before the first mine on sea_pills_A).
    cl.lead = cl.ids[1]
  end
  return clusters, by_pill
end

-- Price ONE cluster: safety of the pills, the entrance S, the firing spot F,
-- the boat path, the resource legs, and the final cost. Fills cl in place.
local function sea_plan_cluster(state, world, info, cl, tmx, tmy)
  local now = state.tick or 0
  cl.cands = {}
  cl.rays  = {}
  local function rej(reason, detail)
    cl.reject = reason
    cl.reject_detail = detail
    return cl
  end

  -- ── Preconditions on us ────────────────────────────────────────────────
  -- Only a DEAD LGM kills the plan. An LGM merely out on a job (LGM_MOVING —
  -- farming, roading) comes back in a few seconds, and rejecting on that made
  -- the row flicker between a price and a REJECT every rescan, churning the
  -- goal. The dispatches themselves already wait for him: builder.decide bails
  -- with lgm_not_in_tank, and the substate machine only leaves lay_mine /
  -- build_boat once he is aboard again.
  if info.man_status == C.LGM_DEAD then
    return rej("lgm_dead", "the LGM lays the mine and builds the boat, and he is dead")
  end
  -- ── Are the pills themselves safe to sit beside in a boat? ─────────────
  local safe_range = C.SEA_PILL_PILL_SAFE_RANGE or 9
  local threats = sea_threat_pills(world, cl.tiles, safe_range)
  cl.threats = threats
  -- PER PILL, not per cluster. A raft can straddle a pillbox's 8-tile line:
  -- on tests/sea_pills_B the middle pill sat exactly 8.0 tiles from a hostile
  -- pill and the two outer ones 9.2, and condemning all three for the middle
  -- one forfeited two perfectly safe pills. A covered member is DROPPED from
  -- the harvest set and gets its own `covered_by_pill#N` row; the rest still go.
  -- Only when nothing is left is the whole cluster rejected.
  cl.dropped = {}
  local keep_ids, keep_tiles = {}, {}
  local first_by = nil
  for i, pid in ipairs(cl.ids) do
    local t = cl.tiles[i]
    local by = sea_tile_covered(world, threats, t[1], t[2], cl.rays)
    if by then
      cl.dropped[pid] = by
      cl.dropped_tiles = cl.dropped_tiles or {}
      cl.dropped_tiles[pid] = { t[1], t[2], by }
      first_by = first_by or by
    else
      keep_ids[#keep_ids + 1] = pid
      keep_tiles[#keep_tiles + 1] = t
    end
  end
  if #keep_ids == 0 then
    return rej("pills_covered_by_pill#" .. tostring(first_by),
      string.format("pill #%s can put a shell on EVERY tile of the cluster — one hit sinks the boat, so there is nothing left to fetch",
                    tostring(first_by)))
  end
  -- From here on the cluster IS the surviving set: its lead, its cost split and
  -- its boat path are all about the pills we can actually reach.
  cl.ids = keep_ids
  cl.tiles = keep_tiles
  cl.n = #keep_ids
  cl.lead = keep_ids[1]
  local et = sea_enemy_tank_near(state, cl.tiles, C.SEA_PILL_ENEMY_TANK_NEAR or 10)
  if et then
    return rej("enemy_tank_near",
      string.format("enemy tank at (%d,%d) is within %d tiles of the cluster",
                    et.mx, et.my, C.SEA_PILL_ENEMY_TANK_NEAR or 10))
  end

  -- ── The cluster's WATER COMPONENT ──────────────────────────────────────
  -- A boat is only useful on the water the pills actually float in. BFS out
  -- from the pill tiles over water (DEEP_SEA / RIVER / BOAT, 4-connected —
  -- the same adjacency floodfill.c uses), bounded by a box and a tile cap so
  -- an ocean map cannot turn this into a map-wide flood. Every entrance below
  -- has to touch THIS component; a separate pond next to the tank is useless
  -- however convenient it looks.
  local comp, comp_n = sea_water_component(cl)
  cl.comp = comp
  cl.comp_n = comp_n
  -- A pill on a one-tile puddle is NOT a sea pill in any useful sense: there is
  -- no water to sail. perception flags it (rightly — it beaches a boat and
  -- strands a tank), but the answer is to leave it alone, not to mine our own
  -- shore next to a puddle. tests/blocked_aim keeps two of our pills on
  -- single-tile deep-sea pedestals exactly so they stay put; without this the
  -- harvest planned a crater beside one (comp=1) and spent the take's budget on
  -- it. Demand real water: more tiles in the component than the pills occupy.
  local min_water = C.SEA_COMPONENT_MIN_WATER or 3
  if comp_n < #cl.tiles + min_water then
    return rej("pills_landlocked", string.format(
      "the water these %d pill(s) sit in is only %d tiles — no boat can sail it (need at least %d more than the pills themselves)",
      #cl.tiles, comp_n, min_water))
  end

  -- ── Candidate water entrances S ────────────────────────────────────────
  -- Preference LADDER, not a score (the cheap options are strictly better —
  -- they skip a mine, a crater and up to 4 shells):
  --   1 existing_boat  — a boat already floating in the component: drive on.
  --   2 existing_river — build the boat straight into it (BUILDMODE_BUILD on
  --                      RIVER = a boat, lgm.c:377). 20 trees, NO mine.
  --   3 mine_crater    — mine a shore tile CARDINALLY adjacent to the
  --                      component, shoot it, let it flood, then build.
  --                      21 trees + 1 mine + shells.
  -- Only when a whole rung is empty do we drop to the next.
  local MAXD = C.SEA_PILL_ENTRANCE_MAX_DIST or 12
  local x0, x1, y0, y1 = 255, 0, 255, 0
  for _, t in ipairs(cl.tiles) do
    if t[1] < x0 then x0 = t[1] end
    if t[1] > x1 then x1 = t[1] end
    if t[2] < y0 then y0 = t[2] end
    if t[2] > y1 then y1 = t[2] end
  end
  x0 = math.max(0, x0 - MAXD); x1 = math.min(255, x1 + MAXD)
  y0 = math.max(0, y0 - MAXD); y1 = math.min(255, y1 + MAXD)
  local BAD = C.TAKE_COVER_BAD_GROUND_PILL_AT or C.TANK_COMBAT_DEFENDED_DANGER or 30
  -- Best candidate per rung, plus a flag for "something was rejected ONLY for
  -- connectivity" so the reject reason can say so.
  local best = { existing_boat = nil, existing_river = nil, mine_crater = nil }
  local best_score = { }
  local saw_unconnected = false
  for my = y0, y1 do
    for mx = x0, x1 do
      local cdist = 1e9
      for _, t in ipairs(cl.tiles) do
        local d = U.mdist(mx, my, t[1], t[2])
        if d < cdist then cdist = d end
      end
      if cdist <= MAXD and cdist > 0 then
        local tt = U.ttype(mx, my)
        local kind = nil
        if tt == C.T_BOAT then kind = "existing_boat"
        elseif tt == C.T_RIVER then kind = "existing_river"
        elseif SEA_MINABLE[tt] then kind = "mine_crater" end
        if kind then
          -- Connectivity. A water entrance must BE in the component; a land
          -- entrance must have a CARDINAL neighbour in it, because that is the
          -- only adjacency that turns the crater into river (floodfill.c).
          local wmx, wmy = nil, nil
          if kind == "mine_crater" then
            for _, d in ipairs(SEA_CARD) do
              local nx2, ny2 = mx + d[1], my + d[2]
              if comp[ny2 * 256 + nx2] then wmx, wmy = nx2, ny2 break end
            end
          elseif comp[my * 256 + mx] then
            wmx, wmy = mx, my
          end
          local touches_any_water = wmx ~= nil
          if not touches_any_water then
            -- Only record it as a candidate (grey, with a reason) when it at
            -- least touches SOME water — otherwise every inland tile in the
            -- box would be listed.
            local near_water = (kind ~= "mine_crater")
            if not near_water then
              for _, d in ipairs(SEA_CARD) do
                local t2 = U.ttype(mx + d[1], my + d[2])
                if t2 == C.T_DEEPSEA or t2 == C.T_RIVER or t2 == C.T_BOAT then
                  near_water = true
                  break
                end
              end
            end
            if near_water then
              saw_unconnected = true
              cl.cands[#cl.cands + 1] = { mx = mx, my = my, kind = kind,
                                          reject = "unconnected" }
            end
          else
            local reject = nil
            local needs_mine = (kind == "mine_crater")
            if needs_mine and bit.band(U.traw(mx, my), TERRAIN_MINE_FLAG) ~= 0 then
              reject = "mined"
            elseif not sea_tile_object_free(world, mx, my) then
              reject = "occupied"
            elseif state._sea_blacklist and (state._sea_blacklist[my * 256 + mx] or 0) > now then
              reject = "blacklisted"
            end
            local hot = 0
            if not reject then
              hot = threat.pill_at(mx, my) or 0
              if hot >= BAD then reject = "hot" end
            end
            -- Where the TANK parks while the LGM works, and where it drives
            -- from when it boards.
            --   mine_crater: the firing spot F, 2 tiles out (mine blast).
            --   river/boat : a standable land tile beside the water tile, so
            --                the LGM can reach it and the tank can step on.
            local park = nil
            if not reject then
              if needs_mine then
                park = sea_pick_F(world, mx, my, threats)
                if not park then reject = "no_F" end
              elseif tt == C.T_RIVER then
                park = sea_pick_board_from(world, mx, my)
                if not park then reject = "no_land_access" end
              else
                -- An existing BOAT tile needs no LGM work at all; we still
                -- want a land tile to approach from so the drive is sane.
                park = sea_pick_board_from(world, mx, my)
                if not park then reject = "no_land_access" end
              end
            end
            if not reject then
              if sea_tile_covered(world, threats, mx, my, nil) then reject = "lof" end
            end
            local travel = nil
            if not reject then
              -- Travel is measured to the tile the TANK actually drives to:
              -- the water tile itself is not on the land layer.
              local tx2 = needs_mine and mx or park.mx
              local ty2 = needs_mine and my or park.my
              travel = cpf.smart_cost_dij_only(KIND_NORMAL, tx2, ty2, 0)
              if not travel or travel >= 1e29 then
                -- The slate has not expanded here yet; keep the candidate with
                -- an estimate and let the winner pay for a real A*.
                travel = U.mdist(tmx, tmy, tx2, ty2) * 16
              end
            end
            if not reject
               and not danger.lgm_path_safe(info, mx, my, C.LGM_DANGER_MED or 20, now, world) then
              reject = "lgm_unsafe"
            end
            local score = nil
            if not reject then
              score = travel
                    + cdist * (C.SEA_PILL_BOAT_STEP_COST or 12)
                    + hot * (C.SEA_PILL_DANGER_W or 2.0)
            end
            cl.cands[#cl.cands + 1] = { mx = mx, my = my, reject = reject,
                                        kind = kind,
                                        score = score, travel = travel, hot = hot,
                                        needs_mine = needs_mine,
                                        fmx = park and park.mx, fmy = park and park.my }
            if not reject then
              local cur = best[kind]
              if cur == nil or score < best_score[kind]
                 or (score == best_score[kind] and (my * 256 + mx) < (cur.my * 256 + cur.mx)) then
                best_score[kind] = score
                best[kind] = { mx = mx, my = my, kind = kind,
                               needs_mine = needs_mine, F = park,
                               wmx = wmx, wmy = wmy,
                               travel = travel, hot = hot, cdist = cdist }
              end
            end
          end
        end
      end
    end
  end
  local best_s = best.existing_boat or best.existing_river or best.mine_crater
  if not best_s then
    if saw_unconnected then
      return rej("no_connected_entrance", string.format(
        "every water tile within %d tiles of the cluster belongs to a DIFFERENT water body (the cluster's component is %d tiles); a boat launched there could never reach the pills",
        MAXD, comp_n))
    end
    return rej("no_entrance", string.format(
      "nothing within %d tiles of the cluster passed the entrance gates (%d candidates scanned; no existing river/boat in the component and no minable shore tile cardinally touching it)",
      MAXD, #cl.cands))
  end
  cl.S = best_s
  cl.F = best_s.F
  -- The land tile we build the boat FROM and step aboard from. For a mine
  -- entrance the firing spot is deliberately 2 tiles back (the blast), which
  -- left a 2-tile walk between "boat exists" and "tank on boat" — and an
  -- unoccupied boat does not wait: on tests/sea_pills_D it was gone 60 ticks
  -- after it went up. Once the crater has flooded there is nothing left to be
  -- blasted by, so the tank closes to the water's edge before the LGM builds.
  cl.board_from = sea_pick_board_from(world, best_s.mx, best_s.my)
  cl.entrance = best_s.kind
  -- Why the cheaper rungs were not taken — the row says so rather than leaving
  -- "why did it mine when there was a river over there?" unanswered.
  cl.entrance_why =
      (best_s.kind == "existing_boat") and "a boat is already floating in the cluster's water"
   or (best_s.kind == "existing_river") and "no existing boat in the component; building into an existing river needs no mine"
   or string.format("no existing boat or river in the cluster's water component within %d tiles", MAXD)

  -- Real travel cost to the tile the tank drives to (the scan used the cheap
  -- slate lookup, which can be INF simply because the slate had not expanded).
  local drive_mx = best_s.needs_mine and best_s.mx or best_s.F.mx
  local drive_my = best_s.needs_mine and best_s.my or best_s.F.my
  local travel = smart_cost(KIND_NORMAL, tmx, tmy, drive_mx, drive_my, 0,
                            info.shells or 32, info.trees or 0,
                            info.mines or 0, info.armour or 40)
  if not travel or travel >= 1e29 then
    return rej("entrance_unreachable", string.format(
      "S=(%d,%d) [%s]: the tank cannot reach (%d,%d) — A* and the Dijkstra slate both INF",
      best_s.mx, best_s.my, best_s.kind, drive_mx, drive_my))
  end
  cl.travel = travel

  -- ── Is the boat path safe? ─────────────────────────────────────────────
  -- The boat starts on the entrance's water tile: the river/boat tile itself,
  -- or the cardinal component neighbour the crater will flood into.
  local swx, swy = best_s.wmx, best_s.wmy
  -- Every tile of this water a threatening pill can shoot. The route search
  -- treats them as WALLS rather than scoring them: sailing through one is a
  -- shell in the boat, and there is no cost at which that is worth it.
  local cov, cov_n = sea_covered_water(world, comp, threats)
  cl.covered = cov
  cl.covered_n = cov_n
  -- DO-NOT-COLLECT HALO. A tank picks a pill up with a 9-probe box around its
  -- centre (tank.c, TANK_PILL_PICKUP_INSET = 16 wu), so hugging the corner of
  -- a neighbouring tile is enough to scoop one. Staying out of the refused
  -- pill's TILE therefore is not enough — tests/sea_pills_B collected it from
  -- (138,127) without ever entering (139,126). Ring every refused pill with a
  -- one-tile halo. This is a "do not collect" rule, not a safety one: the halo
  -- tiles are perfectly safe to sail, we simply must not be there, and the
  -- route to the members we DO want goes round it (via x=137 on that map).
  local nogo = {}
  for k, v in pairs(cov) do nogo[k] = v end
  local function halo_refused()
    for pid, by in pairs(cl.dropped or {}) do
      local d = cl.dropped_tiles and cl.dropped_tiles[pid]
      if d then
        for dy = -1, 1 do
          for dx = -1, 1 do
            local hx, hy = d[1] + dx, d[2] + dy
            if U.in_map(hx, hy) then nogo[hy * 256 + hx] = by end
          end
        end
      end
    end
  end
  halo_refused()
  cl.nogo = nogo
  cl.halo_refused = halo_refused
  local path, boat_cost = {}, 0
  local seen_tile = {}
  local route_keep, route_tiles = {}, {}
  for _, pid in ipairs(cl.ids) do
    local p = world.pills[pid]
    local tiles, why_r, exp_r = sea_water_route(world, comp, nogo,
                                                swx, swy, p.mx, p.my)
    sea_log(state, "route:" .. cl.id .. ":" .. pid, string.format(
      "SEA_ROUTE t=%d plan cluster=%d pill#%d start=(%d,%d) goal=(%d,%d) covered=%d expanded=%d -> %s",
      now, cl.id, pid, swx or -1, swy or -1, p.mx, p.my, cov_n or 0,
      exp_r or 0, tiles and (#tiles .. " tiles") or tostring(why_r)))
    if tiles then
      route_keep[#route_keep + 1] = pid
      route_tiles[#route_tiles + 1] = { p.mx, p.my }
      if #tiles > boat_cost then boat_cost = #tiles end
      for _, t in ipairs(tiles) do
        local k = t[2] * 256 + t[1]
        if not seen_tile[k] then
          seen_tile[k] = true
          path[#path + 1] = t
        end
      end
    else
      -- Reachable in principle, but every route to it is covered. Drop this
      -- member the same way a covered pill is dropped — do not sink the trip.
      local by2 = nogo[p.my * 256 + p.mx] or (threats[1] and threats[1].id) or 0
      cl.dropped[pid] = by2
      cl.dropped_tiles = cl.dropped_tiles or {}
      cl.dropped_tiles[pid] = { p.mx, p.my, by2 }
      cl.route_blocked = cl.route_blocked or {}
      cl.route_blocked[pid] = true
    end
  end
  halo_refused()
  if #route_keep == 0 then
    return rej("path_covered_by_pill#" .. tostring(threats[1] and threats[1].id or 0),
      "every boat route from the entrance to every remaining pill crosses water a hostile pill can shoot")
  end
  cl.ids = route_keep
  cl.tiles = route_tiles
  cl.n = #route_keep
  cl.lead = route_keep[1]
  cl.path = path
  local pet = sea_enemy_tank_near(state, path, C.SEA_PILL_ENEMY_TANK_NEAR or 10)
  if pet then
    return rej("path_enemy_tank", string.format(
      "enemy tank at (%d,%d) is within %d tiles of the boat path",
      pet.mx, pet.my, C.SEA_PILL_ENEMY_TANK_NEAR or 10))
  end
  cl.boat_cost = boat_cost * (C.SEA_PILL_BOAT_STEP_COST or 12)

  -- ── Resources ──────────────────────────────────────────────────────────
  -- WOOD. A wall on river (the boat) costs LGM_COST_BOAT = 20 trees and the
  -- mine placement itself costs LGM_COST_MINE = 1 (lgm.h:54,57), so the whole
  -- plan needs 21 trees IN HAND before the mine goes down. That ordering is
  -- not cosmetic: a live mine on our own shore while the LGM is off farming
  -- is exactly the half-done state the commitment rule exists to prevent.
  -- The boat is only built when S is not ALREADY a boat tile.
  -- SHELLS. Only a crater entrance spends any: one to land on the mine, and
  -- one more for every FOREST tile in the F->S lane, because a shell dies on
  -- the first forest it meets and only turns it to grass (shells.c:803). An
  -- existing river or boat needs none at all. Like wood and mines this is a
  -- LEG, not a veto: a dry gun means "go and fill up first".
  local shells_need = 0
  if best_s.needs_mine then
    shells_need = C.SEA_PILL_MIN_SHELLS or 3
    if best_s.F then
      local dx = (best_s.F.mx > best_s.mx) and -1 or ((best_s.F.mx < best_s.mx) and 1 or 0)
      local dy = (best_s.F.my > best_s.my) and -1 or ((best_s.F.my < best_s.my) and 1 or 0)
      for step = 1, (C.SEA_PILL_FIRE_DIST or 2) - 1 do
        if U.ttype(best_s.F.mx + dx * step, best_s.F.my + dy * step) == C.T_FOREST then
          shells_need = C.SEA_PILL_MIN_SHELLS_FOREST or 5
          break
        end
      end
    end
  end
  cl.shells_need = shells_need

  local needs_boat  = U.ttype(best_s.mx, best_s.my) ~= C.T_BOAT
  local trees_need  = (needs_boat and (C.SEA_BOAT_TREES or 20) or 0)
                    + (best_s.needs_mine and (C.SEA_MINE_TREES or 1) or 0)
  local trees_short = math.max(0, trees_need - (info.trees or 0))
  cl.needs_mine  = best_s.needs_mine
  cl.needs_boat  = needs_boat
  cl.trees_need  = trees_need
  cl.mines_need  = best_s.needs_mine and 1 or 0

  local need_mine_leg  = cl.needs_mine and (info.mines or 0) < 1
  local need_shell_leg = (info.shells or 0) < shells_need
  local leg_mines, refuel_base = 0, nil
  if need_mine_leg or need_shell_leg then
    local bids = {}
    for bid in pairs(world.bases) do bids[#bids + 1] = bid end
    table.sort(bids)
    -- Prefer a base we have SEEN holding mines. Fall back to a reachable
    -- friendly base whose stock we have simply never read: base stock only
    -- becomes known once the tank is close enough for the engine to report it
    -- (world.lua obs_mines), so on a fresh map every base looks empty and the
    -- plan rejected `no_mines_anywhere` for 500 ticks of explore while sitting
    -- 4 tiles from a base with 90 mines (tests/sea_pills_D). Only when every
    -- reachable friendly base has been OBSERVED empty is the answer really no.
    local best_bc, best_seen = nil, -1
    for _, bid in ipairs(bids) do
      local b = world.bases[bid]
      -- Rank, do not veto. Base stock is only reported first-hand when the
      -- tank is close (world.lua obs_*), and a base that reads empty restocks
      -- on its own — so "observed 0 mines" is a preference, not a fact about
      -- the future. Vetoing on it cost tests/sea_pills_D a thousand ticks of
      -- explore while sitting five tiles from a base holding ninety.
      --   tier 2  seen holding mines
      --   tier 1  stock never read (or read long enough ago to be meaningless)
      --   tier 0  read empty — still worth the drive if it is all there is
      -- "Stocked" means it holds everything this trip is short of — mines,
      -- shells, or both. One stop, not two.
      local has_m = (b.obs_mines  or 0) > 0
      local has_s = (b.obs_shells or 0) > 0
      local stocked = (not need_mine_leg or has_m) and (not need_shell_leg or has_s)
      local unknown = (b.obs_tick == nil)
                   or ((now - b.obs_tick) > (C.SEA_BASE_STOCK_STALE or 3000))
      local tier = stocked and 2 or (unknown and 1 or 0)
      if (b.owner == "friendly" or b.owner == "allied") then
        local c1 = smart_cost(KIND_NORMAL, tmx, tmy, b.mx, b.my, 0,
                              info.shells or 32, info.trees or 0,
                              info.mines or 0, info.armour or 40)
        if c1 and c1 < 1e29 then
          -- Detour = the base leg plus the base->S leg, minus the direct run
          -- we would have made anyway. base->S is estimated as the straight
          -- tile distance at road cost; the term breakdown says so.
          local back = U.mdist(b.mx, b.my, best_s.mx, best_s.my) * 16
          local detour = math.max(0, c1 + back - travel)
          local better = (best_bc == nil)
                      or (tier > best_seen)
                      or (tier == best_seen and (detour < best_bc
                          or (detour == best_bc and bid < refuel_base)))
          if better then
            best_bc = detour
            best_seen = tier
            refuel_base = bid
          end
        end
      end
    end
    if not refuel_base then
      if need_shell_leg then
        return rej("no_shells_anywhere", string.format(
          "shells=%d < %d needed to detonate the mine, and there is no reachable friendly/allied base at all to fetch them from",
          info.shells or 0, shells_need))
      end
      return rej("no_mines_anywhere",
        "mines=0 and there is no reachable friendly/allied base at all to fetch one from")
    end
    cl.refuel_guess = (best_seen < 2)
    leg_mines = best_bc or 0
  end
  cl.refuel_base = refuel_base
  cl.leg_mines   = leg_mines

  local leg_trees = 0
  local per_tile   = C.SEA_TREES_PER_FOREST or 4
  local need_tiles = math.ceil(trees_short / per_tile)
  local forest_ok  = 0
  if trees_short > 0 then
    -- Wood comes from FOREST, not from a base, and one harvested tile yields
    -- LGM_GATHER_TREE = 4 trees (lgm.c:999). So the shortfall s needs
    -- ceil(s/4) harvestable tiles. "Harvestable" uses the same gates
    -- seek_trees' find_safe_forest applies — pill heat below the bad-ground
    -- line, threat within SEEK_TREES_MAX_THREAT, and NOT COVERED by a live
    -- hostile/neutral pillbox (see the long note on sea_count_safe_forest: it
    -- replaced an influence test that condemned a corner we were raiding)
    -- — plus tank reachability, within SEA_TREES_RADIUS of the entrance OR of
    -- the tank (the harvest happens on the way). If we cannot count enough,
    -- the plan is not fundable: REJECT rather than march out and stall.
    -- (The LGM's own walk is re-gated at dispatch by builder.decide's gather
    -- priority — lgm_can_reach + lgm_path_safe_enhanced — which is the
    -- authoritative check the moment the trip actually starts.)
    local fmx, fmy, fcov
    forest_ok, fmx, fmy, fcov = sea_count_safe_forest(state, world, info,
                                                best_s.mx, best_s.my, tmx, tmy)
    cl.tree_spot = fmx and { fmx, fmy } or nil
    cl.forest_covered = fcov or 0
    if forest_ok < need_tiles then
      return rej("no_safe_trees", string.format(
        "trees %d/%d short %d -> need %d forest tiles (%d trees each, LGM_GATHER_TREE), found %d uncovered and safe within %d tiles of S(%d,%d) or the tank; found %d covered_by_pill",
        info.trees or 0, trees_need, trees_short, need_tiles, per_tile,
        forest_ok, C.SEA_TREES_RADIUS or 12, best_s.mx, best_s.my,
        fcov or 0))
    end
    leg_trees = trees_short * (C.SEA_PILL_TREE_LEG_PER_TREE or 6)
  end
  cl.trees_short = trees_short
  cl.need_tiles  = need_tiles
  cl.forest_ok   = forest_ok
  cl.leg_trees   = leg_trees

  -- ── Cost ───────────────────────────────────────────────────────────────
  local mult  = C.SEA_PILL_COST_MULT or 0.3
  local floor = C.SEA_PILL_COST_FLOOR or 5
  local gross = travel + leg_mines + leg_trees + boat_cost
  cl.gross = gross
  cl.cost  = math.max(floor, gross * mult / math.max(1, cl.n))
  cl.reject = nil
  return cl
end

-- Refresh the whole sea plan. Cached: re-planned only when the cluster
-- membership changes, the threat grid is rebuilt, the tank moves to a new
-- tile, or SEA_PILL_SCAN_PERIOD ticks pass. Everything the pool, the substate
-- machine and the overlay read comes out of state._sea.
function M.sea_refresh(state, world, info)
  if not C.SEA_PILL_ENABLED then state._sea = nil; return end
  local now = state.tick or 0
  local perc = state.perc
  if not perc or not perc.deepsea_pill_ids then state._sea = nil; return end
  -- Afloat: nothing to plan. compute_pool4_cost already prices the pills
  -- through the boat layer and the ordinary pickup drive takes them.
  if info.inboat then state._sea = nil; return end

  local sig_ids = {}
  for pid in pairs(perc.deepsea_pill_ids) do
    local p = world.pills[pid]
    if p and (p.health or 0) == 0 and not p.in_tank then sig_ids[#sig_ids + 1] = pid end
  end
  table.sort(sig_ids)
  if #sig_ids == 0 then state._sea = nil; return end
  local sig = table.concat(sig_ids, ",")

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local s = state._sea
  -- Cache key: the cluster membership, the threat rebuild, and the clock. NOT
  -- the tank tile — a moving tank changes tile every few ticks and keying on it
  -- turned a 625-tile scan plus a component BFS plus up to three boat A*s into
  -- per-tile work for the whole drive. Travel costs move slowly; the 50-tick
  -- period is the right cadence for them.
  if s and s.sig == sig and s.rebuild == threat.last_rebuild_tick
     and (now - (s.tick or -1e9)) < (C.SEA_PILL_SCAN_PERIOD or 50) then
    return
  end

  local clusters, by_pill = sea_build_clusters(state, world)
  for _, cl in ipairs(clusters) do
    sea_plan_cluster(state, world, info, cl, tmx, tmy)
    if cl.reject then
      sea_log(state, "rej:" .. cl.id .. ":" .. cl.reject, string.format(
        "SEA_REJECT t=%d cluster=%d n=%d pills=[%s] reason=%s (%s)",
        now, cl.id, cl.n, table.concat(cl.ids, ","), cl.reject,
        tostring(cl.reject_detail)))
    else
      sea_log(state, "plan:" .. cl.id, string.format(
        "SEA_PLAN t=%d cluster=%d n=%d entrance=%s comp=%d S=(%d,%d) F=%s mine=%s boat=%s trees=%d/%d short=%d need_tiles=%d forest_ok=%d mines=%d/%d shells=%d/%d dropped=[%s] covwater=%d refuel=%s travel=%.0f legs=%.0f/%.0f boat_path=%.0f cost=%.1f",
        now, cl.id, cl.n, tostring(cl.entrance), cl.comp_n or 0, cl.S.mx, cl.S.my,
        cl.F and string.format("(%d,%d)", cl.F.mx, cl.F.my) or "-",
        tostring(cl.needs_mine), tostring(cl.needs_boat),
        info.trees or 0, cl.trees_need, cl.trees_short or 0,
        cl.need_tiles or 0, cl.forest_ok or 0,
        info.mines or 0, cl.mines_need or 0,
        info.shells or 0, cl.shells_need or 0,
        table.concat((function()
          local d = {}
          for pid, by in pairs(cl.dropped or {}) do
            d[#d + 1] = string.format("%d<-#%s", pid, tostring(by))
          end
          table.sort(d)
          return d
        end)(), ","),
        cl.covered_n or 0,
        cl.refuel_base and (tostring(cl.refuel_base) .. (cl.refuel_guess and "?" or "")) or "nil",
        cl.travel, cl.leg_mines, cl.leg_trees,
        cl.boat_cost, cl.cost))
    end
  end
  state._sea = { tick = now, sig = sig, rebuild = threat.last_rebuild_tick,
                 tmx = tmx, tmy = tmy, clusters = clusters, by_pill = by_pill }
end

-- The plan covering pill `pid`, or nil.
function M.sea_cluster_for(state, pid)
  local s = state._sea
  if not s then return nil end
  local ci = s.by_pill[pid]
  return ci and s.clusters[ci] or nil
end

-- Snapshot of a plan to hang on the goal. The goal must not alias the live
-- plan table: the scan rebuilds it every SEA_PILL_SCAN_PERIOD ticks and the
-- committed substates need the S/F they actually mined, not the newest pick.
local function sea_goal_plan(cl)
  local ids = {}
  for i, v in ipairs(cl.ids) do ids[i] = v end
  local path = {}
  for i, t in ipairs(cl.path or {}) do path[i] = { t[1], t[2] } end
  return {
    cluster = cl.id, ids = ids, n = cl.n,
    S = { cl.S.mx, cl.S.my },
    -- Where the tank parks: the firing spot F for a mine entrance, the
    -- boarding tile beside the water for a river/boat entrance.
    F = cl.F and { cl.F.mx, cl.F.my } or nil,
    -- The water tile the boat starts on (the entrance itself for river/boat,
    -- the cardinal component neighbour the crater floods into for a mine).
    W = cl.S.wmx and { cl.S.wmx, cl.S.wmy } or nil,
    entrance = cl.entrance,
    -- The water the boat may use, and the tiles inside it a hostile pill can
    -- shoot. Carried on the snapshot so the afloat collect phase can route
    -- around them without re-planning (the plan only rescans every 50 ticks).
    comp = cl.comp,
    covered = cl.nogo or cl.covered,
    covered_water = cl.covered,
    -- Last known tile of every member. world.pills can drop a record when the
    -- view goes stale, and a boat trip must not forget where it was going.
    pos = (function()
      local m = {}
      for i, pid in ipairs(cl.ids) do m[pid] = { cl.tiles[i][1], cl.tiles[i][2] } end
      return m
    end)(),
    dropped = cl.dropped,
    needs_mine = cl.needs_mine and true or false,
    needs_boat = cl.needs_boat and true or false,
    trees_need = cl.trees_need or 0,
    mines_need = cl.mines_need or 0,
    shells_need = cl.shells_need or 0,
    -- Where the tank waits for the boat and steps aboard: the land tile at the
    -- water's edge, so the gap between "built" and "boarded" is one step.
    B = cl.board_from and { cl.board_from.mx, cl.board_from.my } or nil,
    -- Where the seek_trees substate parks: the nearest forest tile that passed
    -- the influence/threat/reach gates. The builder's gather machinery takes
    -- over from there (it re-finds forest within its own deploy radius).
    tree_spot = cl.tree_spot and { cl.tree_spot[1], cl.tree_spot[2] } or nil,
    refuel_base = cl.refuel_base,
    path = path,
    cost = cl.cost,
    shots = 0,
  }
end
M.sea_goal_plan = sea_goal_plan

-- =========================================================================
-- Substate machine. Called once per tick from init.lua, after goal selection
-- and BEFORE steering/builder so both see the substate this tick set.
-- =========================================================================
-- ── The LIVE plan, kept on STATE, not only on the goal ───────────────────
-- A capture_pill goal object is rebuilt constantly — every finalize_pools, and
-- by several override paths that construct their own {kind="capture_pill"}
-- table. Hanging the plan solely off the goal meant any of those rebuilds
-- dropped it and the chain restarted at entrance_plan. The plan lives on
-- state._sea_live and is re-attached to whichever capture_pill goal targets
-- this cluster, so a target change WITHIN the cluster is not a new plan.
local function sea_live_store(state, g)
  if not (g and g.sea) then return end
  state._sea_live = {
    cluster       = g.sea.cluster,
    sea           = g.sea,
    substate      = g.substate,
    sub_tick      = g._sea_sub_tick,
    seen_tick     = g._sea_seen_tick,
    recheck       = g._sea_recheck,
    hold          = g._sea_hold,
    shot_eta      = g._sea_shot_eta,
    tick          = state.tick or 0,
  }
end
M.sea_live_store = sea_live_store

-- Re-attach the live plan to a goal for the same cluster. Returns true when it
-- adopted one.
local function sea_live_attach(state, g, cluster_id)
  local L = state._sea_live
  if not (L and L.sea and L.cluster == cluster_id) then return false end
  g.sea            = L.sea
  g.substate       = L.substate
  g._sea_sub_tick  = L.sub_tick
  g._sea_seen_tick = L.seen_tick
  g._sea_recheck   = L.recheck
  g._sea_hold      = L.hold
  g._sea_shot_eta  = L.shot_eta
  return true
end
M.sea_live_attach = sea_live_attach

-- The substate to enter once the resource legs are done: park first when the
-- plan has a parking tile (the firing spot, or the boarding tile beside an
-- existing river), otherwise straight to the build / the boarding drive.
local function sea_next_after_resources(sea)
  if sea.F then return "approach_F" end
  if sea.needs_boat then return "build_boat" end
  return "board"
end

local function sea_set_sub(state, g, to, reason)
  local from = g.substate or "-"
  if from == to then return end
  g.substate = to
  g._sea_sub_tick = state.tick or 0
end

-- The RELEASE half of an abort, on its own: forget the plan and everything
-- that hangs off it. Extracted so the goal-change release below runs exactly
-- the same code as sea_abort rather than a second, drifting copy of it.
-- Optionally blacklists the entrance S (a bad entrance, not a bad moment).
local function sea_release_live(state, sea, blacklist_S)
  local now = state.tick or 0
  if blacklist_S and sea and sea.S then
    state._sea_blacklist = state._sea_blacklist or {}
    state._sea_blacklist[sea.S[2] * 256 + sea.S[1]] =
      now + (C.SEA_PILL_BLACKLIST_TICKS or 1500)
  end
  state._sea = nil          -- force a fresh plan next tick
  state._sea_live = nil
  state._sea_afloat = nil
  state._sea_nogo = nil
end

local function sea_abort(state, g, reason, blacklist_S)
  local now = state.tick or 0
  sea_release_live(state, g.sea, blacklist_S)
  g.sea = nil
  g.substate = nil
  attack.clear_attack_goal(state, "sea harvest abort: " .. tostring(reason))
end

-- Re-run the safety gates that made this plan legal in the first place.
-- Called at `board` (the point of no return: a boat is one shell from death)
-- and while holding at F.
local function sea_still_safe(state, world, info, sea)
  local tiles = {}
  for _, pid in ipairs(sea.ids) do
    local p = world.pills[pid]
    if p then tiles[#tiles + 1] = { p.mx, p.my } end
  end
  for _, t in ipairs(sea.path or {}) do tiles[#tiles + 1] = { t[1], t[2] } end
  if #tiles == 0 then return false, "cluster_gone" end
  local threats = sea_threat_pills(world, tiles, C.SEA_PILL_PILL_SAFE_RANGE or 9)
  for _, t in ipairs(tiles) do
    local by = sea_tile_covered(world, threats, t[1], t[2], nil)
    if by then return false, "covered_by_pill#" .. by end
  end
  if sea_enemy_tank_near(state, tiles, C.SEA_PILL_ENEMY_TANK_NEAR or 10) then
    return false, "enemy_tank_near"
  end
  return true, nil
end
M.sea_still_safe = sea_still_safe

-- How many pills of this cluster are still lying dead in the water.
-- UNKNOWN IS NOT COLLECTED. A pill that has momentarily dropped out of
-- world.pills (fog, a stale view) used to count as fetched, which ended the
-- trip early: tests/sea_pills_B announced "cluster collected" 18 ticks after
-- boarding with nothing aboard, handed the boat back to ordinary navigation,
-- and that promptly sailed through the one tile a hostile pill was watching.
-- Only positive evidence — in a tank, or carried — takes a pill off the list.
local function sea_remaining(world, sea)
  local n = 0
  for _, pid in ipairs(sea.ids or {}) do
    local p = world.pills[pid]
    if p == nil then
      n = n + 1                       -- not seen right now; assume still there
    elseif (p.health or 0) == 0 and not p.in_tank and not p.carrier
           and not p._synth_carry then
      n = n + 1
    end
  end
  return n
end
M.sea_remaining = sea_remaining

-- The still-dead cluster pill nearest the tank that can be reached WITHOUT
-- crossing water a hostile pill can shoot, plus its route. Members with no
-- clean route are dropped from the trip (the trip itself carries on) — losing
-- one pill is not a reason to sail home with the other two.
-- Falls back to plain distance when the plan carries no water map (an older
-- snapshot, or a plan made before the covered set existed).
local function sea_pick_collect_target(state, world, sea, tmx, tmy)
  local best, bd, broute = nil, nil, nil
  local drops = nil
  for _, pid in ipairs(sea.ids or {}) do
    local p = world.pills[pid]
    -- Fall back to the plan's remembered tile when the live record is missing:
    -- unseen is not the same as gone.
    if p == nil and sea.pos and sea.pos[pid] then
      p = { mx = sea.pos[pid][1], my = sea.pos[pid][2], health = 0 }
    end
    if p and (p.health or 0) == 0 and not p.in_tank and not p.carrier then
      local route = nil
      if sea.covered then
        local why_r, exp_r
        route, why_r, exp_r = sea_water_route(world,
                                          sea.comp, sea.covered,
                                              tmx, tmy, p.mx, p.my)
        sea_log(state, "croute:" .. pid .. ":" .. tostring(route ~= nil), string.format(
          "SEA_ROUTE t=%d collect pill#%d start=(%d,%d) goal=(%d,%d) covered=%d expanded=%d -> %s",
          state.tick or 0, pid, tmx, tmy, p.mx, p.my,
          (function() local n = 0 for _ in pairs(sea.covered) do n = n + 1 end return n end)(),
          exp_r or 0, route and (#route .. " tiles") or tostring(why_r)))
        if not route then
          drops = drops or {}
          drops[#drops + 1] = pid
        end
      end
      if route or not sea.covered then
        local d = route and #route or U.mdist(tmx, tmy, p.mx, p.my)
        if bd == nil or d < bd or (d == bd and pid < best) then
          bd, best, broute = d, pid, route
        end
      end
    end
  end
  return best, bd, broute, drops
end
M.sea_pick_collect_target = sea_pick_collect_target

-- The set of water tiles that must not be entered for the rest of this trip.
-- Published on state so the NAVIGATION layer can honour it — a route that
-- avoids covered water is not enough on its own, because the boat-layer A*,
-- the plow lookahead and the ordinary capture drive know nothing about it and
-- will happily cut the corner (tests/sea_pills_B sailed through (139,126) at
-- exactly 8.0 tiles from the hostile pill on its way to the next member).
local function sea_publish_nogo(state, sea, active)
  if not active or not (sea and sea.covered) then
    state._sea_nogo = nil
    return
  end
  state._sea_nogo = sea.covered
end

-- Covered water stays off limits for as long as we are AFLOAT, even after the
-- harvest plan that computed it has ended. The trip home is still a boat trip,
-- and a boat that drives over a covered pill on the way back is just as dead —
-- on tests/sea_pills_B the plan correctly refused the middle pill, released
-- once the other two were aboard, and then scooped the refused one anyway
-- because the no-go set was released with it. Called every tick from init.lua,
-- right after sea_update.
function M.sea_nogo_tick(state, info)
  if info.inboat then
    if state._sea_nogo then
      state._sea_nogo_last = state._sea_nogo
    elseif state._sea_nogo_last then
      state._sea_nogo = state._sea_nogo_last
    end
  else
    state._sea_nogo_last = nil
    if not (state.goal and state.goal.sea) then state._sea_nogo = nil end
  end
end

function M.sea_update(state, world, info)
  local g = state.goal
  -- Adopt the live plan when the goal is a capture_pill for a cluster we are
  -- already working (any member, not just the one the plan started on).
  if g and g.kind == "capture_pill" and not g.sea and state._sea_live then
    local cid = state._sea_live.cluster
    local mine = false
    for _, pid in ipairs(state._sea_live.sea.ids or {}) do
      if pid == g.target_id then mine = true break end
    end
    if mine then sea_live_attach(state, g, cid) end
  end
  if not (g and g.kind == "capture_pill" and g.sea) then
    -- THE PLAN ENDS WITH THE GOAL (T = 0, no grace window).
    --
    -- state._sea_live exists for ONE reason: a capture_pill goal object is
    -- rebuilt on every replan, and hanging the plan solely off the goal made
    -- each rebuild restart the chain at entrance_plan. That is a SAME-GOAL
    -- concern. Keeping the plan alive while the tank is doing something else
    -- buys a slightly smoother resume after a detour and costs
    -- sea.trees_need (21) frozen trees for as long as the detour lasts --
    -- because both readers (builder_pool.tree_reserve's `sea` term and
    -- builder.decide's sea rung) are keyed on the LIVE plan, not on the goal.
    --
    -- 20260903_105448 bot2: the harvest's LGM died at t=28997, the tank moved
    -- on, and nothing ever retired the plan. For the remaining 20 minutes the
    -- reserve read base 4 + pills 16 + sea 21 = 41 against 21 trees, so every
    -- repair and every blocker wall was refused (BP_DENY tree_reserve(41,21,1))
    -- while the bot stood next to three damaged friendly pills.
    --
    -- Dropping it is not giving up: the mine and the boat are visible terrain,
    -- perception still flags whatever is left in the water, and the next
    -- rescan prices a fresh plan for it. The entrance is NOT blacklisted --
    -- a goal change says nothing bad about the entrance.
    if state._sea_live then
      local L = state._sea_live
      sea_release_live(state, L.sea, false)
    end
    state._sea_afloat = nil
    state._sea_nogo = nil
    return
  end
  local sea = g.sea
  local now = state.tick or 0
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local smx, smy = sea.S[1], sea.S[2]

  local sub0 = g.substate or "entrance_plan"
  -- ── AFLOAT: the collect phase ─────────────────────────────────────────
  -- One boat trip takes the WHOLE cluster. While we are afloat with cluster
  -- pills still in the water and the water still safe, the harvest OWNS the
  -- goal (substate "collect", enforced in pick_goal) and the brain must not be
  -- talked ashore: escape_water, a competing pool winner or a second entrance
  -- plan each cost a full mine + 21 trees + boat cycle. On sea_pills_A that
  -- turned "three free pills" into three mines and 60 trees.
  if info.inboat and not (sub0 == "board" or sub0 == "collect") then
    -- Afloat, but not on OUR trip: the spawn boat still under us in its
    -- landlocked pocket, a river crossing, a boat picked up in passing. The
    -- collect phase is only ever entered from `board`. Without this the plan
    -- declared victory in a one-tile pond it had never left (sea_pills_G sat
    -- there in "collect" for 1560 ticks with the pills untouched).
    state._sea_afloat = nil
    sea_live_store(state, g)
    return
  end
  if info.inboat then
    sea.boarded = true
    local remain = sea_remaining(world, sea)
    local safe, why = sea_still_safe(state, world, info, sea)
    sea_publish_nogo(state, sea, true)
    if remain > 0 and safe then
      state._sea_afloat = true
      if g.substate ~= "collect" then
        sea_set_sub(state, g, "collect", string.format(
          "afloat with %d cluster pill(s) still in the water", remain))
      end
      sea_live_store(state, g)
      return
    end
    -- Cluster collected, or the water turned dangerous: release. The ordinary
    -- boat handling (nearest land / escape_water) takes it from here.
    state._sea_afloat = nil
    sea_publish_nogo(state, sea, false)
    if g.substate then
      -- "collected" has to MEAN collected. When the list emptied because every
      -- remaining member was dropped as unroutable, say that instead — the old
      -- message announced a finished harvest at carry=0 and handed the boat
      -- back to ordinary navigation mid-trip (tests/sea_pills_B, t=853).
      local reason
      if not safe then
        reason = "unsafe afloat: " .. tostring(why)
      elseif (sea.left_behind or 0) > 0 then
        reason = string.format(
          "released: members_unroutable — %d member(s) left behind, no boat route clear of hostile pill fire",
          sea.left_behind)
      else
        reason = "cluster collected — releasing the boat to normal handling"
      end
      sea_set_sub(state, g, nil, reason)
    end
    g.substate = nil
    sea.done = true
    sea_live_store(state, g)
    return
  end
  state._sea_afloat = nil
  -- Spent plan, and we are back on land: DROP it rather than walking the chain
  -- again from entrance_plan, which is what beaching after a pickup would
  -- otherwise do (mine our own shore a second time for pills already aboard).
  -- Dropping it is not the same as giving up: perception still flags whatever
  -- is left in the water, and the next rescan prices a fresh plan for it.
  if sea.done then
    g.sea = nil
    g.substate = nil
    state._sea = nil
    state._sea_live = nil
    state._sea_nogo = nil
    return
  end
  -- The LGM does both builds. Losing it mid-chain ends the plan.
  if info.man_status == C.LGM_DEAD then
    return sea_abort(state, g, "lgm_dead", false)
  end

  local sub = g.substate or "entrance_plan"
  g._sea_sub_tick = g._sea_sub_tick or now
  -- The watchdog clock only runs while the sea goal is actually OURS. Another
  -- goal can win the pool mid-chain (tests/sea_pills_C spent 3000 ticks
  -- elsewhere with the mine already down and then "timed out" on a substate
  -- that had never had a chance to run), and time under someone else's goal is
  -- not this substate failing to progress.
  local away = now - (g._sea_seen_tick or now)
  if away > 2 then g._sea_sub_tick = g._sea_sub_tick + away end
  g._sea_seen_tick = now
  -- Progress watchdog: any substate that makes no progress for
  -- SEA_PILL_SUB_TIMEOUT ticks is a stall, not a plan.
  if (now - g._sea_sub_tick) > (C.SEA_PILL_SUB_TIMEOUT or 1500) then
    return sea_abort(state, g, "timeout:" .. sub, false)
  end

  local trees_ok  = (info.trees or 0) >= (sea.trees_need or 0)
  local mines_ok  = (not sea.needs_mine) or (info.mines or 0) >= 1
  -- Shells are spent detonating the mine (plus one per forest tile in the
  -- firing lane), so an empty gun is a resource leg exactly like an empty
  -- mine rack. A strict-tournament tank starts with nothing at all.
  local shells_ok = (info.shells or 0) >= (sea.shells_need or 0)

  -- Covered water is off limits from the moment we are about to step onto the
  -- water, and stays off limits for the rest of the trip.
  sea_publish_nogo(state, sea, sub == "board")
  -- Precision nav for every substate that has to SETTLE on one exact tile: the
  -- firing spot, the boarding tile, the forest the LGM harvests from, and above
  -- all the base — a base only refuels a tank standing ON it, and normal
  -- braking left the tank hovering one tile short of it for 2200 ticks
  -- (tests/sea_pills_D) while the refuel leg waited for a mine that could never
  -- arrive.
  g.nav_mode = (sub == "approach_F" or sub == "refuel_mines"
                or sub == "seek_trees" or sub == "build_boat")
               and "precision" or nil

  if sub == "entrance_plan" then
    if (not mines_ok or not shells_ok) and sea.refuel_base then
      sea_set_sub(state, g, "refuel_mines", string.format(
        "short at the rack (mines %d/%d, shells %d/%d) — base #%s",
        info.mines or 0, sea.mines_need or 0,
        info.shells or 0, sea.shells_need or 0, tostring(sea.refuel_base)))
    elseif not trees_ok then
      sea_set_sub(state, g, "seek_trees", string.format("trees %d < %d for the boat",
        info.trees or 0, sea.trees_need or 0))
    elseif sea.F then
      sea_set_sub(state, g, "approach_F", "resources in hand — park at "
        .. (sea.needs_mine and "the firing spot" or "the boarding tile"))
    elseif sea.needs_boat then
      sea_set_sub(state, g, "build_boat", "entrance is already river — no mine needed")
    else
      sea_set_sub(state, g, "board", "entrance is already a boat")
    end

  elseif sub == "refuel_mines" then
    -- Any stock arriving IS progress: reset the stall watchdog so a slow leg
    -- (the base is across the map, or the wood needs six LGM round trips)
    -- isn't mistaken for a stuck plan.
    if (info.mines or 0) > (sea._seen_mines or -1)
       or (info.shells or 0) > (sea._seen_shells or -1) then
      sea._seen_mines  = math.max(sea._seen_mines  or -1, info.mines  or 0)
      sea._seen_shells = math.max(sea._seen_shells or -1, info.shells or 0)
      g._sea_sub_tick = now
    end
    if mines_ok and shells_ok then
      if not trees_ok then
        sea_set_sub(state, g, "seek_trees", string.format(
          "rack filled (mines %d, shells %d), still short of wood",
          info.mines or 0, info.shells or 0))
      else
        sea_set_sub(state, g, sea_next_after_resources(sea), string.format(
          "rack filled (mines %d, shells %d)", info.mines or 0, info.shells or 0))
      end
    end

  elseif sub == "seek_trees" then
    if (info.trees or 0) > (sea._seen_trees or -1) then
      sea._seen_trees = info.trees or 0
      g._sea_sub_tick = now
    end
    if trees_ok then
      sea_set_sub(state, g, sea_next_after_resources(sea),
                  string.format("trees %d >= %d", info.trees or 0, sea.trees_need or 0))
    end

  elseif sub == "approach_F" then
    -- Hold here (and only here) while a threat is loitering: F is normally the
    -- forest spot, the safest place to wait.
    local ok = true
    if (now - (g._sea_recheck or -1e9)) >= (C.SEA_PILL_ABORT_RECHECK or 25) then
      g._sea_recheck = now
      local why
      ok, why = sea_still_safe(state, world, info, sea)
      g._sea_hold = (not ok) and why or nil
      if not ok then
      end
    else
      ok = (g._sea_hold == nil)
    end
    -- HARD ordering rule: the mine only goes down once BOTH the mine and all
    -- 21 trees (LGM_COST_BOAT 20 + LGM_COST_MINE 1) are in hand. A live mine
    -- on our own shore while the LGM is away farming is the failure mode.
    if not (trees_ok and mines_ok and shells_ok) then
      sea_set_sub(state, g,
        (not (mines_ok and shells_ok)) and "refuel_mines" or "seek_trees",
        string.format("resources slipped: trees %d/%d mines %d/%d shells %d/%d",
          info.trees or 0, sea.trees_need or 0, info.mines or 0, sea.mines_need or 0,
          info.shells or 0, sea.shells_need or 0))
    -- Arrival is GEOMETRY, not one exact tile. What the shot needs is to be
    -- clear of the 384 wu mine blast box — i.e. at least 2 tiles from S on one
    -- axis (Chebyshev >= SEA_PILL_FIRE_DIST) — and to be at the firing spot we
    -- picked. Demanding the exact F tile left the tank parked one tile short
    -- for 1200 ticks on sea_pills_A, long enough for stuck-recovery to
    -- blacklist the goal tile and hand the game to explore. The shot itself is
    -- still gated by shot_path_clear and the landing test in steering, so a
    -- neighbouring tile is safe, not sloppy.
    -- The blast clearance applies ONLY to a mine entrance. For an existing
    -- river or boat there is nothing to detonate and the parking tile is
    -- deliberately ADJACENT to the water (that is where the LGM builds from and
    -- where we step aboard), so demanding 2 tiles of clearance there made the
    -- arrival test unsatisfiable — sea_pills_G sat at its boarding tile and
    -- never entered build_boat.
    elseif sea.F
           and (not sea.needs_mine
                or U.cdist(tmx, tmy, smx, smy) >= (C.SEA_PILL_FIRE_DIST or 2))
           and U.cdist(tmx, tmy, sea.F[1], sea.F[2]) <= 1
           and U.ttype(tmx, tmy) ~= C.T_RIVER and U.ttype(tmx, tmy) ~= C.T_BOAT
           and ok and info.man_status == C.LGM_INTANK then
      sea_set_sub(state, g,
        sea.needs_mine and "lay_mine" or (sea.needs_boat and "build_boat" or "board"),
        string.format("parked at (%d,%d) with trees %d/%d and mines %d/%d and shells %d/%d",
          sea.F[1], sea.F[2], info.trees or 0, sea.trees_need or 0,
          info.mines or 0, sea.mines_need or 0,
          info.shells or 0, sea.shells_need or 0))
    end

  elseif sub == "lay_mine" then
    -- "Is the mine down?" is answered by OUR MINE COUNT, not by the map's mine
    -- flag. The flag is a per-player visibility bit and the brain does not
    -- reliably see its own (tests/sea_pills_D: the LGM laid it, the flag never
    -- appeared, the builder re-issued BUILDMODE_MINE 2672 times and the
    -- substate timed out). The count dropping by one is unambiguous.
    -- Re-baselined on every ENTRY to lay_mine (keyed on the substate's own
    -- entry tick), so a replanned second attempt does not compare against the
    -- first attempt's count.
    if sea._mines_at_lay_tick ~= g._sea_sub_tick then
      sea._mines_at_lay_tick = g._sea_sub_tick
      sea._mines_at_lay = info.mines or 0
    end
    local spent = (info.mines or 0) < sea._mines_at_lay
    local flagged = bit.band(U.traw(smx, smy), TERRAIN_MINE_FLAG) ~= 0
    if (spent or flagged) and info.man_status == C.LGM_INTANK then
      sea_set_sub(state, g, "detonate", string.format(
        "mine is down (%s) and the LGM is back aboard",
        spent and string.format("mines %d -> %d", sea._mines_at_lay, info.mines or 0)
              or "map flag"))
    end

  elseif sub == "detonate" then
    -- Everything here reads TERRAIN, never the mine flag (see lay_mine).
    local tt = U.ttype(smx, smy)
    if tt == C.T_RIVER then
      sea_set_sub(state, g, "build_boat", "S flooded to river")
    elseif tt == C.T_BOAT then
      sea_set_sub(state, g, "board", "S is already a boat")
    elseif tt == C.T_CRATER then
      -- The mine has gone off. The crater does NOT become river on the same
      -- tick: floodAddItem queues it and floodUpdate counts FLOOD_FILL_WAIT
      -- down first (floodfill.c), so there is a real wait before the tile
      -- turns. Give it a grace window — measured on tests/sea_pills_A this is
      -- a couple of hundred ticks — and only then call the tile wrong,
      -- blacklist it and replan.
      sea.flood_since = sea.flood_since or now
      if (now - sea.flood_since) > (C.SEA_FLOOD_WAIT_TICKS or 900) then
        return sea_abort(state, g, "crater_never_flooded", true)
      end
    elseif (sea.shots or 0) >= (C.SEA_PILL_MAX_DETONATE_SHOTS or 4) then
      -- Every shell spent and the tile is still dry land: either the mine was
      -- never really placed or we cannot land a shell on it from here.
      sea.dry_since = sea.dry_since or now
      if (now - sea.dry_since) > (C.SEA_FLOOD_WAIT_TICKS or 900) then
        return sea_abort(state, g, "no_flood_after_shots", true)
      end
    end

  elseif sub == "build_boat" then
    -- The LGM has to be BACK ABOARD before we drive onto the boat: sailing off
    -- with him still ashore strands him, and on the first run of
    -- tests/sea_pills_A that killed him 24 ticks after the boat went up and
    -- cost the plan 2500 ticks of waiting for a replacement.
    if U.ttype(smx, smy) == C.T_BOAT and info.man_status == C.LGM_INTANK then
      sea_set_sub(state, g, "board", "boat built and the LGM is back aboard")
    end

  elseif sub == "board" then
    -- The boat has to still BE there. If it is gone (shot, taken, or reverted)
    -- the destination is open river, and driving onto open river is drowning —
    -- tests/sea_pills_D did exactly that, roaded the tile out from under itself
    -- to survive, and sat on it for the rest of the run. Rebuild if the wood is
    -- still there, otherwise abort and let the pool re-price from scratch.
    local stt = U.ttype(smx, smy)
    if stt ~= C.T_BOAT then
      if stt == C.T_RIVER and (info.trees or 0) >= (C.SEA_BOAT_TREES or 20)
         and info.man_status == C.LGM_INTANK then
        sea_set_sub(state, g, "build_boat", "the boat is gone but the river and the wood are still there")
      else
        return sea_abort(state, g, "boat_gone_before_boarding", false)
      end
    else
      local ok, why = sea_still_safe(state, world, info, sea)
      if not ok then
        return sea_abort(state, g, "unsafe_before_boarding:" .. tostring(why), false)
      end
    end
  end
  sea_live_store(state, g)
end

-- =========================================================================
-- Overlay: sea_harvest_viz
-- =========================================================================
function M.draw_sea_harvest(viz, state)
  if not viz or not viz.is_on or not viz.is_on("sea_harvest_viz") then return end
  local s = state._sea
  if not s then return end
  for _, cl in ipairs(s.clusters or {}) do
    -- The connected water the pills float in. Everything the plan may use has
    -- to touch THIS; a pond next to the tank is shaded differently by being
    -- absent, which is exactly what makes a wrong pick visible.
    for k in pairs(cl.comp or {}) do
      local cx, cy = k % 256, __idiv(k, 256)
    end
    for _, t in ipairs(cl.tiles) do
    end
    -- Water a hostile pill can shoot: the route search treats these as walls.
    for k, by in pairs(cl.covered or {}) do
      local cx, cy = k % 256, __idiv(k, 256)
    end
    -- The firing CIRCLE each threatening pill actually has, and which margin
    -- rule produced it — a calm pill gets its bare 8 tiles, a heated one an
    -- extra tile of buffer.
    for _, tp in ipairs(cl.threats or {}) do
      local lim, hot = sea_cover_limit(tp.pill)
    end
    -- Members dropped from the trip: covered, or no route to them that stays
    -- out of hostile fire. Drawn in place so it is obvious the raft was split
    -- rather than abandoned.
    for pid, d in pairs(cl.dropped_tiles or {}) do
    end
    for _, r in ipairs(cl.rays or {}) do
      if r.out_of_range then
        -- Drawn faint: the geometry says the shell could fly there, but the
        -- pill will never fire at it. This is the ray that used to condemn a
        -- whole raft for one member on the 8-tile line.
      elseif r.reached then
      else
        if r.smx then
        end
      end
    end
    for _, c in ipairs(cl.cands or {}) do
      if c.reject then
        if c.reject == "unconnected" then
        end
      else
      end
    end
    for _, t in ipairs(cl.path or {}) do
    end
    if cl.S then
    end
    if cl.F then
      if cl.S then
      end
    end
    if cl.reject and cl.tiles[1] then
    end
  end
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
            -- Re-apply the SAME per-pool extras step_eval_queue charges on top
            -- of the path. The rescore used to write the bare A* number as the
            -- candidate cost, which silently STRIPPED the staleness penalty and
            -- (pool 7) the armour-aware markup and the threat term from every
            -- base within the radius for the rest of the opening. It shows up
            -- as a row that cannot be added up: 20260902_021746 bot2 t=761,
            -- base #2 reading cand=712 beside its own components
            -- path 700 + base 80 + threat 368, because the 712 was a rescored
            -- path and the components were the earlier, real eval.
            local _age = (obj and obj.last_seen and now > 0)
                         and (now - obj.last_seen) or 0
            local _stale = 0
            if _age > C.STALE_PENALTY_START then
              _stale = (_age - C.STALE_PENALTY_START) * C.STALE_PENALTY_PER_TICK
            end
            local _mk, _mn, _mnfull, _mfrac, _marm, _mage, _mdecay = 0
            local _tv, _thr, _fcm, _fcn = 0, 0, 1.0, 0
            if pool_idx == 7 and obj then
              _mk, _mn, _mnfull, _mfrac, _marm, _mage, _mdecay = base_markup(obj, now)
              _tv  = threat.at(cand.mx, cand.my)
              _thr = _tv * C.ATTACK_BASE_THREAT_WEIGHT
                     * (state.cautious_mode and C.CAUTIOUS_MODE_MULT or 1)
              _fcm, _fcn = base_friendly_cover(world, obj, tmx, tmy)
              if _fcn > 0 then _mk = _mk * _fcm; _thr = _thr * _fcm end
            end
            local total = c + _stale + _mk + _thr
            cand.cost = total
            rescored = rescored + 1
            -- Bump the cost-cache timestamp too, so the pool-grid "age" shows
            -- this as a fresh eval (not the stale last-dijkstra-sweep tick).
            local ck = pool_idx .. ":" .. cand.id
            local entry = state.cost_cache and state.cost_cache[ck]
            if entry then
              entry.cost = total; entry.raw = c; entry.tick = now
              entry._age = _age; entry._stale = _stale
              if pool_idx == 7 then
                entry._base = _mk; entry._tv = _tv; entry._thr = _thr
                entry._b_n = _mn; entry._b_nfull = _mnfull; entry._b_frac = _mfrac
                entry._b_arm = _marm; entry._b_age = _mage; entry._b_decay = _mdecay
                entry._fcov_m = (_fcn > 0) and _fcm or nil
                entry._fcov_n = (_fcn > 0) and _fcn or nil
              end
              entry.formula = nil   -- rebuilt lazily from the new terms
            end
            c = total   -- the log line below reports what the pool now holds
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

  -- Check preconditions that would skip entire pools.
  -- needs_resupply is the REFUEL CANDIDACY test (see refuel_need): the low
  -- watermarks, plus below-full when REFUEL_TOPOFF_CANDIDATE is on.
  -- has_shells is NOT part of that and keeps the raw SHELLS_LOW line — it
  -- gates offence (pill takes), which is a different question.
  local needs_resupply = refuel_need(state, info)
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
    -- state.blocked is a DRIVING cooldown: "we failed to reach this tile on
    -- foot recently". For a pill in deep sea that is not news — we were never
    -- going to walk there, which is the whole point of the harvest. Letting it
    -- veto the plan cost sea_pills_A 680 ticks of explore while the lead pill
    -- sat rejected as `blocked`.
    if reject and reject.reason == "blocked" and perc and perc.deepsea_pill_ids
       and perc.deepsea_pill_ids[id] and not (info and info.inboat) then
      reject = nil
    end
    -- Deep-sea "bait" pills: a dead pill sitting on deep water can't be taken on
    -- foot (the tank/LGM would drown). Reject for capture_pill unless we're
    -- afloat. (perc.deepsea_pill_ids flags them; the bait_pill_marker viz.)
    -- SEA-PILL HARVEST: on land this used to be a flat `deepsea_no_boat`
    -- reject. It is now a priced-or-named-reject row — sea_refresh has already
    -- planned the mine->crater->river->boat entrance for this pill's cluster
    -- (or recorded why it can't). Afloat there is nothing to plan: the ordinary
    -- pricing reads the boat Dijkstra layer and the drive-over pickup runs.
    local sea_cl = nil
    -- Coverage veto FIRST, and independent of the plan: a dead pill sitting
    -- inside a live pillbox's circle is not worth a boat, and the boat is just
    -- as fragile when we are already in one. Without this the raft split
    -- correctly on land, collected its two safe pills, and then — afloat, where
    -- the harvest plan no longer exists — the ordinary pricing sailed straight
    -- into the circle for the third (tests/sea_pills_B).
    if not reject and perc and perc.deepsea_pill_ids and perc.deepsea_pill_ids[id] then
      local by = sea_dead_pill_covered(world, obj)
      if by then reject = { reason = "covered_by_pill#" .. tostring(by) } end
    end
    if not reject and perc and perc.deepsea_pill_ids and perc.deepsea_pill_ids[id]
       and not (info and info.inboat) then
      sea_cl = M.sea_cluster_for(state, id)
      if not sea_cl then
        reject = { reason = "deepsea_no_boat" }
      elseif sea_cl.reject then
        reject = { reason = sea_cl.reject }
      elseif sea_cl.dropped and sea_cl.dropped[id] then
        -- This member of the raft is individually unreachable: a hostile pill
        -- covers its tile, or every boat route to it. The rest of the cluster
        -- still goes.
        reject = { reason = "covered_by_pill#" .. tostring(sea_cl.dropped[id]) }
        sea_cl = nil
      elseif sea_cl.lead ~= id then
        -- Same plan, same boat, same cost: let ONLY the lead compete so the
        -- pool cannot flip the target between members and restart the chain.
        reject = { reason = "cluster_member_of#" .. tostring(sea_cl.lead) }
        sea_cl = nil
      end
    end
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
          local c, _draw, dscore, dval, intcpt, _lm4, _dm4, _free4, _rdmg4, _rfar4,
                _cln4, _cldiv4, _clgn4, _clgm4, _clgids4 =
            compute_pool4_cost(state, world, info, obj, tmx, tmy)
          -- Deep-sea pill with a live harvest plan: the plan's cost REPLACES
          -- the land formula (the tank cannot walk there at all — what it
          -- actually pays is the entrance trip, the resource legs and the
          -- boat crossing, shared across the cluster).
          if sea_cl and not sea_cl.reject then
            c = sea_cl.cost
            _draw = sea_cl.gross
          end
          state.cost_cache[ck] = {
            cost = c, raw = _draw, tick = now, _p = 4, _id = id,
            _sea = sea_cl and not sea_cl.reject and sea_cl or nil,
            _sea_trees_have = info.trees or 0,
            _sea_mines_have = info.mines or 0,
            _sea_shells_have = info.shells or 0,
            _mx = obj.mx, _my = obj.my,
            _ds = dscore, _dv = dval, _intcpt = intcpt,
            _dist_method = _dm4, _free = _free4,  -- _free = scaled value bonus subtracted
            _route_dmg = _rdmg4,                  -- wsim damage on the probed direct route
            _route_far = _rfar4,                  -- set = too far to probe, slate cost stands
            -- Land dead-pill cluster: n members, the divisor applied, and how
            -- many live hostile/neutral pills can shell a cluster tile.
            _cl_n = _cln4, _cl_div = _cldiv4,
            _cl_guard_n = _clgn4, _cl_guard_m = _clgm4, _cl_guard_ids = _clgids4,
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
  -- Evict STALE attack-pill cache entries, the way pool 4 does above. The
  -- loop only refreshes pills that still pass filter_attack_pill; a pill that
  -- turned friendly/allied, died, or got picked up keeps its old entry
  -- forever otherwise. Nothing selects from those (the pool draws from the
  -- queue), but sync_ally_claimed_rejects walks every entry each tick and the
  -- debug SYNC_P6 dump printed 16 rows for a 9-pill queue.
  for ck, ce in pairs(state.cost_cache) do
    if ce._p == 6 then
      local lp = world.pills[ce._id]
      if (not lp) or (lp.health or 0) <= 0 or lp.in_tank
         or not (lp.owner == "hostile" or lp.owner == "neutral") then
        state.cost_cache[ck] = nil
      end
    end
  end

  -- IMMINENT BASE STEAL: pick (at most) one hostile base that is a shell or
  -- three from capturable and sitting right next to us, and record a reason for
  -- every other hostile base. Done here, once per eval cycle, so the per-tick
  -- candidate costing below and the finalize step both read one decision.
  refresh_base_steal(state, world, info)

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
  -- Tie-break on (pool, id): squared tile distances are integers, so exact
  -- ties are common, and a comparator that returns false for ties leaves
  -- their order to the pre-sort array order. This queue decides which
  -- candidates get a full eval within the per-tick step budget, so tie
  -- order is behaviour — make it a total order, immune to any upstream
  -- ordering perturbation (determinism hardening, 20260831).
  table.sort(queue, function(a, b)
    local da = (a.obj.mx - tmx)^2 + (a.obj.my - tmy)^2
    local db = (b.obj.mx - tmx)^2 + (b.obj.my - tmy)^2
    if da ~= db then return da < db end
    if a.pool ~= b.pool then return a.pool < b.pool end
    return (a.id or -1) < (b.id or -1)
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
    -- Say which price this pool-6 row was judged on (always the raw cost_cache
    -- cost -- see sync_ally_claimed_rejects) and what the last outgoing-request
    -- decision was, so the row is reproducible.
    local steal_str = ""
    if e._steal_units then
      steal_str = string.format(" — priced on OUR %s cost %.1f (raw cost_cache %.1f)",
        e._steal_units, e._steal_cost or our, our)
    end
    if e._steal_reason then steal_str = steal_str .. " — " .. tostring(e._steal_reason) end
    if e._steal_note then steal_str = steal_str .. " || request: " .. tostring(e._steal_note) end
    return reject_with_breakdown(e,
      string.format("REJECT ally_claimed %dt @(%d,%d)", rem, e._mx or 0, e._my or 0),
      string.format("reject:ally_claimed %dt — p%s bid %s < ours %.0f%s%s",
        rem, tostring(by or "?"), their_str, our, diff_str, steal_str))
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
    -- Refuel-pad reach (goals.lua refuel_pad_pill_reach): the danger_val above
    -- is a PARKED read, not a driving one. Say which verdict produced it, with
    -- the two distances compared, and — when the pill layer was dropped — what
    -- came off, so danger_val is still hand-derivable from threat.at().
    local _pad_thresh = (C.PILL_FIRE_RANGE or 8) + (C.REFUEL_PAD_TANK_OFFSET or 0.7071)
    local _pad_token = ""
    if e._pad_safe ~= nil then
      _pad_token = (e._pad_safe
        and ((e._pad_pill == nil)
             and " padsafe{no live hostile pill on the map}"
             or string.format(" padsafe{nearest #%s at %.1f > %.2f}",
                              tostring(e._pad_pill), e._pad_dist or 0, _pad_thresh))
        or string.format(" padhit{#%s reaches: %.1f <= %.2f}",
                         tostring(e._pad_pill), e._pad_dist or 0, _pad_thresh))
      if e._pad_safe then
        _d_danger = _d_danger .. string.format(
          " -- PAD SAFE: %s, so the whole PILL layer (%.1f) was REMOVED from "
          .. "threat.at() and danger_val is the TANK-layer remainder %.1f. "
          .. "A pill fires on a tank at <= %.0f[PILL_FIRE_RANGE] tiles from its "
          .. "own tile centre (util.c utilIsItemInRange, PILLBOX_RANGE 2048wu), "
          .. "and a docked tank is at most %.4f[REFUEL_PAD_TANK_OFFSET] tiles "
          .. "off the base centre, so nothing past %.2f can touch the pad. "
          .. "(The PILL_RANGE_MAP %d driving stamp is unchanged.)",
          (e._pad_pill == nil) and "there is no live hostile/neutral pill on the map"
            or string.format("the nearest live pill #%s is %.1f tiles away",
                             tostring(e._pad_pill), e._pad_dist or 0),
          e._pad_removed or 0, e._dv or 0,
          C.PILL_FIRE_RANGE or 8, C.REFUEL_PAD_TANK_OFFSET or 0.7071, _pad_thresh,
          C.PILL_RANGE_MAP or 9)
      else
        _d_danger = _d_danger .. string.format(
          " -- PAD HIT: pill #%s is %.1f tiles from the base centre, inside "
          .. "%.0f[PILL_FIRE_RANGE] + %.4f[REFUEL_PAD_TANK_OFFSET] = %.2f, so it "
          .. "can shell part of this tile and the stamped value stands in full "
          .. "(no fractional scaling -- we do not pick where on the tile we stop).",
          tostring(e._pad_pill), e._pad_dist or 0,
          C.PILL_FIRE_RANGE or 8, C.REFUEL_PAD_TANK_OFFSET or 0.7071, _pad_thresh)
      end
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
        " × ur{%.2f} - def{%.0f} × mult{%.2f fill %.2f sh %d/%d arm %d/%d} scar{%.2f}%s",
        _u, _db, _fm, e._fill or 0,
        e._sh or 0, e._sh_target or 0, e._arm or 0, e._arm_target or 0,
        e._scarcity or 1,
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
          "fill=%.2f = min((sh %d-%d)/(%d-%d), (arm %d-%d)/(%d-%d)) → "
          .. "1 + %.2f² x (%.2f[FULL_MULT]-1) x %.2f[scarcity] = %.2f",
          _fill,
          _sh, C.SHELLS_LOW, e._sh_target or 0, C.SHELLS_LOW,
          _arm, C.ARMOUR_LOW, e._arm_target or 0, C.ARMOUR_LOW,
          _fill, C.REFUEL_FULL_COST_MULT, e._scarcity or 1, _fm)
        or  "fill=0 (at/below LOW thresholds) → 1.00"
      local _d_scar  = string.format(
          "scarcity=%.2f = 1 + %.2f[RATIO_K] x max(0, team/friendly_bases - 1)"
          .. " + %.2f[LOCAL_K] x allies within %d tiles, capped at %.2f",
          e._scarcity or 1, C.REFUEL_SHARE_RATIO_K or 0, C.REFUEL_SHARE_LOCAL_K or 0,
          C.REFUEL_SHARE_LOCAL_TILES or 20, C.REFUEL_SHARE_SCARCITY_CAP or 8.0)
      local _d_lgm   = _lgm
        and string.format("LGM returning, at this base → floor=%.0f (cost capped)", _lgm)
        or  "no LGM-wait active → no floor"
      _shape_detail = string.format("|urgency:%s|deficit:%s|fill:%s|scarcity:%s|lgm:%s",
        _d_urgency, _d_def, _d_fill, _d_scar, _d_lgm)
    end

    local _danger_pen = C.REFUEL_DANGER_PENALTY or (1 / 0.75)
    local _safe_token = (not e._safe_refuel)
      and string.format(" × danger{%.2f}", _danger_pen) or ""
    local _safe_detail = (not e._safe_refuel)
      and string.format(
        "|danger:danger_val>0 (exposed base) → multiply final cost by %.2f[REFUEL_DANGER_PENALTY]",
        _danger_pen)
      or ""
    -- Mine-hoard chip. Shown whenever there IS a hoard at this base, charged or
    -- waived, so a 0 that was waived can never be mistaken for "no mines".
    -- Same text the REFUEL_SHAPE debug line prints (mine_chip is shared).
    local _mine_token, _mine_detail = "", ""
    if e._mine_d and e._mine_d.over > 0 then
      local _mh, _mw = mine_chip(e._mine_d)
      _mine_token  = string.format(" + mines{%s}", _mh)
      _mine_detail = "|mines:" .. _mw
        .. ". Charged ONLY at the base you're parked on, to push a mine-stuffed"
        .. " tank off the pad; waived while that base can still hand it a"
        .. " resource it is short of (C.REFUEL_MINE_HOARD_NEEDS_SUPPLY)."
    end
    local _hop_token = (e._hop and e._hop > 0)
      and string.format(" + hop{%.0f}", e._hop) or ""
    local _hop_detail = (e._hop and e._hop > 0)
      and string.format(
        "|hop:%.0f[REFUEL_BASE_HOP_PENALTY] — we're parked on ANOTHER base; switching to this one is wasteful churn, so it's penalised. Finish where you are (a depleted current base drops out, freeing the move).",
        e._hop)
      or ""
    -- Low-stock markup chips (refuel_low_stock_mult): one per short resource,
    -- multiplied in right after the danger penalty, before the additive
    -- mine/hop terms -- same order the code applies them.
    local _low_token, _low_detail = "", ""
    if e._low_d and (e._low_d.low_arm or e._low_d.low_sh) then
      _low_token = " ×" .. refuel_low_stock_chip(e._low_d)
      _low_detail = string.format(
        "|low:observed stock below %d[REFUEL_MIN_STOCK] for a resource we need (obs %dt ago, fresh < %d[REFUEL_OBS_STALE]): %s%s— multiply by %.2f",
        C.REFUEL_MIN_STOCK, e._low_d.age or 0, C.REFUEL_OBS_STALE,
        e._low_d.low_arm and string.format("armour obs %d, we need armour → x%.2f[REFUEL_LOW_ARMOUR_MULT] ",
                                           e._low_d.obs_arm or 0, C.REFUEL_LOW_ARMOUR_MULT or 1) or "",
        e._low_d.low_sh and string.format("shells obs %d, we need shells → x%.2f[REFUEL_LOW_SHELLS_MULT] ",
                                          e._low_d.obs_sh or 0, C.REFUEL_LOW_SHELLS_MULT or 1) or "",
        e._low_mult or 1)
    end
    local _d_astar = string.format(
      "danger-weighted Dijkstra-slate travel cost to base (%d,%d) = %.0f; path %s",
      e._mx or 0, e._my or 0, raw, e._path or "(not traced)")
    local _d_base = string.format(
      "%.0f[REFUEL_BASE_COST] flat floor so refuel-at-own-base isn't ~0",
      C.REFUEL_BASE_COST)
    f = string.format(
      "A*{%.0f}@(%d,%d) + base{%.0f} + danger{%.0f}%s + stale{%.0f} + contest{%.0f} + deplete{%.0f}%s%s%s%s%s"..
      "||A*:%s|base:%s|danger:%s|stale:%s|contest:%s|deplete:%s%s%s%s%s%s",
      raw, e._mx or 0, e._my or 0, C.REFUEL_BASE_COST, e._dang, _pad_token, e._stale, e._contest, e._dep, _shape_head, _safe_token, _low_token, _mine_token, _hop_token,
      _d_astar, _d_base, _d_danger, _d_stale, _d_contest, _d_deplete, _shape_detail, _safe_detail, _low_detail, _mine_detail, _hop_detail)
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
    -- INF terms print as "INF" (a %.0f of 1e30 is a 31-digit number and
    -- math.huge prints "inf"); the SKIP prefix + skip: row name which
    -- term(s) went INF and why, so an INF row is readable at a glance.
    local function _fmt_inf(v) return (v or 0) >= 1e29 and "INF" or string.format("%.0f", v or 0) end
    local _skip_prefix, _d_skip = "", "cost is finite; no INF term"
    if e._skipped then
      _skip_prefix = "SKIP " .. e._skipped .. " !! "
      local _skip_parts = {}
      for r in string.gmatch(e._skipped, "[^+]+") do
        _skip_parts[#_skip_parts + 1] = ({
          low_shells     = string.format("shells=%d < pill_hp=%d → can't finish the pill, ammo=INF", _sh_now, e._hpv or 0),
          no_spot        = string.format("no firing tile with LOS on the pill within ATTACK_PILL_STANDOFF=%d (or every angle banned) → diff=INF", C.ATTACK_PILL_STANDOFF or 0),
          unreachable    = "no spot AND the Dijkstra slate never reached the pill's cheapest adjacent tile (island / needs a boat / slate still building) → pickup=INF",
          no_pickup_path = string.format("spot (%d,%d) found but the spot→pill A* returned COST_INF (no path, or the 4096-node budget ran out) → pickup=INF", e._spot_mx or 0, e._spot_my or 0),
        })[r] or (r .. ": INF from a term not named above")
      end
      _d_skip = table.concat(_skip_parts, "; ")
    end
    f = string.format(
      "%s(spot{%.0f}@(%d,%d) + pickup{%s}@(%d,%d)→(%d,%d)*wound_x2{%.2f} + (stale{%.0f} + diff{%s} + anger{%.0f} + xfire{%.0f} + intcpt{%.0f}) * hp{%.2f}%s%s + ammo{%s}%s)%s%s"..
      "||spot cost is offset-aware (target pill's danger contribution subtracted via load_danger_offset before A*); NOT scaled by hp or wound"..
      "|skip:%s|pickup:%s|hp:%s|anger:%s|stale:%s|finish_other:%s|ammo:%s|spot:%s|danger_nearby:%s|atk_tank:%s|spike:%s|spike_pen:%s",
      _skip_prefix,
      e._spot, e._spot_mx or 0, e._spot_my or 0,
      _fmt_inf(e._travel),
      e._spot_mx or 0, e._spot_my or 0, e._mx or 0, e._my or 0,
      _tw,
      e._stale, _fmt_inf(e._diff), e._anger, e._xfire, e._intcpt,
      e._hp, _wound_detail, _spike_mult_term, _ammo_str, _atk_tank_term, _spike_pen_term, _danger_term,
      -- (order: atk_tank inside the parens; spike_pen + danger_nearby are
      -- whole-cost multipliers, displayed trailing outside the parens)
      _d_skip, _d_pickup, _d_hp, _d_anger, _d_stale, _d_finish_other, _d_ammo, _d_spot, _d_danger, _d_atk_tank, _d_spike, _d_spike_pen)
  elseif p == 7 then
    local _lgm_mult_b = e._lgm_mult or 1
    local _fcov_detail = e._fcov_m and string.format(
      " x %.2f[%d friendly pill(s) cover the base tile or our approach:"
      .. " ATTACK_BASE_FRIENDLY_COVER_MULT %.2f^%d, floor %.2f]",
      e._fcov_m, e._fcov_n or 0, C.ATTACK_BASE_FRIENDLY_COVER_MULT or 0.8,
      e._fcov_n or 0, C.ATTACK_BASE_FRIENDLY_COVER_FLOOR or 0.5) or ""
    local _d_threat
    if _lgm_mult_b ~= 1 then
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT] x %d (cautious mode)%s = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, _lgm_mult_b, _fcov_detail, e._thr)
    else
      _d_threat = string.format(
        "%.2f[threat_val] x %.1f[ATTACK_BASE_THREAT_WEIGHT]%s = %.0f",
        e._tv, C.ATTACK_BASE_THREAT_WEIGHT, _fcov_detail, e._thr)
    end
    local _d_stale = fmt_stale_detail(e._age, e._stale)
    -- ARMOUR-AWARE MARKUP.  base{} is no longer the flat ATTACK_BASE_EXTRA_COST:
    -- it is that cost scaled by the WORK LEFT on this base, shells-still-needed
    -- over shells-from-full (17).  `hp n/17` is exactly that ratio, so the chip
    -- multiplies out to the printed number by hand.
    local _b_n     = e._b_n            -- shells still needed (nil = never observed)
    local _b_nfull = e._b_nfull or C.BASE_FULL_SHELLS_TO_KILL
    local _b_frac  = e._b_frac or 1.0  -- the fraction charged, AFTER stale decay
    local _b_arm   = e._b_arm          -- observed engine armour (nil = never seen)
    local _b_age   = e._b_age or 0
    local _b_decay = e._b_decay or 1.0
    -- Friendly-pill cover: multiplies BOTH the markup and the threat term.
    -- The chip has to appear on both or neither number multiplies out.
    local _fc_m, _fc_n = e._fcov_m, e._fcov_n
    local _fc_chip = _fc_m
      and string.format(" x fcover{%.2f n=%d}", _fc_m, _fc_n or 0) or ""
    local _b_chip
    if _b_n then
      _b_chip = string.format("%d x hp %d/%d%s = %.0f",
        C.ATTACK_BASE_EXTRA_COST, _b_n, _b_nfull, _fc_chip, e._base or 0)
    else
      _b_chip = string.format("%d x hp ?/%d%s = %.0f (unseen)",
        C.ATTACK_BASE_EXTRA_COST, _b_nfull, _fc_chip, e._base or 0)
    end
    local _d_base
    if _b_n then
      local _fresh = C.ATTACK_BASE_EXTRA_COST * (_b_n / _b_nfull)
      _d_base = string.format(
        "%.0f[ATTACK_BASE_EXTRA_COST] x %.3f[work left]" .. _fc_chip .. " = %.1f. armour=%d, capturable at <= %d,"
        .. " %d[DAMAGE] per shell -> ceil((%d - %d)/%d) = %d shell(s) left of the %d a FULL base"
        .. " (%d armour) needs. Reading is %d tick(s) old: decay = min(1, %d/%d[BASE_MARKUP_STALE]) = %.2f,"
        .. " so the charged fraction is %d/%d + (1 - %d/%d) x %.2f = %.3f (fresh would have been %.1f)",
        C.ATTACK_BASE_EXTRA_COST, _b_frac, e._base or 0,
        _b_arm or 0, C.BASE_CAPTURE_ARMOUR, C.BASE_SHELL_DAMAGE,
        _b_arm or 0, C.BASE_CAPTURE_ARMOUR, C.BASE_SHELL_DAMAGE, _b_n, _b_nfull,
        C.BASE_FULL_ARMOUR, _b_age, _b_age, C.BASE_MARKUP_STALE, _b_decay,
        _b_n, _b_nfull, _b_n, _b_nfull, _b_decay, _b_frac, _fresh)
      _d_base = _d_base .. _fcov_detail
    else
      _d_base = string.format(
        "%.0f[ATTACK_BASE_EXTRA_COST] x 1.000%s = %.1f. This base has NEVER reported its stock"
        .. " (EVENT_BASE_STOCK fires only when a base's armour/shells/mines change), so there is no"
        .. " armour reading to discount and it pays the full flat markup",
        C.ATTACK_BASE_EXTRA_COST,
        e._fcov_m and string.format(" x %.2f", e._fcov_m) or "",
        e._base or 0)
      _d_base = _d_base .. _fcov_detail
    end
    -- STEAL chip: whether the imminent-base-steal floor took this row, or which
    -- gate it failed.  See refresh_base_steal.
    local _st_chip, _d_steal = "", ""
    if e._steal then
      _st_chip = string.format(" STEAL{%.0f}", C.BASE_STEAL_COST)
      _d_steal = string.format(
        "|steal:YES -- fresh armour %d (<= %d[BASE_STEAL_MAX_ARMOUR]) is %d shell(s) from capturable,"
        .. " base is %d tile(s) away (<= %d[BASE_STEAL_RANGE]), nothing covers the base tile or the"
        .. " approach line, and we have the ammo. Cost is SNAPPED to %.0f[BASE_STEAL_COST] so one shell"
        .. " then the armour-0 IMMINENT capture wins the pool outright",
        _b_arm or 0, C.BASE_STEAL_MAX_ARMOUR, _b_n or 0,
        e._steal_dist or 0, C.BASE_STEAL_RANGE, C.BASE_STEAL_COST)
    elseif e._steal_rej then
      _st_chip = string.format(" steal{no:%s}", e._steal_rej)
      local why = ({
        stale_obs      = string.format("no armour reading, or one older than %d[BASE_STEAL_OBS_STALE] ticks (age=%s)",
                           C.BASE_STEAL_OBS_STALE, (e._b_age and e._b_age < 1e17) and tostring(e._b_age) or "never"),
        not_capturable = string.format("armour %s > %d[BASE_STEAL_MAX_ARMOUR] -- %s shells from capturable, that is a normal attack_base errand",
                           tostring(_b_arm), C.BASE_STEAL_MAX_ARMOUR, tostring(_b_n)),
        out_of_range   = string.format("%s tiles away > %d[BASE_STEAL_RANGE]",
                           tostring(e._steal_dist), C.BASE_STEAL_RANGE),
        no_shells      = string.format("shells <= %d[SHELL_RESERVE] + %s needed",
                           C.SHELL_RESERVE or 0, tostring(_b_n)),
        covered        = string.format("live hostile/neutral pill#%s can put a shell on the base tile or on our straight approach to it",
                           tostring(e._steal_pill)),
        inboat         = "we are afloat -- a boat has no business taking a base",
        tank_armour    = string.format("tank armour %s < %d[BASE_STEAL_MIN_ARMOUR]",
                           tostring(e._steal_arm), C.BASE_STEAL_MIN_ARMOUR),
      })[e._steal_rej] or e._steal_rej
      _d_steal = "|steal:no -- " .. why
    end
    if e._steal then
      -- The snap floor is a min(), not another term: print it as one so the
      -- displayed chain still resolves to the cost the pool competed.
      f = string.format(
        "min(A*{%.0f}@(%d,%d) + base{%s} + threat{%.0f} + stale{%.0f},%s)||base:%s|threat:%s|stale:%s%s",
        raw, e._mx or 0, e._my or 0, _b_chip, e._thr, e._stale, _st_chip,
        _d_base, _d_threat, _d_stale, _d_steal)
    else
      f = string.format(
        "A*{%.0f}@(%d,%d) + base{%s} + threat{%.0f} + stale{%.0f}%s||base:%s|threat:%s|stale:%s%s",
        raw, e._mx or 0, e._my or 0, _b_chip, e._thr, e._stale, _st_chip,
        _d_base, _d_threat, _d_stale, _d_steal)
    end
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
        -- SEA-PILL HARVEST rejects (goals.lua sea_plan_cluster). Every one of
        -- these means "the pill is in deep sea and the mine->river->boat
        -- entrance is not on", named so the panel says which gate failed.
        deepsea_no_boat = "pill lies in DEEP SEA and no harvest plan exists for it (sea harvest disabled, or the cluster scan has not run yet)",
        lgm_dead = "pill lies in DEEP SEA: the harvest needs the LGM to lay the mine and build the boat, and he is dead",
        low_shells = string.format("pill lies in DEEP SEA: fewer than %d shells left, and detonating the mine needs a reserve", C.SEA_PILL_MIN_SHELLS or 3),
        enemy_tank_near = string.format("pill lies in DEEP SEA: a visible enemy tank is within %d tiles of the cluster — a boat is one shell from dead", C.SEA_PILL_ENEMY_TANK_NEAR or 10),
        path_enemy_tank = string.format("pill lies in DEEP SEA: a visible enemy tank is within %d tiles of the boat path", C.SEA_PILL_ENEMY_TANK_NEAR or 10),
        pills_landlocked = "pill lies in a one-tile puddle, not in navigable water — a boat could not sail from any entrance to it, so there is nothing to harvest (the tile is still lethal to walk onto, which is why perception flags it)",
        no_entrance = string.format("pill lies in DEEP SEA and no land tile with a CARDINAL deep-sea neighbour within %d tiles passed the gates (pill heat / line of fire / tank or LGM reach / no firing spot)", C.SEA_PILL_ENTRANCE_MAX_DIST or 12),
        entrance_unreachable = "pill lies in DEEP SEA: the chosen entrance has no tank route (Dijkstra and A* both INF)",
        no_shells_anywhere = "pill lies in DEEP SEA: the entrance needs a mine detonated and we have neither the shells nor a reachable friendly base to fetch them from",
        no_mines_anywhere = "pill lies in DEEP SEA, we carry no mine, and there is no reachable friendly/allied base at all to fetch one from — nothing can crater the shore. (A base that merely READ empty is still offered: stock is only reported up close and bases restock.)",
        no_safe_trees = string.format("pill lies in DEEP SEA: short of the %d trees the mine+boat need (LGM_COST_BOAT 20 + LGM_COST_MINE 1) and there are not enough harvestable forest tiles within %d tiles of the entrance (%d trees per tile) that are NOT COVERED by a live hostile/neutral pillbox. Coverage, not influence: whose neighbourhood it is by the numbers does not kill an LGM, a pillbox with a line on the tile does", C.SEA_PILL_TREES_TOTAL or 21, C.SEA_TREES_RADIUS or 12, C.SEA_TREES_PER_FOREST or 4),
      })[e._reject]
      -- The three reject reasons that name a pill id.
      if not desc and type(e._reject) == "string" then
        local pn = e._reject:match("^covered_by_pill#(%d+)$")
        if pn then
          desc = string.format("this pill of the raft sits inside hostile pill #%s's firing circle (PILLBOX_RANGE, euclidean, inclusive) with a clear shell line, or every boat route to it does — one hit sinks the boat, so it is dropped from the trip. The rest of the cluster still goes.", pn)
        end
        if not desc then
          pn = e._reject:match("^cluster_member_of#(%d+)$")
        end
        if pn then
          desc = string.format("pill lies in DEEP SEA and dead pill #%s of the SAME cluster is carrying the harvest plan — one boat trip takes them all, so only the lowest-id member competes for the goal (same plan, same cost, shown here for visibility)", pn)
        end
        if not desc then
          pn = e._reject:match("^pills_covered_by_pill#(%d+)$")
          if pn then
            desc = string.format("pill lies in DEEP SEA and hostile pill #%s has a CLEAR shell line onto it — one hit sinks the boat, so the whole cluster is off (calm pills count: they wake when we sail into range)", pn)
          end
        end
        if not desc then
          pn = e._reject:match("^path_covered_by_pill#(%d+)$")
          if pn then
            desc = string.format("pill lies in DEEP SEA and hostile pill #%s has a CLEAR shell line onto a tile of the boat path", pn)
          end
        end
      end
      desc = desc or "pill exists on the map but cannot be picked this tick"
      f = string.format(
        "REJECT %s%s @(%d,%d)||reject:%s%s — %s",
        e._reject, rem_tok, e._mx or 0, e._my or 0,
        e._reject, rem_tok, desc)
    elseif e._sea then
      -- SEA-PILL HARVEST row. The land distance formula does not apply: the
      -- tank cannot walk to a pill in deep sea at all. What it pays is the
      -- drive to the entrance S, the resource legs, and the boat crossing —
      -- all shared across the cluster, all at SEA_PILL_COST_MULT because the
      -- pills themselves are free. Every factor below appears in the term
      -- list, so the final number is hand-computable from this row alone.
      local sc = e._sea
      local mult  = C.SEA_PILL_COST_MULT or 0.3
      local floor = C.SEA_PILL_COST_FLOOR or 5
      local gross = sc.gross or 0
      local scaled = gross * mult / math.max(1, sc.n or 1)
      f = string.format(
        "SEA c%d (travel{%.0f} + refuel_leg{%.0f} + tree_leg{%.0f} + boat_path{%.0f}) x %.2f[SEA_PILL_COST_MULT] / n{%d} = %.1f%s"
        .. "||sea:cluster %d, %d dead pill(s) in deep sea at %s — entrance{%s} S=(%d,%d)%s water_component{%d tiles}, park=%s, mine=%s, boat=%s (%s)"
        .. "|travel:danger-weighted route tank -> S(%d,%d) = %.0f"
        .. "|refuel_leg:%s"
        .. "|tree_leg:trees{%d}/%d[SEA_PILL_TREES_TOTAL = LGM_COST_BOAT 20 + LGM_COST_MINE 1] short{%d} need_tiles{%d} (LGM_GATHER_TREE %d trees per forest tile) forest_ok{%d} (uncovered, within %dt) -> %d x %.0f[SEA_PILL_TREE_LEG_PER_TREE] = %.0f"
        .. "|mines:mines{%d}/%d needed for the crater"
        .. "|shells:shells{%d}/%d — one to land on the mine, plus one for every FOREST tile in the F->S lane (a shell dies on the first forest and only turns it to grass)"
        .. "|coverage:a pill covers a tile only if the tile centre is within PILLBOX_RANGE %d wu of it (euclidean, inclusive — util.c utilIsItemInRange) AND the shell reaches it. CALM pills use r=%.1f tiles, HEATED ones r=%.1f (they reload fast, so the boat keeps a tile of buffer). Covered water in this component: %d tiles. Dropped members: %s"
        .. "|boat_path:boat-layer A* from the water tile beside S to the farthest pill = %.0f"
        .. "|n:cost is split across the %d pill(s) one boat trip collects"
        .. "|floor:%.0f[SEA_PILL_COST_FLOOR] — a real fight beside us must still win"
        .. "|substate:%s",
        sc.id or 0, sc.travel or 0, sc.leg_mines or 0, sc.leg_trees or 0,
        sc.boat_cost or 0, mult, sc.n or 1, sc.cost or 0,
        (scaled < floor) and string.format(" -> FLOOR %.0f", floor) or "",
        sc.id or 0, sc.n or 1,
        table.concat((function()
          local t = {}
          for i, tt in ipairs(sc.tiles or {}) do t[i] = string.format("(%d,%d)", tt[1], tt[2]) end
          return t
        end)(), " "),
        tostring(sc.entrance),
        sc.S and sc.S.mx or 0, sc.S and sc.S.my or 0,
        sc.S and string.format(" pill_at=%.0f", sc.S.hot or 0) or "",
        sc.comp_n or 0,
        sc.F and string.format("(%d,%d)", sc.F.mx, sc.F.my) or "-",
        tostring(sc.needs_mine), tostring(sc.needs_boat),
        tostring(sc.entrance_why),
        sc.S and sc.S.mx or 0, sc.S and sc.S.my or 0, sc.travel or 0,
        sc.refuel_base
          and string.format("mines=0, detour via base #%s%s = %.0f", tostring(sc.refuel_base),
                            sc.refuel_guess and " (stock not confirmed — nearest reachable friendly base)" or "",
                            sc.leg_mines or 0)
          or "0 (a mine is already aboard, or the entrance needs none)",
        (e._sea_trees_have or 0), sc.trees_need or 0, sc.trees_short or 0,
        sc.need_tiles or 0, C.SEA_TREES_PER_FOREST or 4, sc.forest_ok or 0,
        C.SEA_TREES_RADIUS or 12,
        sc.trees_short or 0, C.SEA_PILL_TREE_LEG_PER_TREE or 6, sc.leg_trees or 0,
        (e._sea_mines_have or 0), sc.mines_need or 0,
        (e._sea_shells_have or 0), sc.shells_need or 0,
        C.PILLBOX_RANGE_WU or 2048,
        ((C.PILLBOX_RANGE_WU or 2048) + (C.SEA_COVER_MARGIN_CALM_WU or 0)) / 256,
        ((C.PILLBOX_RANGE_WU or 2048) + (C.SEA_COVER_MARGIN_HOT_WU or 256)) / 256,
        sc.covered_n or 0,
        (function()
          local d = {}
          for pid, by in pairs(sc.dropped or {}) do
            d[#d + 1] = string.format("#%d covered by #%s", pid, tostring(by))
          end
          table.sort(d)
          return (#d > 0) and table.concat(d, ", ") or "none"
        end)(),
        sc.boat_cost or 0, sc.n or 1, floor,
        tostring(e._sea_sub or "-"))
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
      -- LOADED, BUILDER-LESS: which rule produced the danger multiplier.
      -- "no_builder" means CAPTURE_NO_LGM_DANGER_MULT replaced the ordinary
      -- cautious-mode CAUTIOUS_MODE_MULT for the duration of the state.
      local _mult_src = e._mult_src or (_lgm_mult_c ~= 1 and "cautious" or "none")
      local _lgm_mult_str = (_lgm_mult_c ~= 1)
        and string.format(" × %d (%s)", _lgm_mult_c,
              (_mult_src == "no_builder") and "no_builder{loaded, builder-less}"
              or "cautious mode") or ""
      local _lgm_mult_det = (_mult_src == "no_builder")
        and string.format(
          "|no_builder:LOADED, BUILDER-LESS — the danger multiplier is CAPTURE_NO_LGM_DANGER_MULT{%d}, NOT the cautious-mode CAUTIOUS_MODE_MULT{%d} it replaces. A corpse is only worth the trip if we live to place it, and with no builder we cannot place anything at all",
          C.CAPTURE_NO_LGM_DANGER_MULT or 0, C.CAUTIOUS_MODE_MULT or 0)
        or ""
      local _dm_str = e._dist_method or "dij"
      local _fd = e._free or 0
      local _fm = e._free_mult or 1
      local _free_mult = (_fd > 0.001) and (" − %.1f[FREE]"):format(_fd) or ""
      local _free_det
      if _fm == 0 then
        -- The bonus was computed and then scaled to nothing: say so, or the
        -- row looks like it merely failed the distance/danger ramp.
        local _fd_raw = C.CAPTURE_FREE_PILL_VALUE
                        * (1 - math.min(1, (e._dv or 0) / C.CAPTURE_FREE_PILL_DANGER_FALLOFF))
        _free_det = string.format(
          "|free:0 — freebonus{x%.0f} — LOADED, BUILDER-LESS. The ramp would have paid up to −%.1f, and CAPTURE_NO_LGM_FREE_BONUS_MULT{%d} scales the whole thing to zero: a pill we cannot place is not free, it costs a drive we may not survive",
          _fm, _fd_raw, _fm)
      elseif _fd > 0.001 then
        _free_det = string.format("|free:close/safe grab (≤ %.1f tiles, danger %.1f) → −%.1f (0=none .. %.1f=best, floor %.1f)%s",
              C.TANK_COMBAT_ENGAGE_RANGE * C.CAPTURE_FREE_PILL_RANGE_MULT, e._dv or 0, _fd, C.CAPTURE_FREE_PILL_VALUE, C.CAPTURE_FREE_PILL_MIN_COST,
              (_fm ~= 1) and string.format(" × freebonus{%.2f}", _fm) or "")
      else
        _free_det = "|free:none (too far or too dangerous)"
      end
      if _fm == 0 then _free_mult = " freebonus{x0}" end
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
        local _strict_now = (_mult_src == "no_builder")
                            and C.CAPTURE_NO_LGM_ROUTE_STRICT
        _route_det = string.format(
          "|route:DANGER A* — the direct (danger_scale=0) run %s in the wsim (%d dmg), so we re-priced on the danger-weighted A* route instead%s",
          _strict_now and "took DAMAGE" or "KILLED us", e._route_dmg or 0,
          _strict_now and " |routestrict:LOADED, BUILDER-LESS — CAPTURE_NO_LGM_ROUTE_STRICT moves the bar from \"the sim says we survive it\" to \"the sim says nothing touches us\": ANY predicted damage refuses the direct run" or "")
      elseif e._route_far then
        _route_det = string.format(
          "|route:NOT PROBED — pill is %d tiles away, past CAPTURE_ROUTE_MAX_TILES (%d). No direct-route A* and no wsim were run for it; the dist above is the plain Dijkstra/A* slate cost",
          e._route_far, C.CAPTURE_ROUTE_MAX_TILES or 10)
      end
      -- Land dead-pill cluster (Parts 2/3). Both chips print only when the
      -- pill is actually in a cluster of 2+, and each shows the factor it
      -- applied, so the printed chain still multiplies out to the total:
      --     (...)[−FREE] / div  [floor 5]  × guard
      local _cl_n  = e._cl_n or 1
      local _cl_dv = e._cl_div or 1
      local _cl_gn = e._cl_guard_n or 0
      local _cl_gm = e._cl_guard_m or 1.0
      local _cluster_mult, _cluster_det = "", ""
      if _cl_n > 1 and e._cl_off then
        -- LOADED, BUILDER-LESS: the ÷n discount is OFF. The guard SURCHARGE
        -- (below) still applies; only the discount is suppressed.
        _cluster_mult = string.format(" nocluster{%d pills, /1}", _cl_n)
        _cluster_det = string.format(
          "|nocluster:%d dead pills within %d tiles of each other, and the ÷%d cluster discount is SUPPRESSED — CAPTURE_NO_LGM_CLUSTER_DISCOUNT is false while the tank is loaded and builder-less. \"Three corpses in one trip\" is only a bargain when the trip is the cheap part; here the trip is the thing that kills us",
          _cl_n, C.CAPTURE_CLUSTER_RADIUS or 3,
          math.min(_cl_n, C.CAPTURE_CLUSTER_DIVISOR_MAX or 6))
        if _cl_gn > 0 then
          _cluster_mult = _cluster_mult ..
            string.format(" guard{%d pills, x%.2f}", _cl_gn, _cl_gm)
          _cluster_det = _cluster_det .. string.format(
            "|guard:%d live hostile/neutral pill(s) can put a shell on a cluster tile → x%.2f. The guard is a SURCHARGE and is never suppressed",
            _cl_gn, _cl_gm)
        end
      elseif _cl_n > 1 then
        _cluster_mult = string.format(" cluster{%d} /%dX", _cl_n, _cl_dv)
        _cluster_det = string.format(
          "|cluster:%d dead pills within %d tiles of each other — ONE errand, so each member pays its share:"
          .. " cost / min(%d, %d[DIVISOR_MAX]) = /%d, floored at %.0f",
          _cl_n, C.CAPTURE_CLUSTER_RADIUS or 3, _cl_n,
          C.CAPTURE_CLUSTER_DIVISOR_MAX or 6, _cl_dv,
          C.CAPTURE_CLUSTER_MIN_COST or 5)
        local _gids = e._cl_guard_ids
        if _cl_gn > 0 then
          _cluster_mult = _cluster_mult ..
            string.format(" guard{%d pills, x%.2f}", _cl_gn, _cl_gm)
          _cluster_det = _cluster_det .. string.format(
            "|guard:%d live hostile/neutral pill(s) [%s] can put a shell on a cluster tile"
            .. " (within %.0f wu + heat margin AND a clear line of fire) → x(1 + %.2f × %d) capped at %.1f = x%.2f."
            .. " Route danger is NOT counted here — the Dijkstra/A* dist above already prices it",
            _cl_gn, _gids and table.concat(_gids, ",") or "?",
            C.PILLBOX_RANGE_WU or 2048, C.CAPTURE_CLUSTER_GUARD_MULT or 0.75,
            _cl_gn, C.CAPTURE_CLUSTER_GUARD_MAX or 4.0, _cl_gm)
        else
          _cluster_det = _cluster_det ..
            "|guard:none — no live hostile/neutral pillbox has both the range and a clear line onto any cluster tile, so the discount stands undiluted"
        end
      end
      f = string.format(
        "(base{%d} + dist{%.1f}[%s]@(%d,%d) + danger{%.1f} + intcpt{%.0f})%s%s||dist:%.0f^1.5 × %.3f[DIST_SCALE] = %.1f [%s]|danger:%.1f × %.3f[DANGER_SCALE]%s = %.1f%s%s%s%s%s",
        C.CAPTURE_PILL_BASE_COST, e._ds, _dm_str, e._mx or 0, e._my or 0, _cpill_danger_score, _intcpt, _free_mult, _cluster_mult,
        raw, C.CAPTURE_PILL_DIST_SCALE, e._ds, _dm_str,
        e._dv, C.CAPTURE_PILL_DANGER_SCALE, _lgm_mult_str, _cpill_danger_score, _lgm_mult_det, intcpt_det, _free_det, _route_det, _cluster_det)
    end
  elseif p == 3 then
    -- capture_base: the A* number IS the danger-weighted dijkstra travel cost —
    -- danger is baked into the per-tile path cost, NOT a separate additive term,
    -- so a CLOSE base behind enemy fire can out-cost a FAR safe one (by design:
    -- biases toward safer captures). Plus staleness for a neutral base unseen a
    -- while. Note: BASE_PILL_COVER_PEN is applied only on the finalize path
    -- (eval_capture_base), NOT this rolling cost, so it's not part of this total.
    local _cb_dv = threat.at(e._mx or 0, e._my or 0)
    -- LOADED, BUILDER-LESS surcharge: the one danger term pool 3 otherwise
    -- does not have. Absent (and its chip absent) at the keel value 0.
    local _cb_nb_chip, _cb_nb_det = "", ""
    if e._p3_mult then
      _cb_nb_chip = string.format(" + nobuild_danger{%.1f}", e._p3_dang or 0)
      _cb_nb_det = string.format(
        "|nobuild_danger:LOADED, BUILDER-LESS — threat.at(base)%.1f × DANGER_SCALE{%.3f} × CAPTURE_BASE_NO_LGM_DANGER_MULT{%d} = %.1f, ADDED. A base is a tile you have to sit on, and sitting on a covered one with an unplaceable stack aboard loses the whole load. At the keel value 0 this term does not exist, which is what pool 3 did before",
        e._p3_dv or 0, C.CAPTURE_PILL_DANGER_SCALE, e._p3_mult, e._p3_dang or 0)
    end
    f = string.format(
      "A*{%.0f}@(%d,%d) + stale{%.0f}%s||A*:danger-weighted dijkstra travel to base; danger at base tile=%.1f is BAKED INTO the path cost (not a separate term) — that's why a near dangerous base can cost more than a far safe one|stale:%s%s",
      raw, e._mx or 0, e._my or 0, e._stale or 0, _cb_nb_chip, _cb_dv,
      fmt_stale_detail(e._age, e._stale), _cb_nb_det)
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
  elseif e._blitz_off then
    -- Same slot, but this bot has blitz=off (init_arg): an open call exists on
    -- this pill and the join discount was deliberately NOT applied, so the row
    -- is the plain solo cost.
    local disp = " x blitz_discount{1.00 off}"
    local map  = string.format("|blitz_discount:OFF (%s) — an open call on this pill, but this bot never joins; cost unchanged", tostring(e._blitz_off))
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
    if e._steal_units then
      term_map = term_map .. string.format(" (ours priced on our %s cost %.1f)",
                                           e._steal_units, e._steal_cost or our)
    end
  elseif e._steal_released then
    -- The steal-yield block on this row ENDED EARLY: we had yielded the pill,
    -- and the stealer's own advert then showed it going somewhere else, so the
    -- pill is priceable again well inside STEAL_YIELD_BLOCK. Say so on the row —
    -- otherwise a pill that was ally_claimed for 300 ticks a moment ago just
    -- silently reappears as a candidate.
    local sr = e._steal_released
    term_disp = ""
    term_map  = string.format(
      "|steal_yield:RELEASED — yielded to p%s, released after %dt of STEAL_YIELD_BLOCK(%d) because %s; re-priced at our own cost %.0f",
      tostring(sr.by or "?"), sr.after or 0, C.STEAL_YIELD_BLOCK or 300,
      tostring(sr.reason or "?"), e.cost or 0)
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
      -- Break a cost tie on the lower pill id. state.cost_cache is keyed by
      -- strings ("<pool>:<id>"), so with a bare strict `<` two equal-cost
      -- pool-4 entries were resolved by pairs() order — a per-process hash
      -- order. The winner is the ONE pill that gets an accurate A* route cost
      -- instead of the cheap Dijkstra lookup, which moves pool-4 ranking and
      -- so the chosen goal: a tie forked same-seed games.
      for _, ce in pairs(state.cost_cache or {}) do
        if ce._p == 4 and not ce._reject then
          local c = ce.cost or math.huge
          if c < _best or (c == _best and _fid and (ce._id or math.huge) < _fid) then
            _best = c; _fid = ce._id
          end
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

  -- (The debug-only "chunked pill-eval pre-probe" that used to sit here is
  -- gone: it advanced the pool-6 evaluations in chunks and BAILED the whole
  -- queue drain while any was in flight, so a -brain-debug game evaluated
  -- candidates on different ticks than production and made different
  -- decisions from tick ~53 of seed 4242. Recorded games must be the
  -- production game; the C fast path evaluates inline in both modes now.)

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
      -- Pool-1 candidate trace: every base the refuel pool looked at, priced
      -- or rejected, one line each, so a log answers "why not THAT base".
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
      -- REFUEL-PAD READ (refuel_pad_pill_reach, and the twin in
      -- nearest_resupply_base). The pill layer of the danger grid carries a
      -- 1-tile pad (PILL_RANGE_MAP 9 vs PILL_FIRE_RANGE 8) that is there to
      -- steer the DRIVE. A refuel candidate is a place we PARK, and a pill
      -- either reaches the tile we park on or it does not. When none does,
      -- the pill layer comes off and only the enemy-tank layer is left. This
      -- has to happen BEFORE danger_cost, _safe_refuel and the
      -- REFUEL_DANGER_PENALTY multiply below, all of which read danger_val.
      local _pad_reach, _pad_id, _pad_dist = refuel_pad_pill_reach(world, obj.mx, obj.my)
      local _pad_safe = not _pad_reach
      local _pad_removed = 0
      if _pad_safe then
        _pad_removed = threat.pill_at(obj.mx, obj.my) or 0
        danger_val = math.max(0, danger_val - _pad_removed)
      end
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
      -- Low-stock markup: x1.10 per resource we need that this base is
      -- observed short of (refuel_low_stock_mult). Multiplicative like the
      -- danger penalty above; the additive ally/hop terms below come after.
      local _low_mult, _low_d = refuel_low_stock_mult(obj, info, state, now)
      score = score * _low_mult

      -- Ally-claimed SOFT penalty: +ALLY_CLAIMED_REFUEL_PENALTY for EACH ally
      -- currently broadcasting refuel_at_base on THIS base. Refuel is NOT in
      -- _REJECT_POOLS — instead of a hard first-come-first-served reject, a base
      -- others are already heading to just gets pricier per ally. A closer / more
      -- urgent bot can still pick it (and then brakes beside it via the
      -- wait_for_ally substate in init.lua if an ally tank is parked on the
      -- tile); everyone else drifts to emptier bases. Skip allies whose tank is
      -- dead (a stale broadcast shouldn't price a base we can actually use).
      -- Asymmetric claim resolution (was: every claimant paid, so two bots
      -- eyeing the same base both left it -- g9fix2 bot2/bot3 danced
      -- 11<->12<->13 at every replan, bot3 driving 20 tiles to #13 with #12
      -- two tiles away). Only the LOSER of a claim pays the penalty:
      --   1. need first: a tank below a COMBAT line (ally low=1 flag)
      --      beats a topping-off one, whoever is closer -- a close full
      --      tank must not hog the stock while a starved one waits;
      --   2. both alike: lower player number keeps the base.
      local our_low = (info.armour or 0) < (C.ARMOUR_COMBAT or 30)
                   or (info.shells or 0) < (C.SHELLS_COMBAT or 30)
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
              local ally_low = (h.low == "1")
              local we_pay
              if ally_low ~= our_low then
                we_pay = ally_low          -- the needy one keeps the base
              else
                we_pay = ally_pn < info.player_number
              end
              if we_pay then
                ally_claimed_n  = ally_claimed_n + 1
                ally_claimed_by = ally_claimed_by or ally_pn
              end
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
         and not (obj.mx == tmx and obj.my == tmy)
         -- Yield waiver -- see the helper-path twin above.
         and (now - (state._refuel_yield_tick or -1e9)) > 25 then
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
        -- Refuel-pad reach verdict, so the formula panel can say WHY the
        -- danger term is what it is (and, when zeroed, what came off).
        _pad_safe = _pad_safe, _pad_pill = _pad_id, _pad_dist = _pad_dist,
        _pad_removed = (_pad_removed > 0) and _pad_removed or nil,
        _path = _p1_path_str,
        _ally_n = (ally_claimed_n > 0) and ally_claimed_n or nil,
        ally_claimed_pen = (ally_claimed_cost > 0) and ally_claimed_cost or nil,
        ally_claimed_by  = ally_claimed_by,
        _hop = (_hop_cost > 0) and _hop_cost or nil,
        _low_mult = (_low_mult ~= 1.0) and _low_mult or nil,
        _low_d = _low_d,
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
      -- Attack_base extra costs (same as finalization path).
      -- base_extra is the ARMOUR-AWARE markup, not the flat constant: a base
      -- one shell from falling is not the same errand as an untouched one.
      -- See base_markup (which also handles the staleness decay).
      local base_extra, threat_cost, _threat_val = 0, 0, 0
      local _p7_lgm_mult = 1
      local _p7_n, _p7_nfull, _p7_frac, _p7_arm, _p7_age, _p7_decay
      local _p7_fcov_m, _p7_fcov_n = 1.0, 0
      if pool_idx == 7 then
        base_extra, _p7_n, _p7_nfull, _p7_frac, _p7_arm, _p7_age, _p7_decay =
          base_markup(obj, now)
        _threat_val = threat.at(obj.mx, obj.my)
        _p7_lgm_mult = state.cautious_mode and C.CAUTIOUS_MODE_MULT or 1
        threat_cost = _threat_val * C.ATTACK_BASE_THREAT_WEIGHT * _p7_lgm_mult
        -- Friendly pill cover discounts the ENGAGE half only (markup + threat).
        -- See base_friendly_cover.
        _p7_fcov_m, _p7_fcov_n = base_friendly_cover(world, obj, tmx, tmy)
        if _p7_fcov_n > 0 then
          base_extra  = base_extra  * _p7_fcov_m
          threat_cost = threat_cost * _p7_fcov_m
        end
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
          -- Evaluate inline in EVERY mode. Two things used to sit here and both
          -- made the recorded (-brain-debug) brain play a different game from
          -- production:
          --   * `if BRAIN_DEBUG_MODE and <cache fresh> then <use cache> else
          --      <evaluate> end` -- lua_strip removes an `if BRAIN_DEBUG_MODE`
          --      block WHOLE, else branch included, so from the 1.7 baseline
          --      (7a390beb) the production opt/ brain never called
          --      evaluate_pill_difficulty on this path: diff_score stayed nil
          --      and `diff_cost = diff_score or 999` priced every attack_pill
          --      candidate here at maximum difficulty with no firing spot.
          --   * the debug-only chunked pre-probe (removed, see build_eval_queue)
          --      that filled that cache on a different tick schedule.
          -- Found 2026-09-03 by byte-comparing production vs recorded games of
          -- one seed (first divergence tick 46, then 53). Keep this a plain
          -- call: no `if BRAIN_DEBUG_MODE` with an else around decision code.
          diff_score, _spots, best_spot =
            attack.evaluate_pill_difficulty(obj, world, force_detailed,
                                            _scan_step, state.phase, state, tmx, tmy)
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
        -- NOT `local`: these are the per-candidate accumulators declared at the
        -- top of the queue loop, which the [diag] slow cand line reads.  A
        -- `local` here shadowed them and left diff/spot reading 0.00 while
        -- their real cost showed up only in the unattributed remainder.
        _diff_us = clock_us() - _t_diff
        _spot_us = 0
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
            -- Danger OFF for this leg. It is a walk to a DEAD pill after the
            -- take, and with the field on this A* was returning COST_INF for
            -- perfectly open pills: in a pill cluster the tiles beside the
            -- target cost hundreds while open ground costs 2-10, the
            -- heuristic (plain distance) is useless against that, and the
            -- search floods outward across cheap ground until it burns the
            -- 4096-node budget -- reported as INF, i.e. "no pickup path"
            -- (20260831_000722 bot3 t=14761: ids 12/8/10/1/11 each spent
            -- 1.1-1.65 ms here and scored 1e30 beside a 135-cost neighbour).
            -- Crossfire from OTHER pills is already priced by xfire_cost, so
            -- nothing is lost; a remaining INF now means a genuine wall or
            -- water block (the no_pickup_path skip reason).
            cpf.set_config("danger_scale", 0)
            local raw = cpf.cost_to(best_spot.mx, best_spot.my, obj.mx, obj.my,
                                    boat_flag, shells, trees, mines, armour, 4096)
            cpf.set_config("danger_scale", 1.0)
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
      -- IMMINENT BASE STEAL (pool 7): the per-replan steal pick gets a snap cost
      -- floor so "one shell, then the existing armour-0 IMMINENT capture" wins
      -- the pool outright instead of losing to routine errands. refresh_base_steal
      -- owns the gates (fresh armour, range, ammo, pill coverage, tank armour,
      -- not afloat) and picks exactly ONE base.
      local _p7_steal = false
      if pool_idx == 7 and state.base_steal and state.base_steal.id == id then
        _p7_steal = true
        if c > C.BASE_STEAL_COST then c = C.BASE_STEAL_COST end
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
      local _cpill_sea = nil
      local _cpill_cl_n, _cpill_cl_div, _cpill_cl_gn, _cpill_cl_gm, _cpill_cl_gids = 1, 1, 0, 1.0, nil
      -- LOADED, BUILDER-LESS bookkeeping for the capture row's breakdown:
      -- which rule set the danger multiplier, what the free-pill bonus was
      -- scaled by, and whether the cluster discount was suppressed.
      local _cpill_mult_src, _cpill_free_mult, _cpill_cl_off = "none", 1, false
      if pool_idx == 4 then
        c, _cpill_dist_raw, _cpill_dist_score, _cpill_danger_val, _cpill_intcpt, _cpill_lgm_mult, _cpill_dist_method, _cpill_free_disc, _cpill_route_dmg, _cpill_route_far,
        _cpill_cl_n, _cpill_cl_div, _cpill_cl_gn, _cpill_cl_gm, _cpill_cl_gids,
        _cpill_mult_src, _cpill_free_mult, _cpill_cl_off =
          compute_pool4_cost(state, world, info, obj, tmx, tmy)
        -- Pool 4 skipped the smart_cost block (see above), so backfill
        -- raw_cost from compute_pool4_cost's distance — keeps the panel
        -- formula breakdown showing a meaningful raw value.
        raw_cost = _cpill_dist_raw
        -- SEA-PILL HARVEST override: a dead pill in deep sea is not reachable
        -- on the land surface at all, so the land formula above is meaningless
        -- for it. When sea_refresh produced a live plan, the plan's cost IS
        -- this candidate's cost and the plan rides along for the breakdown.
        if not info.inboat and state.perc and state.perc.deepsea_pill_ids
           and state.perc.deepsea_pill_ids[id] then
          local scl = M.sea_cluster_for(state, id)
          if scl and not scl.reject and scl.lead == id then
            _cpill_sea = scl
            c = scl.cost
            raw_cost = scl.gross
          end
        end
      end

      -- ── capture_base (pool 3) in the LOADED, BUILDER-LESS state ─────────
      -- Driving onto a neutral/beaten base is normally priced on travel and
      -- staleness alone: there is no danger term at all. With a stack aboard
      -- and nobody to place it that omission is the problem -- the base is a
      -- tile you have to sit on, and sitting on a covered one loses the whole
      -- load. So the state adds the one term the row was missing, and nothing
      -- else. CAPTURE_BASE_NO_LGM_DANGER_MULT is the multiplier on it;
      -- its keel value is 0, i.e. the term does not exist, which is exactly
      -- what pool 3 did before.
      local _p3_dv, _p3_dang, _p3_mult = 0, 0, 0
      if pool_idx == 3 and state.loaded_no_lgm
         and (C.CAPTURE_BASE_NO_LGM_DANGER_MULT or 0) > 0 then
        _p3_mult = C.CAPTURE_BASE_NO_LGM_DANGER_MULT
        _p3_dv   = threat.at(obj.mx, obj.my)
        _p3_dang = _p3_dv * C.CAPTURE_PILL_DANGER_SCALE * _p3_mult
        c = c + _p3_dang
      end

      -- Pool 5 (repair_pill): the UNIFIED repair formula, per candidate.
      -- Historically the damage discount lived only in the finalize step
      -- (applied to the raw-path winner, so the pool picked by DISTANCE,
      -- not repair merit) and the base-cost + contested x3 terms only in
      -- the fallback evaluator — two drifting copies. One copy now, here;
      -- finalize passes best_cost straight through. Dead (0-HP) pills
      -- keep raw_cost: it is already the full rebuild-in-place model.
      local _rp_dmg, _rp_contested = 0, false
      local _rp_uf, _rp_hit_age = false, nil
      local _rp_soft, _rp_soft_id, _rp_soft_d, _rp_soft_why = false, nil, nil, nil
      -- Set by the repair_pill split below; becomes entry._skipped so the row
      -- carries its reject chip like every other INF row in the grid.
      local entry_skipped_builder = nil
      if pool_idx == 5 and (obj.health or 0) > 0 then
        _rp_dmg = (C.PILLS_MAX_HEALTH or 15) - obj.health
        c = (C.REPAIR_BASE_COST or 30)
            + math.max(0, raw_cost - _rp_dmg * (C.REPAIR_DAMAGE_BONUS or 10))
            + stale_cost
        -- CONTEST TERMS (2026-09-02 rework; see REPAIR_QUIET_TICKS in
        -- constants.lua for the whole rationale). The old single "contested"
        -- flag was (enemy closer to the pill than us, at ANY range) OR (a
        -- hostile seen within DEFEND_ENEMY_NEAR_RADIUS of the pill in the last
        -- DEFEND_SIGHT_FRESH_TICKS = 600 = 12 s), and neither clause reads
        -- whether the pill is actually being SHOT. Two terms now:
        --
        --   UNDER_FIRE  (x3)   shells are landing RIGHT NOW: obj.last_hit_tick
        --                      (world.lua stamps it on real damage only) is
        --                      fresher than REPAIR_QUIET_TICKS. Walking the LGM
        --                      out here loses him and the pill gets re-damaged.
        --   ENEMY_IN_RANGE (x1.5) a hostile tank we can SEE this tick (perc's
        --                      list is this tick's real sightings, ghosts go in
        --                      a separate list) within PILL_FIRE_RANGE +
        --                      PILL_REPOSITION_ENEMY_TANK_PAD of the pill, or
        --                      closer to it than we are AND inside
        --                      DEFEND_ENEMY_NEAR_RADIUS. It MIGHT start
        --                      shooting; it is not shooting yet.
        --
        -- Exclusive: UNDER_FIRE supersedes, so the desc names exactly one and
        -- the printed multiplier is the one that was applied.
        local _lh5 = obj.last_hit_tick
        if _lh5 and _lh5 > 0 then
          _rp_hit_age = now - _lh5
          if _rp_hit_age < (C.REPAIR_QUIET_TICKS or 75) then _rp_uf = true end
        end
        local _ets5 = state.perc and state.perc.enemy_tanks
        if not _rp_uf and _ets5 then
          local _soft_r = (C.PILL_FIRE_RANGE or 8)
                          + (C.PILL_REPOSITION_ENEMY_TANK_PAD or 5)
          local _near_r = C.DEFEND_ENEMY_NEAR_RADIUS or 10
          local _our_d5 = U.mdist(tmx, tmy, obj.mx, obj.my)
          for _, _e5 in ipairs(_ets5) do
            local _ed5 = U.edist(_e5.mx, _e5.my, obj.mx, obj.my)
            local _md5 = U.mdist(_e5.mx, _e5.my, obj.mx, obj.my)
            if _ed5 <= _soft_r then
              _rp_soft, _rp_soft_id, _rp_soft_d = true, _e5.id, _ed5
              _rp_soft_why = "in_range"
              break
            elseif _md5 < _our_d5 and _md5 <= _near_r then
              -- Bounded version of the old "enemy closer than us" clause: it
              -- used to fire at ANY range, so a tank 40 tiles away that
              -- happened to be nearer the pill tripled the price.
              _rp_soft, _rp_soft_id, _rp_soft_d = true, _e5.id, _ed5
              _rp_soft_why = "closer_than_us"
              break
            end
          end
        end
        if _rp_uf then
          c = c * (C.REPAIR_UNDER_FIRE_MULT or C.REPAIR_CONTESTED_MULT or 3.0)
        elseif _rp_soft then
          c = c * (C.REPAIR_ENEMY_IN_RANGE_MULT or 1.5)
        end
        -- Legacy display flag: anything that still asks "was this contested?"
        _rp_contested = _rp_uf or _rp_soft
      end

      -- repair_pill SPLIT (BUILDER_POOL_PLAN section 7). repair_pill as a TANK
      -- goal means one thing only: "relocate so the repair becomes
      -- leash-reachable". If the man can ALREADY walk to this pill from where
      -- the tank stands, the tank has nothing to add -- the builder pool will
      -- do it without the tank moving at all -- so the row goes INF with the
      -- reason on it rather than spending a replan driving to somewhere it is
      -- already close enough to. Dead pills split the same way (the pool's
      -- `rebuild` row is the one that walks out with the wood).
      --
      -- INF rather than "hidden": the always-show rule. Seeing
      -- `REJECT builder_can (leash 8, eta 44)` on the pool-5 row and the
      -- matching `rebuild` row on the BUILDER strip is how the two halves of
      -- the split are checked against each other.
      if pool_idx == 5 and C.BUILDER_POOL_ENABLED then
        local _bc_eta = bpool.builder_can_repair(state, world, info, obj)
        if _bc_eta then
          c = 1e30
          -- The REPAIR leash, which is what builder_can_repair actually
          -- measured against (11 under BUILDER_POOL_REPAIR_LINEAR, 8 without).
          entry_skipped_builder = string.format("builder_can (leash %d, eta %d)",
                                                bpool.repair_leash(), _bc_eta)
          -- Edge-triggered on the pill: a committed relocate that finally gets
          -- close enough should produce ONE line saying the hand-off happened,
          -- not one per re-eval for the rest of the game.
          state._rsplit_seen = state._rsplit_seen or {}
          if not state._rsplit_seen[id] then
            state._rsplit_seen[id] = now
          end
        elseif state._rsplit_seen then
          state._rsplit_seen[id] = nil
        end
      end

      -- ALLY CAPTURE GUARD (2026-09-05). A dead friendly pill an ALLY has
      -- advertised as its capture_pill / pill_place target is that ally's
      -- pickup: rebuilding it in place makes it a live friendly pill, which is
      -- undriveable, so the scoop and the kill that produced the corpse are
      -- both wasted. filter_repair_pill's own dead-pill rule already covers the
      -- case where WE could take the corpse (capture outranks rebuild); this is
      -- the same rule for a teammate, and it is the only thing that covers the
      -- corner where capture rejects for us (blocked / stale tile) but the ally
      -- can still drive over. Reason and expiry: builder_pool.ally_capture_on.
      -- INF + the chip rather than a filter drop, for the always-show rule.
      if pool_idx == 5 and (obj.health or 0) == 0
         and C.BUILDER_POOL_ALLY_CAPTURE_GUARD and not entry_skipped_builder then
        local _acap = bpool.ally_capture_on(state, info, obj.mx, obj.my, id, now)
        if _acap then
          c = 1e30
          entry_skipped_builder = string.format("ally_capturing (p%d, %dt)",
                                                _acap.pn, _acap.age)
          -- Edge-triggered on the pill, like REPAIR_SPLIT above: one line when
          -- the block starts, not one per re-eval for as long as it lasts.
          state._racap_seen = state._racap_seen or {}
          if state._racap_seen[id] ~= _acap.pn then
            state._racap_seen[id] = _acap.pn
          end
        elseif state._racap_seen then
          state._racap_seen[id] = nil
        end
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
      -- nil for everything but a priced deep-sea capture. The have-counts are
      -- snapshot alongside so the row's tree/mine accounting is readable even
      -- when the panel renders it several ticks later.
      entry._sea = _cpill_sea
      if _cpill_sea then
        entry._sea_trees_have = info.trees or 0
        entry._sea_mines_have = info.mines or 0
        entry._sea_shells_have = info.shells or 0
        entry._sea_sub = (state.goal and state.goal.kind == "capture_pill"
                          and state.goal.sea and state.goal.sea.cluster == _cpill_sea.id)
                         and state.goal.substate or nil
      end
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
        -- Why an attack_pill candidate is INF. Every INF source is named
        -- (joined with "+" when several apply) so the panel / SYNC_P6 log
        -- answer "why can't it take this pill" directly, the way
        -- eval_attack_tank records `skipped`. Display only — the pool
        -- already loses on cost; nothing reads _skipped for decisions.
        --   low_shells      ammo_cost=INF: shells < pill hp, can't finish it
        --   no_spot         diff=INF: evaluate_pill_difficulty found no
        --                   firing tile with LOS inside ATTACK_PILL_STANDOFF
        --   unreachable     no spot AND the Dijkstra slate never reached the
        --                   pill's cheapest adjacent tile (island / needs a
        --                   boat / slate still building) → pickup=INF
        --   no_pickup_path  spot found but the spot→pill A* returned
        --                   COST_INF (no path or 4096-node budget spent)
        if c >= 1e29 then
          local why = {}
          if ammo_cost >= 1e29 then why[#why + 1] = "low_shells" end
          if diff_cost >= 1e29 then why[#why + 1] = "no_spot" end
          if travel >= 1e29 then
            -- goal_spot_method is only set when a firing spot was found
            -- (best_spot itself is scoped to the pool-6 block above).
            why[#why + 1] = goal_spot_method and "no_pickup_path" or "unreachable"
          end
          if #why == 0 then why[1] = "inf_other" end
          entry._skipped = table.concat(why, "+")
        end
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
        -- Armour-aware markup terms + the steal verdict, for the panel row.
        entry._b_n=_p7_n; entry._b_nfull=_p7_nfull; entry._b_frac=_p7_frac
        entry._b_arm=_p7_arm; entry._b_age=_p7_age; entry._b_decay=_p7_decay
        entry._fcov_m = (_p7_fcov_n > 0) and _p7_fcov_m or nil
        entry._fcov_n = (_p7_fcov_n > 0) and _p7_fcov_n or nil
        entry._steal = _p7_steal or nil
        local _sr = state.base_steal_rejects and state.base_steal_rejects[id]
        entry._steal_rej  = _sr and _sr.reason or nil
        entry._steal_pill = _sr and _sr.pill or nil
        entry._steal_dist = (_sr and _sr.dist)
                            or (state.base_steal and state.base_steal.id == id
                                and state.base_steal.dist) or nil
        entry._steal_arm  = info.armour or 0
      elseif pool_idx == 4 then
        entry._ds=_cpill_dist_score; entry._dv=_cpill_danger_val
        entry._intcpt=_cpill_intcpt
        entry._lgm_mult=_cpill_lgm_mult
        entry._dist_method=_cpill_dist_method
        entry._free=_cpill_free_disc
        entry._route_dmg=_cpill_route_dmg
        entry._route_far=_cpill_route_far
        entry._cl_n=_cpill_cl_n; entry._cl_div=_cpill_cl_div
        entry._cl_guard_n=_cpill_cl_gn; entry._cl_guard_m=_cpill_cl_gm
        entry._cl_guard_ids=_cpill_cl_gids
        -- LOADED, BUILDER-LESS: which rule produced the danger multiplier,
        -- what scaled the free-pill bonus, and whether the cluster discount
        -- was suppressed. All three appear as chips on the row.
        entry._mult_src=_cpill_mult_src
        entry._free_mult=_cpill_free_mult
        entry._cl_off=_cpill_cl_off or nil
      elseif pool_idx == 3 then
        entry._stale=stale_cost; entry._age=_gen_age
        -- LOADED, BUILDER-LESS danger surcharge (0 / absent otherwise).
        entry._p3_dv=(_p3_mult > 0) and _p3_dv or nil
        entry._p3_dang=(_p3_mult > 0) and _p3_dang or nil
        entry._p3_mult=(_p3_mult > 0) and _p3_mult or nil
      elseif pool_idx == 5 then
        entry._stale=stale_cost; entry._age=_gen_age
        entry._dmg=_rp_dmg
        entry._contested=_rp_contested or nil
        entry._rp_uf = _rp_uf or nil
        entry._rp_hit_age = _rp_hit_age
        entry._rp_soft = _rp_soft or nil
        entry._rp_soft_id = _rp_soft_id
        entry._rp_soft_d = _rp_soft_d
        entry._rp_soft_why = _rp_soft_why
        entry._skipped = entry_skipped_builder
      else
        entry._stale=stale_cost; entry._age=_gen_age
      end
      state.cost_cache[cache_key] = entry

      pr.candidates[#pr.candidates + 1] = {
        id = id, mx = obj.mx, my = obj.my, cost = c,
        own = obj.owner or "?", hp = obj.health or 0,
        stale = obj.last_seen and (now - obj.last_seen) or 0,
        obj = obj,  -- ref needed by rederive_pool_partial_best after sync
        reject = entry._skipped,  -- surfaces in the goal log's winner_cands
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
    --
    -- The chips add up to `total` by construction: raw (the cost computation
    -- up to _t_raw, of which adj + smart are the measured parts and `rest` is
    -- everything else inside it) + diff + spot + a final `rest` for the work
    -- after the raw window that has no timer of its own.  Before this, diff and
    -- spot printed 0.00 because of a shadowed local (see above) and ~85% of
    -- `total` was unaccounted for.
    if BRAIN_PROFILE_LOG and _t_total > 500 then
      opt.append("optimize.log", string.format(
        "  [diag] slow cand pool=%d id=%s total=%.2f = raw %.2f (adj %.2f + smart %.2f"
        .. " + rest %.2f) + diff %.2f + spot %.2f + rest %.2f  cost=%.0f obj=(%d,%d) hp=%s",
        pool_idx, tostring(id),
        _t_total / 1000, _t_raw / 1000,
        _t_adj / 1000, _t_smart / 1000,
        (_t_raw - _t_adj - _t_smart) / 1000,
        _diff_us / 1000, _spot_us / 1000,
        (_t_total - _t_raw - _diff_us - _spot_us) / 1000,
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

-- SYNC_P6 edge trigger.  The four SYNC_P6 shapes below repeat verbatim for
-- thousands of consecutive ticks -- together they were 58% of every byte print2
-- wrote (37% entry dump, 21% NO_MATCH, measured over a 12000-tick 2v2) --
-- because they describe a pool-6 cache entry that only changes when the pill,
-- the cost or the ally does.  Each line is remembered per (shape, pid) and
-- reprinted only when its text changes.  No information is lost: a line that is
-- not printed is by construction identical to the last one printed under that
-- key, so the log still states the current value of every field at every tick.
-- Debug-only: every caller sits inside an `if BRAIN_DEBUG_MODE` block that
-- lua_strip removes, so opt/ never reaches this.
local function sync_p6_log(state, key, s)
  local last = state._sync_p6_last
  if not last then last = {}; state._sync_p6_last = last end
  if last[key] ~= s then
    last[key] = s
  end
end

-- Goal kinds whose journey passes through danger, so they are worth a world
-- simulation. Module scope: this is a constant set, and rebuilding it inside
-- the per-candidate wsim loop cost one identical 7-entry table per candidate
-- per replan.
local WSIM_SIM_KINDS = { capture_base=true, capture_pill=true,
                         attack_pill=true, attack_base=true,
                         attack_tank=true, kill_lgm=true,
                         place_pill_strategic=true }

-- panel_refresh: the pool-grid JSON builder re-runs this sweep every time the
-- panel is read, which happens from Brain.get_pool_breakdown_json -- OUTSIDE
-- Brain.think, after print2.flush() has already run. Anything print2'd there
-- lands in a buffer that the next tick's print2.set_tick() clears, so those
-- SYNC_P6 lines never reached the log; they only moved the edge trigger's
-- "last printed" state and made the think path drop or repeat a line. The
-- panel pass does the same sync work, silently.
-- =========================================================================
-- STEAL HANDSHAKE: one targeted request per replan + early yield release
-- (20260903_193428_1 bot3 t=1266-1270 -- see the STEAL_YIELD_RELEASE_GRACE
-- block in constants.lua for the incident.)
-- =========================================================================

-- VARIANT (c): NO commitment-aware pricing.  The handshake trades the RAW
-- cost_cache cost on both sides, exactly as the pre-handshake baseline did --
-- there is no steal_competed_cost, no `cq=` advert tag and no competed reply
-- price.  What DOES survive from the ledger work is the per-pool bookkeeping
-- (_pool_competed / _pool_competed_raw / _competed_winner) that change 2's
-- "would goal selection actually pick this row?" gate needs; the per-row
-- competed totals are not recorded at all, because nothing reads them.
-- The `(raw)` unit tags stay on every handshake log line so the price a
-- decision was made on is still stated outright -- here it is always `raw`.

-- steal_drain_requests -- decide which (if any) of this tick's candidate steal
-- requests actually goes on the wire. Called from goal_selection, right after
-- the competed ledger is rebuilt and the pool is sorted, NOT from the sync
-- pass: the gate is "would goal selection pick this pill if the ally claim
-- were lifted?", and only here is the answer available (the sorted pool plus
-- this replan's winner). sync_ally_claimed_rejects merely QUEUES candidates on
-- state._steal_req_pending (it still owns the cheaper-than-the-holder test and
-- the STEAL_REQ_COOLDOWN), and this drains them.
--
-- The claimed row is REJECTed, so it never enters the pool and never has a
-- competed total of its own. Its total is ESTIMATED from the shaping this
-- replan applied to pool 6's actual representative:
--   shape = competed(pool 6) / raw(pool 6),  est = raw(claimed row) * shape
-- and it must clear BOTH gates:
--   1. it would win pool 6 at all      -- raw(claimed) < raw(pool-6 rep)
--   2. it would win the replan         -- est < competed(this replan's winner)
-- Gate 1 makes the estimate honest: below the rep's raw cost, this row IS the
-- rep, so it gets the rep's shaping. Where the rep is our current goal the
-- shaping carries a hysteresis DISCOUNT the claimed row would not get, which
-- makes `est` optimistic -- gate 1 still holds it to being genuinely cheaper,
-- and the log line names the shape so the call can be reproduced.
-- At most ONE request leaves per replan: the surviving row with the lowest est.
-- Vetoed rows burn no cooldown (nothing was sent), so they retry next replan.
function M.steal_drain_requests(state)
  local pend = state._steal_req_pending
  state._steal_req_pending = nil
  state._steal_req_verdict = state._steal_req_verdict or {}
  if not pend or #pend == 0 then return end
  -- sync walks cost_cache with pairs(), so the queue order is a per-process
  -- hash order. Sort by pill id: which row wins a tie (and which veto text a
  -- row gets) must be identical on two same-seed runs.
  table.sort(pend, function(a, b) return a.pid < b.pid end)
  local now = state.tick or 0
  local win      = state._competed_winner
  local rep_raw  = state._pool_competed_raw and state._pool_competed_raw[6]
  local rep_comp = state._pool_competed and state._pool_competed[6]
  -- No pool-6 representative means every attack_pill row was rejected -- which
  -- is what an ally claim on the only pill looks like. There is then no shaping
  -- to copy, so the raw pool cost goes up against the winner unscaled.
  local shape, shape_txt = 1.0, "x1.000 (no pool-6 row competed this replan; comparing the RAW pool cost against the winner)"
  if rep_raw and rep_comp and rep_raw > 0 and rep_comp < 1e29 then
    shape = rep_comp / rep_raw
    if shape < 0.1 then shape = 0.1 elseif shape > 10 then shape = 10 end
    shape_txt = string.format("x%.3f (pool-6 rep competed %.1f / raw %.1f)",
                              shape, rep_comp, rep_raw)
  end
  local best, best_est
  for _, r in ipairs(pend) do
    r.est = (r.raw or 0) * shape
    if rep_raw and (r.raw or math.huge) >= rep_raw then
      r.veto = string.format("would not even win pool 6 (our raw %.1f >= pool-6 pick's raw %.1f)",
                             r.raw or -1, rep_raw)
    elseif win and r.est >= (win.cost or math.huge) then
      r.veto = string.format("would not win the replan (est competed %.1f >= winner %s%s %.1f)",
                             r.est, win.kind or "?",
                             win.id and ("#" .. tostring(win.id)) or "", win.cost or -1)
    elseif not best or r.est < best_est then
      if best then
        best.veto = string.format("not the best claimed row this replan (est %.1f > #%d's %.1f)",
                                  best.est, r.pid, r.est)
      end
      best, best_est = r, r.est
    else
      r.veto = string.format("not the best claimed row this replan (est %.1f > #%d's %.1f)",
                             r.est, best.pid, best_est)
    end
  end
  for _, r in ipairs(pend) do
    if r == best then
      state._steal_req_sent = state._steal_req_sent or {}
      state._steal_req_sent[r.pid] = { to = r.to, tick = now, cost = r.our_cost }
      state._steal_outbox = state._steal_outbox or {}
      state._steal_outbox[#state._steal_outbox + 1] =
        string.format("/info stq %d %d %d", r.pid, r.to,
                      math.floor(math.min(r.our_cost, 9999999) + 0.5))
      state._steal_req_verdict[r.pid] = string.format(
        "SENT t=%d: our %.1f(%s) vs p%d's %.1f, est competed %.1f vs winner %.1f, shape %s",
        now, r.our_cost, r.units, r.to, r.match_cost or -1, r.est,
        win and win.cost or -1, shape_txt)
    else
      state._steal_req_verdict[r.pid] = string.format("HELD t=%d: %s", now, r.veto or "?")
    end
  end
end

local function sync_ally_claimed_rejects(state, info, panel_refresh)
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

  -- Candidate steal requests for THIS pass. Rebuilt every sync so a row that
  -- stopped qualifying can't leave a stale request queued; drained (at most one
  -- sent) by steal_drain_requests at the end of goal_selection, which is the
  -- only place that knows what this replan actually picked. The panel refresh
  -- re-runs this whole sweep outside think and must not touch the queue.
  if not panel_refresh then state._steal_req_pending = nil end

  local priority_ticks = C.ALLY_PILL_TAKE_PRIORITY_TICKS or 100
  -- Per-pill snapshot keyed by pill_id: { until_t, by }.  Set ONCE the
  -- first time a pool-4 entry for that pill appears in cost_cache; the
  -- countdown ticks down from there regardless of the ally's substate.
  -- Survives cache eviction-and-recreation (e._priority_check_done on
  -- the entry alone wouldn't, because step_eval_queue rebuilds the
  -- entry table on every re-eval).
  state.pill_priority_set = state.pill_priority_set or {}
  local pill_priority_set = state.pill_priority_set

  -- SEA-PILL HARVEST claim. An ally's `goal=capture_pill target=N` already
  -- becomes an ally_claimed REJECT for pill N below. A deep-sea target is
  -- different: the ally is mining a shore and building ONE boat, and that boat
  -- takes the whole CLUSTER — so the claim has to cover every dead sea pill
  -- within SEA_PILL_CLAIM_RADIUS of N, from entrance_plan onward, or two bots
  -- mine the same shore for the same three pills. Our OWN cluster members are
  -- never claimed against us: we are the claimant.
  local sea_ally_claim = nil
  do
    local perc = state.perc
    if perc and perc.deepsea_pill_ids and next(perc.deepsea_pill_ids) ~= nil then
      local ours = nil
      local g = state.goal
      if g and g.kind == "capture_pill" and g.sea then
        ours = {}
        for _, pid in ipairs(g.sea.ids or {}) do ours[pid] = true end
      end
      local R = C.SEA_PILL_CLAIM_RADIUS or 3
      -- Sorted ally ids: the claim decides rejections, so pairs() order must
      -- not choose which ally "wins" a pill claimed by two.
      local apns = {}
      for apn in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
        if apn ~= self_pn then apns[#apns + 1] = apn end
      end
      table.sort(apns)
      for _, apn in ipairs(apns) do
        local slot = ally_state.get(apn)
        local h = slot and slot.active and slot.info
        if h and h.goal == "capture_pill" then
          -- Positions come from the cost_cache rows themselves (they carry
          -- _mx/_my for every pool-4 pill), so no world reference is needed.
          local aid = tonumber(h.target)
          if aid and perc.deepsea_pill_ids[aid] then
            local amx, amy
            for _, ce in pairs(cache) do
              if ce._p == 4 and ce._id == aid then amx, amy = ce._mx, ce._my break end
            end
            if amx then
              for _, ce in pairs(cache) do
                if ce._p == 4 and ce._id and perc.deepsea_pill_ids[ce._id]
                   and ce._mx and U.mdist(ce._mx, ce._my, amx, amy) <= R
                   and not (ours and ours[ce._id]) then
                  sea_ally_claim = sea_ally_claim or {}
                  if sea_ally_claim[ce._id] == nil then
                    sea_ally_claim[ce._id] = { by = apn,
                      remaining = math.max(0, (C.SQUAD_ALLY_MAX_AGE or 1750) - (now - (slot.last_tick or now))),
                      root = aid }
                  end
                end
              end
            end
          end
        end
      end
    end
  end

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
      -- Sea-cluster claim (see sea_ally_claim above): an ally harvesting a
      -- deep-sea cluster owns every pill in it, not just its broadcast target.
      local sac = sea_ally_claim and sea_ally_claim[e._id]
      -- "noclaimdead": an ally's sea-cluster claim is still an ally claim on a
      -- DEAD pill, so it is ignored too — otherwise the flag would leave the
      -- biggest pool-4 ally_claimed source in place.
      if sac and state.ally_claim_dead_off then
        sac = nil
        if e._reject == "ally_claimed" then
          e._reject = nil
          e._reject_remaining = 0
          e.formula = nil
        end
      end
      if sac and not we_targeted_it then
        if e._reject ~= "ally_claimed" then
          e._reject = "ally_claimed"
          e.formula = nil
        end
        e._reject_remaining = sac.remaining or 0
        e._ally_by          = sac.by
        e._ally_score       = nil
        goto continue_entry
      end
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
        -- (4 tanks total by default). If the squad on this pill is already full and we're
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
          local full_n = squad.blitz_max()   -- commander + soldiers, per-bot
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
      -- Units of the ally's advertised `cost=`. VARIANT (c) advertises no cost
      -- tag at all, so every ally price is a RAW pool cost. Kept as a variable
      -- (rather than inlined) so the DECISION line still states the units the
      -- comparison was made in.
      local match_units = "raw"
      local tank_dead_at = state.tank_dead_at
      local _diag_p6 = BRAIN_DEBUG_MODE and pool_idx == 6 and e._id
                       and not panel_refresh
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
          sync_p6_log(state, "M" .. tostring(e._id), string.format(
            "SYNC_P6 pid=%d MATCHED ally=p%d sub=%s force_engaging=%s ally_cost=%s our_cost=%.0f",
            e._id, match_pn, tostring(match_sub or "?"),
            tostring(force_engaging_reject),
            tostring(match_cost), e.cost or 0))
        elseif _diag_scanned > 0 then
          sync_p6_log(state, "M" .. tostring(e._id), string.format(
            "SYNC_P6 pid=%d NO_MATCH (%d allies scanned, none on attack_pill #%d)",
            e._id, _diag_scanned, e._id))
        end
      end

      if match_pn then
        local our_cost = e.cost
        -- VARIANT (c): our side of the handshake is the RAW cost_cache cost on
        -- every pool, pool 6 included. No commitment/hysteresis adjustment is
        -- read into the price.
        local our_units = "raw"
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
        if pool_idx == 4 and state.ally_claim_dead_off then
          -- ── "noclaimdead" (BRAIN_INIT_ARG): allies' claims on a DEAD pill are
          -- ignored, so several bots race to scoop the same body — which is the
          -- point: they draw fire on the way in. Keep the row unconditionally
          -- (the we_keep branch below clears any ally_claimed reject already on
          -- it). Scope is pool 4 only: pool 6 (attack_pill on a LIVE pill)
          -- keeps today's claim/steal de-confliction, and the pool-1 refuel
          -- soft penalty is untouched.
          we_keep = true
          _reason = "claims_dead_off (noclaimdead init_arg — allies' capture_pill claims ignored)"
        elseif pool_idx == 6 and e._id then
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
                  -- QUEUE, don't send. Being cheaper than the holder is not a
                  -- reason to ask for a pill we would not then go and take:
                  -- 20260903_193428_1 p4 asked bot3 for pills #4 and #0 while its
                  -- own goal was (and stayed) attack_pill #5, and bot3's pool 6
                  -- carried both as ally_claimed for 300 ticks for nothing. The
                  -- "would we actually pick it?" test needs this replan's sorted
                  -- pool, so steal_drain_requests makes the call at the end of
                  -- goal_selection and sends at most one.
                  if not panel_refresh then
                    state._steal_req_pending = state._steal_req_pending or {}
                    state._steal_req_pending[#state._steal_req_pending + 1] = {
                      pid = e._id, to = match_pn,
                      our_cost = our_cost, units = our_units,
                      raw = e.cost, match_cost = match_cost, tick = now,
                    }
                  end
                  _reason = "steal_candidate (we_cheaper than p" .. match_pn
                            .. ", pending the would-we-pick-it check)"
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
        -- Steal chips for the pool-grid row: which price we quoted, and what
        -- steal_drain_requests did with the request last replan.
        if pool_idx == 6 and e._id then
          local _note = state._steal_req_verdict and state._steal_req_verdict[e._id]
          if e._steal_reason ~= _reason or e._steal_units ~= our_units
             or e._steal_note ~= _note or e._steal_cost ~= our_cost then
            e._steal_reason = _reason
            e._steal_units  = our_units
            e._steal_cost   = our_cost
            e._steal_note   = _note
            e.formula = nil
          end
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
        -- ── YIELD RELEASE ────────────────────────────────────────────────
        -- The block exists to cover the GAP between our yield and the winner's
        -- own claim broadcast landing. Once the stealer has spoken again and is
        -- plainly NOT on this pill, the gap is over and the block is just a
        -- self-inflicted lockout: 20260903_193428_1 bot3 held pills #0 and #4
        -- ally_claimed by p4 for the full 300 ticks (still up at t=1487) while
        -- p4 sat on attack_pill #5 the whole time. STEAL_YIELD_BLOCK stays the
        -- ceiling; this just ends it early when the facts say so.
        --
        -- "Has spoken again" is slot.last_tick (any /info from them -- the 1 Hz
        -- /info extra cost refresh keeps it moving), NOT state_tick: /info state
        -- is event-driven and a stealer that never changed goal would never
        -- re-send it, which is exactly the case this has to catch. The price is
        -- that their goal/target can be a PRE-yield reading, so we give them
        -- STEAL_YIELD_RELEASE_GRACE (one replan interval + slack) to pick the
        -- pill up before their advert is allowed to release the block.
        if y and (now - y.tick) >= (C.STEAL_YIELD_RELEASE_GRACE or 60) then
          local slot = ally_state.get(y.to)
          local h = slot and slot.active and slot.info
          if h and (slot.last_tick or 0) > y.tick then
            local ag  = h.goal
            local at  = tonumber(h.target)
            local on_this = (ag == "attack_pill" or ag == "capture_pill")
              and ((at and e._id and at == e._id)
                   or (at == nil and tonumber(h.mx) == e._mx
                       and tonumber(h.my) == e._my))
            if not on_this then
              -- Name what they ARE on, not just that it isn't us: a target id
              -- when they advertised one (with the goal kind alongside, since
              -- object ids are per-kind and "#1" alone is ambiguous), the bare
              -- goal kind when they didn't (explore/take_cover carry no id).
              local reason
              if ag == nil or ag == "" then
                reason = "stealer_goal=none"
              elseif at then
                reason = string.format("stealer_target=%d kind=%s", at, tostring(ag))
              else
                reason = "stealer_goal=" .. tostring(ag)
              end
              local after = now - y.tick
              state._steal_yielded[e._id] = nil
              e._steal_released = { by = y.to, reason = reason,
                                    tick = now, after = after }
              y = nil
              e.formula = nil
            end
          end
        end
        -- Age the release note out so it can't sit on the row forever.
        if e._steal_released
           and (now - (e._steal_released.tick or 0)) > (C.STEAL_YIELD_BLOCK or 300) then
          e._steal_released = nil
          e.formula = nil
        end
        if y and (now - y.tick) <= (C.STEAL_YIELD_BLOCK or 300) then
          if e._reject ~= "ally_claimed" then
            e._reject = "ally_claimed"
            e.formula = nil
          end
          e._reject_remaining = (C.STEAL_YIELD_BLOCK or 300) - (now - y.tick)
          e._ally_by = y.to
          local _yr = string.format(
            "yield block: we yielded this pill to p%d at t=%d; holding %dt more of STEAL_YIELD_BLOCK(%d) until their claim broadcast lands, or until their advert shows them on something else",
            y.to, y.tick, e._reject_remaining, C.STEAL_YIELD_BLOCK or 300)
          if e._steal_reason ~= _yr then
            e._steal_reason = _yr
            e._steal_units  = nil   -- no price was compared: nobody is claiming it
            e._steal_cost   = nil
            e.formula = nil
          end
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
  -- "noblitz" (BRAIN_INIT_ARG) OR BLITZ_ENABLED=false (e.g. Easy difficulty):
  -- never join, so no call gets a discount. This is the ONE join path that does
  -- NOT run through M.availability -- the distance-scaled pool-6 discount is what
  -- pulls a bot toward a blitz, decoupled from negotiation -- so gating it here is
  -- required for BLITZ_ENABLED=false to mean "solo only" (the three availability /
  -- membership / suicider gates alone would leave the discount live and still
  -- steer a "solo" bot onto a commander's pill). Mark the pool-6 rows the discount
  -- WOULD have touched so the Term Breakdown / DECISION line says why the row is at
  -- its plain solo cost instead of silently differing from a normal bot.
  if state.blitz_disabled or not C.BLITZ_ENABLED then
    local why = state.blitz_disabled and "noblitz" or "blitz_off"
    for _, call in pairs(calls) do
      local pid = call.pill
      if pid then
        for _, e in pairs(cache) do
          if e._p == 6 and e._id == pid then e._blitz_off = why end
        end
      end
    end
    return
  end
  local now     = state.tick or 0
  local self_pn = (_SELF_PN ~= -1) and _SELF_PN or (info.player_number or -1)
  local cap     = squad.blitz_soldier_cap()   -- party MAX minus the commander
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
  refuel_need(state, info)   -- keep the topoff tag current (see goal_selection)
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
      desc = BRAIN_POOL_VIZ and string.format("refuel#%d@(%d,%d) score=%.0f×%.2f=%.0f arm=%d/%d sh=%d/%d%s",
             bid, base.mx, base.my, bscore, urgency, cost,
             info.armour, state.armour_target or C.TANK_FULL_ARMOUR,
             info.shells, state.shell_target or C.TANK_FULL_SHELLS,
             state._refuel_topoff_only and " topoff=on" or "") or "",
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
    local goal4 = { kind = "capture_pill", mx = pill.mx, my = pill.my,
                    wx = U.m2w(pill.mx), wy = U.m2w(pill.my), target_id = pid,
                    race_mode = race_mode4 }
    -- SEA-PILL HARVEST: a deep-sea target carries the whole entrance plan on
    -- the goal and enters the substate chain. The plan is SNAPSHOT (not
    -- aliased): once the mine is down, the committed substates must keep the
    -- S/F they actually mined, not whatever the next 50-tick rescan prefers.
    -- Already afloat = no chain; today's pickup drive owns it.
    if not info.inboat then
      local scl = M.sea_cluster_for(state, pid)
      if scl and not scl.reject then
        -- Hold the plan we are ALREADY executing rather than swapping in a
        -- freshly-scanned one for the same cluster mid-chain.
        -- The live plan wins over a fresh snapshot, and it is keyed on the
        -- CLUSTER, not on target_id: pill #0 -> #1 inside the same cluster is
        -- the same job, not a new one. (It also carries a FINISHED plan, so an
        -- already-loaded tank is never sent back to mine the shore again.)
        if not sea_live_attach(state, goal4, scl.id) then
          goal4.sea = M.sea_goal_plan(scl)
          goal4.substate = "entrance_plan"
        end
        if BRAIN_POOL_VIZ then
          desc4 = desc4 .. string.format(" SEA(c%d n=%d sub=%s S=(%d,%d))",
                    scl.id, scl.n, tostring(goal4.substate), scl.S.mx, scl.S.my)
        end
      end
    end
    pc[4] = {
      cost = raw_cost4,
      imminent = imminent4,
      goal = goal4,
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
        -- Name the contest term that actually fired, with its evidence, so the
        -- row's multiplier is attributable (a bare "CONTESTED" told you nothing
        -- about which of two very different signals tripped).
        local _c5 = ""
        if ce5 and ce5._rp_uf then
          _c5 = string.format(" x%.1f UNDER_FIRE(hit %st ago < quiet %d)",
                  C.REPAIR_UNDER_FIRE_MULT or 3.0,
                  ce5._rp_hit_age and string.format("%d", ce5._rp_hit_age) or "?",
                  C.REPAIR_QUIET_TICKS or 75)
        elseif ce5 and ce5._rp_soft then
          _c5 = string.format(" x%.1f ENEMY_IN_RANGE(#%s %.1ft %s)",
                  C.REPAIR_ENEMY_IN_RANGE_MULT or 1.5,
                  tostring(ce5._rp_soft_id), ce5._rp_soft_d or -1,
                  tostring(ce5._rp_soft_why))
        end
        desc5 = string.format("repair_pill#%d@(%d,%d) cost=%.0f (base+path-dam=%d×%d)%s",
                pid, pill.mx, pill.my, pcost,
                ce5 and ce5._dmg or (C.PILLS_MAX_HEALTH - pill.health),
                C.REPAIR_DAMAGE_BONUS, _c5)
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
  -- Re-run the steal pick at the decision tick: the armour reading that arms it
  -- moves every time anyone puts a shell into the base, and build_eval_queue's
  -- copy can be a whole cycle old by now.
  refresh_base_steal(state, world, info)
  local pr7 = partial[7]
  local _steal7 = state.base_steal
  if _steal7 then
    local sb = world.bases[_steal7.id]
    -- Only a base that is still hostile and still standing can be stolen; a
    -- base whose armour already fell to 0 belongs to capture_base, not here.
    if not (sb and sb.owner == "hostile" and (sb.health or 0) > 0) then
      _steal7 = nil
    end
  end
  if (pr7 and pr7.best_obj) or _steal7 then
    local base, bid, bcost
    if _steal7 then
      -- The steal wins pool 7 outright, even when another base is cheaper by
      -- path: it is one push from being ours.
      base, bid = world.bases[_steal7.id], _steal7.id
      bcost = (pr7 and pr7.best_id == bid and pr7.best_cost) or C.BASE_STEAL_COST
    else
      base, bid, bcost = pr7.best_obj, pr7.best_id, pr7.best_cost
    end
    local threat_at_base = threat.at(base.mx, base.my)
    -- ARMOUR-AWARE markup + FRIENDLY COVER, same shape and same numbers as the
    -- per-candidate cost in step_eval_queue (base_markup / base_friendly_cover
    -- are the single sources). Recomputed here for the DISPLAY only -- see the
    -- double-charge note below.
    local b_markup, b_n, b_nfull, b_frac, b_arm, b_age, b_decay =
      base_markup(base, state.tick or 0)
    local b_fcov_m, b_fcov_n = base_friendly_cover(world, base, tmx, tmy)
    b_markup = b_markup * b_fcov_m
    local b_threat = threat_at_base * C.ATTACK_BASE_THREAT_WEIGHT * b_fcov_m
    -- DOUBLE-CHARGE FIX (2026-09-02). pr7.best_cost IS the per-candidate cost
    -- step_eval_queue computed, and for pool 7 that is already
    --     path + stale + base_markup + threat*ATTACK_BASE_THREAT_WEIGHT
    -- (see the `pool_idx == 7` block there). Adding the markup and the threat a
    -- second time here charged both twice: at 20260902_000405 bot2 t=22561 base
    -- #9's own candidate cost was 130 (~50 path + 80 markup + 0 threat) and it
    -- competed at 210. Pass the candidate cost straight through.
    -- The steal snap is unchanged and still lands on the same number: a steal
    -- candidate is already clamped to BASE_STEAL_COST inside step_eval_queue,
    -- and a base that only became a steal at THIS tick gets clamped here.
    local adj_cost = bcost
    if _steal7 and adj_cost > C.BASE_STEAL_COST then
      adj_cost = C.BASE_STEAL_COST
    end
    -- Keep the pool-grid row for the winner in step with the cost that just won,
    -- so the panel and the decision never disagree about the steal.
    if _steal7 and state.cost_cache then
      local _ce = state.cost_cache["7:" .. tostring(bid)]
      if _ce then
        _ce.cost = adj_cost; _ce._steal = true; _ce._steal_rej = nil
        _ce._steal_dist = _steal7.dist; _ce.formula = nil
        _ce._base = b_markup; _ce._b_n = b_n; _ce._b_nfull = b_nfull
        _ce._b_frac = b_frac; _ce._b_arm = b_arm; _ce._b_age = b_age
        _ce._b_decay = b_decay
        _ce._fcov_m = (b_fcov_n > 0) and b_fcov_m or nil
        _ce._fcov_n = (b_fcov_n > 0) and b_fcov_n or nil
      end
    end
    -- The desc breaks `cand` into the four terms that MAKE it, and they have to
    -- come from the cost_cache entry that produced it -- not from a fresh
    -- base_markup / threat.at here. Those are re-read at the DECISION tick and
    -- the candidate was priced earlier in the eval cycle, so mixing the two
    -- printed rows like "cand=712 = path 700 + base 80 + threat 364" (1144).
    -- The values recomputed above stay, but only as the fallback for a winner
    -- with no cache entry (the steal can name a base the queue never scored),
    -- and that case is marked `~` so nobody hand-checks a sum that cannot close.
    local _ce7 = state.cost_cache and state.cost_cache["7:" .. tostring(bid)]
    local d_path  = (_ce7 and _ce7.raw) or 0
    local d_stale = (_ce7 and _ce7._stale) or 0
    local d_base  = (_ce7 and _ce7._base) or b_markup
    local d_thr   = (_ce7 and _ce7._thr) or b_threat
    local d_tv    = (_ce7 and _ce7._tv) or threat_at_base
    local d_n     = _ce7 and _ce7._b_n or b_n
    local d_nfull = (_ce7 and _ce7._b_nfull) or b_nfull
    local d_fcm   = _ce7 and _ce7._fcov_m or ((b_fcov_n > 0) and b_fcov_m or nil)
    local d_fcn   = _ce7 and _ce7._fcov_n or ((b_fcov_n > 0) and b_fcov_n or nil)
    local d_fchip = d_fcm
      and string.format(" x fcover{%.2f n=%d}", d_fcm, d_fcn or 0) or ""
    -- "~" = the terms printed below are NOT the ones inside `cand`: either the
    -- winner has no cost_cache row at all (the steal can name a base the eval
    -- queue never scored), or the row was re-evaluated after the partial-pool
    -- minimum that won was recorded. An UNMARKED row always adds up.
    local d_mark  = (_ce7 and math.abs((_ce7.cost or -1e30) - bcost) <= 1.0)
                    and "" or "~"
    pc[7] = {
      cost = adj_cost,
      _shells_on_arrival = pr7 and pr7.best_shells_on_arrival or nil,
      _steal = _steal7 and true or nil,
      goal = { kind = "attack_base", mx = base.mx, my = base.my,
               wx = U.m2w(base.mx), wy = U.m2w(base.my), target_id = bid },
      desc = BRAIN_POOL_VIZ and string.format(
             "attack_base#%d@(%d,%d) cost=%.0f (cand%s=%.0f = path{%.0f} +stale{%.0f}"
             .. " +base{%d x hp %s/%d%s = %.0f} +threat{%.2f x %d%s = %.0f})%s",
             bid, base.mx, base.my, adj_cost, d_mark, bcost,
             d_path, d_stale,
             C.ATTACK_BASE_EXTRA_COST,
             d_n and tostring(d_n) or "?", d_nfull, d_fchip, d_base,
             d_tv, C.ATTACK_BASE_THREAT_WEIGHT, d_fchip, d_thr,
             _steal7 and string.format(" STEAL(snapped to %.0f)", C.BASE_STEAL_COST)
                      or "") or "",
      cands = pr7 and pr7.candidates or nil,
    }
    -- P7_CANDS: pool 7 competes exactly ONE row (pc is keyed by pool index and
    -- several loops in init.lua walk it as `for pi = 0,10` / `1,13`), so the
    -- runner-up can never appear in FINAL_SCORES. It is also the row that
    -- explains the winner -- base #8 vs #9 in the incident above. Print the top
    -- ATTACK_BASE_LOG_CANDS candidates with their components instead.
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
  -- Debug-only stopwatches: _t2/_t3 are read only by the fp_time print2 that
  -- lua_strip removes.
  local _t2 = BRAIN_DEBUG_MODE and clock_us() or 0
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
  end
  local _t3 = BRAIN_DEBUG_MODE and clock_us() or 0

  -- wait_for_lgm: extra "park and wait for the LGM" candidate at a
  -- fixed low cost so it competes with normal pool winners. See
  -- eval_wait_for_lgm for suppression conditions (won't fire while a
  -- goal that needs the LGM is already running).
  state.pool_cache[12] = eval_wait_for_lgm(state, info)
  -- take_cover: pool 14. ALWAYS produces an entry (rejected ones carry
  -- _reject + TAKE_COVER_REJECT_COST) so its score is visible every replan.
  state.pool_cache[14] = eval_take_cover(state, world, info, tmx, tmy, boat, ammo)
  -- Pool 15: the kill_me_wait INITIATOR row (see eval_kill_me_wait).
  state.pool_cache[15] = KM.eval_wait(state, world, info, tmx, tmy)
  -- kill_lgm: pool 13 was just wiped by `state.pool_cache = {}` above.
  -- Re-inject so an LGM-sighting urgent_replan doesn't miss it and
  -- pick_goal can see kill_lgm as a candidate this tick.
  M.refresh_kill_lgm(state, info, world)

  -- Summary: which pools got finalized + a phase-by-phase time breakdown so
  -- a slow finalize_pools self-reports WHERE the time went (no need to enable
  -- BRAIN_PROFILE). FINALIZE_POOLS = {2,8,9,10}: 2=defend_pill,
  -- 8=place_pill_strategic, 9=attack_tank, 10=reposition. Pools 12
  -- (wait_for_lgm), 13 (kill_lgm) and 14 (take_cover) are injected just
  -- above, so they appear in the list too.
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
  -- ── CAPTURE-TARGET LGM PRIORITY (C.CAPTURE_LGM_PRIORITY, 2026-09-08) ────
  -- Remember which pill the capture flow is driving at, so the discount below
  -- survives the goal switch it causes.  Without the memory the whole thing
  -- flip-flops: the discount is only visible while the goal is capture_pill,
  -- so the tick kill_lgm wins the pool the discount vanishes, kill_lgm's cost
  -- jumps back to full and capture_pill wins straight back.
  --
  -- WHICH MEMORY.  state.capture_objective is the OPERATOR's `cp` command and
  -- nothing else -- it is nil in an ordinary game -- so it cannot be the only
  -- source.  The source that matters is the CURRENT GOAL while it is
  -- capture_pill (goal.mx/my is the target pill tile, the same pair
  -- CAPTURE_LGM_HUNT reads in steering.lua).  The rule:
  --   goal == capture_pill  -> (re)stamp the memory from the goal
  --   goal == kill_lgm      -> HOLD it (this is the switch we caused)
  --   anything else         -> drop it (the capture intent is gone)
  -- plus: a live `cp` objective always overrides, and the memory is dropped
  -- as soon as the tile stops being capturable (nothing there any more, or a
  -- live FRIENDLY pill = somebody already took it / rebuilt it).
  -- The man dying needs no clause: with no LGM in the box nothing is
  -- discounted, kill_lgm leaves the pool, the goal moves and the rule above
  -- drops the memory on the next tick.
  local cap_mx, cap_my = nil, nil
  if C.CAPTURE_LGM_PRIORITY then
    local _cg = state.goal
    if _cg and _cg.kind == "capture_pill" and _cg.mx and _cg.my then
      state.cap_prio = { mx = _cg.mx, my = _cg.my }
    elseif not (_cg and _cg.kind == "kill_lgm") then
      state.cap_prio = nil
    end
    if state.capture_objective and state.capture_objective.mx then
      state.cap_prio = { mx = state.capture_objective.mx,
                         my = state.capture_objective.my }
    end
    local _cp = state.cap_prio
    if _cp then
      -- Still a capture target?  pill_at() only returns LIVE pills and a
      -- corpse is health 0, so read the raw per-tile list instead.
      local _pl = world.pill_at and world.pill_at[_cp.my * 256 + _cp.mx]
      local _ok = false
      if _pl then
        for i = 1, #_pl do
          local _pp = _pl[i].pill
          if _pp and not _pp.in_tank
             and not (_pp.owner == "friendly" and (_pp.health or 0) > 0) then
            _ok = true
            break
          end
        end
      end
      if _ok then
        cap_mx, cap_my = _cp.mx, _cp.my
      else
        state.cap_prio = nil
      end
    end
  else
    state.cap_prio = nil
  end
  local _cp_hit = nil   -- cheapest discounted row this tick, for the log line
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


      -- ── Capture-target priority discount (C.CAPTURE_LGM_PRIORITY) ───────
      -- A hostile man standing within CAPTURE_LGM_PRIORITY_RADIUS tiles
      -- (CHEBYSHEV -- the same box metric CAPTURE_LGM_HUNT uses, so the two
      -- radii mean the same shape and can be compared directly) of the pill
      -- the capture flow is driving at is rebuilding that corpse out from
      -- under us.  Multiply the FINAL competed cost of HIS row so kill_lgm
      -- outbids capture_pill, shoot him, then let the pool run normally --
      -- capture_pill is priced exactly as before and simply wins back.
      -- Applied LAST, after the repair-futile penalty, so it scales the whole
      -- number the chips add up to.
      if cap_mx then
        local cdx = lgm.mx - cap_mx; if cdx < 0 then cdx = -cdx end
        local cdy = lgm.my - cap_my; if cdy < 0 then cdy = -cdy end
        local cdist = (cdx > cdy) and cdx or cdy
        if cdist <= (C.CAPTURE_LGM_PRIORITY_RADIUS or 8) then
          -- A CAP, not a multiplier (Andrew 2026-09-08): min(cost, MAX_COST),
          -- one under IMMINENT_CAPTURE_FLOOR, so the row wins the pool
          -- wherever in the box the man stands.
          local ccap = C.CAPTURE_LGM_PRIORITY_MAX_COST or 4
          local cbefore = cost
          if cost > ccap then cost = ccap end
          -- Chip goes in the DISPLAY half (before "||") so the pool-grid term
          -- table sees it and the total stays hand-computable; the pill tile,
          -- the man's distance to it and the pre-cap total go in the DETAIL
          -- half.  pre_cap is printed because REPAIR_FUTILE, when it fires,
          -- is added after the display half's "= N" was formatted.
          local _dsp, _det = formula_str:match("^(.-)||(.*)$")
          if _dsp then
            formula_str = string.format(
              "min(%s, cap_prio{%.0f}) = %.0f||%s; cap_prio_pill=(%d,%d); "..
              "cap_prio_d=%d (cheb, radius %d); cap_prio_pre_cap=%.0f",
              _dsp, ccap, cost, _det, cap_mx, cap_my, cdist,
              C.CAPTURE_LGM_PRIORITY_RADIUS or 8, cbefore)
          else
            formula_str = string.format(
              "min(%s, cap_prio{%.0f}) = %.0f", formula_str, ccap, cost)
          end
          if (not _cp_hit) or cost < _cp_hit.after then
            _cp_hit = { lgm = lgm, d = cdist, cap = ccap,
                        before = cbefore, after = cost }
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
  -- ── CAPTURE_LGM_PRIORITY decision line ─────────────────────────────────
  -- One line when the discount starts applying, one when it stops, and a
  -- heartbeat every CAPTURE_LGM_PRIORITY_LOG_TICKS brain ticks in between.
  -- refresh_kill_lgm runs twice per tick (rolling refresh + finalize), and
  -- the rate limit keys off state.tick, so the second call is silent.
  if C.CAPTURE_LGM_PRIORITY then
    local _now = state.tick or 0
    if _cp_hit then
      if (not state.cap_prio_on)
         or (_now - (state.cap_prio_log_t or -1e9))
            >= (C.CAPTURE_LGM_PRIORITY_LOG_TICKS or 50) then
        state.cap_prio_log_t = _now
      end
      state.cap_prio_on = true
    elseif state.cap_prio_on then
      state.cap_prio_on   = false
      state.cap_prio_log_t = _now
    end
  elseif state.cap_prio_on then
    state.cap_prio_on = false
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
  state.pool_cache[14] = eval_take_cover(state, world, info, tmx, tmy, boat, ammo)
  -- Pool 15: the kill_me_wait INITIATOR row (see eval_kill_me_wait).
  state.pool_cache[15] = KM.eval_wait(state, world, info, tmx, tmy)
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
        -- pill is actively shooting at us. _shot_by_tank (took tank
        -- damage with no predicted shell — point-blank etc.) is the
        -- unchanged half. The other half used to be perc.under_fire,
        -- which is the STATIC danger field at our tile
        -- (danger.danger_at(tile) > 0) and so stays true for as long
        -- as ANY hostile or neutral pill's stamp covers where we are
        -- parked. Anger is not required and neither is firing: a CALM
        -- pill stamps PILL_DANGER_BASE over a PILL_RANGE_MAP disk all
        -- the same. That is a suppression the tank can only lift
        -- by moving, and the goal that would have moved it was itself
        -- waiting on this man — the 2026-09-07 Everard deadlock.
        -- RESCUE_LGM_SUPPRESS_BY_FIRE_AGE swaps it for the sustained
        -- clock the builder pool already uses for the same question:
        -- danger.tank_fire_age is "armour actually dropped, or a hostile
        -- round's closest approach lands inside SWERVE_HIT_RADIUS_WU",
        -- and it goes quiet BUILDER_POOL_UNDER_FIRE_TICKS after the last
        -- such event. The stranded flag stays set either way, so the
        -- rescue fires as soon as the gate opens; until then the normal
        -- pools deal with the attacker.
        local fire_age, fire_why = nil, nil
        local fire_hot
        if C.RESCUE_LGM_SUPPRESS_BY_FIRE_AGE then
          fire_age, fire_why = danger.tank_fire_age(state, now)
          fire_hot = (fire_age ~= nil
                      and fire_age < (C.BUILDER_POOL_UNDER_FIRE_TICKS or 100))
        else
          fire_hot = (state.perc and state.perc.under_fire) or false
        end
        local fire_suppress = (fire_hot or state._shot_by_tank) or false
        if state._lgm_stranded_factors then
          state._lgm_stranded_factors.fire_suppress = fire_suppress
          state._lgm_stranded_factors.fire_age = fire_age
          state._lgm_stranded_factors.fire_why = fire_why
        end
        if fire_suppress then
          log.reason("goal", {
            pick = "rescue_lgm-suppressed",
            why = C.RESCUE_LGM_SUPPRESS_BY_FIRE_AGE
                  and string.format("under fire (fire_age=%s why=%s shot_by_tank=%s)",
                                    tostring(fire_age), tostring(fire_why),
                                    tostring(state._shot_by_tank or false))
                  or "under fire (hostile tank/pill shooting at us)",
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
  -- The haul-protection LEVEL and its triggers now live in haul_flee_eval
  -- (defined beside eval_take_cover), because take_cover fires on the SAME
  -- condition and two copies of the ladder would drift apart the first time
  -- either was tuned. Behaviour here is unchanged: level 1.0 when carrying
  -- with the builder DEAD or OUT (pills can't be placed soon, pure
  -- liability), a weaker pill-count-scaled level for a STACK with the builder
  -- aboard, triggers scaled by level, and critical armour always bailing.
  local _flee_mode = C.CRITICAL_FLEE_ENABLED
  local _do_flee = false
  local _haul = haul_flee_eval(state, info, tmx, tmy)
  -- "KILL ME" in progress: the claimant's shells are taking the armour off on
  -- purpose, so critical armour must not inject a flee that drives us away
  -- from our own executioner. (An enemy inside the cancel radius clears
  -- state.km.executing on the same tick, so real danger still flees.)
  local _km_exec = (state.km and state.km.executing) or false
  if _flee_mode == "no_builder_and_carrying_only" then
    if _haul.level > 0 then
      _do_flee = (_haul.triggered or critical) and not _km_exec
    end
  elseif _flee_mode then
    _do_flee = critical and not _km_exec
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
    local _ref_bonus, _ref_mult, _ref_fill, _ref_scarcity, _ref_mine_cost,
          _ref_urgency, _ref_arm_def, _ref_sh_def, _ref_mine_d = refuel_shape(info, state, now)
    -- Refresh the top-off tag. build_eval_queue (its other writer) only rebuilds
    -- when the cost cache goes stale, up to 100 ticks apart, so the tag on the
    -- REFUEL_SHAPE / row-desc lines would otherwise describe a tank state that
    -- is no longer true. Unconditional, not debug-gated: nothing decides on it,
    -- but prod and the recorded brain must still set it the same way.
    refuel_need(state, info)
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
          -- The waive decision only means anything at the base we're parked
          -- on, which is the only row that can be charged the surcharge.
          e._mine_d         = (e._mx == tmx and e._my == tmy) and _ref_mine_d or nil
          e._arm            = info.armour
          e._sh             = info.shells
          e._arm_target     = state.armour_target or C.TANK_FULL_ARMOUR
          e._sh_target      = state.shell_target  or C.TANK_FULL_SHELLS
          e._arm_def        = _ref_arm_def
          e._sh_def         = _ref_sh_def
          e._lgm_wait_floor = nil
          e.formula         = nil  -- invalidate cached formula so live shape re-renders
        end
      end
    end

    -- Why a pool row is NOT in the competition, keyed by pool index. The raw
    -- pool_cache dump prints a cost for every populated slot; several of them
    -- are dropped here and the dump gave no hint, so a row could sit on the
    -- panel at cost 48 having never competed. Rebuilt fresh every replan.
    state._pool_excluded = {}
    for idx, entry in pairs(pc) do
      if entry and entry._reject then
        state._pool_excluded[idx] = "entry._reject: " .. tostring(entry._reject)
      end
      if entry and not entry._reject then
        -- Skip entries whose destination is blocked (e.g. by goal lookahead)
        local gmx = entry.goal and entry.goal.mx
        local gmy = entry.goal and entry.goal.my
        if gmx and gmy and state.blocked then
          local bk = U.mkey(gmx, gmy)
          if state.blocked[bk] and now < state.blocked[bk] then
            state._pool_excluded[idx] = string.format(
              "dest (%d,%d) blocked for %dt", gmx, gmy, state.blocked[bk] - now)
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
          state._pool_excluded[idx] = "goal shape blacklisted by the tick-budget kill"
          goto continue_pool
        end
        -- Skip goals on abandon cooldown (prevent oscillation loops)
        if gmx and gmy and entry.goal and state.goal_cooldowns then
          local cd_key = entry.goal.kind .. ":" .. gmx .. "," .. gmy
          local cd_exp = state.goal_cooldowns[cd_key]
          if cd_exp and now < cd_exp then
            state._pool_excluded[idx] = string.format(
              "abandon cooldown on %s for %dt", cd_key, cd_exp - now)
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
            -- Say so. This branch silently removed a pool row that the raw
            -- pool[] dump still printed a cost for, so the panel showed
            -- "pool[1] refuel cost=48" next to a competition that never saw it.
            state._pool_excluded = state._pool_excluded or {}
            state._pool_excluded[idx] = string.format(
              "refuel at target (arm %d/%d, sh %d/%d)%s",
              info.armour or -1, state.armour_target or -1,
              info.shells or -1, state.shell_target or -1,
              lgm_returning and ", but LGM not returning" or ", no LGM wait")
            if (state._refuel_skip_log_tick or -1) ~= now then
              state._refuel_skip_log_tick = now
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
          -- Every factor behind the multiplier, on one line: the ramp is the
          -- whole mechanism that decides whether a top-off beats real work, and
          -- `x2.75` on its own is not something you can check by hand.
          -- The mine chip is printed whenever there is a hoard to price at THIS
          -- base, charged or waived, so a reader can always see the surcharge
          -- that was (or was not) added and why. Built outside the debug gate is
          -- pointless work, so it is built here — nothing decides on it.
          local _mine_seg = ""
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
        -- Follow-through hold: pinned to the flat constant AFTER the phase
        -- weight, same trick. place_strategic is x3.0 in opening and x0.7 mid,
        -- which would turn 25 into 75 (losing to attack_base at 60) or 17.5
        -- (beating a live attack_tank engage) -- and neither is what the number
        -- means. A hold has no travel for a LOCAL phase preference to scale.
        if entry.goal and entry.goal.follow_through then
          cost = C.PLACE_FOLLOW_THROUGH_COST or 25
        end
        pool[#pool + 1] = {
          cost = cost, _base_cost = cost,  -- _base_cost preserved for breakdown display
          goal = entry.goal, desc = entry.desc,
          cands = entry.cands, _pill = entry._pill, _pill_id = entry._pill_id,
          phase_weight = pw,
          _pool_idx = idx,   -- so the pool_cache dump can print the COMPETED total
          -- The pool cost BEFORE any of the shaping below (phase weight,
          -- influence, hysteresis, commitment, history, wsim). The steal
          -- handshake needs both ends of that chain: the competed total is
          -- what it trades, and competed/raw is the only handle it has on how
          -- much the shaping moved this pool THIS replan (see the ledger at
          -- the end of goal_selection).
          _raw_cost = entry.cost,
          _engage_break_lock = entry._engage_break_lock,
        }
        ::continue_pool::
      end
    end

    -- ── Pin REJECT sentinels ──
    -- A rejected row's cost is a DISPLAY sentinel (1e30), not a score. Running
    -- it through the influence x2, the suicider multiplier, the phase weight
    -- and the hysteresis ratio produced totals like 5e43 in FINAL_SCORES and
    -- the WINNERS panel — unreadable, and different for every rejected row for
    -- no reason. Tag them here and pin them back to the one sentinel after the
    -- shaping passes, the way take_cover's TAKE_COVER_REJECT_COST row stays put.
    for _, c in ipairs(pool) do
      if (c.cost or 0) >= 1e29 then c._reject_sentinel = true end
    end

    -- ── Influence-based cost scaling ──
    -- Goals in hostile territory (influence < -50) cost 2×; goals in
    -- friendly territory (influence > 50) cost 0.5×. Skipped during
    -- opening phase (territory not established yet).
    if state.phase ~= "opening" then
      local INF_EXEMPT = {
        refuel_at_base=true,
        -- defend_pill: the pill sitting in enemy influence is exactly WHY it
        -- needs defending. Doubling the bid for a pill in their half makes the
        -- bot cheapest on the pills nobody is attacking, which is backwards --
        -- 20260902_000405 bot2 t=22561, pill #14 one tile from a hostile base
        -- being shelled at 5 hits/s, defend priced 76 -> 153 by the x2 and
        -- beaten by a calm take_cover tile at home.
        defend_pill=true,
        -- take_cover: the goal tile is always a few tiles from the tank, so the
        -- influence read is a property of where the TANK is standing, not of
        -- the errand. Halving a cover tile near home moved the bid out of the
        -- band TAKE_COVER_BASE_COST is tuned against ("loses to a real attack
        -- (20-30)") -- 48 became 24 in the same incident.
        take_cover=true,
      }
      -- repair_pill has its own danger model (dead-pill: tank-snipe, advantage-
      -- scaled; pill-fire ignored) and should NOT be ×2'd for sitting in enemy
      -- influence — but only under the repair fix. Off → 1.90-beta1: subject to
      -- the influence ×0.5/×2 like any other goal.
      if C.REPAIR_FIX_ENABLED then INF_EXEMPT.repair_pill = true end
      -- place_pill_strategic is NOT kind-exempt (a normal placement drives to
      -- the spot and should pay for it being in their half), so the
      -- follow-through row is exempted by its FLAG instead: it is a hold, the
      -- influence under the trip tile was already priced when the pool picked
      -- that tile, and doubling a fixed 25 would silently move it out of the
      -- band PLACE_FOLLOW_THROUGH_COST is documented against.
      for _, c in ipairs(pool) do
        -- km_ally_pn: the "kill me" DELIVERY row (a pool-9 attack_tank whose
        -- target is an ALLY who asked to be killed for its cargo). It is not
        -- an attack on enemy ground -- the tile is our own team-mate's -- so
        -- halving/doubling it by whose half of the map he happens to be
        -- standing in prices the errand on something it has nothing to do
        -- with. Exempt by FLAG, not by kind: ordinary attack_tank rows are
        -- still scaled.
        if c.goal and c.goal.mx and c.goal.my
           and not INF_EXEMPT[c.goal.kind] and not c.goal.follow_through
           and not c.goal.km_ally_pn
           and not c._reject_sentinel then
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
        if sm ~= 1.0 and c.cost and c.cost > 0 and not c._reject_sentinel then
          c.cost = c.cost * sm
          c._suicider_mult = sm
        end
      end
    end

    -- ── Refuel-group cost multiplier ──
    -- THE choke point for "this bot values resupply more/less than usual":
    -- one pass over the assembled pool, the same layer as the phase weight and
    -- the suicider surcharge above. Covers BOTH members of the refuel
    -- GOAL_GROUP, so the critical-armour flee injection (a flee_to_base that
    -- overwrites pool 1) is scaled too — the whole point is that a bot with
    -- refuel=1.2 goes back for supplies less readily, and exempting the
    -- emergency case would leave the biggest refuel decision unchanged.
    -- No-op at the 1.0 default. _refuel_mult is stashed for the WINNERS-row
    -- reconciliation exactly like _suicider_mult.
    if REFUEL_MULT ~= 1.0 then
      for _, c in ipairs(pool) do
        if c.goal and GOAL_GROUPS[c.goal.kind] == "refuel"
           and c.cost and c.cost > 0 and not c._reject_sentinel then
          c.cost = c.cost * REFUEL_MULT
          c._refuel_mult = REFUEL_MULT
        end
      end
    end

    -- ── Behind-team attack surcharge (lead gate) ──
    -- "Press when ahead": when our team is NOT ahead (state.team_ahead, set in
    -- strategy.lua from AHEAD_PILL_FRAC / AHEAD_BASE_FRAC), multiply the offensive
    -- rows -- attack_pill / attack_base / attack_tank -- by BEHIND_ATTACK_MULT so
    -- the bot picks fewer fights while behind and leans on defend / refuel /
    -- placement instead. Same selection layer as the suicider / refuel passes
    -- above. No-op at the 1.0 default, and team_ahead is always true when the
    -- fracs are 0, so Hard is unchanged. _ahead_mult is stashed for the WINNERS-row
    -- reconciliation exactly like _suicider_mult / _refuel_mult.
    if C.BEHIND_ATTACK_MULT ~= 1.0 and not state.team_ahead then
      for _, c in ipairs(pool) do
        if c.goal and (c.goal.kind == "attack_pill" or c.goal.kind == "attack_base"
                       or c.goal.kind == "attack_tank")
           and c.cost and c.cost > 0 and not c._reject_sentinel then
          c.cost = c.cost * C.BEHIND_ATTACK_MULT
          c._ahead_mult = C.BEHIND_ATTACK_MULT
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
    -- ── ESCAPE_NO_BUILDER_SKIP_HYST ──────────────────────────────────────
    -- LOADED, BUILDER-LESS: getting out pays NOTHING to enter. flee_to_base
    -- and a take_cover row that actually fired on its haul / panic trigger
    -- skip the type-switch flat, the commitment stack AND the same-group
    -- target penalty -- a full zero, not the ordinary HYST_EXEMPT tier
    -- (which still charges GOAL_TARGET_SWITCH_PENALTY within a group).
    --
    -- ASYMMETRIC, exactly like the rest of the pool: the waiver is on the
    -- row ENTERING. Once one of them is the incumbent, nothing about it is
    -- special -- every other row pays its normal switch + commitment to
    -- displace it, because those penalties are charged to the CHALLENGER
    -- and the challenger is not on this list. That is what stops the tank
    -- bouncing straight back out of cover.
    --
    -- Auditable: the rows carry hysteresis = "none:no_lgm" with switch_flat
    -- and commit_val pinned to 0, so FINAL_SCORES prints
    -- `hyst=none:no_lgm sw=0.0 cmt=0.0` and the breakdown says why.
    -- "none:no_lgm" is deliberately NOT "type"/"target", so the
    -- multiplicative stickiness bar below does not fire on it either.
    local ESCAPE_NO_HYST = nil
    if C.ESCAPE_NO_BUILDER_SKIP_HYST and state.loaded_no_lgm then
      ESCAPE_NO_HYST = { flee_to_base = true }
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
    -- SEA-PILL HARVEST: from lay_mine until the boat exists there is a LIVE
    -- MINE sitting on our own shore and 21 trees already committed. Walking
    -- away leaves the mine there for our own tank to drive over, so the same
    -- kind of surcharge WS_SUBS gets applies until build_boat completes.
    if cur_sub and state.goal.sea then
      if SEA_COMMITTED[cur_sub] then
        commitment = commitment + (C.SEA_PILL_COMMITMENT or 400)
      elseif SEA_ENGAGED[cur_sub] then
        commitment = commitment + (C.SEA_PILL_LEG_COMMITMENT or 150)
      end
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
      -- ESCAPE_NO_BUILDER_SKIP_HYST: a FULL zero for the escape rows while
      -- the tank is loaded and builder-less. Checked BEFORE the ordinary
      -- exempt branch because that branch still charges the same-group
      -- target penalty, and "no penalty to enter" means none.
      if ESCAPE_NO_HYST then
        local _esc = ESCAPE_NO_HYST[c.goal.kind]
        if not _esc and c.goal.kind == "take_cover" then
          -- Only a take_cover that actually FIRED on its haul / panic
          -- trigger is an escape. The calm / bad_ground / released rows are
          -- ordinary pool business and keep their hysteresis.
          -- (_tc_trigger picks up a "_holding" suffix while standing on the
          -- pick, so match on the prefix.)
          local _t = c.goal._tc_trigger or ""
          _esc = (_t:sub(1, 4) == "haul") or (_t:sub(1, 14) == "panic_no_build")
        end
        if _esc then
          c.hysteresis  = "none:no_lgm"
          c.switch_flat = 0
          c.commit_val  = 0
          c._escape_no_hyst = true
          goto continue_hyst
        end
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

    -- ── ATTACK_NO_BUILDER_MULT ───────────────────────────────────────────
    -- LOADED, BUILDER-LESS: picking a fight while holding a stack we cannot
    -- place is the trade that lost five pillboxes in one drive (see the block
    -- in constants.lua). Every OFFENSIVE row -- attack_tank, attack_pill,
    -- attack_base, and the defend_pill alarm, which is also "drive at a fight"
    -- -- has its cost multiplied here.
    --
    -- WHERE: the FINAL competed number. After the influence x0.5/x2 scale,
    -- after the suicider/refuel shaping, after hysteresis and after the
    -- oscillation history penalty -- so the multiplier is the last word and
    -- nothing downstream can add a flat that dilutes it.
    --
    -- A MULTIPLIER, NOT A REJECT, on purpose: with an empty pool (nothing to
    -- capture, nowhere to hide, no base to reach) the bot must still be able
    -- to shoot back at the tank in front of it. x10 loses to any real
    -- alternative and wins when there is none.
    --
    -- The kill_me DELIVERY row is exempt by flag: its target is an ALLY who
    -- asked for it, and the whole point of the errand is to get the stack off
    -- a builder-less tank. Multiplying it by 10 would price the cure like the
    -- disease.
    if C.ATTACK_NO_BUILDER_MULT and C.ATTACK_NO_BUILDER_MULT ~= 1
       and state.loaded_no_lgm then
      local nbm = C.ATTACK_NO_BUILDER_MULT
      for _, c in ipairs(pool) do
        if c.goal and KM.ATTACK_KINDS[c.goal.kind]
           and not c.goal.km_ally_pn
           and c.cost and c.cost > 0 and not c._reject_sentinel then
          c.cost = c.cost * nbm
          c._nobuild_mult = nbm
        end
      end
      if not quiet and BRAIN_DEBUG_MODE then
      end
    end

    if not quiet and BRAIN_DEBUG_MODE then
    end
    -- A NORMAL place_pill_strategic must never out-rank an attack_tank WHILE A
    -- HOSTILE TANK IS IN SHOOTING RANGE: fighting a tank beats casually
    -- dropping a pill, but only when the fight is actually imminent. Outside
    -- C.PLACE_PIN_ENEMY_RANGE (7 tiles = gun range) the placement competes on
    -- its own cost and the carry pressure decides — 20260903_105448 bot2
    -- t=34774 pinned a cost-1.0 placement (four pills aboard) to 2163 against
    -- an attack_tank row for an enemy 15 tiles away on the far side of water,
    -- and never placed anything again. The EMERGENCY defensive build
    -- (goal._place_forced, set on the offensive_build path) is exempt at any
    -- range — that's the "build now while fighting before I die" behavior and
    -- stays as-is. Done on the post-penalty pool costs so it's weight/penalty
    -- aware.
    --
    -- The verdict is stamped on the place entry's desc as a ` pin{...}` /
    -- ` nopin{...}` chip so the FINAL_SCORES row still reproduces its own
    -- number: without it the pin silently overwrote the cost the chips print.
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
        local nh = state.perc and state.perc.nearest_hostile_tank
        local ed = nh and nh.dist or nil
        local rng = C.PLACE_PIN_ENEMY_RANGE or 7
        local chip
        if ed and ed <= rng then
          chip = string.format(" pin{atk %.1f+1, enemy %dt<=%d}", at_cost, ed, rng)
          place_entry.cost = at_cost + 1
          place_entry.pin_atk_cost = at_cost
          place_entry.pin_enemy_d  = ed
        elseif ed then
          chip = string.format(" nopin{enemy %dt>%d, atk %.1f}", ed, rng, at_cost)
          place_entry.pin_enemy_d = ed
        else
          chip = string.format(" nopin{no visible enemy tank, atk %.1f}", at_cost)
        end
        place_entry.pin_range = rng
        if place_entry.desc and place_entry.desc ~= "" then
          place_entry.desc = place_entry.desc .. chip
        end
      end
    end
    -- ── Sort by cost, pick winner ──
    -- Total order: bare cost comparison leaves exact-cost ties (floors and
    -- flat penalties make them common) in pre-sort array order, which turns
    -- any upstream ordering wobble into a different goal. Tie-break on kind
    -- then tile then id so the winner is a pure function of the entries
    -- (determinism hardening, 20260831). Shared by the two re-sorts below.
    local function pool_cost_lt(a, b)
      if a.cost ~= b.cost then return a.cost < b.cost end
      local ak, bk = a.kind or "", b.kind or ""
      if ak ~= bk then return ak < bk end
      local at = (a.my or 0) * 256 + (a.mx or 0)
      local bt = (b.my or 0) * 256 + (b.mx or 0)
      if at ~= bt then return at < bt end
      return (a.id or -1) < (b.id or -1)
    end
    state._pool_cost_lt = pool_cost_lt
    table.sort(pool, pool_cost_lt)
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
        end
        if not cur_entry and not _declined_complete and state.pool_cache then
          for pi = 0, 15 do
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
              break
            end
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
        -- Don't sim our current attack target if we're mid-attack
        local skip = cur_attack_active
          and c.goal.kind == state.goal.kind
          and c.goal.mx == state.goal.mx and c.goal.my == state.goal.my

        -- The follow-through hold is never simulated: there is no journey to
        -- price (the hold tile is inside PLACE_FOLLOW_THROUGH_HOLD_DIST, often
        -- the tile we are already on) and no LGM dwell to add (the man is
        -- already out walking). A wsim add would also un-pin the flat cost the
        -- constant is documented against.
        if WSIM_SIM_KINDS[c.goal.kind] and not skip and not c.goal.follow_through then
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
          end
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
      -- Re-sort after sim adjustments (same total order as the first sort)
      for _, c in ipairs(pool) do
        if c._reject_sentinel then
          c.cost = C.POOL_REJECT_COST or 1e30
          c._base_cost = c.cost
        end
      end
      table.sort(pool, state._pool_cost_lt)
    elseif _persist_applied then
      -- wsim didn't run this tick, but a persisted KILL changed costs — re-sort
      -- so the lethal goal can't win just because the live pass was skipped.
      table.sort(pool, state._pool_cost_lt)
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

    for _, c in ipairs(pool) do
      if c._reject_sentinel then
        c.cost = C.POOL_REJECT_COST or 1e30
        -- _base_cost is snapshotted mid-shaping, so pin it too or the row
        -- prints a readable total next to an unreadable base.
        c._base_cost = c.cost
      end
    end

    -- Final competed total per pool index, so the pool_cache dump in init.lua
    -- can print the number that actually competed next to the raw cached cost
    -- (they differ by the phase weight, the influence multiplier, the refuel
    -- shape and the hysteresis).
    state._pool_competed = {}
    -- Parallel raw (pre-shaping) cost per pool index. Only reader today is the
    -- steal handshake's shaping estimate; see steal_drain_requests.
    state._pool_competed_raw = {}
    -- VARIANT (c): no per-row competed ledger. The two per-POOL tables above
    -- and _competed_winner below are all change 2's outgoing-request gate
    -- needs (shape = pool-6 competed / pool-6 raw, measured against the goal
    -- that won the replan); no handshake price is ever read from them.
    local _comp_now = state.tick or 0
    for _, c in ipairs(pool) do
      if c._pool_idx then
        state._pool_competed[c._pool_idx] = c.cost
        state._pool_competed_raw[c._pool_idx] = c._raw_cost
      end
    end
    -- The goal that actually won this replan's competition (pool is sorted,
    -- COST_INF sentinels already dropped). The outgoing-steal gate asks
    -- "would we pick this pill if the claim were lifted?", and that question
    -- is answered against this number.
    if pool[1] then
      state._competed_winner = {
        cost     = pool[1].cost,
        tick     = _comp_now,
        pool_idx = pool[1]._pool_idx,
        kind     = pool[1].goal and pool[1].goal.kind or "?",
        id       = pool[1]._pill_id or (pool[1].goal and pool[1].goal.target_id),
      }
    else
      state._competed_winner = nil
    end
    -- One outgoing stq per replan, for the row we would actually pick.
    M.steal_drain_requests(state)

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
      for i, c in ipairs(pool) do
        local base = c._base_cost or c.cost
        local penalty = (c.cost or 0) - base
        -- inf{} and nobuild{} are MULTIPLIERS on the whole row, so they are
        -- printed as their own fields: without them `total` cannot be
        -- reconstructed from base + pen at all.
        --   inf     — the territory-influence x0.5/x2 scale
        --   nobuild — ATTACK_NO_BUILDER_MULT, the LOADED, BUILDER-LESS
        --             surcharge on the four "drive at a fight" kinds
      end
      end -- REPLAN_LOG_MIN_TIER

      local winner = pool[1]

      -- Pool log + winner_cands + log.reason are debug/log-only — wrapped
      -- so lua_strip removes them from opt/.

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
  -- SEA-PILL HARVEST, collect phase. Afloat, on purpose, with cluster pills
  -- still in the water and the water still safe: this OWNS the goal. Anything
  -- else winning the pool here means going ashore, and going ashore costs
  -- another mine, another 21 trees and another boat for the pills we came for.
  -- Released the moment the cluster is collected or the water turns unsafe
  -- (M.sea_update clears state._sea_afloat), after which normal boat handling
  -- resumes. Sits above the command goal deliberately: a command cannot make a
  -- boat trip safe to abandon halfway.
  if state._sea_afloat and state._sea_live and info.inboat then
    local L = state._sea_live
    local tmx = bit.rshift(info.tankx, 8)
    local tmy = bit.rshift(info.tanky, 8)
    local pid, bd, route, drops =
      sea_pick_collect_target(state, world, L.sea, tmx, tmy)
    -- A member with no covered-free route is off the trip, not the end of it.
    if drops then
      for _, dp in ipairs(drops) do
        for i = #(L.sea.ids or {}), 1, -1 do
          if L.sea.ids[i] == dp then
            table.remove(L.sea.ids, i)
            L.sea.dropped = L.sea.dropped or {}
            L.sea.dropped[dp] = L.sea.dropped[dp] or true
            -- A member dropped for routing is LEFT BEHIND, not fetched. Count
            -- it separately so the release reason cannot claim otherwise.
            L.sea.left_behind = (L.sea.left_behind or 0) + 1
          end
        end
      end
    end
    if pid then
      local p = world.pills[pid]
      if p == nil and L.sea.pos and L.sea.pos[pid] then
        p = { mx = L.sea.pos[pid][1], my = L.sea.pos[pid][2] }
      end
      local best = { kind = "capture_pill", mx = p.mx, my = p.my,
                     wx = U.m2w(p.mx), wy = U.m2w(p.my), target_id = pid,
                     race_mode = C.CAPTURE_RACE_MODE_CAPTURE }
      sea_live_attach(state, best, L.cluster)
      best.substate = "collect"
      -- The route is what the drive must follow; hand it to steering so the
      -- boat goes AROUND covered water instead of straight at the pill.
      best.sea_route = route
      if not quiet and BRAIN_DEBUG_MODE then
      end
      return best
    end
    state._sea_afloat = nil
  end

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
  -- Warm-up exemption: a CHEAP dead-pill pickup wins even while the pools are
  -- still warming. The warm gate below exists so a half-evaluated pool can't
  -- lock hysteresis onto a poor goal — but a capture_pill a few tiles away is
  -- never that goal: it's short, has no commitment lock-in worth fearing, and
  -- is the best first move a fresh tank can make. Without this, 20260831_173448
  -- bot3 respawned IN A BOAT at sea with two dead team pills 13 tiles away by
  -- water (capture_pill cost 34 already in the pool), rode the explore
  -- fallback to land instead, and once ashore the water pills were
  -- unreachable for the rest of the game. The nearest-candidate eval has
  -- already priced it via the current slate (boat or land), so the cost is a
  -- real travel cost, not a guess.
  if not M.warm_ready(state) then
    local pce = state.pool_cache and state.pool_cache[4]
    if pce and pce.goal and pce.goal.kind == "capture_pill"
       and (pce.cost or math.huge) <= (C.WARMUP_CAPTURE_MAX_COST or 150) then
      return pce.goal
    end
  end

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
    local rfm = refuel_mult_for_pool(pname)
    local weighted = cost_val >= 0 and (cost_val * pw * sui * rfm) or -1
    if rfm ~= 1.0 and formula ~= "" then
      formula = formula .. " * " .. M.refuel_mult_label()
    end
    if sui ~= 1.0 and formula ~= "" then
      -- Name the RULE that made this bot a suicider (forced token / blitz
      -- designation at GO / harasser slate) — the surcharge is meaningless
      -- without knowing which one is in force and whether it is temporary.
      formula = formula .. string.format(" * suicider{%.1f, %s}", sui, tostring(state.suicider_src or "?"))
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
    local rfm = refuel_mult_for_pool(pname)
    local weighted = cost >= 0 and (cost * pw * sui * rfm) or -1
    local fdesc = entry.desc or ""
    if rfm ~= 1.0 then fdesc = fdesc .. " * " .. M.refuel_mult_label() end
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
  -- THIS FUNCTION IS A REPORT. IT MUST NOT ADVANCE BRAIN STATE.
  --
  -- The host calls it once per bot per tick while recording (winbolods
  -- -brain-debug, BrainTest's pool panel) and NEVER in a production game.
  -- Anything it mutates is therefore state the recorded brain has and the
  -- production brain does not -- a different brain, on the recorder's
  -- schedule rather than the replan's.
  --
  -- It used to open with a refresh of the panel's reject flags:
  --
  --     sync_ally_claimed_rejects(state, nil, true)
  --     apply_blitz_target(state, state._last_info)
  --     apply_blitz_join_discount(state, state._last_info, state.world)
  --     apply_blitz_capture_defer(state, state._last_info)
  --
  -- "so the displayed _reject flags track live ally_state without waiting for
  -- the next replan cycle".  All four write cost_cache / pool state that
  -- goal_selection then reads, and M.finalize_pools already runs the same four
  -- on the real per-tick path -- so these were a SECOND application on a
  -- different clock.  On seed 1 that repriced attack_pill at brain tick 15561
  -- and the recorded bot 1 stayed on attack_pill where production switched to
  -- defend_pill (found 2026-09-06; -bd-nopool, which skips this call, made the
  -- recorded game byte-identical to production again).
  --
  -- Dropping them also makes the panel HONEST: it now shows the reject flags
  -- the brain actually decided on at the last replan, not a fresher set the
  -- brain never used.
  if state.player_number then _SELF_PN = state.player_number end
  if not BRAIN_POOL_VIZ then
    return string.format(
      '{"phase":"%s","tick":%d,"replan_left":0,"bot":%d,"sections":[{"id":"off","label":"Pool viz","rows":[{"id":0,"mx":0,"my":0,"cost":0,"formula":"BRAIN_POOL_VIZ is off","stale":-1,"active":false,"imminent":false,"reject":null}]}]}',
      state.phase or "?", state.tick or 0, state.player_number or 0)
  end
  local now = state.tick or 0
  local cache = state.cost_cache or {}
  local pc = state.pool_cache or {}
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
      local bonus, mult, fill, scarcity, mine_cost, urgency, arm_def, sh_def, mine_d =
        refuel_shape(li, state, state.tick or 0)
      -- The surcharge is only ever charged at the base under the tank, so only
      -- that row gets the mines chip — same rule the cost path applies.
      local _ltmx = li.tankx and bit.rshift(li.tankx, 8) or nil
      local _ltmy = li.tanky and bit.rshift(li.tanky, 8) or nil
      for _, e in pairs(cache) do
        if e._p == 1 then
          local _here = (_ltmx ~= nil and e._mx == _ltmx and e._my == _ltmy)
          e._urgency     = urgency
          e._base_floor  = C.REFUEL_BASE_COST
          e._defic_bonus = bonus
          e._fill_mult   = mult
          e._fill        = fill
          e._scarcity    = scarcity
          e._mine_cost   = mine_cost
          e._mine_d      = _here and mine_d or nil
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
        -- Pool 6 INF rows carry their INF reason (_skipped) in the same chip
        -- when no real reject is set — same as attack_tank's `skipped` rows.
        reject = cached and ((cached._reject == "ally_claimed" and cached._reject_joinable_blitz)
                             and "blitz" or cached._reject or cached._skipped) or nil,
        reject_remaining = cached and cached._reject_remaining or 0,
        ally_score = cached and cached._ally_score or nil,
        ally_by    = cached and cached._ally_by    or nil,
        stealing   = (cached and cached._stealing) or false,
        imminent = (cached and cached.imminent) or false,
      }
    end
  end

  local function attack_tank_formula(b)
    -- "KILL ME" delivery row: the subject is an ALLY, and its price is
    -- base + travel and nothing else. Its own formula, so the row can never
    -- be read as an ordinary attack_tank standoff calculation.
    if b.kill_me then
      local head = string.format("subject=ally p%d @(%d,%d)", b.km_ally_pn or -1,
                                 b.mx or 0, b.my or 0)
      if b.skipped then
        return string.format(
          "REJECT %s %s||reject:%s -- %s|kill_me:p%d is loaded and builder-less and has asked to be killed for its cargo."
          .. " Allied shells DO damage allied tanks (tank.c tankIsTankHit ignores only the shooter's own shells) and the"
          .. " corpses keep the dead player's ownership, which capture_pill already scoops."
          .. " The row is priced INF but still shown so the reason is visible|need:ceil(%d armour / %d[TANK_SHELL_DAMAGE])"
          .. " + %d[KILL_ME_SHELL_MARGIN], floored at %d[TANK_COMBAT_MIN_SHELLS] = %d shells; we hold %d",
          b.skipped, head, b.skipped, tostring(b.kill_me_why),
          b.km_ally_pn or -1, b.km_ally_armour or 0, C.TANK_SHELL_DAMAGE or 5,
          C.KILL_ME_SHELL_MARGIN or 2, C.TANK_COMBAT_MIN_SHELLS or 10,
          b.km_need_shells or 0, b.tank_shells or 0)
      end
      return string.format(
        "KILL_ME %s (base{%.0f} + travel{%.0f}) = %.0f"
        .. "||kill_me:p%d is loaded and builder-less and has asked to be killed for its cargo. Drive into gun range and"
        .. " fire until it dies; the corpses it drops keep p%d's ownership and capture_pill takes them"
        .. "|base:KILL_ME_RESPONDER_BASE{%.0f}|travel:Dijkstra/A* cost from (%d,%d) to the ally's advertised tile (%d,%d)"
        .. "|terms:NOTHING else is charged -- no aim bonus, no crossfire, no low-shells ramp, no influence x0.5/x2 and no"
        .. " ATTACK_NO_BUILDER_MULT. A delivery is a delivery"
        .. "|need:ceil(%d armour / %d[TANK_SHELL_DAMAGE]) + %d[KILL_ME_SHELL_MARGIN], floored at %d[TANK_COMBAT_MIN_SHELLS]"
        .. " = %d shells; we hold %d",
        head, b.base or 0, b.path_cost or 0, b.cost or 0,
        b.km_ally_pn or -1, b.km_ally_pn or -1,
        b.base or 0,
        state._last_info and bit.rshift(state._last_info.tankx or 0, 8) or -1,
        state._last_info and bit.rshift(state._last_info.tanky or 0, 8) or -1,
        b.mx or 0, b.my or 0,
        b.km_ally_armour or 0, C.TANK_SHELL_DAMAGE or 5,
        C.KILL_ME_SHELL_MARGIN or 2, C.TANK_COMBAT_MIN_SHELLS or 10,
        b.km_need_shells or 0, b.tank_shells or 0)
    end
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
    local pwx = pw * sui * refuel_mult_for_pool(pname)
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
    -- BASE POOLS: cap the rendered rows.  A 16-base map otherwise buries the
    -- interesting bases (the ones a shell or two from falling, which now sort to
    -- the top on the armour-aware markup) under a wall of full-armour rows.
    -- Rows are already sorted cheapest-first; keep the top BASE_PANEL_MAX and
    -- never drop the winner / active-goal row.
    if (idx == 3 or idx == 7) and #rows > (C.BASE_PANEL_MAX or 6) then
      local keep = {}
      for i = 1, (C.BASE_PANEL_MAX or 6) do keep[i] = rows[i] end
      if win then
        local present = false
        for i = 1, #keep do if keep[i] == win then present = true break end end
        if not present then keep[#keep + 1] = win end
      end
      rows = keep
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
        -- Walk comp in sorted key order. Several goal kinds can sit on the same
        -- tile and loosely substring-match ("refuel" vs "refuel_at_base"), so
        -- more than one entry can match and `break` takes whoever comes first.
        -- comp is string-keyed ("<kind>@<mx>,<my>"), so raw pairs() order is a
        -- per-process hash order. Panel display only — no gameplay effect — but
        -- it kept the pool breakdown from matching between paired replays.
        local _cks = {}
        for _ck in pairs(comp) do _cks[#_cks + 1] = _ck end
        table.sort(_cks)
        for _, _ck in ipairs(_cks) do
          local entry = comp[_ck]
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

      -- Territory-influence scale (x0.5 in our half, x2 in theirs), applied at
      -- selection right before the suicider surcharge. It was stashed on the
      -- competition record but never rendered, which left every row in scaled
      -- territory unable to reproduce its own total from the chips.
      local inf_mult_d = (gc and gc.inf_mult) or 1.0
      if inf_mult_d ~= 1.0 then
        detail_formula = string.format("%s * inf{x%.1f}", detail_formula, inf_mult_d)
        detail_map[#detail_map + 1] = string.format(
          "inf:territory influence at the GOAL TILE (%d,%d) is %s -> x%.1f. cpf.influence_at < -50 (their half) doubles the bid, > 50 (our half) halves it, in between is x1. Skipped in the opening phase and for refuel_at_base / defend_pill / take_cover / repair_pill (with the repair fix on), for the place follow-through row, and for the kill_me delivery row.",
          w.mx or -1, w.my or -1,
          (inf_mult_d > 1) and "hostile" or "friendly", inf_mult_d)
      end

      -- LOADED, BUILDER-LESS surcharge. The LAST multiplier applied to the
      -- row -- after influence, after the suicider/refuel shaping, after
      -- hysteresis and the history penalty -- so it multiplies the total the
      -- other chips add up to, and the row reads
      --   (base x pw x inf x suicider x refuelmult + penalties) x nobuild.
      local nobuild_mult = (gc and gc.nobuild_mult) or 1.0
      if nobuild_mult ~= 1.0 then
        detail_formula = string.format("%s * nobuild{x%.0f}", detail_formula, nobuild_mult)
        detail_map[#detail_map + 1] = string.format(
          "nobuild:LOADED, BUILDER-LESS -- %s. ATTACK_NO_BUILDER_MULT multiplies the FINAL competed cost of attack_tank / attack_pill / attack_base / defend_pill by %.0f while the state holds, so a fight has to be the ONLY thing on offer before a tank carrying an unplaceable stack takes it. Not a reject: with an empty pool the bot can still shoot back. The kill_me delivery row is exempt.",
          M.loaded_no_lgm_label(state), nobuild_mult)
      end

      if gc and gc.escape_no_hyst then
        detail_map[#detail_map + 1] = string.format(
          "hyst:none:no_lgm -- ESCAPE_NO_BUILDER_SKIP_HYST. %s, so this escape row (flee_to_base, or a take_cover that fired on its haul / panic trigger) pays switch(0) + commit(0) to ENTER and skips the multiplicative stickiness bar. Asymmetric: once it IS the goal, every other row pays the normal switch + commitment to displace it.",
          M.loaded_no_lgm_label(state))
      end

      -- Pillbox-suicider surcharge, applied at selection alongside the phase
      -- weight / influence scale. Rendered whenever it isn't 1.0 so the row's
      -- numbers still reconcile (base x pw x inf x suicider + penalties = total).
      local suicider_mult = (gc and gc.suicider_mult) or 1.0
      if suicider_mult ~= 1.0 then
        detail_formula = string.format("%s * suicider{%.1f, %s}", detail_formula, suicider_mult,
                                       tostring(state.suicider_src or "?"))
        local _sui_bs = state.blitz_suicider
        detail_map[#detail_map + 1] = string.format(
          "suicider:pill_suicider role (source=%s%s) -> this goal kind (%s) costs x%.1f (attack_pill and the refuel group are exempt; defend_pill x%.1f, everything else x%.1f)",
          tostring(state.suicider_src or "?"),
          _sui_bs and string.format(", TEMPORARY for blitz pill #%s designated by p%s, %s",
                                    tostring(_sui_bs.pill), tostring(_sui_bs.by),
                                    (_sui_bs.why == "contested")
                                      and "contested{yes -- a hostile tank was within BLITZ_CONTESTED_RANGE of that pill, so BLITZ_CONTESTED_SUICIDERS of the blitz were designated}"
                                      or  "contested{no -- BLITZ_MIN_SUICIDERS quota top-up}") or "",
          tostring(w.kind or (gc and gc.kind) or pname), suicider_mult,
          C.PILL_SUICIDER_DEFEND_MULT or 1.0, C.PILL_SUICIDER_OTHER_MULT or 1.0)
      end

      -- Refuel-group multiplier, same layer as the suicider surcharge, so the
      -- row's numbers still reconcile (base x pw x inf x suicider x refuelmult
      -- + penalties = total).
      local refuel_mult_d = (gc and gc.refuel_mult) or 1.0
      if refuel_mult_d ~= 1.0 then
        detail_formula = string.format("%s * %s", detail_formula, M.refuel_mult_label())
        detail_map[#detail_map + 1] = string.format(
          "refuelmult:the refuel GOAL_GROUP (refuel_at_base + flee_to_base) costs x%.2f for this bot (%s) -- REFUEL_COST_MULT, per-bot overridable with the \"refuel=X\" init token",
          refuel_mult_d, M.refuel_mult_source)
      end

      -- Behind-team attack surcharge, same layer as suicider/refuel, so the row's
      -- numbers still reconcile (base x pw x inf x suicider x refuelmult x ahead
      -- + penalties = total).
      local ahead_mult_d = (gc and gc.ahead_mult) or 1.0
      if ahead_mult_d ~= 1.0 then
        detail_formula = string.format("%s * ahead{x%.2f}", detail_formula, ahead_mult_d)
        detail_map[#detail_map + 1] = string.format(
          "ahead:team NOT ahead (state.team_ahead false: pill lead %.2f < AHEAD_PILL_FRAC %.2f AND base lead %.2f < AHEAD_BASE_FRAC %.2f) -> this offensive row (attack_pill / attack_base / attack_tank) costs x%.2f (BEHIND_ATTACK_MULT), so the bot picks fewer fights while behind",
          state.strength or 0, C.AHEAD_PILL_FRAC, state.base_strength or 0, C.AHEAD_BASE_FRAC, ahead_mult_d)
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
        row_summary = row_summary .. string.format(" x suicider{%.1f, %s}", suicider_mult,
                                                   tostring(state.suicider_src or "?"))
      end
      local refuel_mult = (gc and gc.refuel_mult) or 1.0
      if refuel_mult ~= 1.0 then
        row_summary = row_summary .. " x " .. M.refuel_mult_label()
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
  for _, idx in ipairs({10, 11, 12, 13, 14, 15}) do
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
  -- kill_lgm (13), take_cover (14).  Only emit the section if the brain actually
  -- produced candidates for that pool this tick — keeps the renderer
  -- from drawing empty placeholders when the brain doesn't use the slot.
  -- kill_lgm doesn't go through eval_queue (it's injected directly into
  -- pool_cache from perception); seed by_pool[13] from pool_cache[13]
  -- here so build_section finds rows.
  if pc[13] and pc[13].cands then by_pool[13] = pc[13].cands end
  -- take_cover (14) is the same shape: injected straight into pool_cache by
  -- eval_take_cover with the full candidate scan in `cands`.
  if pc[14] and pc[14].cands then by_pool[14] = pc[14].cands end
  for _, idx in ipairs({11, 12, 13, 14}) do
    if by_pool[idx] and #by_pool[idx] > 0 then
      sections[#sections + 1] = (build_section(idx))
    end
  end

  -- BUILDER (15) — the builder pool's own strip. NOT a new pool and not part
  -- of the eval_queue: this arbiter spends the MAN, not the tank, so it has no
  -- goal, no phase weight and no WINNERS entry. It is built entirely by
  -- builder_pool.panel_section from the record update() left on state, which
  -- is why the numbers on it are the same numbers the dispatch used.
  -- Numbering starts above take_cover (14) so every recording made before
  -- this existed still loads: nothing that was 1..14 has moved.
  --
  -- The section carries `hdr` (owner / eligibility / active job) on top of the
  -- usual rows; the renderer prints those lines above the row list. Rows use
  -- the shared cost + short||long formula renderer like every other pool row.
  do
    local bsec = bpool.panel_section(state)
    if bsec then sections[#sections + 1] = bsec end
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
