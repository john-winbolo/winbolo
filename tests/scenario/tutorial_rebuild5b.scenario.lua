-- GATE: ticks=20000 bots=0 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5B: a dead blocker is rebuilt after 2 seconds.
--
-- The same set-up as tutorial_park5b: a scripted shooter as the tutorial's
-- player, its 5B blocker built by its man, then the tank on the parking
-- square firing at the target. Here the arena makes the target's first shell on the
-- blocker kill it outright (pill_damage_scale 10000 for that one blow).
--
-- PASS: the blocker died; 2 to 3 seconds later it is back on its square,
-- the player's, at full armour; and the tank on the parking square still
-- kills the target within 60 seconds.

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
    ARENA.died_at = ARENA.died_at or game.tick()
    game.log("ARENA the 5B blocker died at tick " .. game.tick())
  end
end

scenario.callbacks.pill_damage_scale =
  "Arena: the target's first shell on the 5B blocker kills it."
function pill_damage_scale(attacker, n, cause, pill)
  if n == P.t5b_blocker.n and ARENA.phase >= 3 and ARENA.died_at == nil then
    return 10000
  end
  return nil
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
    -- With no blocker for 2 seconds the target's shells knock the tank off
    -- the parking square; a player would drive back, the arena puts it back.
    local me = game.tank(S.player)
    if tick % 10 == 0 and me ~= nil and not me.dead and
       (me.mx ~= PT.t5b_park[1] or me.my ~= PT.t5b_park[2]) then
      game.teleport(S.player, PT.t5b_park[1], PT.t5b_park[2], 0)
    end
    local bl = game.pill(P.t5b_blocker.n)
    local L = P.t5b_blocker
    if A.died_at ~= nil and A.back_at == nil and bl ~= nil and not bl.in_tank
       and bl.x == L.x and bl.y == L.y and bl.owner == S.player and
       bl.armour == rule("pill_max_armour") then
      A.back_at = tick
      game.log(string.format("ARENA blocker back %.2fs after it died",
                             (tick - A.died_at) / 100))
    end
    local tg = game.pill(P.t5b_target.n)
    local dead = tg ~= nil and not tg.in_tank and tg.armour == 0
    if (dead and A.back_at ~= nil) or tick >= A.t + 6000 then
      A.over = true
      local gap = A.back_at and A.died_at and (A.back_at - A.died_at) / 100
      verdict(A.died_at ~= nil and gap ~= nil and gap >= 1.9 and gap <= 3.1
              and dead,
        string.format("blocker died %s; back after %s s; target dead %s",
          tostring(A.died_at ~= nil), tostring(gap), tostring(dead)))
    end
  end
end
