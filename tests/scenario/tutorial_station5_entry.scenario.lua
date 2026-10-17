-- GATE: ticks=12000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5 on entry, on leaving and on a respawn.
--
-- Seat 0 plays the tutorial's player, held near still. The arena moves its
-- tank in and out of Station 5 and kills it there, and checks two things
-- at each step:
--
--   * the 5A demo bot (bot5a) is on the field only while the player's tank
--     is in Station 5: fielded on entry, taken off (seat no longer fielded,
--     no tank) a little after the tank leaves, fielded again on re-entry;
--   * once 5A is watched and while the 5B blocker is not built, the
--     blocker is in the player's tank: the moment 5B becomes the goal, on
--     every entry to Station 5, and after a respawn there.
--
-- PASS: every step below logged ok.

TUTORIAL_PLAYER = 0
ARENA = { step = 0, fails = 0 }

local ROAD5 = { 127, 110 }    -- the main road in Station 5
local ROAD4 = { 127, 140 }    -- the main road in Station 4

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local function carried()
  local bl = game.pill(P.t5b_blocker.n)
  return bl ~= nil and bl.in_tank and S.carrier[P.t5b_blocker.n] == 0
end

local function bot_on()
  if S.bot5a == nil then return false end
  local s = game.lobby_slot(S.bot5a)
  local t = game.tank(S.bot5a)
  return S.fielded.bot5a == true and s ~= nil and s.fielded and t ~= nil
end

local function check(name, ok)
  game.log(string.format("ARENA step %s: %s", name, ok and "ok" or "FAILED"))
  if not ok then ARENA.fails = ARENA.fails + 1 end
end

-- Steps: { tick after the previous step, action, check }.
local STEPS = {
  { 300, function() game.teleport(0, ROAD5[1], ROAD5[2], 0) end, nil },
  { 200, nil, function() check("bot5a fielded on entry", bot_on()) end },
  { 10, function() mark(5, "a_watch") end,
        function() check("blocker in the tank as 5B starts", carried()) end },
  { 10, function() game.teleport(0, ROAD4[1], ROAD4[2], 0) end, nil },
  { 300, nil, function()
      check("bot5a off after leaving", not bot_on() and
            (S.bot5a == nil or game.tank(S.bot5a) == nil))
      local tg = game.pill(P.t5a_target.n)
      local L = P.t5a_target
      check("5A target home", tg ~= nil and not tg.in_tank and tg.x == L.x
            and tg.y == L.y)
      check("blocker gone home after leaving", not carried())
    end },
  { 10, function() game.teleport(0, ROAD5[1], ROAD5[2], 0) end, nil },
  { 200, nil, function()
      check("bot5a fielded on re-entry", bot_on())
      check("blocker in the tank on re-entry", carried())
    end },
  { 10, function() game.kill_tank(0) end, nil },
  { 600, nil, function()
      local t = game.tank(0)
      check("respawned in Station 5", t ~= nil and not t.dead and
            station_of(t.mx, t.my) == 5)
      check("blocker in the tank after a respawn", carried())
      check("bot5a still fielded", bot_on())
    end },
}

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.over then return end
  local s = STEPS[A.step + 1]
  if s == nil then
    A.over = true
    verdict(A.fails == 0, string.format("%d steps, %d failed", #STEPS,
                                        A.fails))
    return
  end
  A.at = A.at or tick
  if tick < A.at + s[1] then return end
  if s[2] ~= nil then s[2]() end
  if s[3] ~= nil then s[3]() end
  A.step, A.at = A.step + 1, tick
end
