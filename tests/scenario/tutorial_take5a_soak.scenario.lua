-- GATE: ticks=62000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 5A demo take, again and again.
--
-- Seat 0 plays the tutorial's player, held on the green watching square in
-- Station 5, which fields the 5A demo bot. The arena then lets the demo run
-- for ten minutes: the bot builds its blocker, takes the target, and the
-- script puts the island back and the bot home, over and over.
--
-- PASS: at least 6 takes; every blocker the bot built itself was on a
-- square orthogonally next to the target (Manhattan distance 1); the bot
-- never died (no drowning, no shells); the bot was never off its island.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, takes = 0, built = 0, bad = 0, deaths = 0, off = 0 }
scenario.callbacks.on_tank_killed = "Arena: counts the 5A demo bot's deaths."

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_placed = on_pill_placed
function on_pill_placed(n, p, armour, scripted)
  arena_real_placed(n, p, armour, scripted)
  if n == LAYOUT.pill.t5a_blocker.n and p == S.bot5a and not scripted then
    local pb = game.pill(n)
    local T = LAYOUT.pill.t5a_target
    ARENA.built = ARENA.built + 1
    if pb == nil or math.abs(pb.x - T.x) + math.abs(pb.y - T.y) ~= 1 then
      ARENA.bad = ARENA.bad + 1
      game.log(string.format("ARENA 5A blocker off-square at %s,%s",
        tostring(pb and pb.x), tostring(pb and pb.y)))
    end
  end
end

function on_tank_killed(victim, killer, cause, scripted)
  if victim == S.bot5a then
    ARENA.deaths = ARENA.deaths + 1
    local t = game.tank(victim)
    local bl = game.pill(LAYOUT.pill.t5a_blocker.n)
    game.log(string.format("ARENA 5A bot died: %s by %s at %s,%s tick %d; "
      .. "blocker %s", tostring(cause), tostring(killer), tostring(t and t.mx),
      tostring(t and t.my), game.tick(),
      bl and (bl.in_tank and "carried" or string.format("%d,%d a%d", bl.x,
        bl.y, bl.armour)) or "gone"))
  end
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  if p == S.bot5a and n == LAYOUT.pill.t5a_target.n and not scripted then
    ARENA.takes = ARENA.takes + 1
    game.log(string.format("ARENA 5A take %d at tick %d", ARENA.takes,
                           game.tick()))
  end
  arena_real_picked(n, p, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  local w = LAYOUT.point.watch5a
  if A.phase == 0 and tick >= 300 then
    game.teleport(0, w[1], w[2], 0)
    A.phase = 1
    return
  end
  if A.phase ~= 1 then return end
  -- Seat 0 is a bot held at 1 percent speed, so it creeps: put it back.
  if tick % 10 == 0 then
    local me = game.tank(0)
    if me ~= nil and not me.dead and (me.mx ~= w[1] or me.my ~= w[2]) then
      game.teleport(0, w[1], w[2], 0)
    end
  end
  local t = game.tank(S.bot5a)
  if t ~= nil and not t.dead and
     not game.in_region("island5a", t.mx, t.my) then
    A.off = A.off + 1
  end
  if tick >= 55000 then
    A.done = true
    verdict(A.takes >= 6 and A.bad == 0 and A.deaths == 0 and A.off == 0,
      string.format("%d takes; blockers %d built, %d off-square; bot deaths "
        .. "%d; ticks off the island %d", A.takes, A.built, A.bad, A.deaths,
        A.off))
  end
end
