-- GATE: ticks=22000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 7 bot never kills the player's man.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it in Station 7, which fields the Easy bot. The arena then parks the
-- player's tank near the bot's start and keeps the player's man out of the
-- tank, building road on the squares beside it, for three minutes. Whenever
-- the player's tank dies the arena puts the new one back near the bot.
--
-- The arena counts every builder death the tutorial's can_die refuses
-- because the Station 7 bot is the killer, and every death of the player's
-- man by any killer.
--
-- PASS: in three minutes the player's man never died with the Station 7
-- bot as the killer, and his man was out of the tank for at least half of
-- the time.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, refused = 0, died = 0, by_bot = 0, out = 0, seen = 0,
          k = 0 }

local SPOT = { 136, 36 }
local WORK = { { 135, 36 }, { 135, 37 }, { 136, 37 }, { 137, 37 } }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then
    game.set_modifiers(0, { speed = 1, turn = 1 })
    if ARENA.phase >= 2 then ARENA.move = true end
  end
end

local arena_real_can_die = can_die
function can_die(kind, n, killer, cause, pill)
  local r = arena_real_can_die(kind, n, killer, cause, pill)
  if kind == "builder" and n == 0 and r == false and S.bot7 ~= nil and
     killer == S.bot7 then
    ARENA.refused = ARENA.refused + 1
    game.log(string.format("ARENA refused the man's death by the bot (%s)",
                           tostring(cause)))
  end
  return r
end

local arena_real_lgm_died = on_lgm_died
function on_lgm_died(p, killer, mx, my, scripted)
  arena_real_lgm_died(p, killer, mx, my, scripted)
  if p == 0 then
    ARENA.died = ARENA.died + 1
    if S.bot7 ~= nil and killer == S.bot7 then ARENA.by_bot = ARENA.by_bot + 1 end
    game.log(string.format("ARENA player's man died at %d,%d, killer %s",
                           mx, my, tostring(killer)))
  end
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
  if A.phase == 0 then return end
  if A.phase == 1 then
    if S.bot7 ~= nil and game.tank(S.bot7) ~= nil and tick >= A.t + 200 then
      A.phase, A.t, A.move = 2, tick, true
    elseif tick >= A.t + 3000 then
      verdict(false, "the Station 7 bot was never fielded")
    end
    return
  end
  local t = game.tank(0)
  if A.move and t ~= nil and not t.dead then
    game.teleport(0, SPOT[1], SPOT[2], 0)
    A.move = false
  end
  local b = game.builder(0)
  if b ~= nil then
    A.seen = A.seen + 1
    if b.state ~= "in_tank" then A.out = A.out + 1 end
    if b.state == "in_tank" and t ~= nil and not t.dead and tick % 25 == 0 then
      A.k = A.k % #WORK + 1
      local w = WORK[A.k]
      game.set_tile(w[1], w[2], game.TERRAIN.grass)
      game.set_stocks(0, { trees = 40 })
      game.builder_order(0, "road", w[1], w[2])
    end
  end
  if tick >= A.t + 18000 then
    local ok = A.by_bot == 0 and A.seen > 0 and A.out * 2 >= A.seen
    verdict(ok, string.format("man killed by the bot %d times; deaths "
      .. "refused %d; man deaths by anyone %d; man out %d of %d ticks",
      A.by_bot, A.refused, A.died, A.out, A.seen))
  end
end
