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
  local hostile_pill_count = 0
  local attackable_pill_count = 0  -- hostile or neutral with health > 0

  for id, p in pairs(world.pills) do
    if p.owner == "friendly" then
      friendly_pill_count = friendly_pill_count + 1
      if p.health > 0 and p.health < C.PILLS_MAX_HEALTH then
        friendly_pills_damaged = friendly_pills_damaged + 1
      end
    else
      -- hostile or neutral
      if p.health == 0 and p.owner == "neutral" then
        dead_neutral_pill_count = dead_neutral_pill_count + 1
      elseif p.health > 0 then
        attackable_pill_count = attackable_pill_count + 1
        if p.owner == "hostile" then
          hostile_pill_count = hostile_pill_count + 1
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
  perc.hostile_pill_count = hostile_pill_count
  perc.attackable_pill_count = attackable_pill_count

  -- ----- Hostile tanks from info.objects (speed from C ObjectInfo) -----
  local nearest_hostile_tank = nil
  local nearest_hostile_tank_dist = math.huge
  local enemy_tank_count = 0
  local enemy_tanks = {}  -- all visible hostile tanks

  for _, ob in ipairs(info.objects) do
    if ob.type == OBJECT_TANK and (ob.info & OBJECT_HOSTILE) ~= 0 then
      enemy_tank_count = enemy_tank_count + 1
      local omx = ob.x >> 8
      local omy = ob.y >> 8
      local d = U.mdist(tmx, tmy, omx, omy)

      -- Speed comes directly from TankSnapshot (actual_speed * 4)
      local entry = { mx = omx, my = omy, dist = d, obj = ob,
                       speed = ob.speed or 0 }
      enemy_tanks[#enemy_tanks + 1] = entry

      if d < nearest_hostile_tank_dist then
        nearest_hostile_tank_dist = d
        nearest_hostile_tank = entry
      end
    end
  end

  perc.nearest_hostile_tank = nearest_hostile_tank
  perc.enemy_tank_count = enemy_tank_count
  perc.enemy_tanks = enemy_tanks

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
