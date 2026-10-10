-- GATE: ticks=40000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 7 bot shoots at the player's man and mostly misses.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it in Station 7, which fields the Easy bot with the tutorial's own init
-- table. The arena then parks the player's tank near the bot's start and
-- sends the player's man out again and again to build road on squares two
-- and three away from the tank, for six minutes. Whenever the player's tank
-- dies the arena puts the new one back at the same spot.
--
-- Two arena-only changes keep the count about the bot's aim at the man:
-- the bot gets TANK_COMBAT_ENABLED=false on top of the tutorial's table, so
-- it does not shoot the player's tank beside him, and the bot's shells pass
-- through the player's tank (can_hit), so a shell cannot burst on the tank
-- as he gets in or out.
--
-- A shot at the man: each tick the arena reads the bot's shells. When the
-- count drops while the man is out, the arena works out where that shell
-- bursts (the bot's tank, its facing and its gun sight) and counts the shot
-- as aimed at the man when the burst is within 2 squares of him and
-- nearer him than the player's tank. A hit is a death of the player's man
-- with the Station 7 bot as the killer, within 120 ticks of a shot at him
-- that burst within 2 squares of where he stood.
--
-- PASS: the bot fired at least 40 shots at the man; between 5 and 15
-- percent of them killed him (so he is not invulnerable, and the bot
-- mostly misses); and the tutorial's can_die never refused a death of the
-- player's man. Seeds 1-8 and 42 measured 5.0 to 14.1 percent, 8.7 percent
-- over all nine (docs/TUTORIAL_DECISIONS.md).

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, refused = 0, died = 0, by_bot = 0, out = 0, seen = 0,
          k = 0, shots = 0, at_man = 0, near_wu = 0, shells = nil,
          logged = 0, hit_man = 0, pend = {}, hist = { 0, 0, 0, 0 },
          last_wx = nil, last_wy = nil }

local SPOT = { 136, 36 }
-- Road squares two and three away from the tank, on the bot's side.
local WORK = { { 136, 34 }, { 138, 34 }, { 138, 36 }, { 136, 33 },
               { 139, 35 }, { 134, 34 } }

local AT_MAN_WU = 512     -- 2 squares: a burst this close was aimed at him

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then
    game.set_modifiers(0, { speed = 1, turn = 1 })
    if ARENA.phase >= 2 then ARENA.move = true end
  end
end

-- The bot's shells pass through the player's tank. A shell aimed near the
-- man that strikes the tank beside him bursts there and kills him, which
-- says nothing about the bot's aim at the man.
local arena_real_can_hit = can_hit
function can_hit(attacker, kind, n, pill)
  if kind == "tank" and n == 0 and S.bot7 ~= nil and attacker == S.bot7 then
    return false
  end
  return arena_real_can_hit(attacker, kind, n, pill)
end

local arena_real_can_die = can_die
function can_die(kind, n, killer, cause, pill)
  local r = arena_real_can_die(kind, n, killer, cause, pill)
  if kind == "builder" and n == 0 and r == false then
    ARENA.refused = ARENA.refused + 1
    game.log(string.format("ARENA can_die refused the man's death (killer %s, %s)",
                           tostring(killer), tostring(cause)))
  end
  return r
end

local arena_real_lgm_died = on_lgm_died
function on_lgm_died(p, killer, mx, my, scripted)
  arena_real_lgm_died(p, killer, mx, my, scripted)
  if p == 0 then
    ARENA.died = ARENA.died + 1
    if S.bot7 ~= nil and killer == S.bot7 then
      ARENA.by_bot = ARENA.by_bot + 1
      -- A hit on a shot at the man: a shot counted as aimed at him, fired
      -- in the last 120 ticks, bursting within 2 squares of where he
      -- stood the tick before he died.
      local A, now = ARENA, ARENA.now or 0
      for i = #A.pend, 1, -1 do
        local q = A.pend[i]
        if now - q.tick <= 120 and A.last_wx ~= nil and
           math.sqrt((q.bx - A.last_wx) ^ 2 + (q.by - A.last_wy) ^ 2)
             <= AT_MAN_WU then
          A.hit_man = A.hit_man + 1
          table.remove(A.pend, i)
          break
        end
      end
    end
    game.log(string.format("ARENA player's man died at %d,%d, killer %s",
                           mx, my, tostring(killer)))
  end
end

