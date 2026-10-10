-- GATE: ticks=6000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the checkpoint respawn.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it on the road in Station 3, which makes Station 3 the checkpoint, then
-- kills it.
--
-- PASS: the tank comes back on Station 3's checkpoint start, facing north
-- (game.start dir 0 and tank dir 0: the engine counts clockwise from north;
-- the map file writes north as 4, anticlockwise from east), with Station 3's
-- loadout (full shells and armour, no trees), and the checkpoint is still
-- Station 3.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
  if p == 0 and respawn and ARENA.phase == 2 then
    ARENA.phase = 3
    local t = game.tank(0)
    ARENA.at = { mx = mx, my = my, t = game.tick(), dir = t and t.dir }
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, 115, 176, 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 200 then
    if S.reached ~= 3 then
      verdict(false, "checkpoint is " .. tostring(S.reached) .. ", not 3")
      return
    end
    game.kill_tank(0)
    A.phase, A.t = 2, tick
  elseif A.phase == 3 and tick >= A.at.t + 100 then
    local t = game.tank(0)
    local cp = LAYOUT.start.cp3
    local north = game.start(cp.n)
    local ok = A.at.mx == cp.x and A.at.my == cp.y and S.reached == 3 and
               north ~= nil and north.dir == 0 and A.at.dir == 0 and
               t ~= nil and t.trees == 0 and
               t.shells == game.rule("tank_full_shells") and
               t.armour == game.rule("tank_full_armour")
    verdict(ok, string.format("respawn at %d,%d (cp3 %d,%d) dir=%s "
      .. "(start dir %s) reached=%d shells=%s armour=%s trees=%s",
      A.at.mx, A.at.my, cp.x, cp.y, tostring(A.at.dir),
      tostring(north and north.dir),
      S.reached, tostring(t and t.shells), tostring(t and t.armour),
      tostring(t and t.trees)))
  elseif A.phase == 2 and tick >= A.t + 3000 then
    verdict(false, "no respawn in 30 seconds")
  end
end
