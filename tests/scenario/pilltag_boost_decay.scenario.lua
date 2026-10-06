-- GATE: ticks=3000 bots=2 script=data/mods/PillboxTag.scenario.lua
--
-- Pillbox Tag, each take before a death gives less boost, and a death gives
-- the full boost back.
--
-- The arena hands the prize back and forth between seat 0 and seat 1 (the
-- holder drops it under the other, who is given it), one give every 60
-- ticks from tick 300, and reads boost_use.mult (the running
-- boost's full factor) 10 ticks after each give. With the defaults (boost
-- 200%, decay 50% a take):
--  * seat 0, 1, 0, 1, 0: 2.0, 2.0, 1.5, 1.5, 1.25;
--  * seat 0's tank is killed while seat 1 holds, and once it is back:
--    seat 0: 2.0 (its count went with its death);
--  * the decay is put at 0, then seat 1, 0, 1, 0: 2.0 every time. Neither
--    count went back to 0 (seat 1 is on its third and fourth takes), but
--    with no decay every take gets the full BOOST_MULT.
--
-- PASS: every read matches, and the holder after each give is the seat given
-- to.

ARENA = { step = 1, next_at = 300 }

-- What to do, in order. give = seat to give the prize to, want = the factor
-- 10 ticks later; kill = seat to kill, then wait until it is back; decay =
-- the "Boost decay per take" to put in.
ARENA.plan = {
  { give = 0, want = 2.0 },
  { give = 1, want = 2.0 },
  { give = 0, want = 1.5 },
  { give = 1, want = 1.5 },
  { give = 0, want = 1.25 },
  { kill = 0 },
  { give = 0, want = 2.0 },
  { decay = 0 },
  { give = 1, want = 2.0 },
  { give = 0, want = 2.0 },
  { give = 1, want = 2.0 },
  { give = 0, want = 2.0 },
}

ARENA.reads = {}

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if pill == nil or A.step == nil then return end
  local row = A.plan[A.step]
  if row == nil then
    verdict(true, "factors " .. table.concat(A.reads, ", "))
    A.step = nil
    return
  end
  if tick < A.next_at then
    -- waiting
  elseif row.decay ~= nil then
    boost_use.decay = row.decay
    A.step = A.step + 1
  elseif row.kill ~= nil then
    local t = game.tank(row.kill)
    if not A.killed then
      game.kill_tank(row.kill)
      A.killed = true
    elseif t ~= nil and not t.dead then
      A.killed = nil
      A.step = A.step + 1
      A.next_at = tick + 20
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
      verdict(false, string.format("step %d: holder %s, gave to seat %d",
                                   A.step, tostring(holder), row.give))
      A.step = nil
      return
    end
    local got = boost_use.mult
    A.reads[#A.reads + 1] = string.format("%d:%.3g", row.give, got)
    game.log(string.format("ARENA step %d seat %d factor %.4f count %d", A.step,
                           row.give, got, boost_use.count[row.give] or 0))
    if math.abs(got - row.want) > 1e-6 then
      verdict(false, string.format("step %d: seat %d factor %.4f, wanted "
                                   .. "%.4f", A.step, row.give, got,
                                   row.want))
      A.step = nil
      return
    end
    A.given_at = nil
    A.step = A.step + 1
    A.next_at = tick + 50
  end
  if A.step ~= nil and tick >= GATE_TICKS - 100 then
    verdict(false, "stuck at step " .. A.step)
  end
end
