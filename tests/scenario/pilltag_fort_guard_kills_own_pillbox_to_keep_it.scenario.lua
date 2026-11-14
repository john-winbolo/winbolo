-- GATE: ticks=8000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag: the guard kills his own built prize to keep it.
--
-- Player 0 gets the prize at tick 300 and is set up for a built prize the way
-- pilltag_shoot_built_prize is (armour held at 15, player 1 7 squares behind,
-- then 5 when the built prize goes up). Once player 0's builder is back in his
-- tank, the arena sets the built prize to LAST_SHOT_ARMOUR, the armour a
-- hunter's shots would bring it down to. The mod then starts the take ("last
-- shot").
--
-- PASS: the take starts at LAST_SHOT_ARMOUR (the plan turns to "take" with the
-- built prize still standing), player 0 shoots it down himself, and he drives
-- over it within PICKUP_SECONDS (5) of the kill, so he has the prize in his
-- tank again.
--
-- From the moment the built prize goes up, player 1 has no shells, and the
-- mod's shell a second is not given to it. With shells, player 1 shoots at
-- the built prize, and a second shell already in the air kills it after the
-- take starts. That is a race the hunter wins (see
-- pilltag_guard_quickly_takes_prize_a_hunter_kills), not this test. Before
-- FIRE_BLOCK_FRIENDLY_BUILDER, the guard's first shot killed his own builder
-- on the built prize's square, and the hunter's shot set the armour.
--
-- The pickup limit tests the drive onto a dead pill one square away: the
-- guard is beside his fort when he shoots it. Without the steering fixes of
-- 2026-10-15 he circled beside it for 45 s or more on most seeds, or the
-- hunter took it.

-- PICKUP_SECONDS and the rest are fields, not locals: the script and the
-- arena are near Lua's limit of 200 locals in one chunk.
ARENA = { given = false, PICKUP_SECONDS = 5, dry = {} }

-- A seat in ARENA.dry gets no shells from game.add_stocks (the mod's shell a
-- second); anything else in the same call still goes through.
ARENA.real_add_stocks = game.add_stocks
game.add_stocks = function(p, s)
  if ARENA.dry[p] and s ~= nil and s.shells ~= nil then
    local rest = {}
    for k, v in pairs(s) do
      if k ~= "shells" then rest[k] = v end
    end
    if next(rest) == nil then return true end
    s = rest
  end
  return ARENA.real_add_stocks(p, s)
end

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
    -- The guard's tank is made whole, so this tests the take, not whether a
    -- hurt tank outlives a hunter 5 squares off.
    game.set_stocks(0, { armour = game.rule("tank_full_armour") })
    A.dry[1] = true
    game.log(string.format("ARENA fort up at %d,%d t=%d, player 1 has no shells",
                           plan.x, plan.y, tick))
  end
  -- From the build on, player 1 has no shells (a respawn fills them, so this
  -- is asked every tick).
  if A.dry[1] then
    local h = game.tank(1)
    if h ~= nil and not h.dead and h.shells > 0 then
      game.set_stocks(1, { shells = 0 })
    end
  end
  -- The armour a hunter's shots would leave, set once the guard's builder is
  -- back in his tank.
  if A.built_at ~= nil and A.armour_at == nil and plan ~= nil and
     plan.kind == "fort" and standing(pb) then
    local man = game.builder(0)
    if man ~= nil and man.state == "in_tank" then
      A.armour_at = tick
      game.set_pill_armour(pill, LAST_SHOT_ARMOUR)
      game.log(string.format("ARENA builder in tank t=%d, armour set to %d",
                             tick, LAST_SHOT_ARMOUR))
    end
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
  if A.built_at ~= nil and A.take_at == nil and (plan == nil or not standing(pb)) then
    verdict(false, "the fort was lost before the guard took it back")
  end
  if A.killed_by ~= nil and A.killed_by ~= 0 then
    verdict(false, string.format("player %s shot the fort down, not the guard",
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
