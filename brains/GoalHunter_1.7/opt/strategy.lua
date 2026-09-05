-- =========================================================================
-- GoalHunter/strategy.lua — Game phase detection + front line (Phases 2-3)
--
-- Classifies each tick into: opening / middle / endgame_winning / endgame_losing
-- with hysteresis to prevent flapping.  Also computes strength ratios and
-- front line summary for later use by goal weighting and pill placement.
-- =========================================================================

local C   = require("constants")
local cpf = require("cpathfinder")
local log = require("logger")
local print2 = require("print2")

local M = {}

-- Shells likely needed for the next 1-2 attack missions, read from the
-- previous replan's pool_cache. At tick 0 the cache is empty and this
-- returns 0 — the baseline floor still applies.
--
-- For each entry we combine:
--   path_shells  = shells spent shooting walls en route
--                  (info.shells - entry._shells_on_arrival, floored at 0)
--   target_shells = shells to destroy the target (pill.health / base.health)
-- capped at TANK_FULL_SHELLS, then the top two estimates are summed.
local function estimate_mission_shells(state, world, info)
  local pc = state.pool_cache
  if not pc then return 0 end

  local current = info and info.shells or C.TANK_FULL_SHELLS

  local function path_cost(entry)
    local soa = entry._shells_on_arrival
    if not soa then return 0 end
    return math.max(0, current - soa)
  end

  local estimates = {}
  local p6 = pc[6]  -- attack_pill
  if p6 and p6._pill and p6._pill.health then
    estimates[#estimates + 1] = math.min(path_cost(p6) + p6._pill.health,
                                         C.TANK_FULL_SHELLS)
  end
  local p7 = pc[7]  -- attack_base
  if p7 and p7.goal and p7.goal.target_id and world and world.bases then
    local base = world.bases[p7.goal.target_id]
    local hp = (base and base.health) or 20  -- base HP fallback if unresolvable
    estimates[#estimates + 1] = math.min(path_cost(p7) + hp, C.TANK_FULL_SHELLS)
  end

  table.sort(estimates, function(a, b) return a > b end)
  local sum = 0
  for i = 1, math.min(2, #estimates) do sum = sum + estimates[i] end
  return sum
end

local function tank_combat_shells(state, info)
  if not (state.perc and state.perc.enemy_tanks) then return 0 end
  local count = 0
  for _, et in ipairs(state.perc.enemy_tanks) do
    if (et.dist or math.huge) <= C.REFUEL_ENEMY_TANK_RANGE then
      count = count + 1
      if count >= C.REFUEL_MAX_ENEMY_TANKS_COUNTED then break end
    end
  end
  return count * C.REFUEL_PER_ENEMY_TANK
end

-- Sets state.shell_target and state.armour_target. Called once per tick
-- at the end of M.update, so the rest of the brain (init.lua refuel check,
-- goals.lua pool-1 scaling, nearest_resupply_base filters) reads a single
-- consistent pair.
local function compute_refuel_targets(state, world, info)
  local baseline = C.REFUEL_BASELINE_SHELLS
  local mission  = estimate_mission_shells(state, world, info)
  local combat   = tank_combat_shells(state, info)

  local target = math.max(baseline, mission + C.SHELL_RESERVE, combat)
  state.shell_target = math.min(target, C.TANK_FULL_SHELLS)

  -- Armour ceiling is full: the bot fills toward 40 when bases are plentiful,
  -- but the scarcity top-off ramp (goals.lua refuel_shape) makes it leave near
  -- ARMOUR_LOW (15) when the team is base-starved. ARMOUR_LOW is the real floor.
  state.armour_target = C.TANK_FULL_ARMOUR
end

-- Compute center of gravity of friendly bases + pills
local function friendly_cog(world)
  local sx, sy, count = 0, 0, 0
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      sx = sx + b.mx; sy = sy + b.my; count = count + 1
    end
  end
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health > 0 then
      sx = sx + p.mx; sy = sy + p.my; count = count + 1
    end
  end
  if count == 0 then return nil, nil end
  return math.floor(sx / count + 0.5), math.floor(sy / count + 0.5)
end

-- Called once per tick from init.lua, after perception update.
-- Sets state.phase, state.strength, state.base_strength, and front line data.
function M.update(state, world, info)
  local perc = state.perc

  -- Ticks since THIS brain instance opened. state.tick is now seeded from the
  -- engine clock (init.lua Brain.open), so it is game time, not the bot's own
  -- age. The phase tests below were all written against a counter that
  -- restarted at 0 on every Brain.open, so they use `age` and keep behaving
  -- exactly as they did — a bot re-created mid-game still runs its opening
  -- land-grab from ITS start. Identical to state.tick for a game-start bot.
  local age = state.tick - (state.birth_tick or 0)

  -- Count all pills and bases — neutrals count as unclaimed territory.
  -- Endgame requires ALL pills AND bases to be captured (no neutrals left).
  local friendly_pills = perc.friendly_pill_count or 0
  local hostile_pills = perc.hostile_pill_count or 0
  local neutral_pills = (perc.neutral_pill_count or 0) + (perc.dead_neutral_pill_count or 0)
  local total_pills = friendly_pills + hostile_pills + neutral_pills
  local friendly_ratio = total_pills > 0 and (friendly_pills / total_pills) or 0.5

  local friendly_bases = perc.friendly_base_count or 0
  local hostile_bases = perc.hostile_base_count or 0
  local contested_bases = friendly_bases + hostile_bases
  local base_ratio = contested_bases > 0 and (friendly_bases / contested_bases) or 0.5

  -- Total counts (including neutrals) still needed for opening phase check
  local total_bases = friendly_bases + hostile_bases + (perc.neutral_base_count or 0)

  -- Determine candidate phase
  -- Opening threshold: as time passes, tolerate more neutral bases remaining.
  -- At t=0: must capture ALL neutral bases (threshold=0)
  -- At 4min (12000t): allow 1/4 of bases neutral (threshold = total * 0.25)
  -- At 6min (18000t): allow 1/2 of bases neutral (threshold = total * 0.5)
  local neutral_count = perc.neutral_base_count or 0
  local opening_tolerance = 0
  if age > C.OPENING_MIN_TICKS and total_bases > 0 then
    local minutes = age / (50 * 60)
    -- Exponential ramp: 0 at 0min, ~0.25 at 4min, ~0.5 at 6min, ~0.75 at 10min
    opening_tolerance = total_bases * (1.0 - math.exp(-minutes * 0.18))
  end

  -- Opening-exit count: after a grace period, don't let a neutral base we
  -- genuinely CANNOT path to keep us stuck in the opening land-grab forever.
  -- The brain holds the full true terrain from the start (botLoadMapFromServer)
  -- and unknown/fog tiles are passable, so an INF dijkstra cost = truly
  -- walled/water-locked, NOT merely unexplored. Gated by OPENING_UNREACHABLE_-
  -- GRACE_TICKS so the cold-start window (surface not yet expanded, bases not
  -- reached yet) and the earliest land-grab aren't disrupted. Only narrows the
  -- OPENING test; perc.neutral_base_count is left intact for every other reader.
  local opening_neutral_count = neutral_count
  if neutral_count > 0
     and age > (C.OPENING_UNREACHABLE_GRACE_TICKS or 1500) then
    local reachable = 0
    local boat = (info and info.inboat) and 1 or 0
    for _, b in pairs(world.bases) do
      if b.owner == "neutral" then
        local c = cpf.smart_cost_dij_only(cpf.KIND_NORMAL, b.mx, b.my, boat)
        if c and c < 1e29 then reachable = reachable + 1 end
      end
    end
    opening_neutral_count = reachable
  end

  -- All bases claimed (none neutral left) → the opening land-grab is definitively
  -- over, end opening NOW even inside the OPENING_MIN_TICKS window (and bypass the
  -- phase hysteresis below). Guard on total_bases>0 so a cold-start tick where
  -- perception hasn't counted bases yet (all counts 0) can't false-trigger.
  local all_bases_taken = (total_bases > 0 and neutral_count == 0)

  local new_phase, phase_reason
  if age < C.OPENING_MIN_TICKS and not all_bases_taken then
    new_phase = "opening"
    phase_reason = string.format("tick %d < %d", age, C.OPENING_MIN_TICKS)
  elseif opening_neutral_count > opening_tolerance then
    new_phase = "opening"
    phase_reason = string.format("neutral_bases %d (reachable %d) > %.0f tol",
                                 neutral_count, opening_neutral_count, opening_tolerance)
  elseif neutral_pills > 0 or neutral_count > 0 then
    new_phase = "middle"
    phase_reason = string.format("neutrals remain: %d pills %d bases", neutral_pills, neutral_count)
  elseif friendly_ratio > C.ENDGAME_PILL_RATIO then
    new_phase = "endgame_winning"
    phase_reason = string.format("pills %.0f%% > %.0f%% (%d of %d)", friendly_ratio*100, C.ENDGAME_PILL_RATIO*100, friendly_pills, total_pills)
  elseif base_ratio > C.ENDGAME_BASE_RATIO then
    new_phase = "endgame_winning"
    phase_reason = string.format("bases %.0f%% > %.0f%% (%d of %d contested)", base_ratio*100, C.ENDGAME_BASE_RATIO*100, friendly_bases, contested_bases)
  elseif friendly_ratio < (1.0 - C.ENDGAME_PILL_RATIO) then
    new_phase = "endgame_losing"
    phase_reason = string.format("pills %.0f%% < %.0f%% (%d of %d)", friendly_ratio*100, (1.0 - C.ENDGAME_PILL_RATIO)*100, friendly_pills, total_pills)
  elseif base_ratio < (1.0 - C.ENDGAME_BASE_RATIO) then
    new_phase = "endgame_losing"
    phase_reason = string.format("bases %.0f%% < %.0f%% (%d of %d contested)", base_ratio*100, (1.0 - C.ENDGAME_BASE_RATIO)*100, friendly_bases, contested_bases)
  else
    new_phase = "middle"
    phase_reason = string.format("pills=%.0f%% (%d/%d) bases=%.0f%% (%d/%d)", friendly_ratio*100, friendly_pills, total_pills, base_ratio*100, friendly_bases, contested_bases)
  end

  -- One-way ratchet: "opening" is a startup-only phase. Once we've advanced past
  -- it (state.phase is anything non-opening), never fall back to opening — a late
  -- base reverting to neutral shouldn't re-trigger opening-phase behavior. Hold
  -- at "middle" instead. (Endgame<->middle still flexes freely; only opening is
  -- latched off.)
  if new_phase == "opening" and state.phase and state.phase ~= "opening" then
    phase_reason = "ratchet: past opening (was: " .. phase_reason .. ")"
    new_phase = "middle"
  end
  state.phase_reason = phase_reason

  -- Hysteresis: require N consecutive ticks before switching
  if new_phase ~= state.phase then
    if state.phase == "opening" and all_bases_taken and new_phase ~= "opening" then
      -- Immediate, no-hysteresis exit from opening once every base is claimed.
      local old = state.phase
      state.phase = new_phase
      state.phase_pending = nil
      log.event("phase_change", (old or "none") .. " -> " .. new_phase .. " (all bases taken)")
    elseif not state.phase_pending or state.phase_pending.phase ~= new_phase then
      state.phase_pending = { phase = new_phase, count = 1 }
    else
      state.phase_pending.count = state.phase_pending.count + 1
      if state.phase_pending.count >= C.PHASE_HYSTERESIS_TICKS then
        local old = state.phase
        state.phase = new_phase
        state.phase_pending = nil
        log.event("phase_change", (old or "none") .. " -> " .. new_phase)
      end
    end
  else
    state.phase_pending = nil
  end

  -- Strength ratios (used by goal weighting in Phase 4)
  state.strength = friendly_ratio       -- 0.0 = losing, 1.0 = dominating
  state.base_strength = base_ratio

  -- Ammo deprivation: shells held below AMMO_DEPRIVED_SHELLS for
  -- AMMO_DEPRIVED_TICKS of normal (non-opening) play, with no recovery. We do
  -- NOT care WHY it can't refuel (no base / chose not to) — pure time-below-the-
  -- line. The clock clears the instant shells recover to the line, or in
  -- opening (low ammo is expected during the land-grab). state.ammo_deprived
  -- is read by the squad/attack layer to allow joining any blitz in suicide mode.
  local sh = (info and info.shells) or 0
  if sh >= C.AMMO_DEPRIVED_SHELLS or state.phase == "opening" then
    state.ammo_low_since = nil
  else
    state.ammo_low_since = state.ammo_low_since or state.tick
  end
  local was_deprived = state.ammo_deprived
  state.ammo_deprived = state.ammo_low_since ~= nil
                    and (state.tick - state.ammo_low_since) >= (state.test_deprive_ticks or C.AMMO_DEPRIVED_TICKS)
  -- Latch when the flag first turned on so we can time-box it.
  if state.ammo_deprived and not was_deprived then state._ammo_deprived_since = state.tick end
  -- 5-min cap: a deprived (suicide) bot periodically drops the flag so it gets a
  -- window to refuel/flee normally again; if it's still starved it re-earns
  -- deprivation ~60 s later. (Death also resets it, in init.lua.) We clear
  -- ammo_low_since so the deprivation clock restarts from scratch.
  if state.ammo_deprived and state._ammo_deprived_since
     and (state.tick - state._ammo_deprived_since) >= (C.AMMO_DEPRIVED_MAX_TICKS or 15000) then
    state.ammo_low_since       = nil
    state.ammo_deprived        = false
    state._ammo_deprived_since = nil
  end

  -- Front line computation (every N ticks, not every tick)
  if not state.front_line_tick or (state.tick - state.front_line_tick) >= C.FRONT_LINE_INTERVAL then
    state.front_line_tick = state.tick
    local pts = cpf.find_front_line()
    local n = #pts / 2

    if n > 0 then
      -- Compute centroid of front line
      local sx, sy = 0, 0
      for i = 1, #pts, 2 do
        sx = sx + pts[i]
        sy = sy + pts[i + 1]
      end
      state.front_center_mx = math.floor(sx / n + 0.5)
      state.front_center_my = math.floor(sy / n + 0.5)
      state.front_line_count = n

      -- Compute front direction (from friendly COG to front center)
      local fcx, fcy = friendly_cog(world)
      if fcx then
        local dx = state.front_center_mx - fcx
        local dy = state.front_center_my - fcy
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 0.1 then
          state.front_dir_x = dx / len
          state.front_dir_y = dy / len
        end
      end
    else
      -- No front line (one side has zero influence, or no contested ground)
      state.front_center_mx = nil
      state.front_center_my = nil
      state.front_line_count = 0
      state.front_dir_x = nil
      state.front_dir_y = nil
    end
  end

  -- Run last so every downstream reader (init.lua refuel gate, goals.lua
  -- pool-1 scaling, nearest_resupply_base filters) sees the same targets
  -- this tick.
  compute_refuel_targets(state, world, info)
end

-- Call from Brain.open() to set initial phase
function M.init(state)
  state.phase = "opening"
  state.phase_pending = nil
  state.strength = 0.5
  state.base_strength = 0.5
  state.front_center_mx = nil
  state.front_center_my = nil
  state.front_line_count = 0
  state.front_dir_x = nil
  state.front_dir_y = nil
  state.front_line_tick = nil

  -- Safe conservative defaults for any tick-0 reader that runs before
  -- strategy.update() (and thus compute_refuel_targets) has fired.
  state.shell_target  = C.TANK_FULL_SHELLS
  state.armour_target = C.TANK_FULL_ARMOUR
end

return M
