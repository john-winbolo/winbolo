-- GATE: ticks=12000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, each trip onto deep sea and off again cuts the holder's next
-- deep sea time, and a death or a new holder gives the full time back.
--
-- The arena does not sail anybody. It puts game.map_tile over the host's,
-- so the square under ARENA.wet's tank reads as deep sea while ARENA.wet is
-- set, and the map's own deep sea reads as river (a bot holder runs out
-- onto it now and then, which would start the clock early). The holder is kept out of harm
-- (invuln_seat over him every tick) and never hops, so only the deep sea
-- clock kills him. With the defaults (10 s, decay 50% a trip):
--  * seat 0 holds: 2 s out and back, then his time reads 5 s; out again,
--    he is killed 5 s later (the second trip);
--  * seat 0 holds again after the death: killed 10 s out (the death gave
--    the full time back; a fresh seat's first trip is the full time);
--  * seat 0 holds: 2 s out and back (5 s next), then the prize is handed to
--    seat 1: seat 0's time is full again, and seat 1, out on the sea, is
--    killed 10 s later (the new holder starts at full);
--  * the decay is put at 0: seat 0, 2 s out and back, still has 10 s, and
--    out again is killed 10 s later.
-- Times are game.tick() ticks, a hundred a second. A kill is read to the
-- frame it is seen; the clock is looked at five times
-- a second, so it may come up to 40 ticks after the time.
--
-- PASS: every time read and every kill falls where it should.

ARENA = { step = 1, next_at = 300 }

-- give = seat to give the prize to; wet = seat to put on deep sea, for
-- `hold` ticks, or until his tank dies, wanted `die` ticks out; dry = ticks
-- to wait on land; check = { seat, seconds } his next trip's time; decay =
-- the "Deep water decay per trip" to put in.
ARENA.plan = {
  { give = 0 },
  { wet = 0, hold = 200 },
  { dry = 100, check = { 0, 5 } },
  { wet = 0, die = 500 },
  { give = 0, check = { 0, 10 } },
  { wet = 0, die = 1000 },
  { give = 0 },
  { wet = 0, hold = 200 },
  { dry = 100, check = { 0, 5 } },
  { give = 1, check = { 0, 10 } },
  { wet = 1, die = 1000 },
  { decay = 0 },
  { give = 0 },
  { wet = 0, hold = 200 },
  { dry = 100, check = { 0, 10 } },
  { wet = 0, die = 1000 },
}

ARENA.reads = {}

ARENA.real_map_tile = game.map_tile
game.map_tile = function(x, y)
  local s = ARENA.wet
  if s ~= nil then
    local t = game.tank(s)
    if t ~= nil and not t.dead and t.mx == x and t.my == y then
      return game.TERRAIN.deep_sea
    end
  end
  local real = ARENA.real_map_tile(x, y)
  if real == game.TERRAIN.deep_sea then
    return game.TERRAIN.river
  end
  return real
end

function ARENA.fail(why)
  verdict(false, string.format("step %d: %s", ARENA.step, why))
  ARENA.step = nil
  ARENA.wet = nil
end

function ARENA.check(row)
  if row.check == nil then return true end
  local seat, want = row.check[1], row.check[2]
  local got = wet_time.of(seat)
  ARENA.reads[#ARENA.reads + 1] = string.format("%d:%.3gs", seat, got)
  game.log(string.format("ARENA step %d seat %d next trip %.3f s",
                         ARENA.step, seat, got))
  if math.abs(got - want) > 1e-6 then
    ARENA.fail(string.format("seat %d next trip %.3f s, wanted %.3f",
                             seat, got, want))
    return false
  end
  return true
end

function ARENA.next(tick, wait)
  ARENA.step = ARENA.step + 1
  ARENA.next_at = tick + (wait or 30)
  ARENA.t0 = nil
  ARENA.given_at = nil
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  -- The times are read on game.tick(), the clock the script's deep sea
  -- timer reads; the hook's own tick does not keep step with it here.
  local gate_tick = tick
  tick = game.tick()
  local A = ARENA
  if pill == nil or A.step == nil then return end
  -- Only the deep sea clock may kill the holder, and he never puts the
  -- prize down.
  if holder ~= nil then
    invuln_seat = holder
    invuln_to = tick + 1000
  end
  hop_after = tick + 100000
  local row = A.plan[A.step]
  if row == nil then
    verdict(true, table.concat(A.reads, ", "))
    A.step = nil
    return
  end
  if tick < A.next_at then
    return
  elseif row.decay ~= nil then
    wet_time.decay = row.decay
    ARENA.next(tick)
  elseif row.dry ~= nil then
    if A.t0 == nil then
      A.t0 = tick
    elseif tick >= A.t0 + row.dry then
      if ARENA.check(row) then ARENA.next(tick) end
    end
  elseif row.wet ~= nil then
    local t = game.tank(row.wet)
    local dead = (t == nil or t.dead)
    if A.t0 == nil then
      if holder ~= row.wet or dead then
        return ARENA.fail("seat " .. row.wet .. " is not a live holder")
      end
      if wet_seat ~= nil then
        return ARENA.fail("a deep sea clock already runs for " .. wet_seat)
      end
      A.t0 = tick
      A.wet = row.wet
    elseif A.wet ~= nil then
      if dead then
        local took = tick - A.t0
        A.wet = nil
        A.reads[#A.reads + 1] = string.format("%d:died@%d", row.wet, took)
        game.log(string.format("ARENA step %d seat %d killed %d ticks out",
                               A.step, row.wet, took))
        if row.die == nil then
          return ARENA.fail("killed " .. took .. " ticks out on a short trip")
        elseif took < row.die - 5 or took > row.die + 60 then
          return ARENA.fail(string.format("killed %d ticks out, wanted %d",
                                          took, row.die))
        end
      elseif holder ~= row.wet then
        return ARENA.fail("seat " .. row.wet .. " lost the prize on the sea")
      elseif row.hold ~= nil and tick >= A.t0 + row.hold then
        A.wet = nil
        ARENA.next(tick, 0)
      elseif row.die ~= nil and tick > A.t0 + row.die + 200 then
        return ARENA.fail("still alive " .. (tick - A.t0) .. " ticks out")
      end
    elseif not dead then
      -- back from the death
      ARENA.next(tick, 20)
    end
  elseif A.given_at == nil then
    -- give_pill will not take a pillbox out of a tank, so the holder drops
    -- it on the square of the seat it goes to. That seat drives over it or
    -- is given it on a later tick; one take either way.
    local pb = game.pill(pill)
    local t = game.tank(row.give)
    if t ~= nil and not t.dead then
      if pb ~= nil and pb.in_tank and holder ~= nil and holder ~= row.give then
        game.drop_pill(holder, pill, t.mx, t.my)
      elseif holder == row.give then
        A.given_at = tick
      elseif pb ~= nil and not pb.in_tank then
        game.give_pill(row.give, pill)
        A.given_at = tick
      end
    end
  elseif tick >= A.given_at + 10 then
    if holder ~= row.give then
      return ARENA.fail(string.format("holder %s, gave to seat %d",
                                      tostring(holder), row.give))
    end
    if ARENA.check(row) then ARENA.next(tick, 50) end
  end
  if A.step ~= nil and gate_tick >= GATE_TICKS - 100 then
    ARENA.fail("stuck")
  end
end

ARENA.real_killed = on_tank_killed
function on_tank_killed(victim, killer, cause, scripted)
  game.log(string.format("ARENA tank %s killed by %s, cause %s, tick %d",
                         tostring(victim), tostring(killer), tostring(cause),
                         game.tick()))
  return ARENA.real_killed(victim, killer, cause, scripted)
end
