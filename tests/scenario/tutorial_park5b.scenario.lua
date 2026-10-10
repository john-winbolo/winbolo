-- GATE: ticks=20000 bots=0 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, Station 5B: the parking square works, and the blocker takes
-- real damage and gets armour back only from the announced rebuild.
--
-- The tutorial's player is a scripted shooter (tests/brains/
-- tutorial_shoot_north): it faces the target with its gun sight at the
-- longest range and fires only while it stands on the 5B parking square.
-- The arena spawns it on a free seat, adopts it as the tutorial's player,
-- and puts it on grass in 5B, south of the parking square and out of the
-- target's range. It ticks 5A off as watched, which must put the 5B blocker
-- in the player's tank at once. The player's man builds the blocker on its
-- green square.
--
-- Then two phases on the parking square (4 east and 6 south of the target):
--   1. Watch, 60 seconds: the tank has no shells, so the target shoots the
--      blocker the whole time. The arena logs the blocker's armour every
--      second, counts its deaths and the announced rebuilds, and flags any
--      rise in its armour that does not come with a rebuild announcement (a
--      silent repair). The tank is kept at full armour so it lives; the
--      armour the target's shells take off it is summed.
--   2. Take: the tank gets its shells back and fires until the target dies.
--      The armour the tank loses here is summed.
-- In both the arena puts the tank back on the square when a hit knocks it
-- off, as a player would drive back.
--
-- PASS: the blocker was in the tank as 5B became the goal; it was built on
-- its square; in the watch it lost armour, died at least once and every
-- rise in its armour came with a rebuild announcement; in the take the
-- target died within 60 seconds and the tank lived.

TUTORIAL_PLAYER = -1          -- no seat yet; the arena spawns the shooter
ARENA = { phase = 0, hits = { 0, 0, 0, 0 }, lost = { 0, 0, 0, 0 },
          deaths = { 0, 0, 0, 0 }, rebuilds = { 0, 0, 0, 0 }, silent = 0,
          curve = {} }

local arena_real_tank_hit = on_tank_hit
function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  arena_real_tank_hit(victim, attacker, cause, amount, pill, scripted)
  local A = ARENA
  if victim == S.player and A.phase >= 3 then
    A.hits[A.phase] = A.hits[A.phase] + 1
    A.lost[A.phase] = A.lost[A.phase] + (amount or 0)
  end
end

local arena_real_pill_killed = on_pill_killed
function on_pill_killed(n, by, scripted)
  arena_real_pill_killed(n, by, scripted)
  local A = ARENA
  if n == P.t5b_blocker.n and A.phase >= 3 then
    A.deaths[A.phase] = A.deaths[A.phase] + 1
    game.log(string.format("ARENA the 5B blocker died at tick %d (phase %d)",
                           game.tick(), A.phase))
  end
end

-- The rebuild announcement: the only time the blocker may gain armour.
local real_announce = game.announce
game.announce = function(text, ...)
  local A = ARENA
  if A.phase >= 3 and type(text) == "string" and
     string.find(text, "automatically rebuilt", 1, true) then
    A.rebuilds[A.phase] = A.rebuilds[A.phase] + 1
    A.rebuilt_tick = game.tick()
  end
  return real_announce(text, ...)
end

local function hold_on_park()
  local me = game.tank(S.player)
  if me ~= nil and not me.dead and
     (me.mx ~= PT.t5b_park[1] or me.my ~= PT.t5b_park[2]) then
    game.teleport(S.player, PT.t5b_park[1], PT.t5b_park[2], 0)
  end
end

local function watch_blocker(tick)
  local A = ARENA
  local bl = game.pill(P.t5b_blocker.n)
  local a = (bl ~= nil and not bl.in_tank) and bl.armour or nil
  if a ~= nil and A.last_bl ~= nil and a > A.last_bl and
     A.rebuilt_tick ~= tick then
    A.silent = A.silent + 1
    game.log(string.format("ARENA silent blocker repair %d -> %d at tick %d",
                           A.last_bl, a, tick))
  end
  if a ~= nil and A.last_bl ~= nil and a < A.last_bl then A.wore = true end
  A.last_bl = a
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
      game.set_stocks(S.player, { shells = 0 })
      game.teleport(S.player, PT.t5b_park[1], PT.t5b_park[2], 0)
      A.phase, A.t = 3, tick
      game.log("ARENA parked, watching, at tick " .. tick)
    elseif tick >= A.watched + 3000 then
      A.over = true
      verdict(false, "the 5B blocker was never built")
    end
    return
  end
  if A.phase == 3 then
    if tick % 10 == 0 then hold_on_park() end
    watch_blocker(tick)
    if tick % 100 == 0 then
      A.curve[#A.curve + 1] = tostring(A.last_bl or "-")
      game.set_stocks(S.player, { shells = 0,
                                  armour = rule("tank_full_armour") })
    end
    if tick >= A.t + 6000 then
      -- game.log cuts long lines: ten seconds a line.
      for i = 1, #A.curve, 10 do
        game.log(string.format("ARENA watch blocker armour s%d-%d: %s", i,
          math.min(i + 9, #A.curve),
          table.concat(A.curve, " ", i, math.min(i + 9, #A.curve))))
      end
      local me = game.tank(S.player)
      game.set_stocks(S.player, { shells = rule("tank_full_shells"),
                                  armour = rule("tank_full_armour") })
      A.armour0 = rule("tank_full_armour")
      A.phase, A.t = 4, tick
      game.log("ARENA take starts at tick " .. tick)
    end
    return
  end
  if A.phase == 4 then
    if tick % 10 == 0 then hold_on_park() end
    watch_blocker(tick)
    local tg = game.pill(P.t5b_target.n)
    local dead = tg ~= nil and not tg.in_tank and tg.armour == 0
    local me = game.tank(S.player)
    if dead or tick >= A.t + 6000 or (me ~= nil and me.dead) then
      A.over = true
      local alive = me ~= nil and not me.dead
      local ok = A.handed and S.done[5].b_build and A.wore and
                 A.deaths[3] >= 1 and A.silent == 0 and dead and alive
      game.log(string.format("ARENA WATCH 60s: deaths %d rebuilds %d silent %d",
        A.deaths[3], A.rebuilds[3], A.silent))
      game.log(string.format("ARENA WATCH 60s: tank hits %d armour %d",
        A.hits[3], A.lost[3]))
      game.log(string.format("ARENA TAKE: dead %s after %.1fs, deaths %d",
        tostring(dead), (tick - A.t) / 100, A.deaths[4]))
      game.log(string.format("ARENA TAKE: rebuilds %d hits %d lost %d alive %s",
        A.rebuilds[4], A.hits[4], A.lost[4], tostring(alive)))
      verdict(ok, string.format("handed %s; WATCH 60s: blocker deaths %d, "
        .. "announced rebuilds %d, silent repairs %d, tank hits %d (armour "
        .. "%d); TAKE: target dead %s after %.1fs, blocker deaths %d, "
        .. "rebuilds %d, tank hits %d (armour lost %d), tank alive %s",
        tostring(A.handed), A.deaths[3], A.rebuilds[3], A.silent, A.hits[3],
        A.lost[3], tostring(dead), (tick - A.t) / 100, A.deaths[4],
        A.rebuilds[4], A.hits[4], A.lost[4], tostring(alive)))
    end
  end
end
