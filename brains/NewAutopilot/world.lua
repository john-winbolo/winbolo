-- =========================================================================
-- NewAutopilot/world.lua — base/pill tracking, anger model, spatial index
-- =========================================================================

local C   = require("constants")
local log = require("logger")
local TAG = "[" .. C.BRAIN_NAME .. "]"

local M = {}

local function owner_string(obj_info)
  local hostile = (obj_info & OBJECT_HOSTILE) ~= 0
  local neutral = (obj_info & OBJECT_NEUTRAL) ~= 0
  if neutral then return "neutral" end
  if hostile then return "hostile" end
  return "friendly"
end

local function mkey(mx, my) return my * 256 + mx end

-- Rebuild spatial index tables from the id-keyed tables.
-- Called after any update that changes positions.
local function rebuild_index(world)
  local pill_at = {}
  for id, p in pairs(world.pills) do
    local k = mkey(p.mx, p.my)
    pill_at[k] = pill_at[k] or {}
    pill_at[k][#pill_at[k] + 1] = { id = id, pill = p }
  end
  world.pill_at = pill_at

  local base_at = {}
  for id, b in pairs(world.bases) do
    local k = mkey(b.mx, b.my)
    base_at[k] = { id = id, base = b }
  end
  world.base_at = base_at
end

function M.update(world, info, tick)
  world.tick = tick  -- store for staleness reporting
  for _, obj in ipairs(info.objects) do
    if obj.type == OBJECT_REFBASE then
      local prev = world.bases[obj.idnum]
      world.bases[obj.idnum] = {
        mx        = obj.x >> 8,
        my        = obj.y >> 8,
        health    = obj.direction,
        owner     = owner_string(obj.info),
        last_seen = tick,
        -- Preserve last_health from previous observation for change detection
        last_health = prev and prev.health or obj.direction,
      }
    elseif obj.type == OBJECT_PILLBOX then
      local prev = world.pills[obj.idnum]
      local new_health = obj.direction
      local anger      = 0
      local anger_tick = 0

      if prev then
        anger      = prev.anger      or 0
        anger_tick = prev.anger_tick or 0

        -- Health dropped since last observation -> pill was shot -> now angry
        if new_health < prev.health and new_health > 0 then
          anger      = 1.0
          anger_tick = tick
        end

        -- Decay anger linearly over PILL_ANGER_DECAY ticks
        if anger > 0 and tick > anger_tick then
          local elapsed = tick - anger_tick
          anger = math.max(0, 1.0 - elapsed / C.PILL_ANGER_DECAY)
        end
      end

      -- Track health drops on friendly pills for defend-under-attack
      local under_attack  = prev and prev.under_attack or false
      local attack_tick   = prev and prev.attack_tick or 0
      local attack_damage = prev and prev.attack_damage or 0
      local owner_str     = owner_string(obj.info)

      if owner_str == "friendly" and prev and prev.health
         and new_health < prev.health and new_health > 0 then
        local damage = prev.health - new_health
        under_attack  = true
        attack_tick   = tick
        attack_damage = attack_damage + damage
        log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                  obj.idnum, obj.x >> 8, obj.y >> 8, attack_damage, new_health))
      elseif under_attack and attack_tick > 0
             and (tick - attack_tick) > C.PILL_ATTACK_COOLDOWN then
        under_attack  = false
        attack_damage = 0
      end

      world.pills[obj.idnum] = {
        mx            = obj.x >> 8,
        my            = obj.y >> 8,
        health        = new_health,
        owner         = owner_str,
        anger         = anger,
        anger_tick    = anger_tick,
        last_seen     = tick,
        under_attack  = under_attack,
        attack_tick   = attack_tick,
        attack_damage = attack_damage,
      }
    end
  end

  rebuild_index(world)
end

