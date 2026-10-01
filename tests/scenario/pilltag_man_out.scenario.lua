-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the man-out rule.
--
-- Seat 0 gets the prize at tick 300 and is set up for a hop the way
-- pilltag_hop_ahead is. The moment his man walks out with the prize, the arena
-- turns the hop square into a building, so the man cannot put it down and
-- brings it back.
--
-- While the man is out with the prize: no points, and the gun gains about a
-- shell a second. When he is back in the tank with it: the gun is empty and
-- the points start again.
--
-- PASS: all four of those hold.

ARENA = { given = false }

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil then return end
  if not A.given then
    if tick >= 300 then
      local pb = game.pill(pill)
      game.teleport(0, pb.x, pb.y, 64)
      game.teleport(1, pb.x - 7, pb.y, 64)
      game.give_pill(0, pill)
      A.given = true
    end
    return
  end
  local me = game.tank(0)
  if me == nil or me.dead then
    verdict(false, "holder died")
    return
  end
  local out = man_out(0)
  if A.out_at == nil then
    if out then
      A.out_at, A.out_shells, A.out_points = tick, me.shells, seconds[0]
      A.last_shells = me.shells
      A.blocked = { x = plan.x, y = plan.y }
      game.set_tile(plan.x, plan.y, game.TERRAIN.building)
      game.log(string.format("ARENA man out t=%d shells=%d points=%d", tick,
                             me.shells, seconds[0]))
    elseif tick % 100 == 0 then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
      -- 7 squares by the game's own count (the larger of the two
      -- offsets), on the line the tank faces.
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
    end
  elseif A.back_at == nil then
    if not out then
      A.back_at = tick
      local secs = (tick - A.out_at) / 100
      local gained = A.last_shells - A.out_shells
      game.log(string.format("ARENA man back t=%d out %.1fs shells %d->%d "
                             .. "points %d->%d now shells=%d in_tank=%s", tick,
                             secs, A.out_shells, A.last_shells, A.out_points,
                             seconds[0], me.shells, tostring(game.pill(pill).in_tank)))
      if seconds[0] ~= A.out_points then
        verdict(false, "scored while the man was out")
      elseif gained < math.floor(secs) - 1 then
        verdict(false, string.format("gun gained %d in %.1fs out", gained, secs))
      elseif not game.pill(pill).in_tank then
        verdict(false, "the prize did not come back")
      elseif me.shells ~= 0 then
        verdict(false, string.format("gun not emptied on return: %d", me.shells))
      end
      A.back_points = seconds[0]
    else
      A.last_shells = me.shells
    end
  elseif tick >= A.back_at + 350 then
    verdict(seconds[0] >= A.back_points + 2,
            string.format("points %d -> %d in 3.5s after the man came back",
                          A.back_points, seconds[0]))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.out_at == nil and "the man never went out" or "the man never came back")
  end
end
