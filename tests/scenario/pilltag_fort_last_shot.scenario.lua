-- GATE: ticks=8000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, the fort's last shot.
--
-- Seat 0 gets the prize at tick 300 and is set up for a fort the way
-- pilltag_shoot_standing is (armour held at 15, seat 1 7 squares behind, then
-- 5 when the fort goes up). Once the fort stands, the arena sets it to
-- LAST_SHOT_ARMOUR + 1, so the next hit on it, by anybody, brings it to
-- LAST_SHOT_ARMOUR.
--
-- PASS: at LAST_SHOT_ARMOUR the guard takes the fort back (the plan turns to
-- "take" with the fort still standing), then shoots it down himself and
-- drives over it within PICKUP_SECONDS (5) of the kill, so seat 0 has the prize
-- in his tank again. From the take on, seat 1 is kept at no shells, so it
-- cannot fire the last shot itself.
--
-- The pickup limit tests the drive onto a dead pill one square away: the
-- guard is beside his fort when he shoots it. Without the steering fixes of
-- 2026-10-15 he circled beside it for 45 s or more on most seeds, or the
-- hunter took it.

-- PICKUP_SECONDS is a field, not a local: the script and the arena are near
-- Lua's limit of 200 locals in one chunk.
ARENA = { given = false, PICKUP_SECONDS = 5 }

-- The holder's boost is his carry share times the factor here, on slow
-- ground too, not the road's share. The arena places the hunter off where
-- the guard is when the plan starts, and the guard's route from the take is
-- what puts him there; driving slow ground at a road's speed for the first
-- BOOST_SECONDS he goes somewhere else and the play no longer happens.
-- pilltag_boost_road tests the road boost.
boost_use.road = false

local arena_real_killed = on_pill_killed
function on_pill_killed(n, by, scripted)
  local A = ARENA
  if n == pill and A.take_at ~= nil and A.killed_by == nil then
    A.killed_by, A.killed_at = by, game.tick()
    game.log(string.format("ARENA fort killed by %s t=%d", tostring(by), game.tick()))
  end
  arena_real_killed(n, by, scripted)
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
    A.built_at = tick
    if plan.kind ~= "fort" then
      verdict(false, "built a " .. plan.kind .. " with armour 15")
      return
    end
    game.set_pill_armour(pill, LAST_SHOT_ARMOUR + 1)
    -- The guard's tank is made whole, so this tests the take, not whether a
    -- hurt tank outlives a hunter 5 squares off.
    game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    game.log(string.format("ARENA fort up at %d,%d t=%d, armour set to %d",
                           plan.x, plan.y, tick, LAST_SHOT_ARMOUR + 1))
  end
  if A.built_at ~= nil and A.take_at == nil and plan ~= nil then
    if plan.kind == "take" then
      A.take_at, A.take_armour = tick, pb.armour
      game.log(string.format("ARENA taken back t=%d armour=%d", tick, pb.armour))
      if not standing(pb) or pb.armour > LAST_SHOT_ARMOUR then
        verdict(false, string.format("taken back at armour %d, not the last shot",
                                     pb.armour))
      end
    end
  end
  -- Once the guard takes the fort back the hunter has no shells, so the last
  -- shot is the guard's to fire: this tests the take, not a race with a
  -- hunter who keeps firing at the fort.
  if A.take_at ~= nil and A.killed_by == nil then
    local h = game.tank(1)
    if h ~= nil and not h.dead and h.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
  end
  if A.built_at ~= nil and A.take_at == nil and (plan == nil or not standing(pb)) then
    verdict(false, "the fort was lost before the guard took it back")
  end
  if A.killed_by ~= nil and A.killed_by ~= 0 then
    verdict(false, string.format("seat %s shot the fort down, not the guard",
                                 tostring(A.killed_by)))
  end
  if A.killed_by == 0 and pb.in_tank and holder == 0 then
    local after = (tick - A.killed_at) / 100
    verdict(after <= A.PICKUP_SECONDS,
            string.format("taken back at armour %d, shot and picked up %.1fs later, %.1fs after the kill%s",
                          A.take_armour, (tick - A.take_at) / 100, after,
                          after <= A.PICKUP_SECONDS and "" or
                            string.format(" (over %d s)", A.PICKUP_SECONDS)))
  end
  if A.killed_by == 0 and not GATE_SAID and
     tick - A.killed_at > A.PICKUP_SECONDS * 100 then
    verdict(false, string.format("not picked up within %d s of the kill", A.PICKUP_SECONDS))
  end
  if tick >= GATE_TICKS - 100 then
    verdict(false, A.built_at == nil and "no fort" or
                   (A.take_at == nil and "never taken back" or "never picked up again"))
  end
end
