-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, bot holder hop with no hunter close.
--
-- The real script runs as it ships; this arena only sets the scene and reads
-- the script's own state. Seat 0 gets the prize at tick 300. Until he starts a
-- hop, the arena keeps his armour full (so he hops, not forts) and keeps seat 1
-- 7 squares behind him: inside HOP_HUNTER_WITHIN (8), outside HOP_SIDE_WITHIN
-- (5). So the hop square is straight ahead, BUILT_ARMOUR squares along the way
-- the tank faces when the order goes.
--
-- PASS: the hop square is that square, the prize stands there, and then seat 0
-- has it in his tank again (shot down and picked up).

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
  if A.plan == nil then
    if plan ~= nil then
      local fx, fy = facing(me.dir)
      A.plan = { x = plan.x, y = plan.y, kind = plan.kind,
                 ex = whole(me.mx + BUILT_ARMOUR * fx),
                 ey = whole(me.my + BUILT_ARMOUR * fy) }
      game.log(string.format("ARENA %s at %d,%d; ahead is %d,%d", plan.kind,
                             plan.x, plan.y, A.plan.ex, A.plan.ey))
      if plan.kind ~= "hop" or plan.x ~= A.plan.ex or plan.y ~= A.plan.ey then
        verdict(false, string.format("%s at %d,%d, ahead is %d,%d", plan.kind,
                                     plan.x, plan.y, A.plan.ex, A.plan.ey))
      end
    elseif tick % 100 == 0 and me ~= nil and not me.dead then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
      -- 7 squares by the game's own count (the larger of the two
      -- offsets), on the line the tank faces.
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
    end
  end
  local pb = game.pill(pill)
  if A.plan ~= nil and A.built == nil and pb ~= nil and standing(pb) then
    A.built = tick
    if pb.x ~= A.plan.x or pb.y ~= A.plan.y then
      verdict(false, string.format("built at %d,%d, not the hop square", pb.x, pb.y))
    end
  end
  if A.built ~= nil and pb ~= nil and pb.in_tank and holder == 0 then
    verdict(true, string.format("hop to %d,%d, picked up again %d ticks later",
                                A.plan.x, A.plan.y, tick - A.built))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.plan == nil and "no hop" or
                   (A.built == nil and "never built" or "built, never picked up"))
  end
end
