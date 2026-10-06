-- GATE: ticks=4000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, hunters drive over a dropped prize.
--
-- At tick 300 the arena puts the prize on the ground, dead, 6 squares north
-- of the middle, with nobody holding it. Seat 0 waits 3 squares west of it
-- and seat 1 4 squares east, both facing south, so each has to turn to it.
-- Both are inside DROPPED_WITHIN (4), so the script sends each onto the
-- prize's own square.
--
-- PASS: a hunter picks the prize up within 10 seconds, and the order that bot
-- held was the "onto" order for the prize's square.

ARENA = { placed = false }

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local A = ARENA
  if A.placed and A.picked == nil and n == pill then
    A.picked = { p = p, t = game.tick(), order = told_for[p] }
    game.log(string.format("ARENA picked by %d t=%d order=%s", p, game.tick(),
                           tostring(told_for[p])))
  end
  arena_real_picked(n, p, scripted)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.placed then
    if tick >= 300 then
      local pb = game.pill(pill)
      A.x, A.y = pb.x, pb.y - 6
      if holder ~= nil and pb.in_tank then
        game.drop_pill(holder, pill, A.x, A.y)
      else
        game.move_pill(pill, A.x, A.y)
      end
      game.set_pill_armour(pill, 0)
      game.teleport(0, A.x - 3, A.y, 128)
      game.teleport(1, A.x + 4, A.y, 128)
      A.placed, A.at = true, tick
      game.log(string.format("ARENA prize down at %d,%d holder=%s", A.x, A.y,
                             tostring(holder)))
    end
    return
  end
  if A.picked ~= nil then
    local want = string.format("onto %d,%d", A.x, A.y)
    verdict(A.picked.order == want, string.format("seat %d took it %.1fs in, order %s",
            A.picked.p, (A.picked.t - A.at) / 100, tostring(A.picked.order)))
  elseif tick >= A.at + 1000 then
    verdict(false, "nobody picked it up in 10 seconds")
  end
end