-- ---------------------------------------------------------------------------
-- M.process_events(world, info, state)
-- Process game events received from the C brain interface.
-- Events provide instant updates before the normal object-scan in M.update().
-- ---------------------------------------------------------------------------
function M.process_events(world, info, state)
  local events = info.events
  if not events or #events == 0 then return end
  local tick = state.tick

  for _, ev in ipairs(events) do
    local d = ev.data
    if ev.type == EVENT_PILL_CAPTURED and d then
      -- data: [newOwner, prevOwner]
      -- We don't know which pill index this is from the event alone,
      -- but EVENT_PILL_UPDATE follows with full state; this is informational.

    elseif ev.type == EVENT_BASE_CAPTURED and d then
      -- Informational; EVENT_BASE_UPDATE follows with full state.

    elseif ev.type == EVENT_PILL_UPDATE and d then
      -- data: [pillIndex, x, y, owner, armour, speed, inTank]
      local idx = d[1]
      if idx then
        local prev = world.pills[idx]
        local new_health = d[5] or 0
        local owner_val = d[4] or 0xFF
        local in_tank = (d[7] or 0) ~= 0
        local owner_str
        if owner_val == NEUTRAL_PLAYER then
          owner_str = "neutral"
        elseif owner_val == info.player_number then
          owner_str = "friendly"
        else
          -- Check alliance (we don't have full alliance info here,
          -- so default to hostile for other players)
          owner_str = "hostile"
        end
        -- Preserve anger from previous observation
        local anger = prev and prev.anger or 0
        local anger_tick = prev and prev.anger_tick or 0
        if prev and new_health < prev.health and new_health > 0 then
          anger = 1.0
          anger_tick = tick
        end

        -- Track health drops on friendly pills for defend-under-attack
        local under_attack  = prev and prev.under_attack or false
        local attack_tick_v = prev and prev.attack_tick or 0
        local attack_damage = prev and prev.attack_damage or 0

        if owner_str == "friendly" and prev and prev.health
           and new_health < prev.health and new_health > 0 then
          local damage = prev.health - new_health
          under_attack  = true
          attack_tick_v = tick
          attack_damage = attack_damage + damage
          log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                    idx, d[2] or 0, d[3] or 0, attack_damage, new_health))
        elseif under_attack and attack_tick_v > 0
               and (tick - attack_tick_v) > C.PILL_ATTACK_COOLDOWN then
          under_attack  = false
          attack_damage = 0
        end

        world.pills[idx] = {
          mx            = d[2] or (prev and prev.mx or 0),
          my            = d[3] or (prev and prev.my or 0),
          health        = new_health,
          owner         = owner_str,
          anger         = anger,
          anger_tick    = anger_tick,
          last_seen     = tick,
          in_tank       = in_tank,
          under_attack  = under_attack,
          attack_tick   = attack_tick_v,
          attack_damage = attack_damage,
        }
      end

    elseif ev.type == EVENT_BASE_UPDATE and d then
      -- data: [baseIndex, owner, armour, shells, mines]
      local idx = d[1]
      if idx then
        local prev = world.bases[idx]
        local owner_val = d[2] or 0xFF
        local owner_str
        if owner_val == NEUTRAL_PLAYER then
          owner_str = "neutral"
        elseif owner_val == info.player_number then
          owner_str = "friendly"
        else
          owner_str = "hostile"
        end
        world.bases[idx] = {
          mx         = prev and prev.mx or 0,
          my         = prev and prev.my or 0,
          health     = d[3] or 0,
          owner      = owner_str,
          last_seen  = tick,
          last_health = prev and prev.health or (d[3] or 0),
          obs_shells = d[4],
          obs_armour = d[3],
          obs_tick   = tick,
        }
      end

    elseif ev.type == EVENT_TANK_KILLED and d then
      -- data: [killedPlayer, killerPlayer] (or just [killedPlayer])
      -- Informational; threat module can use this.

    elseif ev.type == EVENT_PLAYER_LEAVE and d then
      -- data: [playerNum]
      -- Player left; downstream systems will notice missing objects.

    elseif ev.type == EVENT_LGM_LOST and d then
      -- data: [playerNum] — a player's builder was killed.
      -- Informational; could be used for tactical decisions.
    end
  end

  rebuild_index(world)
end

-- Reset spatial index (call from Brain.open after clearing bases/pills)
function M.reset(world)
  world.pill_at = {}
  world.base_at = {}
end

-- Lookup: return the pill entry at (mx, my) with health > 0, or nil.
-- When multiple pills share a tile (rare), returns the first live one.
function M.pill_at(world, mx, my)
  local entries = world.pill_at[mkey(mx, my)]
  if not entries then return nil end
  for _, e in ipairs(entries) do
    if e.pill.health > 0 then return e.pill end
  end
  return nil
end

-- Lookup: return the base entry at (mx, my), or nil.
function M.base_at(world, mx, my)
  local entry = world.base_at[mkey(mx, my)]
  return entry and entry.base or nil
end

function M.print_report(world)
  local base_ids = {}
  for id in pairs(world.bases) do base_ids[#base_ids + 1] = id end
  table.sort(base_ids)
  print(string.format(TAG .. " === Bases (%d) ===", #base_ids))
  for _, id in ipairs(base_ids) do
    local b = world.bases[id]
    local age = b.last_seen and (world.tick and (world.tick - b.last_seen) or 0) or -1
    print(string.format(TAG .. "   Base #%d  (%d, %d)  hp=%d  %s  seen=%dt ago",
          id, b.mx, b.my, b.health or 0, b.owner, age))
  end

  local pill_ids = {}
  for id in pairs(world.pills) do pill_ids[#pill_ids + 1] = id end
  table.sort(pill_ids)
  print(string.format(TAG .. " === Pillboxes (%d) ===", #pill_ids))
  for _, id in ipairs(pill_ids) do
    local p = world.pills[id]
    local age = p.last_seen and (world.tick and (world.tick - p.last_seen) or 0) or -1
    print(string.format(TAG .. "   Pill #%d  (%d, %d)  health=%d  %s  seen=%dt ago",
          id, p.mx, p.my, p.health, p.owner, age))
  end
end

return M
