-- =========================================================================
-- NewAutopilot/perception.lua — shared per-tick perception cache
--
-- Computes a snapshot of commonly-queried world state once per tick at the
-- start of Brain.think().  Downstream systems (goals, builder, steering)
-- read from state.perc instead of re-scanning world.pills, world.bases,
-- and info.objects independently.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")

local M = {}

-- -------------------------------------------------------------------------
-- M.update(state, world, info)
-- Call once per tick, before goal selection / builder / steering.
-- Populates state.perc with the current perception snapshot.
-- -------------------------------------------------------------------------
function M.update(state, world, info)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local now = state.tick

  local perc = {}

  -- ----- Pill threats: hostile/neutral pills within firing range -----
  local pill_threats = {}
  local nearest_hostile_pill = nil
  local nearest_hostile_pill_dist = math.huge
  local friendly_pills_damaged = 0
  local friendly_pill_count = 0
  local dead_neutral_pill_count = 0
  local neutral_pill_count = 0
  local hostile_pill_count = 0
  local attackable_pill_count = 0  -- hostile or neutral with health > 0

  for id, p in pairs(world.pills) do
    if p.owner == "friendly" then
      friendly_pill_count = friendly_pill_count + 1
      if p.health == 0 and not p.in_tank then
        dead_neutral_pill_count = dead_neutral_pill_count + 1
      elseif p.health > 0 and p.health < C.PILLS_MAX_HEALTH then
        friendly_pills_damaged = friendly_pills_damaged + 1
      end
    else
      -- hostile or neutral
      if p.health == 0 and p.owner == "neutral" and not p.in_tank then
        dead_neutral_pill_count = dead_neutral_pill_count + 1
      elseif p.health > 0 then
        attackable_pill_count = attackable_pill_count + 1
        if p.owner == "hostile" then
          hostile_pill_count = hostile_pill_count + 1
        elseif p.owner == "neutral" then
          neutral_pill_count = neutral_pill_count + 1
        end
        local d = U.mdist(tmx, tmy, p.mx, p.my)
        -- Track nearest hostile/neutral pill (any distance)
        if d < nearest_hostile_pill_dist then
          nearest_hostile_pill_dist = d
          nearest_hostile_pill = { pill = p, id = id, dist = d }
        end
        -- Collect pills within firing range (both hostile AND neutral fire at the tank)
        if d <= C.PILL_RANGE_MAP then
          pill_threats[#pill_threats + 1] = {
            pill = p, id = id, dist = d, anger = p.anger or 0,
          }
        end
      end
    end
  end

  -- Find friendly pills under attack (for defend-under-attack response)
  local worst_attack_pill = nil
  local worst_attack_damage = 0
  for id, p in pairs(world.pills) do
    if p.owner == "friendly" and p.under_attack and p.health > 0 then
      local dmg = p.attack_damage or 0
      if dmg > worst_attack_damage then
        worst_attack_damage = dmg
        worst_attack_pill = { id = id, mx = p.mx, my = p.my, damage = dmg, health = p.health }
      end
    end
  end
  perc.pill_under_attack = worst_attack_pill

  perc.nearest_hostile_pill = nearest_hostile_pill
  perc.pill_threats = pill_threats
  perc.friendly_pill_count = friendly_pill_count
  perc.friendly_pills_damaged = friendly_pills_damaged
  perc.dead_neutral_pill_count = dead_neutral_pill_count
  perc.neutral_pill_count = neutral_pill_count
  perc.hostile_pill_count = hostile_pill_count
  perc.attackable_pill_count = attackable_pill_count

  -- ----- Hostile tanks from info.objects (speed from C ObjectInfo) -----
  -- Velocity tracking: match tanks frame-to-frame by proximity to compute
  -- true velocity (WU per tick) for lead-time aiming.
  local prev_tanks = state._prev_enemy_tanks or {}
  local nearest_hostile_tank = nil
  local nearest_hostile_tank_dist = math.huge
  local enemy_tank_count = 0
  local allied_tank_count = 0
  local enemy_tanks = {}  -- all visible hostile tanks

  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_TANK and (ob.info & OBJECT_HOSTILE) == 0 then
      allied_tank_count = allied_tank_count + 1
    end
    if ob.type == OBJECT_TANK and (ob.info & OBJECT_HOSTILE) ~= 0 then
      enemy_tank_count = enemy_tank_count + 1
      local omx = ob.x >> 8
      local omy = ob.y >> 8
      local d = U.mdist(tmx, tmy, omx, omy)

      -- Match to closest previous-frame tank, then compute velocity by
      -- averaging position delta over the last N ticks of history. Raw
      -- single-tick delta is too noisy: WU positions quantize to
      -- engine-tick resolution, so a tank moving smoothly NE at ~11
      -- wu/tick reports per-tick deltas like (8,0), (0,-8), (8,-8) —
      -- prediction flips between N / E / NE every tick and lead even
      -- toggles off at the magnitude≤8 stationary cutoff. Averaging over
      -- 3 ticks smooths the quantization and keeps magnitude consistent
      -- without the EMA trail-off of past direction changes.
      local HIST_LEN = 3
      local vx, vy = 0, 0
      local svx, svy = 0, 0
      local best_match_d = 5 * 256
      local matched_pt = nil
      for _, pt in ipairs(prev_tanks) do
        local dx = ob.x - pt.wx
        local dy = ob.y - pt.wy
        local md = math.abs(dx) + math.abs(dy)
        if md < best_match_d then
          best_match_d = md
          matched_pt = pt
        end
      end
      -- Build new history newest-first, capped at HIST_LEN. Index 1 is
      -- the CURRENT tick's position; index N is N-1 ticks back.
      local hist = { { wx = ob.x, wy = ob.y } }
      if matched_pt and matched_pt.hist then
        for i = 1, math.min(HIST_LEN - 1, #matched_pt.hist) do
          hist[#hist + 1] = matched_pt.hist[i]
        end
      end
      -- Velocity from the oldest entry in the buffer back to current,
      -- divided by the number of ticks the span actually covers. Falls
      -- back to single-tick when only 2 entries are available (target
      -- just appeared / re-acquired).
      if #hist >= 2 then
        local oldest = hist[#hist]
        local n_ticks = #hist - 1
        vx = (ob.x - oldest.wx) / n_ticks
        vy = (ob.y - oldest.wy) / n_ticks
        -- Sanity-cap: a real tank can't exceed ~16 wu/tick. > 40 wu/tick
        -- means the matcher snapped to a different tank; reuse last
        -- tick's velocity instead of feeding garbage to lead-prediction.
        if vx * vx + vy * vy > 1600 then
          vx = (matched_pt and matched_pt.vx) or 0
          vy = (matched_pt and matched_pt.vy) or 0
        end
        svx, svy = vx, vy
      end

      local entry = { mx = omx, my = omy, dist = d, obj = ob,
                       id = ob.idnum,
                       speed = ob.speed or 0,
                       wx = ob.x, wy = ob.y, vx = vx, vy = vy,
                       svx = svx, svy = svy, hist = hist }
      enemy_tanks[#enemy_tanks + 1] = entry

      if d < nearest_hostile_tank_dist then
        nearest_hostile_tank_dist = d
        nearest_hostile_tank = entry
      end
    end
  end

  -- Save current positions + velocity + position history for next tick's
  -- computation. hist is the rolling 3-tick lookback used to smooth
  -- velocity against engine-tick position quantization.
  state._prev_enemy_tanks = {}
  for _, et in ipairs(enemy_tanks) do
    state._prev_enemy_tanks[#state._prev_enemy_tanks + 1] = {
      wx = et.wx, wy = et.wy,
      vx = et.vx, vy = et.vy,
      svx = et.svx, svy = et.svy,
      hist = et.hist,
    }
  end

  perc.nearest_hostile_tank = nearest_hostile_tank
  perc.enemy_tank_count = enemy_tank_count
  perc.enemy_tanks = enemy_tanks

  -- ----- Enemy LGM tracking: detect parachutes (dead enemy LGM) -----
  local enemy_lgm_sightings = state._enemy_lgm_sightings or {}
  -- (was: redundant `local now = state.tick or 0` — outer `now` from
  -- line 25 is in scope and identical when state.tick is set.)
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_PARACHUTE and (ob.info & OBJECT_HOSTILE) ~= 0 then
      local omx = ob.x >> 8
      local omy = ob.y >> 8
      enemy_lgm_sightings[U.mkey(omx, omy)] = {
        tick = now, mx = omx, my = omy,
      }
    end
  end
  -- enemy_lgm_dead: TRUE while any parachute sighting is within the
  -- ENEMY_LGM_RETURN_TICKS window (i.e. an enemy LGM was seen
  -- parachuting recently and is presumed still respawning). This is
  -- NOT a "currently dead" check — we never see the LGM resurrect,
  -- so the flag is "saw a parachute in the last N ticks". A live LGM
  -- being spotted again does NOT clear the flag; the flag clears only
  -- when ENEMY_LGM_RETURN_TICKS elapses since the last sighting.
  -- Consumers (notably eLGMdead discount on attack_pill cost) should
  -- treat this as "LGM probably can't repair right now" not "definitely
  -- dead".
  local enemy_lgm_dead = false
  local enemy_lgm_eta = math.huge
  for k, s in pairs(enemy_lgm_sightings) do
    local return_tick = s.tick + C.ENEMY_LGM_RETURN_TICKS
    if now < return_tick then
      enemy_lgm_dead = true
      if return_tick < enemy_lgm_eta then enemy_lgm_eta = return_tick end
    else
      enemy_lgm_sightings[k] = nil  -- expired
    end
  end
  state._enemy_lgm_sightings = enemy_lgm_sightings
  perc.enemy_lgm_dead = enemy_lgm_dead
  perc.enemy_lgm_return_tick = enemy_lgm_dead and enemy_lgm_eta or nil

  -- Base Killer Mode: auto-activate when team outnumbers opponents
  -- Count includes self (+1 for our tank)
  perc.allied_tank_count = allied_tank_count + 1  -- +1 = us
  perc.team_advantage = (allied_tank_count + 1) - enemy_tank_count
  perc.base_killer_mode = perc.team_advantage >= C.BASE_KILLER_TEAM_ADVANTAGE
      and enemy_tank_count > 0  -- need at least 1 enemy to make sense

  -- Dead pills on deep sea (bait detection). Piggyback on U.terrain_prev:
  -- every U.ttype/U.traw call from pathfinding/steering/threat populates it,
  -- so we reuse that shared cache instead of running a dedicated view scan.
  -- Unseen tiles are absent (nil), so == C.T_DEEPSEA won't false-flag.
  local terrain_prev = U.terrain_prev
  perc.deepsea_pill_ids = {}
  for pid, p in pairs(world.pills) do
    if p.health == 0 and terrain_prev[U.mkey(p.mx, p.my)] == C.T_DEEPSEA then
      perc.deepsea_pill_ids[pid] = true
    end
  end

  -- ----- Allied LGM protection (aIndy: avoid driving over allied LGMs) -----
  -- ----- Enemy LGM tracking (live sightings in 15x15 view) --------------
  -- Both passes share one scan over info.objects.  Enemy LGMs are
  -- frame-to-frame proximity-matched against last tick's sightings
  -- (LGMs move ~3 wu/tick, so a 1-tile match radius is tight).  We
  -- also try to associate each enemy LGM with the nearest enemy tank
  -- by idnum on first sighting — useful both for "this LGM came out
  -- of tank K" attribution and for predicting where it's heading.
  local allied_lgm_positions = {}
  local enemy_lgms = {}
  local prev_enemy_lgms = state._prev_enemy_lgms or {}
  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_BUILDMAN then
      local lmx = ob.x >> 8
      local lmy = ob.y >> 8
      if (ob.info & OBJECT_HOSTILE) == 0 then
        allied_lgm_positions[#allied_lgm_positions + 1] = { mx = lmx, my = lmy }
      else
        -- Match to last tick's nearest enemy LGM (by wu distance) so
        -- we keep stable identity / velocity even when ObjectInfo
        -- idnum changes across frames.
        local best_match, best_d = nil, 6 * 256  -- 6 wu tolerance
        for _, pl in ipairs(prev_enemy_lgms) do
          local md = math.abs(ob.x - pl.wx) + math.abs(ob.y - pl.wy)
          if md < best_d then best_d = md; best_match = pl end
        end
        local vx, vy = 0, 0
        local seen_since = now
        local near_tank_idnum = nil
        if best_match then
          vx = ob.x - best_match.wx
          vy = ob.y - best_match.wy
          seen_since = best_match.seen_since
          near_tank_idnum = best_match.near_tank_idnum
        end
        -- First sighting: associate with the nearest visible enemy tank.
        if near_tank_idnum == nil then
          local best_t_d = 4 * 256  -- 4 tile tolerance
          for _, et in ipairs(enemy_tanks) do
            local md = math.abs(et.wx - ob.x) + math.abs(et.wy - ob.y)
            if md < best_t_d then best_t_d = md; near_tank_idnum = et.id end
          end
        end
        enemy_lgms[#enemy_lgms + 1] = {
          mx = lmx, my = lmy,
          wx = ob.x, wy = ob.y,
          vx = vx, vy = vy,
          idnum = ob.idnum,
          seen_since = seen_since,
          near_tank_idnum = near_tank_idnum,
          dist = U.mdist(tmx, tmy, lmx, lmy),
        }
      end
    end
  end
  state._prev_enemy_lgms = enemy_lgms
  perc.allied_lgm_positions = allied_lgm_positions
  perc.enemy_lgms = enemy_lgms

  -- ----- Under fire: shell danger or angry pill in range -----
  local threat_at_tank = danger.danger_at(tmx, tmy, now, world)
  perc.threat_at_tank = threat_at_tank

  local under_fire = false
  local fire_source_mx, fire_source_my = nil, nil

  if threat_at_tank > 0 then
    under_fire = true
    -- Find dominant threat source: closest angry pill, or shell direction
    local worst_threat = 0
    for _, pt in ipairs(pill_threats) do
      if pt.anger > 0.3 then
        local threat_val = C.PILL_DANGER_BASE + pt.anger * C.PILL_DANGER_ANGER
        if threat_val > worst_threat then
          worst_threat = threat_val
          fire_source_mx = pt.pill.mx
          fire_source_my = pt.pill.my
        end
      end
    end
  end

  perc.under_fire = under_fire
  perc.fire_source_mx = fire_source_mx
  perc.fire_source_my = fire_source_my

  -- ----- Base supply: what the nearby friendly base offers -----
  if info.base then
    perc.base_supply = {
      armour = info.base.armour or 0,
      shells = info.base.shells or 0,
      mines  = info.base.mines or 0,
    }
    -- Store observed stock on the world.bases entry so goal selection can
    -- skip low-stock bases when choosing where to refuel.
    local bmx = info.base.x
    local bmy = info.base.y
    for id, b in pairs(world.bases) do
      if b.mx == bmx and b.my == bmy then
        b.obs_shells = info.base.shells or 0
        b.obs_armour = info.base.armour or 0
        b.obs_tick   = now
        break
      end
    end
  else
    perc.base_supply = nil
  end

  -- ----- Base counts by owner (for quick-skip in goal selection) -----
  local friendly_base_count = 0
  local neutral_base_count = 0
  local hostile_base_count = 0
  local capturable_hostile_base_count = 0
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      friendly_base_count = friendly_base_count + 1
    elseif b.owner == "neutral" then
      neutral_base_count = neutral_base_count + 1
    elseif b.owner == "hostile" then
      hostile_base_count = hostile_base_count + 1
      if b.health == 0 then
        capturable_hostile_base_count = capturable_hostile_base_count + 1
      end
    end
  end
  perc.friendly_base_count = friendly_base_count
  perc.neutral_base_count = neutral_base_count
  perc.hostile_base_count = hostile_base_count
  perc.capturable_hostile_base_count = capturable_hostile_base_count

  -- ----- Nearest forest within gather radius of tank -----
  local best_forest_d = math.huge
  local best_forest_x, best_forest_y = nil, nil
  local radius = C.FARM_GATHER_RADIUS
  for dy = -radius, radius do
    for dx = -radius, radius do
      local fx, fy = tmx + dx, tmy + dy
      if U.in_map(fx, fy) and U.ttype(fx, fy) == C.T_FOREST then
        local d = U.mdist(tmx, tmy, fx, fy)
        if d < best_forest_d then
          best_forest_d = d
          best_forest_x = fx
          best_forest_y = fy
        end
      end
    end
  end

  if best_forest_x then
    perc.nearest_forest = { mx = best_forest_x, my = best_forest_y, dist = best_forest_d }
  else
    perc.nearest_forest = nil
  end

  state.perc = perc
end

return M
