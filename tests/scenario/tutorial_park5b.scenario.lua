-- GATE: ticks=20000 bots=0 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5B: the parking square works.
--
-- The tutorial's player is a scripted shooter (tests/brains/
-- tutorial_shoot_north): it faces north with its gun sight at the longest
-- range and fires only while it stands on the 5B parking square. The arena
-- spawns it on a free seat, adopts it as the tutorial's player, and puts it
-- on the road in 5B, south of the parking square and out of the target's
-- range. It ticks 5A off as watched, which must put the 5B blocker in the
-- player's tank at once. The player's man builds the blocker on its green
-- square. Then the arena moves the tank onto the parking square, seven
-- squares due south of the target, where it fires straight up the column.
--
-- PASS: the blocker was in the tank as 5B became the goal; it was built on
-- its square; on the parking square the shooter killed the target (armour
-- 0) within 60 seconds; the blocker never died; and the player's tank was
-- never hit.

TUTORIAL_PLAYER = -1          -- no seat yet; the arena spawns the shooter
ARENA = { phase = 0, tank_hits = 0, blocker_died = 0, min_bl = nil }

local arena_real_tank_hit = on_tank_hit
function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  arena_real_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if victim == S.player and ARENA.phase >= 3 then
    ARENA.tank_hits = ARENA.tank_hits + 1
    game.log(string.format("ARENA tank hit by %s (pill %s) for %s",
      tostring(attacker), tostring(pill), tostring(amount)))
  end
end

local arena_real_pill_killed = on_pill_killed
function on_pill_killed(n, by, scripted)
  arena_real_pill_killed(n, by, scripted)
  if n == P.t5b_blocker.n and ARENA.phase >= 3 then
    ARENA.blocker_died = ARENA.blocker_died + 1
    game.log("ARENA the 5B blocker died at tick " .. game.tick())
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.over then return end
  if A.phase == 0 and tick >= 50 then
    local q, code = game.spawn_bot({ slot = game.max_tanks() - 1, team = 1,
      brain = "tutorial_shoot_north", name = "Shooter", loadout = "open" })
    if type(q) ~= "number" then
      A.over = true
      verdict(false, "shooter spawn refused: " .. tostring(code))
      return
    end
    TUTORIAL_PLAYER = q
    adopt_player(q)
    A.phase, A.t = 1, tick
    return
  end
  if A.phase == 1 and tick >= A.t + 100 then
    local q = S.player
    game.teleport(q, PT.t5b_park[1], PT.t5b_park[2] + 3, 0)
    A.phase, A.t = 2, tick
    return
  end
  if A.phase == 2 and A.watched == nil and tick >= A.t + 50 then
    mark(5, "a_watch")
    local bl = game.pill(P.t5b_blocker.n)
    A.handed = bl ~= nil and bl.in_tank and S.carrier[P.t5b_blocker.n] == S.player
    game.log("ARENA 5B blocker handed with 5B: " .. tostring(A.handed))
    local ok, code = game.builder_order(S.player, "pill", PT.t5b_blocker[1],
                                        PT.t5b_blocker[2])
    game.log("ARENA builder order: " .. tostring(ok) .. " " .. tostring(code))
    A.watched = tick
    return
  end
  if A.phase == 2 and A.watched ~= nil then
    if S.done[5].b_build then
      game.teleport(S.player, PT.t5b_park[1], PT.t5b_park[2], 0)
      A.phase, A.t = 3, tick
      game.log("ARENA parked at tick " .. tick)
    elseif tick >= A.watched + 3000 then
      A.over = true
      verdict(false, "the 5B blocker was never built")
    end
    return
  end
  if A.phase == 3 then
    local bl = game.pill(P.t5b_blocker.n)
    if bl ~= nil and not bl.in_tank then
      if A.min_bl == nil or bl.armour < A.min_bl then A.min_bl = bl.armour end
    end
    local tg = game.pill(P.t5b_target.n)
    local dead = tg ~= nil and not tg.in_tank and tg.armour == 0
    if dead or tick >= A.t + 6000 then
      A.over = true
      local me = game.tank(S.player)
      verdict(A.handed and dead and A.blocker_died == 0 and A.tank_hits == 0,
        string.format("handed %s; target dead %s after %.1fs; blocker died "
          .. "%d (min armour %s); tank hits %d; tank at %s,%s",
          tostring(A.handed), tostring(dead), (tick - A.t) / 100,
          A.blocker_died, tostring(A.min_bl), A.tank_hits,
          tostring(me and me.mx), tostring(me and me.my)))
    end
  end
end
