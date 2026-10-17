-- GATE: ticks=40000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 7 bot keeps to its island.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it in Station 7, which fields the Easy bot there. The arena then watches
-- the bot and its builder for two minutes.
--
-- PASS: the Station 7 bot was fielded, and neither its tank nor its builder
-- left the island7 region in two minutes.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, seen = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    A.phase, A.t = 1, tick
    return
  end
  if A.phase ~= 1 or S.bot7 == nil then
    if A.phase == 1 and tick >= A.t + 3000 then
      verdict(false, "the Station 7 bot was never fielded")
    end
    return
  end
  local t = game.tank(S.bot7)
  if t ~= nil and not t.dead then
    A.seen = A.seen + 1
    if not game.in_region("island7", t.mx, t.my) then
      verdict(false, string.format("bot7 tank left the island at %d,%d",
                                   t.mx, t.my))
      return
    end
  end
  local b = game.builder(S.bot7)
  if b ~= nil and b.state ~= "in_tank" and b.state ~= "dead" and
     b.state ~= "parachuting" and
     not game.in_region("island7", b.mx, b.my) then
    verdict(false, string.format("bot7 builder left the island at %d,%d",
                                 b.mx, b.my))
    return
  end
  if tick >= A.t + 12000 then
    verdict(A.seen > 0, string.format("bot7 (seat %d) stayed on the island "
      .. "for 2 minutes (%d live samples)", S.bot7, A.seen))
  end
end
