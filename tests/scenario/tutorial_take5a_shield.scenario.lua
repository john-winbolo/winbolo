-- GATE: ticks=32000 bots=1 budget=on script=data/maps/tutorials/Tutorial.scenario.lua
--
-- Tutorial, the Station 5A demo take, with the bots' time limit on.
--
-- The demo is the bot building its blocker next to the target pillbox and
-- parking behind it, so that the target's shells hit the blocker and not
-- the bot. The arena runner normally turns the bots' per-tick time limit
-- off; the game never does. With the limit on, the first shield scan of a
-- take was stopped part way and never tried again, and the bot charged the
-- target with the blocker still in its tank. This arena runs the demo the
-- way the game does (budget=on) and counts where the target's shells land.
--
-- Seat 0 plays the tutorial's player, held on the green watching square in
-- Station 5, as in tutorial_take5a_soak.
--
-- PASS: at least 6 takes, and the target's shells reached the blocker at
-- least three times as often as the bot's tank. With the fix the blocker
-- took about 7 in 8; without it, about 3 in 5. The time limit makes the
-- count vary from run to run, so the line is set well between the two.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, takes = 0, built = 0, bad = 0, deaths = 0, off = 0,
          on_tank = 0, on_blocker = 0, on_other = 0 }

-- Every shell the 5A target fires is asked about once for each thing it
-- reaches: the bot's tank, or a pillbox.
local arena_real_can_hit = can_hit
function can_hit(attacker, kind, n, pill)
  local r = arena_real_can_hit(attacker, kind, n, pill)
  if pill == LAYOUT.pill.t5a_target.n then
    if kind == "tank" and n == S.bot5a then
      ARENA.on_tank = ARENA.on_tank + 1
    elseif kind == "pill" and n == LAYOUT.pill.t5a_blocker.n then
      ARENA.on_blocker = ARENA.on_blocker + 1
    else
      ARENA.on_other = ARENA.on_other + 1
    end
  end
  return r
end
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
  if tick >= 30000 then
    A.done = true
    verdict(A.takes >= 6 and A.on_blocker >= 3 * A.on_tank,
      string.format("%d takes; blockers %d built; target shells reaching: "
        .. "blocker %d, bot tank %d, other %d", A.takes, A.built,
        A.on_blocker, A.on_tank, A.on_other))
  end
end
