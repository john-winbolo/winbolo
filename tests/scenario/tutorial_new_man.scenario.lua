-- GATE: ticks=8000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, a new man comes in close by.
--
-- Seat 0 plays the tutorial's player, held still beside the Station 3 grove.
-- Three times the arena sends the man out to cut a tree and kills him as
-- soon as he leaves the tank. The tutorial's on_lgm_died starts each new
-- man two squares short of his tank (game.builder_parachute with from = 2)
-- instead of on a random start.
--
-- For each death the arena logs the first square the new man is seen on and
-- how long he took to land back in the tank.
--
-- PASS: three deaths; each new man first seen within three squares of the
-- tank; each back in the tank within 15 seconds of his death.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, deaths = {}, bad = 0 }

local SPOT = { 126, 166 }
local TREE = { 124, 166 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_lgm_died = on_lgm_died
function on_lgm_died(p, killer, mx, my, scripted)
  arena_real_lgm_died(p, killer, mx, my, scripted)
  if p == 0 then
    ARENA.cur = { at = game.tick() }
    ARENA.deaths[#ARENA.deaths + 1] = ARENA.cur
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, SPOT[1], SPOT[2], 0)
    A.phase, A.t = 1, tick
    return
  end
  if A.phase == 0 then return end
  local t = game.tank(0)
  local b = game.builder(0)
  if t == nil or b == nil then return end
  local c = A.cur
  if c ~= nil then
    if c.first == nil and b.state ~= "dead" and b.state ~= "in_tank" then
      c.first = { b.mx, b.my }
      c.dist = math.max(math.abs(b.mx - t.mx), math.abs(b.my - t.my))
    end
    if b.state == "in_tank" then
      c.back = tick
      game.log(string.format("ARENA new man %d: first seen at %s, %s squares "
        .. "from the tank; back in the tank %.2f s after his death",
        #A.deaths, c.first and (c.first[1] .. "," .. c.first[2]) or "never",
        tostring(c.dist), (c.back - c.at) / 100))
      if c.first == nil or c.dist > 3 or c.back - c.at > 1500 then
        A.bad = A.bad + 1
      end
      A.cur = nil
    elseif tick - c.at > 3000 then
      verdict(false, "the new man did not land in 30 seconds")
    end
    return
  end
  if #A.deaths >= 3 then
    verdict(A.bad == 0, string.format("%d new men, %d too far or too slow",
                                      #A.deaths, A.bad))
    return
  end
  if b.state == "in_tank" and tick >= A.t + 100 then
    if game.map_tile(TREE[1], TREE[2]) ~= game.TERRAIN.forest then
      game.set_tile(TREE[1], TREE[2], game.TERRAIN.forest)
    end
    game.builder_order(0, "trees", TREE[1], TREE[2])
    A.t = tick
  elseif b.state == "going" then
    game.kill_lgm(0)
  end
end
