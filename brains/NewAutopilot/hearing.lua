-- =========================================================================
-- NewAutopilot/hearing.lua — sound awareness / combat heat map
--
-- Processes EVENT_SOUND, EVENT_SOUND_SHOOT, EVENT_SOUND_TANK_HIT from
-- info.events each tick.  Maintains a decaying combat heat map.
-- =========================================================================

local C = require("constants")
local U = require("util")

local M = {}

-- Combat heat entries: heat[mkey] = { mx, my, intensity, tick }
local heat = {}

local HEAT_DECAY_TICKS = 200   -- heat decays to zero over this many ticks
local HEAT_SHOOT       = 0.3   -- intensity bump per shoot event
local HEAT_HIT         = 0.8   -- intensity bump per tank-hit event
local HEAT_SOUND       = 0.5   -- intensity bump per generic combat sound
local HEAT_MAX         = 3.0   -- clamp per cell

local function mkey(mx, my) return my * 256 + mx end

-- Combat-related sound IDs (from sndEffects enum)
local COMBAT_SOUNDS = {}
if SND_SHOOT_NEAR     then COMBAT_SOUNDS[SND_SHOOT_NEAR]     = HEAT_SHOOT end
if SND_SHOOT_FAR      then COMBAT_SOUNDS[SND_SHOOT_FAR]      = HEAT_SHOOT end
if SND_HIT_TANK_NEAR  then COMBAT_SOUNDS[SND_HIT_TANK_NEAR]  = HEAT_HIT end
if SND_HIT_TANK_FAR   then COMBAT_SOUNDS[SND_HIT_TANK_FAR]   = HEAT_HIT end
if SND_BIG_EXPLOSION_NEAR then COMBAT_SOUNDS[SND_BIG_EXPLOSION_NEAR] = HEAT_SOUND end
if SND_BIG_EXPLOSION_FAR  then COMBAT_SOUNDS[SND_BIG_EXPLOSION_FAR]  = HEAT_SOUND end
if SND_MINE_EXPLOSION_NEAR then COMBAT_SOUNDS[SND_MINE_EXPLOSION_NEAR] = HEAT_SOUND end
if SND_MINE_EXPLOSION_FAR  then COMBAT_SOUNDS[SND_MINE_EXPLOSION_FAR]  = HEAT_SOUND end

local function add_heat(mx, my, amount, tick)
  local k = mkey(mx, my)
  local h = heat[k]
  if h then
    -- Decay existing intensity before adding
    local elapsed = tick - h.tick
    if elapsed > 0 then
      h.intensity = math.max(0, h.intensity - elapsed / HEAT_DECAY_TICKS)
    end
    h.intensity = math.min(HEAT_MAX, h.intensity + amount)
    h.tick = tick
  else
    heat[k] = { mx = mx, my = my, intensity = amount, tick = tick }
  end
end

-- -------------------------------------------------------------------------
-- M.update(info, tick)
-- Process sound events from this tick's info.events.
-- -------------------------------------------------------------------------
function M.update(info, tick)
  local events = info.events
  if not events then return end

  for _, ev in ipairs(events) do
    local d = ev.data
    if ev.type == EVENT_SOUND and d then
      -- data: [soundId, mx, my]
      local sid = d[1]
      local bump = COMBAT_SOUNDS[sid]
      if bump then
        add_heat(d[2], d[3], bump, tick)
      end
    elseif ev.type == EVENT_SOUND_SHOOT and d then
      -- data: [soundId, mx, my, firingPlayer]
      add_heat(d[2], d[3], HEAT_SHOOT, tick)
    elseif ev.type == EVENT_SOUND_TANK_HIT and d then
      -- data: [soundId, mx, my, hitPlayer]
      add_heat(d[2], d[3], HEAT_HIT, tick)
    end
  end

  -- Prune dead entries periodically
  if tick % 100 == 0 then
    for k, h in pairs(heat) do
      local elapsed = tick - h.tick
      if elapsed > HEAT_DECAY_TICKS then
        heat[k] = nil
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- M.nearest_combat(mx, my)
-- Returns direction (dx, dy) and distance to nearest recent combat, or nil.
-- -------------------------------------------------------------------------
function M.nearest_combat(mx, my)
  local best_d = math.huge
  local best_h = nil
  for _, h in pairs(heat) do
    if h.intensity > 0.1 then
      local d = U.mdist(mx, my, h.mx, h.my)
      if d < best_d then
        best_d = d
        best_h = h
      end
    end
  end
  if not best_h then return nil end
  return {
    mx = best_h.mx, my = best_h.my,
    dx = best_h.mx - mx, dy = best_h.my - my,
    dist = best_d, intensity = best_h.intensity,
  }
end

-- -------------------------------------------------------------------------
-- M.is_area_hot(mx, my, tick)
-- Returns true if there has been recent combat near (mx, my).
-- -------------------------------------------------------------------------
function M.is_area_hot(mx, my, tick)
  local radius = 5
  for dy = -radius, radius do
    for dx = -radius, radius do
      local k = mkey(mx + dx, my + dy)
      local h = heat[k]
      if h then
        local elapsed = tick - h.tick
        local intensity = math.max(0, h.intensity - elapsed / HEAT_DECAY_TICKS)
        if intensity > 0.2 then return true end
      end
    end
  end
  return false
end

-- -------------------------------------------------------------------------
-- M.reset()
-- Clear all heat data (call from Brain.open).
-- -------------------------------------------------------------------------
function M.reset()
  heat = {}
end

return M
