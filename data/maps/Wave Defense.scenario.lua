-- =========================================================================
-- Wave Defense — scripted map scenario (runs on the SERVER).
--
-- Up to 6 human defenders (scenario.max_players) hold the central keep
-- against 5 waves of 10 AI tanks. A wave spawns, fights until every one
-- of its tanks is destroyed (wave bots do not respawn — the script
-- removes them on death), then the next wave arrives after a short
-- breather. Survive all 5 waves and the defenders win. If the attackers
-- ever hold every base, the engine's normal all-bases sweep ends the
-- round for them instead.
--
-- Script surface reference: src/server/scenario.h
-- =========================================================================

scenario = {
  name = "Wave Defense",
  max_players = 6,     -- humans; 10 slots stay free for the wave (6+10=16)
  default_brain = "brains/GoalHunter_1.6/init.lua",  -- wave bot AI
}

local WAVES        = 5
local WAVE_SIZE    = 10
local WAVE_TEAM    = 15     -- alliance id shared by every wave bot
local GRACE_TICKS  = 3000   -- 60 s to capture the keep before wave 1
local BREATHER     = 750    -- 15 s between a cleared wave and the next
local ANNOUNCE_GAP = 250    -- "incoming" warning this many ticks early

local wave = 0              -- waves launched so far
local wave_bots = {}        -- playerNum -> true for living wave members
local next_wave_at = nil    -- tick the next wave spawns (nil = wave live)
local announced = false
local ended = false

-- Count the wave's living tanks; remove dead ones so they never respawn
-- and their slots free up for the next wave.
local function living(game)
  local n = 0
  for p in pairs(wave_bots) do
    local t = game.tank(p)
    if t == nil then
      wave_bots[p] = nil
    elseif t.dead then
      game.remove_bot(p)
      wave_bots[p] = nil
    else
      n = n + 1
    end
  end
  return n
end

local function spawn_wave(game)
  wave = wave + 1
  local ok = 0
  for i = 1, WAVE_SIZE do
    local p = game.spawn_bot(string.format("Wave %d-%d", wave, i), nil, WAVE_TEAM)
    if p then
      wave_bots[p] = true
      ok = ok + 1
    end
  end
  game.message(string.format("*** Wave %d/%d: %d attackers inbound! ***",
                             wave, WAVES, ok))
end

function on_start(game)
  game.message(string.format(
    "*** WAVE DEFENSE: capture the keep! First of %d waves in %d seconds. ***",
    WAVES, GRACE_TICKS / 50))
end

function on_tick(game, tick)
  if ended then return end

  -- Arm wave 1 off the round's first running tick.
  if wave == 0 and next_wave_at == nil then
    next_wave_at = tick + GRACE_TICKS
    return
  end

  if next_wave_at ~= nil then
    -- Countdown to the next wave.
    if not announced and tick >= next_wave_at - ANNOUNCE_GAP then
      announced = true
      game.message(string.format("*** Wave %d incoming in %d seconds! ***",
                                 wave + 1, ANNOUNCE_GAP / 50))
    end
    if tick >= next_wave_at then
      next_wave_at = nil
      announced = false
      spawn_wave(game)
    end
    return
  end

  -- A wave is live: watch for it being wiped out.
  if living(game) == 0 then
    if wave >= WAVES then
      ended = true
      game.end_round(string.format(
        "*** All %d waves destroyed — the defenders win! ***", WAVES))
    else
      next_wave_at = tick + BREATHER
      game.message(string.format(
        "*** Wave %d cleared! %d seconds until the next. ***",
        wave, BREATHER / 50))
    end
  end
end
