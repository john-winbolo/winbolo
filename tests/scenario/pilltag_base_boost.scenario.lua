-- GATE: ticks=4000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, a base's pit stop gives a speed boost by the two base boost
-- settings, to the holder and to everybody else.
--
-- Seat 0 gets the prize at tick 300 and keeps it. Once the take's boost is
-- over, each case puts a new neutral base by one tank and that tank on it,
-- so the engine captures it and the script gives the pit stop:
--  1. "carrying" Yes, seat 0 (the holder): his boost starts again at the
--     capture, at the full BOOST_MULT, with no cover and no take counted;
--  2. "carrying" No, seat 0: no boost;
--  3. "not carrying" Yes, seat 1: his speed modifier goes over 100 at once,
--     and once BOOST_SECONDS + BOOST_DECAY_SECONDS are up it is back at
--     100 (the classic tank, 0) and his base boost is over;
--  4. "not carrying" No, seat 1: no base boost and his modifier stays at
--     100 for a second.
--
-- PASS: every case does what it says. A case waits for its capture for
-- 5 seconds at most.

ARENA = { step = "give", case = 1, caught = {} }

ARENA.cases = {
  { seat = 0, setting = "base_carrying", on = true },
  { seat = 0, setting = "base_carrying", on = false },
  { seat = 1, setting = "base_others", on = true },
  { seat = 1, setting = "base_others", on = false },
}

-- Every capture the script takes, with the seat and the tick.
local arena_real_captured = on_base_captured
function on_base_captured(n, old, new, scripted)
  arena_real_captured(n, old, new, scripted)
  ARENA.caught[#ARENA.caught + 1] = { n = n, p = new, t = game.tick() }
end

-- A new neutral base three squares east of seat p's tank, on a road, or
-- nil when the map will not take one there.
ARENA.new_base = function(t)
  local x, y = t.mx + 3, t.my
  game.set_tile(x, y, game.TERRAIN.road)
  local n = game.add_base(x, y)
  if n == nil then
    return nil
  end
  return n, { x = x, y = y }
end

-- The capture of base n by seat p since tick `since`, or nil.
ARENA.capture = function(n, p, since)
  for _, c in ipairs(ARENA.caught) do
    if c.n == n and c.p == p and c.t >= since then
      return c
    end
  end
  return nil
end

ARENA.fail = function(why)
  verdict(false, string.format("case %d: %s", ARENA.case, why))
  ARENA.step = "done"
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil or A.step == "done" then return end
  local c = A.cases[A.case]
  if A.step == "give" and tick >= 300 then
    game.give_pill(0, pill)
    A.step = "settle"
  elseif A.step == "settle" then
    -- The take's boost is over, and seat 0 holds the prize in his tank.
    if holder == 0 and boost_from == nil and carrying(0) then
      A.step = "put"
    end
  elseif A.step == "put" then
    if c == nil then
      verdict(true, "holder Yes/No and others Yes/No all as set")
      A.step = "done"
      return
    end
    local t = game.tank(c.seat)
    if t == nil or t.dead then return end
    -- A bot holder hops now and then; the case waits for the prize to be
    -- back in his tank.
    if c.seat == 0 and (holder ~= 0 or not carrying(0)) then
      if holder ~= 0 then
        A.fail("seat 0 lost the prize")
      end
      return
    end
    if c.seat == 0 and boost_from ~= nil then return end
    if c.seat ~= 0 and boost_use.base[c.seat] ~= nil then return end
    local n, b = ARENA.new_base(t)
    if n == nil then
      A.fail("no base could be put down")
      return
    end
    boost_use[c.setting] = c.on
    A.count0 = boost_use.count[0]
    A.base_n, A.put_at = n, tick
    game.teleport(c.seat, b.x, b.y, 64)
    A.step = "caught"
  elseif A.step == "caught" then
    local got = ARENA.capture(A.base_n, c.seat, A.put_at)
    if got == nil then
      if tick >= A.put_at + 500 then
        A.fail("no capture of base " .. A.base_n)
      end
      return
    end
    A.got_at = got.t
    if c.seat == 0 then
      if c.on then
        if boost_from ~= got.t then
          A.fail(string.format("holder boost_from %s, capture at %d",
                               tostring(boost_from), got.t))
          return
        end
        if boost_use.mult ~= BOOST_MULT then
          A.fail(string.format("holder factor %.3f", boost_use.mult))
          return
        end
        if invuln_seat ~= nil then
          A.fail("a base boost gave cover")
          return
        end
        if boost_use.count[0] ~= A.count0 then
          A.fail("a base boost counted as a take")
          return
        end
      elseif boost_from ~= nil then
        A.fail("holder boosted with the setting at No")
        return
      end
      game.log(string.format("ARENA case %d ok t=%d", A.case, tick))
      A.case = A.case + 1
      A.step = "put"
    else
      A.step = c.on and "other_on" or "other_off"
    end
  elseif A.step == "other_on" then
    local t = game.tank(c.seat)
    local ends = A.got_at + (BOOST_SECONDS + BOOST_DECAY_SECONDS) * 100
    if tick <= A.got_at + 5 then
      if t ~= nil and not t.dead and tick == A.got_at + 5
         and (t.mods.speed == 0 or t.mods.speed <= 100) then
        A.fail(string.format("seat %d modifier %d after the base", c.seat,
                             t.mods.speed))
      end
      return
    end
    if tick >= ends + 10 then
      if boost_use.base[c.seat] ~= nil then
        A.fail("the base boost outlived its fade")
        return
      end
      if t ~= nil and t.mods.speed ~= 0 then
        A.fail(string.format("seat %d modifier %d after the fade", c.seat,
                             t.mods.speed))
        return
      end
      game.log(string.format("ARENA case %d ok t=%d", A.case, tick))
      A.case = A.case + 1
      A.step = "put"
    end
  elseif A.step == "other_off" then
    local t = game.tank(c.seat)
    if boost_use.base[c.seat] ~= nil then
      A.fail("seat boosted with the setting at No")
      return
    end
    if t ~= nil and t.mods.speed ~= 0 then
      A.fail(string.format("seat %d modifier %d with the setting at No",
                           c.seat, t.mods.speed))
      return
    end
    if tick >= A.got_at + 100 then
      game.log(string.format("ARENA case %d ok t=%d", A.case, tick))
      A.case = A.case + 1
      A.step = "put"
    end
  end
  if A.step ~= "done" and tick >= GATE_TICKS - 100 then
    verdict(false, string.format("stuck at case %d step %s", A.case, A.step))
  end
end
