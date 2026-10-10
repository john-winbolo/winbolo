-- GATE: ticks=24000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial, the Station 5A demo take.
--
-- Seat 0 plays the tutorial's player, held still at Station 1. The script
-- fields the 5A demo bot at the start; on its island it should knock out the
-- neutral pillbox and pick it up, after which the script puts the pillbox
-- back and sends the bot home.
--
-- PASS: the demo bot picks up the 5A target within 90 seconds of the round
-- starting, and a second later the target is back on its square, neutral and
-- at full armour, with the bot on the island.

TUTORIAL_PLAYER = 0
ARENA = {}

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  if ARENA.took == nil and p == S.bot5a and n == LAYOUT.pill.t5a_target.n
     and not scripted then
    ARENA.took = game.tick()
    game.log(string.format("ARENA 5A take at tick %d", ARENA.took))
  end
  arena_real_picked(n, p, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.took ~= nil then
    if tick >= A.took + 100 then
      local L = LAYOUT.pill.t5a_target
      local pb = game.pill(L.n)
      local t = game.tank(S.bot5a)
      local ok = pb ~= nil and not pb.in_tank and pb.x == L.x and
                 pb.y == L.y and pb.owner == game.NEUTRAL and
                 pb.armour == game.rule("pill_max_armour") and t ~= nil and
                 game.in_region("island5a", t.mx, t.my)
      verdict(ok, string.format("take after %.1fs; target %s at %s,%s "
        .. "owner %s armour %s; bot at %s,%s", A.took / 100,
        pb and (pb.in_tank and "carried" or "down") or "gone",
        tostring(pb and pb.x), tostring(pb and pb.y),
        tostring(pb and pb.owner), tostring(pb and pb.armour),
        tostring(t and t.mx), tostring(t and t.my)))
    end
  elseif tick >= 9000 then
    verdict(false, "no 5A take in 90 seconds (bot5a=" .. tostring(S.bot5a)
            .. ")")
  end
end
