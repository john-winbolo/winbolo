-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a guard who dies while his fort stands loses the prize.
--
-- Seat 0 gets the prize at tick 300 and is set up for a fort the way
-- pilltag_shoot_standing is: his armour is held at 15 (at or below
-- FORT_ARMOUR_AT) and seat 1 is kept 7 squares behind him until he builds,
-- then put 5 squares behind him, so the fort is not taken straight back.
-- Seat 1's gun is kept empty and his tank whole while the fort stands, so
-- the fort is never shot down. Half a second after the fort goes up the
-- arena kills the guard.
--
-- PASS, all three:
--   * within one second (100 ticks) of the death the prize is dead (armour
--     0) on its square and nobody holds it;
--   * a hunter picks it up within 10 s of the death. Seat 1 is within 5
--     squares of the fort when the guard dies, and is not moved after. That
--     is at most 5 squares to drive, about 2 s at a tank's grass speed (12
--     world units a frame, 50 frames a second, 256 units a square), plus up
--     to a second for the order to reach the brain, the turn, and the half
--     second park on the "onto" square. 10 s is over twice that;
--   * nobody scored while the guard was dead and nobody held the prize.

ARENA = { given = false }

local arena_real_picked = on_pill_picked_up
function on_pill_picked_up(n, p, scripted)
  local A = ARENA
  if A.killed_at ~= nil and A.picked == nil and n == pill then
    A.picked = { p = p, t = game.tick(), order = told_for[p] }
    game.log(string.format("ARENA picked by %d t=%d (%.1fs after the death) "
                           .. "order=%s", p, game.tick(),
                           (game.tick() - A.killed_at) / 100,
                           tostring(told_for[p])))
  end
  arena_real_picked(n, p, scripted)
end

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
  local s = game.tank(1)
  local pb = game.pill(pill)
  if plan == nil and A.built_at == nil and tick % 100 == 0 and me ~= nil
     and not me.dead then
    game.set_stocks(0, { armour = 15 })
    local fx, fy = facing(me.dir)
    local k = 7 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and not plan.built and A.moved == nil and me ~= nil then
    A.moved = true
    local fx, fy = facing(me.dir)
    local k = 5 / math.max(math.abs(fx), math.abs(fy))
    game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
  end
  if plan ~= nil and plan.built and A.built_at == nil then
    A.built_at, A.fx, A.fy = tick, pb.x, pb.y
    game.log(string.format("ARENA %s up at %d,%d t=%d", plan.kind, pb.x, pb.y,
                           tick))
    if plan.kind ~= "fort" then
      verdict(false, "built a " .. plan.kind .. " with armour 15")
      return
    end
  end
  if A.built_at == nil then
    if tick >= GATE_TICKS - 100 then
      verdict(false, "the fort never went up")
    end
    return
  end
  -- The hunter's gun is kept empty and his tank whole while the fort stands,
  -- so only the death can put the fort down.
  if A.killed_at == nil and s ~= nil and not s.dead then
    if s.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
    if s.armour < game.rule("tank_full_armour") then
      game.set_stocks(1, { armour = game.rule("tank_full_armour") })
    end
  end
  if A.killed_at == nil and tick >= A.built_at + 50 then
    if holder ~= 0 or not standing(pb) then
      verdict(false, "the fort was not standing with seat 0 on it at the kill")
      return
    end
    A.killed_at = tick
    A.score0, A.score1 = seconds[0] or 0, seconds[1] or 0
    game.kill_tank(0)
    game.log(string.format("ARENA guard killed t=%d, seat 1 %d from the fort",
                           tick, s and chebyshev(s.mx, s.my, A.fx, A.fy) or -1))
    return
  end
  if A.killed_at == nil then
    return
  end
  if A.dead_at == nil and pb ~= nil and not pb.in_tank and pb.armour == 0 and
     holder == nil then
    A.dead_at = tick
    game.log(string.format("ARENA prize dead at %d,%d t=%d (%d ticks after "
                           .. "the death)", pb.x, pb.y, tick,
                           tick - A.killed_at))
  end
  if A.dead_at == nil and tick > A.killed_at + 100 then
    verdict(false, string.format("1 s after the death: armour %d, holder %s",
                                 pb and pb.armour or -1, tostring(holder)))
    return
  end
  if A.picked == nil and A.dead_at ~= nil and
     ((seconds[0] or 0) ~= A.score0 or (seconds[1] or 0) ~= A.score1) then
    verdict(false, "somebody scored while nobody held the prize")
    return
  end
  if A.picked ~= nil then
    verdict(A.dead_at ~= nil and A.picked.t - A.killed_at <= 1000,
            string.format("dead %d ticks after the death, picked up by %d "
                          .. "%.1fs after it", A.dead_at - A.killed_at,
                          A.picked.p, (A.picked.t - A.killed_at) / 100))
    return
  end
  if tick > A.killed_at + 1000 then
    verdict(false, "nobody picked the dead prize up within 10 s")
  end
end
