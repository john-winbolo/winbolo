-- GATE: ticks=30000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 5A demo take.
--
-- Seat 0 plays the tutorial's player, held still. At tick 300 the arena puts
-- it on the watching strip by the road in Station 5, which fields the 5A
-- demo bot, the player's ally. On its island the bot carries a pillbox: it
-- builds it on a square next to the neutral target, shoots the target dead
-- from behind it, and picks the target up, after which the script puts the
-- target back and sends the bot home.
--
-- The arena logs the bot's square once a second, where the blocker went
-- down and who put it there, and where the bot was for each shot that took
-- armour off the target, so a run shows whether it shot from behind its
-- blocker.
--
-- PASS: the demo bot is on team 1 (the player's); before the take it built
-- its blocker itself on a square next to the target (orthogonal or
-- diagonal); the blocker took the target's fire (its armour fell) while the
-- bot's own shots took the target's; and the bot picked up the target
-- within 150 seconds of being fielded; a second
-- later the target is back on its square, neutral and at full armour, with
-- the bot on the island.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, shots = 0 }

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
  if p == S.bot5a and ARENA.fielded == nil then
    ARENA.fielded = game.tick()
    game.log(string.format("ARENA 5A bot (seat %d) fielded at tick %d", p,
                           ARENA.fielded))
  end
end

local arena_real_placed = on_pill_placed
function on_pill_placed(n, p, armour, scripted)
  arena_real_placed(n, p, armour, scripted)
  if n == LAYOUT.pill.t5a_blocker.n then
    local pb = game.pill(n)
    local T = LAYOUT.pill.t5a_target
    if ARENA.took == nil and pb ~= nil and p == S.bot5a and not scripted and
       math.max(math.abs(pb.x - T.x), math.abs(pb.y - T.y)) == 1 then
      ARENA.blocker_ok = true
    end
    game.log(string.format("ARENA 5A blocker placed by seat %s%s at %s,%s",
      tostring(p), scripted and " (script)" or "", tostring(pb and pb.x),
      tostring(pb and pb.y)))
  end
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local L = LAYOUT.pill.t5a_blocker
  if ARENA.took == nil and p == S.bot5a and n == LAYOUT.pill.t5a_target.n
     and not scripted then
    ARENA.took = game.tick()
    local bl = game.pill(L.n)
    local t = game.tank(p)
    game.log(string.format("ARENA 5A take at tick %d; bot at %s,%s; blocker "
      .. "%s at %s,%s owner %s armour %s; %d hits on the target",
      ARENA.took, tostring(t and t.mx), tostring(t and t.my),
      bl and (bl.in_tank and "carried" or "down") or "gone",
      tostring(bl and bl.x), tostring(bl and bl.y), tostring(bl and bl.owner),
      tostring(bl and bl.armour), ARENA.shots))
  end
  arena_real_picked(n, p, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, 131, 108, 0)          -- the watching strip, Station 5
    A.phase = 1
    return
  end
  -- The blocker soaking up the target's fire.
  local bk = game.pill(LAYOUT.pill.t5a_blocker.n)
  if bk ~= nil and not bk.in_tank and A.took == nil then
    if A.bk_last ~= nil and bk.armour < A.bk_last then A.soaked = true end
    A.bk_last = bk.armour
  else
    A.bk_last = nil
  end
  -- Each fall in the target's armour, with where the bot was.
  local tg = game.pill(LAYOUT.pill.t5a_target.n)
  if tg ~= nil and not tg.in_tank then
    if A.last_armour ~= nil and tg.armour < A.last_armour and A.took == nil then
      A.shots = A.shots + 1
      local t = game.tank(S.bot5a)
      game.log(string.format("ARENA 5A target %d -> %d, bot at %s,%s "
        .. "(wx %s, wy %s)", A.last_armour, tg.armour, tostring(t and t.mx),
        tostring(t and t.my), tostring(t and t.wx), tostring(t and t.wy)))
    end
    A.last_armour = tg.armour
  end
  if A.fielded ~= nil and A.took == nil and (tick - A.fielded) % 100 == 0 then
    local t = game.tank(S.bot5a)
    local bl = game.pill(LAYOUT.pill.t5a_blocker.n)
    game.log(string.format("ARENA 5A t=%ds bot %s,%s shells %s; blocker %s",
      math.floor((tick - A.fielded) / 100), tostring(t and t.mx), tostring(t and t.my),
      tostring(t and t.shells),
      bl and (bl.in_tank and "carried" or string.format("%d,%d a%d", bl.x,
        bl.y, bl.armour)) or "gone"))
  end
  if A.took ~= nil then
    if tick >= A.took + 100 then
      local L = LAYOUT.pill.t5a_target
      local pb = game.pill(L.n)
      local t = game.tank(S.bot5a)
      local slot = game.lobby_slot(S.bot5a)
      local ok = pb ~= nil and not pb.in_tank and pb.x == L.x and
                 pb.y == L.y and pb.owner == game.NEUTRAL and
                 pb.armour == game.rule("pill_max_armour") and t ~= nil and
                 game.in_region("island5a", t.mx, t.my) and A.blocker_ok and
                 A.soaked and A.shots >= 1 and slot ~= nil and slot.team == 1
      verdict(ok, string.format("take %.1fs after fielding; blocker built "
        .. "next to the target %s, soaked fire %s; "
        .. "team %s; target %s at %s,%s owner %s armour %s; bot at %s,%s",
        (A.took - A.fielded) / 100, tostring(A.blocker_ok),
        tostring(A.soaked),
        tostring(slot and slot.team),
        pb and (pb.in_tank and "carried" or "down") or "gone",
        tostring(pb and pb.x), tostring(pb and pb.y),
        tostring(pb and pb.owner), tostring(pb and pb.armour),
        tostring(t and t.mx), tostring(t and t.my)))
    end
  elseif A.fielded ~= nil and tick >= A.fielded + 15000 then
    verdict(false, "no 5A take in 150 seconds (bot5a=" .. tostring(S.bot5a)
            .. ")")
  elseif A.fielded == nil and tick >= 3300 then
    verdict(false, "the 5A bot was never fielded")
  end
end
