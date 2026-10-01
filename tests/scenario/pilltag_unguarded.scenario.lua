-- GATE: ticks=6000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a hunter and a standing prize with nobody near it.
--
-- Seat 0 gets the prize at tick 300 and hops, the pilltag_hop_ahead set-up:
-- his armour is kept full and seat 1 is kept 7 squares behind him until the
-- hop order goes. When the hop stands and his man is back in the tank, seat 1 is put 10 squares from it
-- along x, gun full, and seat 0 25 squares from it along x and y, and kept
-- there. The two tanks are 25 squares apart, past the 14 a brain sees a tank
-- at. The script
-- hands a standing prize no order, so what seat 1 does is the brain's choice.
--
-- PASS: seat 1 hits the prize within 15 s of being put there. 10 squares at
-- a tank's grass speed is about 4 s to drive; the brain stops short at its
-- firing range and needs about a second to turn and fire. 15 s is over twice
-- that.

ARENA = { given = false }
scenario.callbacks.pill_damage_scale = "Arena: counts the hunter's hits on the prize."

function pill_damage_scale(attacker, n, cause, by_pill)
  local A = ARENA
  if n == pill and attacker == 1 and A.placed ~= nil and A.hit == nil then
    A.hit = game.tick()
  end
  return 100
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
  if plan == nil and A.placed == nil then
    if tick % 100 == 0 and me ~= nil and not me.dead then
      game.set_stocks(0, { armour = game.rule("tank_full_armour") })
      local fx, fy = facing(me.dir)
      local k = 7 / math.max(math.abs(fx), math.abs(fy))
      game.teleport(1, whole(me.mx - k * fx), whole(me.my - k * fy), me.dir)
    end
    if tick >= GATE_TICKS - 100 then
      verdict(false, "no hop")
    end
    return
  end
  if A.placed == nil then
    -- His man walks back to the tank after the build; the tank is moved only
    -- once he is in it, or the hunter goes after a man crossing the island.
    local man = game.builder(0)
    if pb == nil or not standing(pb) or man == nil or
       man.state ~= "in_tank" then
      return
    end
    if plan.kind ~= "hop" then
      verdict(false, "built a " .. plan.kind)
      return
    end
    -- The island is 40 squares across, so both are sent from the hop
    -- towards its middle: the hunter 10 squares along x, the holder 25
    -- along both.
    local dx, dy = sign(126 - pb.x), sign(126 - pb.y)
    if dx == 0 then dx = 1 end
    if dy == 0 then dy = 1 end
    A.hx, A.hy = standable_near(pb.x + 25 * dx, pb.y + 25 * dy)
    local wx, wy = standable_near(pb.x + 10 * dx, pb.y)
    if A.hx == nil or wx == nil then
      verdict(false, "no square for the holder or the hunter")
      return
    end
    game.teleport(0, A.hx, A.hy, 64)
    game.teleport(1, wx, wy, 64)
    game.set_stocks(1, { shells = game.rule("tank_full_shells") })
    A.placed = tick
    game.log(string.format("ARENA hop at %d,%d t=%d; holder at %d,%d, "
                           .. "hunter at %d,%d, hunter order %s", pb.x, pb.y,
                           tick, A.hx, A.hy, wx, wy, tostring(told[1])))
    return
  end
  if me ~= nil and not me.dead and tick % 25 == 0 and
     chebyshev(me.mx, me.my, A.hx, A.hy) > 1 then
    game.teleport(0, A.hx, A.hy, 64)
  end
  if A.hit ~= nil then
    verdict(A.hit - A.placed <= 1500,
            string.format("hunter hit the prize %.1fs after it was left alone",
                          (A.hit - A.placed) / 100))
    return
  end
  if tick % 500 == 0 then
    local s = game.tank(1)
    game.log(string.format("ARENA t=%d hunter at %d,%d order %s, prize %s",
                           tick, s.mx, s.my, tostring(told[1]),
                           pb and (pb.in_tank and "in a tank" or
                                   string.format("armour %d", pb.armour)) or "?"))
  end
  if tick > A.placed + 1500 or tick >= GATE_TICKS - 100 then
    verdict(false, "the hunter did not hit the prize within 15 s")
  end
end
