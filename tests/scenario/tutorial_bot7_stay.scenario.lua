-- GATE: ticks=40000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 7 bot's STAY_AREA keeps it on its island without
-- the script's leash.
--
-- The tutorial gives the Station 7 bot the GoalHunter knob STAY_AREA set to
-- the island7 region: every goal, build order and route outside it is
-- rejected by the brain itself. The script's leash (on_tick sends the bot
-- to its start the frame its tank leaves island7) stays as a backstop. This
-- arena counts how often the leash fires: with the knob it must never fire.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena
-- puts it in Station 7, which fields the Easy bot there. Off the island the
-- map still holds goals that drew the bot out before the knob: the two dead
-- Station 7 pills on the road in, the dead 5B blocker, the dead 4A pill and
-- the neutral 2B base. The arena then watches the bot for three minutes.
--
-- PASS: the Station 7 bot was fielded and seen alive, the leash never sent
-- it back, its tank was never outside island7 on any tick, and its builder
-- never left island7.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, seen = 0, leash = 0, tank_out = 0, man_out = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

-- Count the leash: the script sends the bot to its start with
-- game.teleport_to_start the frame its tank is off the island.
local arena_real_tts = game.teleport_to_start
game.teleport_to_start = function(p, n, ...)
  if S.bot7 ~= nil and p == S.bot7 then
    ARENA.leash = ARENA.leash + 1
    game.log("ARENA leash sent the Station 7 bot back")
  end
  return arena_real_tts(p, n, ...)
end

local arena_real_tick = on_tick
function on_tick(tick)
  -- Before the script's own tick, so a tank the leash is about to move is
  -- still seen where it went.
  local A = ARENA
  if A.phase == 1 and S.bot7 ~= nil then
    local t = game.tank(S.bot7)
    if t ~= nil and not t.dead then
      A.seen = A.seen + 1
      if not game.in_region("island7", t.mx, t.my) then
        A.tank_out = A.tank_out + 1
        if A.tank_out == 1 then
          game.log(string.format("ARENA bot7 tank off the island at %d,%d",
                                 t.mx, t.my))
        end
      end
    end
    local b = game.builder(S.bot7)
    if b ~= nil and b.state ~= "in_tank" and b.state ~= "dead" and
       b.state ~= "parachuting" and
       not game.in_region("island7", b.mx, b.my) then
      A.man_out = A.man_out + 1
      if A.man_out == 1 then
        game.log(string.format("ARENA bot7 builder off the island at %d,%d",
                               b.mx, b.my))
      end
    end
  end
  arena_real_tick(tick)
  if A.phase == 0 and tick >= 300 then
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    A.phase, A.t = 1, tick
    return
  end
  if A.phase ~= 1 then return end
  if S.bot7 == nil then
    if tick >= A.t + 3000 then
      verdict(false, "the Station 7 bot was never fielded")
    end
    return
  end
  if tick >= A.t + 18000 then
    local ok = A.seen > 0 and A.leash == 0 and A.tank_out == 0
               and A.man_out == 0
    verdict(ok, string.format("bot7 (seat %d) 3 minutes: leash %d, tank "
      .. "ticks off the island %d, builder ticks off %d (%d live samples)",
      S.bot7, A.leash, A.tank_out, A.man_out, A.seen))
  end
end
