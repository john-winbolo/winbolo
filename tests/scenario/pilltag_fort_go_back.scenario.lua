-- GATE: ticks=9000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a guard who dies beside his fort comes back for the prize.
--
-- Seat 0 gets the prize at tick 300 and is set up for a fort the way
-- pilltag_shoot_standing is, with seat 1 put far ahead on the board so the
-- holder trails and the fort is taken back at FORT_SECONDS. Half a second
-- after the fort goes up the arena kills the guard; when he is back, it puts
-- his tank 15 squares from the fort, towards the middle of the map. Seat 1's
-- gun is kept empty; he is kept near the fort until it is taken back, then
-- parked far from it and from the guard's way back, so the prize is the
-- guard's to fetch.
--
-- A bot answers no order while its man is out, and the guard's new man
-- walks over to the tank first, so the clock starts once the guard is alive
-- with his man aboard and the fort has been taken back.
--
-- PASS: the guard is sent back (a "back to" order), shoots the fort down and
-- has the prize in his tank again within 30 s of that.

ARENA = { given = false, trail = true }

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
  if A.killed_at == nil and tick >= A.built_at + 50 then
    A.killed_at = tick
    game.kill_tank(0)
    game.log(string.format("ARENA guard killed t=%d", tick))
  end
  if A.take_at == nil and plan ~= nil and plan.kind == "take" then
    A.take_at = tick
    game.log(string.format("ARENA taken back t=%d (%.1fs after the build) "
                           .. "guard dead=%s", tick,
                           (tick - A.built_at) / 100, tostring(me.dead)))
  end
  if A.take_at == nil then
    -- The hunter is kept within 5 squares of the fort.
    if s ~= nil and not s.dead and chebyshev(s.mx, s.my, A.fx, A.fy) > 5 then
      local x, y = standable_near(A.fx - 5, A.fy)
      if x ~= nil then
        game.teleport(1, x, y, 64)
      end
    end
  elseif s ~= nil and not s.dead then
    -- Parked across the map from the fort, off the guard's way back.
    local x, y = standable_near(A.fx, 252 - A.fy)
    if x ~= nil and chebyshev(s.mx, s.my, x, y) > 2 then
      game.teleport(1, x, y, 64)
    end
  end
  if A.killed_at ~= nil and A.saw_dead == nil and me ~= nil and me.dead then
    A.saw_dead = tick
  end
  if A.saw_dead ~= nil and A.back_at == nil and me ~= nil and not me.dead then
    A.back_at = tick
    local x, y = standable_near(A.fx + 15 * sign(126 - A.fx),
                                A.fy + 15 * sign(126 - A.fy))
    if x ~= nil then
      game.teleport(0, x, y, 64)
    end
    game.log(string.format("ARENA guard back t=%d put at %s,%s, %d from the "
                           .. "fort", tick, tostring(x), tostring(y),
                           x and chebyshev(x, y, A.fx, A.fy) or -1))
  end
  if A.back_at ~= nil and A.ready_at == nil and me ~= nil and not me.dead then
    local man = game.builder(0)
    if man ~= nil and man.state == "in_tank" then
      A.ready_at = tick
      game.log(string.format("ARENA guard's man aboard t=%d", tick))
    end
  end
  if A.told_back == nil and told_for[0] ~= nil and
     told_for[0]:sub(1, 7) == "back to" then
    A.told_back = tick
    game.log(string.format("ARENA guard told %s t=%d", told_for[0], tick))
  end
  if holder ~= 0 then
    verdict(false, "seat 0 lost the prize")
  elseif A.ready_at ~= nil and A.take_at ~= nil and pb.in_tank then
    local from = math.max(A.ready_at, A.take_at)
    verdict(A.told_back ~= nil and tick - from <= 3000,
            string.format("picked up %.1fs after the guard was ready, told "
                          .. "back=%s", (tick - from) / 100,
                          tostring(A.told_back ~= nil)))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.back_at == nil and "the guard never came back"
                   or "the guard never picked the prize up again")
  end
end