-- Where a shell fired now by tank t bursts: sight half squares (128 wu
-- each) along the facing, 0 = north, clockwise, as the brain reads it.
local function burst_of(t)
  local rad = (t.dir or 0) * math.pi * 2 / 256
  local d = (t.sight or 14) * 128
  return t.wx + math.sin(rad) * d, t.wy - math.cos(rad) * d
end

local function count_shots(b)
  local A = ARENA
  local bt = S.bot7 ~= nil and game.tank(S.bot7) or nil
  if bt == nil or bt.dead then A.shells = nil; return end
  local was = A.shells
  A.shells = bt.shells
  if was == nil or bt.shells >= was then return end
  local fired = was - bt.shells
  if b == nil or b.state == "in_tank" or b.state == "dead"
     or b.state == "parachuting" then return end
  A.shots = A.shots + fired
  local bx, by = burst_of(bt)
  local dm = math.sqrt((bx - b.wx) ^ 2 + (by - b.wy) ^ 2)
  local pt = game.tank(0)
  local dt = math.huge
  if pt ~= nil and not pt.dead then
    dt = math.sqrt((bx - pt.wx) ^ 2 + (by - pt.wy) ^ 2)
  end
  if dm <= AT_MAN_WU and dm < dt then
    A.at_man = A.at_man + fired
    A.near_wu = A.near_wu + dm * fired
    local bin = dm < 64 and 1 or dm < 128 and 2 or dm < 256 and 3 or 4
    A.hist[bin] = A.hist[bin] + fired
    A.pend[#A.pend + 1] = { tick = A.now or 0, bx = bx, by = by }
    if #A.pend > 8 then table.remove(A.pend, 1) end
  end
  if A.logged < 12 then
    A.logged = A.logged + 1
    game.log(string.format("ARENA shot: bot (%d,%d) dir %d sight %d burst "
      .. "(%d,%d) man (%d,%d) %d wu off, tank %s wu", bt.wx, bt.wy, bt.dir,
      bt.sight, bx, by, b.wx, b.wy, dm, tostring(math.floor(dt))))
  end
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    local a = LAYOUT.point.s7_arrive
    game.teleport(0, a[1], a[2], 0)
    -- A player drives over the two dead pillboxes on the road in, and so
    -- carries them. Left on the road, they draw the bot off its island.
    hand_pill(0, P.p7_sw1.n)
    hand_pill(0, P.p7_sw2.n)
    A.phase, A.t = 1, tick
    return
  end
  if A.phase == 0 then return end
  if A.phase == 1 then
    if S.bot7 ~= nil and game.tank(S.bot7) ~= nil and tick >= A.t + 200 then
      A.phase, A.t, A.move = 2, tick, true
      -- The tutorial's own init table for the bot, plus tank combat off,
      -- so the bot's shells go at the man and not at the player's tank
      -- beside him (a tank shot bursting near him would count as his).
      local init = bot_init("bot7")
      -- (A value holds 63 bytes, so this rides under a key of its own; the
      -- "0;" makes that key's own token, cfg2=0, mean nothing.)
      init.cfg2 = "0;cfg=TANK_COMBAT_ENABLED=false"
      local ok, code = game.bot_init(S.bot7, init)
      game.log(string.format("ARENA bot_init %s %s: %s / %s", tostring(ok),
                             tostring(code), init.cfg, init.cfg2))
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
  A.now = tick
  local b = game.builder(0)
  count_shots(b)
  if b ~= nil and b.state ~= "in_tank" and b.state ~= "dead"
     and b.state ~= "parachuting" then
    A.last_wx, A.last_wy = b.wx, b.wy
  end
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
  if tick >= A.t + 36000 and not A.done then
    A.done = true
    local rate = A.at_man > 0 and A.hit_man / A.at_man or 0
    local ok = A.refused == 0 and A.at_man >= 40
               and A.hit_man * 20 >= A.at_man and A.hit_man * 20 <= A.at_man * 3
    game.log(string.format("ARENA totals: at man %d, hits %d (%.1f%%), "
      .. "bursts <64/<128/<256/<512: %d/%d/%d/%d", A.at_man, A.hit_man,
      rate * 100, A.hist[1], A.hist[2], A.hist[3], A.hist[4]))
    game.log(string.format("ARENA totals: shots while out %d, deaths %d, "
      .. "by bot %d, refused %d, out %d/%d", A.shots, A.died, A.by_bot,
      A.refused, A.out, A.seen))
    verdict(ok, string.format("hits %d of %d shots at the man (%.1f%%); "
      .. "refused %d", A.hit_man, A.at_man, rate * 100, A.refused))
  end
end
