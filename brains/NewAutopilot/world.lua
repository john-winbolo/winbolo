-- =========================================================================
-- NewAutopilot/world.lua — base/pill tracking, anger model, spatial index
--
-- Invariant: pill and base IDs are stable for the entire game session (Bolo
-- allocates them at map load and never frees them). That lets us mutate the
-- existing record in place on each tick instead of reallocating, and keep
-- the pill_at / base_at spatial indexes incrementally maintained instead of
-- rebuilding both from scratch every tick.
-- =========================================================================

local C       = require("constants")
local log     = require("logger")
local metrics = require("metrics")
local TAG     = "[" .. C.BRAIN_NAME .. "]"

-- clock_us is registered as a global by braincore.c; fall back to 0 so the
-- replay harness (which doesn't inject a real clock_us) doesn't crash.
local clock_us = clock_us or function() return 0 end

local M = {}

local function owner_string(obj_info)
  local hostile = (obj_info & OBJECT_HOSTILE) ~= 0
  local neutral = (obj_info & OBJECT_NEUTRAL) ~= 0
  if neutral then return "neutral" end
  if hostile then return "hostile" end
  return "friendly"
end

local function mkey(mx, my) return my * 256 + mx end

-- Incremental pill_at maintenance. Multiple pills can share a tile (pickup /
-- replace transients), so pill_at[k] is a list.
local function pill_index_add(world, id, p)
  local k = mkey(p.mx, p.my)
  local list = world.pill_at[k]
  if not list then
    list = {}
    world.pill_at[k] = list
  end
  list[#list + 1] = { id = id, pill = p }
end

local function pill_index_remove(world, id, old_mx, old_my)
  local k = mkey(old_mx, old_my)
  local list = world.pill_at[k]
  if not list then return end
  for i = 1, #list do
    if list[i].id == id then
      table.remove(list, i)
      break
    end
  end
  if #list == 0 then world.pill_at[k] = nil end
end

-- Safety net only — nothing calls this in the hot path now that update and
-- process_events maintain pill_at / base_at incrementally. Kept around for
-- debugging / replay re-seeding if a snapshot ever arrives without indexes.
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

  local obj_count = 0
  local t0 = clock_us()
  for _, obj in ipairs(info.objects) do
    obj_count = obj_count + 1
    if obj.type == OBJECT_REFBASE then
      local new_mx     = obj.x >> 8
      local new_my     = obj.y >> 8
      local new_health = obj.direction
      local new_owner  = owner_string(obj.info)
      local b = world.bases[obj.idnum]
      if b == nil then
        b = {
          mx          = new_mx,
          my          = new_my,
          health      = new_health,
          owner       = new_owner,
          last_seen   = tick,
          last_health = new_health,
        }
        world.bases[obj.idnum] = b
        world.base_at[mkey(new_mx, new_my)] = { id = obj.idnum, base = b }
      else
        -- Capture last_health BEFORE overwriting health — change detection
        -- downstream (siege, capture alerts) compares health vs last_health.
        b.last_health = b.health
        if b.mx ~= new_mx or b.my ~= new_my then
          world.base_at[mkey(b.mx, b.my)] = nil
          b.mx = new_mx
          b.my = new_my
          world.base_at[mkey(new_mx, new_my)] = { id = obj.idnum, base = b }
        end
        b.health    = new_health
        b.owner     = new_owner
        b.last_seen = tick
      end
    elseif obj.type == OBJECT_PILLBOX then
      local new_mx     = obj.x >> 8
      local new_my     = obj.y >> 8
      local new_health = obj.direction
      local owner_str  = owner_string(obj.info)
      local p = world.pills[obj.idnum]
      if p == nil then
        p = {
          mx            = new_mx,
          my            = new_my,
          health        = new_health,
          owner         = owner_str,
          anger         = 0,
          anger_tick    = 0,
          last_seen     = tick,
          under_attack  = false,
          attack_tick   = 0,
          attack_damage = 0,
        }
        world.pills[obj.idnum] = p
        pill_index_add(world, obj.idnum, p)
      else
        -- Read old state BEFORE writing new — damage detection, anger bump,
        -- and the index move all need the previous tick's values.
        local old_health = p.health
        local old_mx     = p.mx
        local old_my     = p.my

        -- Anger: each fresh damage hit adds C.PILL_ANGER_BUMP, capped at
        -- 1.0. Three hits saturate. Otherwise decays linearly from the
        -- last bump's tick. Resetting anger_tick on every bump keeps the
        -- decay consistent — the next decay step measures from "now",
        -- not from the first hit hours ago.
        if new_health < old_health and new_health > 0 then
          p.anger      = math.min(1.0, (p.anger or 0) + C.PILL_ANGER_BUMP)
          p.anger_tick = tick
        elseif p.anger > 0 and tick > p.anger_tick then
          local elapsed = tick - p.anger_tick
          p.anger = math.max(0, p.anger - elapsed / C.PILL_ANGER_DECAY)
          p.anger_tick = tick
        end

        -- Under-attack tracking for friendly pills.
        if owner_str == "friendly" and new_health < old_health and new_health > 0 then
          local damage = old_health - new_health
          p.attack_damage = p.attack_damage + damage
          p.under_attack  = true
          p.attack_tick   = tick
          log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                    obj.idnum, new_mx, new_my, p.attack_damage, new_health))
        elseif p.under_attack and p.attack_tick > 0
               and (tick - p.attack_tick) > C.PILL_ATTACK_COOLDOWN then
          p.under_attack  = false
          p.attack_damage = 0
        end

        if old_mx ~= new_mx or old_my ~= new_my then
          pill_index_remove(world, obj.idnum, old_mx, old_my)
          p.mx = new_mx
          p.my = new_my
          pill_index_add(world, obj.idnum, p)
        end
        p.health    = new_health
        p.owner     = owner_str
        p.last_seen = tick
      end
    end
  end
  local t1 = clock_us()

  metrics.set("us_world_update_objects", t1 - t0)
  metrics.set("world_update_obj_count", obj_count)
  -- Index is maintained incrementally now; kept for report continuity.
  metrics.set("us_world_update_reindex", 0)
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

        local p = world.pills[idx]
        if p == nil then
          p = {
            mx            = d[2] or 0,
            my            = d[3] or 0,
            health        = new_health,
            owner         = owner_str,
            anger         = 0,
            anger_tick    = 0,
            last_seen     = tick,
            in_tank       = in_tank,
            under_attack  = false,
            attack_tick   = 0,
            attack_damage = 0,
          }
          world.pills[idx] = p
          pill_index_add(world, idx, p)
        else
          -- Read old state BEFORE writing new — index move and damage
          -- detection both need the previous tick's values.
          local old_health = p.health
          local old_mx     = p.mx
          local old_my     = p.my
          local new_mx     = d[2] or old_mx
          local new_my     = d[3] or old_my

          if new_health < old_health and new_health > 0 then
            p.anger      = math.min(1.0, (p.anger or 0) + C.PILL_ANGER_BUMP)
            p.anger_tick = tick
          end

          if owner_str == "friendly" and new_health < old_health and new_health > 0 then
            local damage = old_health - new_health
            p.attack_damage = p.attack_damage + damage
            p.under_attack  = true
            p.attack_tick   = tick
            log.event("pill_under_attack", string.format("pill#%d@(%d,%d) dmg=%d hp=%d",
                      idx, new_mx, new_my, p.attack_damage, new_health))
          elseif p.under_attack and p.attack_tick > 0
                 and (tick - p.attack_tick) > C.PILL_ATTACK_COOLDOWN then
            p.under_attack  = false
            p.attack_damage = 0
          end

          if old_mx ~= new_mx or old_my ~= new_my then
            pill_index_remove(world, idx, old_mx, old_my)
            p.mx = new_mx
            p.my = new_my
            pill_index_add(world, idx, p)
          end
          p.health    = new_health
          p.owner     = owner_str
          p.last_seen = tick
          p.in_tank   = in_tank
        end
      end

    elseif ev.type == EVENT_BASE_UPDATE and d then
      -- data: [baseIndex, owner, armour, shells, mines]
      local idx = d[1]
      if idx then
        local owner_val = d[2] or 0xFF
        local new_health = d[3] or 0
        local owner_str
        if owner_val == NEUTRAL_PLAYER then
          owner_str = "neutral"
        elseif owner_val == info.player_number then
          owner_str = "friendly"
        else
          owner_str = "hostile"
        end

        local b = world.bases[idx]
        if b == nil then
          -- EVENT_BASE_UPDATE doesn't carry position; seed at (0,0) and let
          -- the next M.update object-scan move the base_at entry to the
          -- correct key.
          b = {
            mx          = 0,
            my          = 0,
            health      = new_health,
            owner       = owner_str,
            last_seen   = tick,
            last_health = new_health,
            obs_shells  = d[4],
            obs_armour  = new_health,
            obs_tick    = tick,
          }
          world.bases[idx] = b
          world.base_at[mkey(0, 0)] = { id = idx, base = b }
        else
          -- Capture last_health BEFORE overwriting health — change detection
          -- downstream relies on this ordering.
          b.last_health = b.health
          b.health      = new_health
          b.owner       = owner_str
          b.last_seen   = tick
          b.obs_shells  = d[4]
          b.obs_armour  = new_health
          b.obs_tick    = tick
        end
      end

    elseif ev.type == EVENT_TANK_KILLED and d then
      -- data: [killedPlayer, killerPlayer] (or just [killedPlayer])
      -- Informational; threat module can use this.

    elseif ev.type == EVENT_PLAYER_LEAVE and d then
      -- data: [playerNum]
      -- Player left; downstream systems will notice missing objects.

    elseif ev.type == EVENT_LGM_LOST and d then
      -- data: [victim_pn, killer_pn] — a player's builder was killed.
      -- Stamp the death + respawn ETA on the lgm_registry so
      -- attack_pill cost shaping, ally coordination, etc. can react.
      local _lgmreg = package.loaded["lgm_registry"]
      if _lgmreg then
        _lgmreg.note_death(d[1] or 0, d[2] or 0, tick or 0)
      end
    end
  end
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

-- True if the tank is currently parked on a friendly or neutral base tile.
-- Note: info.base in BrainInfo is set whenever a base is within ~7 tiles
-- (BASE_STATUS_RANGE), so it cannot be used for "actually on the base".
function M.tank_on_friendly_base(world, info)
  local tmx = info.tankx >> 8
  local tmy = info.tanky >> 8
  local b = M.base_at(world, tmx, tmy)
  if not b then return false end
  return b.owner == "friendly" or b.owner == "neutral"
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
