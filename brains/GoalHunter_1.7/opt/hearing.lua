local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/hearing.lua — sound awareness / combat heat map
--
-- Processes EVENT_SOUND, EVENT_SOUND_SHOOT, EVENT_SOUND_TANK_HIT from
-- info.events each tick.  Maintains a decaying combat heat map.
-- =========================================================================

local C = require("constants")
local U = require("util")

local M = {}

-- Combat heat entries live in two parallel flat tables keyed by mkey:
--   M.heat_intensity[k] -> float (current heat level)
--   M.heat_tick[k]      -> int   (tick of most recent update)
-- mx and my are recoverable from k (mx = k & 255, my = k >> 8) since
-- mkey(mx, my) = my * 256 + mx, so we don't store them per entry.
-- This eliminates the per-first-sight { mx, my, intensity, tick } table
-- allocation that caused the us_threat_hearing ~5 ms GC outlier.
-- Both tables live on M (not closure-private) so the state serializer
-- snapshots and restores them for replay fidelity.
M.heat_intensity = {}
M.heat_tick      = {}

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
  local cur  = M.heat_intensity[k]
  local last = M.heat_tick[k]
  -- Require BOTH parallel entries: if the two tables ever desync (e.g. a
  -- partial state restore leaves heat_intensity[k] set but heat_tick[k]
  -- nil), treat it as a fresh entry rather than doing `tick - nil`, which
  -- crashed the brain in production.
  if cur and last then
    local elapsed = tick - last
    if elapsed > 0 then
      cur = cur - elapsed / HEAT_DECAY_TICKS
      if cur < 0 then cur = 0 end
    end
    cur = cur + amount
    if cur > HEAT_MAX then
      M.heat_intensity[k] = HEAT_MAX
    else
      M.heat_intensity[k] = cur
    end
    M.heat_tick[k] = tick
  else
    M.heat_intensity[k] = amount
    M.heat_tick[k] = tick
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
    for k, t in pairs(M.heat_tick) do
      if tick - t > HEAT_DECAY_TICKS then
        M.heat_tick[k]      = nil
        M.heat_intensity[k] = nil
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
  local best_k = nil
  local best_i = nil
  for k, intensity in pairs(M.heat_intensity) do
    if intensity > 0.1 then
      local hx = bit.band(k, 255)
      local hy = bit.rshift(k, 8)
      local d = U.mdist(mx, my, hx, hy)
      if d < best_d then
        best_d = d
        best_k = k
        best_i = intensity
      end
    end
  end
  if not best_k then return nil end
  local hx = bit.band(best_k, 255)
  local hy = bit.rshift(best_k, 8)
  return {
    mx = hx, my = hy,
    dx = hx - mx, dy = hy - my,
    dist = best_d, intensity = best_i,
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
      local cur  = M.heat_intensity[k]
      local last = M.heat_tick[k]
      if cur and last then
        local elapsed = tick - last
        local intensity = math.max(0, cur - elapsed / HEAT_DECAY_TICKS)
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
  -- Mutate in place so any cached references stay valid.
  for k in pairs(M.heat_intensity) do M.heat_intensity[k] = nil end
  for k in pairs(M.heat_tick)      do M.heat_tick[k]      = nil end
end

return M
