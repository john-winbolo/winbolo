-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the leader's fort outlasts FORT_SECONDS.
--
-- Seat 0 gets the prize at tick 300 and is set up for a fort the way
-- pilltag_shoot_standing is (armour held at 15, seat 1 7 squares behind, then
-- 5 when the fort goes up). He has carried the prize for some seconds and
-- seat 1 has none, so he is strictly top. Once the fort stands, both guns are
-- kept empty (nobody hurts the fort) and seat 1 is kept within 5 squares of
-- it, so no take-back but the time one could fire.
--
-- PASS: the fort is still standing, not taken back, 15 s after it went up,
-- with the holder still strictly top.

ARENA = { given = false, trail = false }

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
      if A.trail then
        -- The hunter is put far ahead on the board, so the holder trails.
        seconds[1] = 1000
      end
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
    game.log(string.format("ARENA %s up at %d,%d t=%d leads=%s points %d v %d",
                           plan.kind, pb.x, pb.y, tick, tostring(holder_leads()),
                           seconds[0] or 0, seconds[1] or 0))
    if plan.kind ~= "fort" then
      verdict(false, "built a " .. plan.kind .. " with armour 15")
      return
    end
  end
  if A.built_at == nil then
    return
  end
  -- The hunter's gun is kept empty, so he never hurts the fort, and his
  -- tank is kept whole.
  if s ~= nil and not s.dead then
    if s.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
    if s.armour < game.rule("tank_full_armour") then
      game.set_stocks(1, { armour = game.rule("tank_full_armour") })
    end
  end
  if me ~= nil and not me.dead and me.shells > 0 then
    game.set_stocks(0, { shells = 0 })
  end
  -- The hunter is kept within 5 squares of the fort.
  if s ~= nil and not s.dead and chebyshev(s.mx, s.my, A.fx, A.fy) > 5 then
    local x, y = standable_near(A.fx - 5, A.fy)
    if x ~= nil then
      game.teleport(1, x, y, 64)
    end
  end
  if plan == nil or plan.kind ~= "fort" or not standing(pb) then
    verdict(false, string.format("fort over after %.1fs while leads=%s",
                                 (tick - A.built_at) / 100,
                                 tostring(holder_leads())))
  elseif tick >= A.built_at + 1500 then
    verdict(holder_leads(), string.format("fort stood %.1fs, leads=%s, "
                                          .. "points %d v %d",
                                          (tick - A.built_at) / 100,
                                          tostring(holder_leads()),
                                          seconds[0] or 0, seconds[1] or 0))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, "no fort")
  end
end
